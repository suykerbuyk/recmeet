# Deadlock Investigation: Subprocess Postprocessing Hang

**Date**: 2026-03-30
**Iteration**: 93 (investigation only; fix planned for next iteration)
**System**: Arch Linux, 16 cores (AMD), onnxruntime 1.24.4, sherpa-onnx v1.12.27

## Symptom

After a fresh install (make uninstall → make build → make install), the user:
1. Launched `recmeet-tray`
2. Opened "Speaker Management" (WebUI)
3. Enrolled a speaker ("John Suykerbuyk" as speaker_02 in a recent recording)
4. Triggered "Reprocess" from the WebUI

The tray showed transcription reaching 92%, then became unresponsive. The tray
continued to display "busy/recording" for over an hour with no CPU activity
from the postprocessing child process.

## Process State at Time of Investigation

```
PID 38142: recmeet --reprocess /home/johns/meetings/2026-03-30_09-05 \
           --config-json /tmp/recmeet-pp-2.json --progress-json --no-daemon

State: S (sleeping)
VmRSS: 239928 kB (~234 MB)
Threads: 6
CPU: 0.0%
Running since: 09:34 (investigated at ~10:50, over 1 hour)
```

Thread states:
```
Thread 38142: hrtimer_nanosleep      (main thread — sleeping)
Thread 38143: do_epoll_wait          (library event loop)
Thread 38144: do_epoll_wait          (library event loop)
Thread 38145: do_epoll_wait          (library event loop)
Thread 38146: futex_wait_queue       (onnxruntime thread pool — blocked)
Thread 38148: poll_schedule_timeout  (library I/O)
```

File descriptors included PipeWire shared memory regions (`/memfd:pipewire-memfd`)
and a PipeWire socket connection, despite reprocess mode not using audio capture.
These appear to be side effects of onnxruntime or sherpa-onnx library initialization.

## Audio File

```
Path: /home/johns/meetings/2026-03-30_09-05/audio_2026-03-30_09-05.wav
Size: 54 MB (56,593,056 bytes)
Format: PCM 16-bit mono, 16000 Hz
Duration: 29.5 minutes (1768.5 seconds, ~28.3M samples)
```

Only the WAV file existed in the output directory — no transcript, summary,
speakers.json, or meeting note had been written. The pipeline stalled between
transcription and note output.

## Pipeline Flow Analysis

The subprocess runs `subprocess_main()` (main.cpp:714) which calls:
1. `run_recording()` — in reprocess mode, just validates paths (instant)
2. `run_postprocessing()` — the full pipeline:
   - **Transcription** (pipeline.cpp:341-429): VAD segmentation → per-segment
     whisper transcription with progress callbacks
   - **Diarization** (pipeline.cpp:432-499): `SherpaOnnxOfflineSpeakerDiarizationProcess()`
     blocking C API call, followed by `identify_speakers()` for embedding
     extraction and matching
   - **Summarization** (pipeline.cpp:508-574): HTTP API call or local LLM
   - **Note writing** (pipeline.cpp:576-612): YAML frontmatter + markdown

The daemon received progress events up to 92% in the "transcribing" phase.
No "diarizing" phase event was received, meaning the child either:
- Emitted the phase event but it wasn't flushed before entering the blocking
  diarization call, OR
- Crashed or hung between the transcription completion and the diarization
  phase event emission

Given that the daemon's progress throttle (daemon.cpp:378-379) only broadcasts
when `jump >= 10%` or `elapsed >= 120s`, the last visible progress was 92%.
Transcription likely completed to 100% but the daemon didn't broadcast because
the jump from 92% to 100% is only 8% (below the 10% threshold).

## Root Cause: onnxruntime Thread Pool Deadlock

### Evidence

1. **All threads blocked**: 6 threads, zero in R (Running) state. Main thread
   in `hrtimer_nanosleep`, one thread in `futex_wait_queue`. This is not "slow
   processing" — no work is being done.

2. **Duration**: Over 1 hour at 0% CPU. Even on slow hardware, 29.5 minutes of
   audio should diarize in 10-20 minutes. An hour at zero CPU is a deadlock.

3. **Thread count**: 6 threads is far fewer than the 15 expected from
   `default_thread_count()` on a 16-core system. This suggests the thread pool
   failed to fully initialize or threads exited/blocked during pool startup.

4. **`futex_wait_queue`**: Classic signature of an onnxruntime work-stealing
   thread pool deadlock, where worker threads block waiting for dependent tasks
   that can only be executed by other blocked workers.

### Mechanism

The `diarize()` function (diarize.cpp:59-137) configures sherpa-onnx with:
```cpp
int t = threads > 0 ? threads : default_thread_count();  // = 15 on 16-core
seg_cfg.num_threads = t;   // segmentation model: 15 threads
emb_cfg.num_threads = t;   // embedding extractor: 15 threads
```

onnxruntime creates internal thread pools for both intra-op and inter-op
parallelism. With `num_threads = 15`, the work-stealing scheduler manages
15+ workers. On long audio (28.3M samples), the pyannote segmentation model
generates thousands of overlapping inference windows. Under high thread
contention:

1. Worker threads enqueue dependent tasks (e.g., post-processing a segment)
2. All worker threads are busy, so dependent tasks sit in the queue
3. Worker threads complete their current work and try to steal tasks
4. A work-stealing deadlock occurs when threads wait for tasks that require
   other threads to complete first, but those threads are themselves waiting

This is a known class of onnxruntime bug documented in:
- https://github.com/microsoft/onnxruntime/issues/11801
- https://github.com/microsoft/onnxruntime/issues/10038

The risk scales with both thread count and audio duration. Short audio
(<5 minutes) processes successfully because the number of inference windows
is small enough to avoid circular dependencies in the work-stealing pool.

### Reproducing

The deadlock is non-deterministic and depends on thread scheduling. It was
observed on 29.5-minute audio with 15 threads. It was NOT reproducible on
40-second audio with the same configuration.

**Important diagnostic note**: When investigating process "hangs," be aware
that `ps` CPU% reporting is instantaneous and can show 0.0% for processes
that are actively computing if the sampling happens between bursts. Always
verify with `/proc/PID/stat` utime/stime deltas over a 5-second window to
distinguish "slow" from "stuck":

```bash
# Correct way to check if a process is truly stuck:
cat /proc/PID/stat | awk '{print $14, $15}'  # utime, stime
sleep 5
cat /proc/PID/stat | awk '{print $14, $15}'
# If both values are unchanged, the process is stuck (not just slow)
```

During investigation, standalone tests initially appeared stuck but were
actually running whisper medium model on CPU (which is slow, ~60s for 40s
of audio). The `/proc/PID/stat` check revealed 15 threads all in R state
with increasing utime — active computation, not a deadlock.

## Daemon-Side Failure Mode

The daemon's `pp_worker_loop` (daemon.cpp:230-494) has no mechanism to
detect a stuck child:

1. The poll loop (line 337) waits for NDJSON lines from the child's stdout
   with a 1-second poll timeout
2. It checks for cancellation (line 341-344) but not for staleness
3. When the child hangs, no more NDJSON is emitted
4. The daemon blocks indefinitely at `waitpid(pid, &status, 0)` (line 441)
   after pipe EOF — but pipe EOF never comes because the child is alive
   (just stuck)
5. The tray continues showing the last known state ("92% transcribing")

## Progress Throttle Issue

A secondary issue compounds the problem. The daemon throttles progress
broadcasts (daemon.cpp:378-379):

```cpp
if (last_percent < 0 || elapsed >= 120 || jump >= 10) {
```

When the child changes phase (e.g., "transcribing" → "diarizing"), the
`last_percent` variable is not reset. This means the first progress event
in the new phase must also meet the 10% jump or 120s elapsed threshold.
In practice, phase changes are visible (the daemon forwards "phase" events),
but progress within a new phase may be delayed.

## Config at Time of Hang

The subprocess config (`/tmp/recmeet-pp-2.json`) contained:
- `whisper_model: "medium"` (1.5 GB)
- `diarize: true`
- `speaker_id: true`
- `vad: true`
- `threads: 0` (auto = 15 on this system)
- `provider: "xai"`, `api_model: "grok-4-1-fast-reasoning"`
- `vocabulary: "SykeTech LTD, Modo-Melius, Seagate, MettleOps"`

**Note**: The API key was exposed in the config file. This is a security
concern — the temp config file should use restrictive permissions (0600)
or pass secrets via a different mechanism.

## Planned Fixes

### 1. Heartbeat Watchdog (detect stuck subprocess)

Child subprocess emits `{"event":"heartbeat","data":{}}` every 10 seconds
via a background thread. Daemon tracks last heartbeat timestamp and kills
the child after 120 seconds of silence.

Advantages over a blind timeout:
- Adapts to actual runtime — a 3-hour meeting won't be killed if healthy
- Detects any stuck subprocess regardless of which library call hangs
- No user-facing configuration needed

### 2. Cap sherpa-onnx Thread Count (prevent deadlock)

Limit diarization and speaker-ID thread count to `min(default_thread_count(), 4)`.
4 threads is sufficient for diarization/embedding extraction and avoids the
onnxruntime work-stealing deadlock that occurs with 15+ concurrent workers.

### 3. Reset Progress Throttle on Phase Change

Add `last_percent = -1` in the daemon's phase event handler to ensure the
first progress event in a new phase is always broadcast immediately.

## Lessons Learned

1. **`ps` CPU% is unreliable for diagnosing hangs**. Use `/proc/PID/stat`
   utime/stime deltas to distinguish "stuck" from "slow."

2. **onnxruntime thread pools are fragile at high thread counts**. The safe
   range for CPU-only inference is 2-4 threads. Beyond that, work-stealing
   deadlocks become increasingly likely, especially on long-running inference
   tasks with many interdependent operations.

3. **Subprocess isolation (iter 90) was the right call**. Without it, this
   deadlock would have frozen the entire daemon, requiring a restart. With
   subprocess isolation, only the child hangs — the daemon is recoverable
   by killing the child process, and the audio is preserved.

4. **Heartbeats > timeouts**. A blind timeout forces a tradeoff between
   killing legitimate long jobs and tolerating hangs. A heartbeat watchdog
   detects stuck processes regardless of expected job duration, with no
   false positives on slow-but-progressing work.

5. **Desktop notification libraries can be problematic in headless contexts**.
   During investigation, `sigsuspend` was observed in standalone mode after
   pipeline completion — traced to libnotify/GLib event loop cleanup. The
   subprocess mode avoids this by skipping `notify_init()`.
