# Live Captioning Feasibility Study

An exploration of real-time closed captioning for recmeet, targeting systems
without GPU acceleration and with no more than 16 GB of RAM.

## Executive Summary

**Verdict: feasible, with the right model choice.**

Real-time captioning is achievable on the target hardware, but not by forcing
whisper.cpp into a streaming role. The most practical path uses sherpa-onnx's
streaming ASR models — a dependency recmeet already vendors for diarization and
VAD. A secondary path uses whisper.cpp in a sliding-window mode with the `tiny`
model, trading accuracy for simplicity.

Both approaches require the same architectural change to the audio capture
layer: a callback mechanism that delivers audio chunks in real-time during
recording, rather than accumulating them for post-hoc retrieval.

## Why Not Just Stream Whisper?

Whisper is an encoder-decoder transformer designed for batch inference. The
`whisper_full()` API processes a complete audio buffer and returns all segments
at once. It has no incremental output mode. The community workaround — a
sliding window that re-transcribes overlapping chunks — works, but has
fundamental limitations:

| Concern | Impact |
|---|---|
| Redundant computation | Each window re-transcribes 50-80% of the previous window's audio |
| Hallucination on silence | Whisper generates plausible-sounding text from silence or noise, requiring explicit VAD gating |
| No partial words | Output appears in complete segments, not word-by-word, creating a "bursty" caption experience |
| Context discontinuity | Each window starts fresh — no memory of prior speech, causing repeated or contradictory output at boundaries |

Whisper sliding-window is a viable fallback (see Approach B below), but it is
not the recommended path.

## Approach A: Sherpa-onnx Streaming ASR (Recommended)

### Why this is the right fit

Sherpa-onnx already ships with recmeet for VAD and speaker diarization. It also
provides streaming (online) ASR models purpose-built for real-time transcription:

- **Streaming transducer models** (Zipformer, Conformer) process audio
  frame-by-frame and emit tokens incrementally as speech unfolds.
- **Designed for CPU inference** — these models target edge devices, phones,
  and embedded systems. A modern x86 laptop is well within their performance
  envelope.
- **No sliding window needed** — the model maintains internal state across
  frames, producing stable incremental output without redundant computation.
- **Low memory footprint** — streaming models are typically 20-80 MB on disk,
  with runtime memory under 200 MB.

### Performance expectations (CPU-only, no GPU)

Based on published sherpa-onnx benchmarks for streaming transducer models:

| Model | Disk | RTF (x86 4-core) | Latency | WER (LibriSpeech) |
|---|---|---|---|---|
| zipformer-transducer-small (English) | ~30 MB | 0.05-0.15 | 200-500 ms | ~5-7% |
| conformer-transducer (English) | ~80 MB | 0.10-0.25 | 300-700 ms | ~4-6% |
| paraformer-streaming (Chinese/English) | ~60 MB | 0.08-0.20 | 300-600 ms | ~5-8% |

RTF (real-time factor) below 1.0 means the model transcribes faster than
real-time. Values of 0.05-0.25 mean it uses only 5-25% of available CPU time
per audio second — leaving ample headroom for the recording pipeline, system
processes, and even concurrent batch transcription.

### Memory budget on 16 GB system

| Component | Memory |
|---|---|
| Streaming ASR model (loaded) | ~100-200 MB |
| Audio ring buffer (30s @ 16kHz mono int16) | ~1 MB |
| Caption text buffer | negligible |
| Existing daemon overhead | ~50 MB |
| **Total recmeet footprint** | **~250-450 MB** |

This leaves 15+ GB for the OS, desktop environment, browser, and other
applications. Even during post-recording batch transcription (where a larger
whisper model loads temporarily), peak usage stays well under 4 GB.

### What needs to change

#### 1. Audio tap (capture layer)

The current capture classes (`PipeWireCapture`, `PulseMonitorCapture`)
accumulate audio in a growing `std::vector<int16_t>` behind a mutex, accessible
only via `drain()` after recording stops.

For live captioning, the capture layer needs a real-time audio callback:

```
// New callback type
using AudioChunkCallback = std::function<void(const int16_t*, size_t)>;

// Added to capture classes
void set_audio_callback(AudioChunkCallback cb);
```

In `PipeWireCapture::on_process()`, after appending to the accumulation buffer,
invoke the callback with the same samples. The callback runs on the PipeWire
thread, so the captioning engine must either be lock-free or copy samples into
its own ring buffer.

The accumulation buffer remains unchanged — batch transcription after recording
still works exactly as before.

#### 2. Streaming ASR engine

A new component that:

- Owns a sherpa-onnx online recognizer (streaming model instance)
- Receives audio chunks from the capture callback
- Feeds chunks to the recognizer frame-by-frame
- Extracts partial and finalized text via `SherpaOnnxOnlineRecognizerGetResult()`
- Emits caption events through the daemon's `broadcast()` mechanism

The engine runs on its own thread, consuming from a small ring buffer fed by
the capture callback. This decouples audio capture timing from inference timing.

#### 3. IPC additions

New event type:

| Event | Data | When |
|---|---|---|
| `caption` | `{text, is_partial, timestamp}` | Streaming ASR produces output |

- `is_partial: true` — interim hypothesis (may change as more audio arrives)
- `is_partial: false` — finalized segment (stable, won't change)

Clients replace partial captions in-place and append finalized ones. This
is standard practice for streaming ASR UIs.

New method:

| Method | Params | Result | Notes |
|---|---|---|---|
| `captions.enable` | `{enabled}` | `{ok}` | Toggle live captioning for a recording session |

Captioning should be opt-in — it consumes CPU that some users may want reserved
for audio capture quality or other work.

#### 4. Model management

Extend the existing model manager to handle sherpa-onnx streaming ASR models:

- New model category alongside `whisper/` and `sherpa/vad/`
- Download from sherpa-onnx model zoo (hosted on Hugging Face)
- Config key: `caption_model` (default: a recommended English zipformer)

#### 5. Tray and CLI display

- **Tray:** a small overlay or tooltip showing recent caption text, updated via
  `caption` events. GTK text rendering is straightforward.
- **CLI:** in daemon client mode, optionally print caption events to stderr
  during recording (similar to how elapsed time is displayed today).
- **Network clients (Phase 2):** caption events broadcast like any other event.
  Remote tray clients see captions with network-induced latency (~10-50 ms on
  LAN/Tailscale, negligible for text).

## Approach B: Whisper Sliding Window (Fallback)

If sherpa-onnx streaming models prove insufficient for a specific language or
use case, whisper.cpp can provide captioning via sliding window:

### How it works

1. Maintain a rolling buffer of the last 5 seconds of audio
2. Every 2-3 seconds, copy the buffer and run `whisper_full()` with the `tiny`
   model
3. Compare output against previous window to deduplicate
4. Gate with VAD (Silero, already available) to skip windows that are pure
   silence — this prevents whisper hallucination

### Performance (CPU-only)

| Model | Disk | RAM | 3s chunk inference (4-core) | Viable? |
|---|---|---|---|---|
| tiny | 75 MB | ~273 MB | 0.3-0.8s | Yes |
| base | 142 MB | ~388 MB | 0.6-1.5s | Marginal |
| small | 466 MB | ~852 MB | 2-5s | No (exceeds chunk interval) |

The `tiny` model comfortably transcribes a 3-second chunk in under 1 second on
a modern 4-core CPU, leaving a 2-second margin before the next chunk. The `base`
model is borderline — it works on fast hardware but may fall behind on older or
thermally constrained systems.

### Tradeoffs vs Approach A

| | Sherpa streaming | Whisper sliding window |
|---|---|---|
| Latency | 200-700 ms | 2-5 seconds |
| Output style | Incremental (word-level) | Bursty (full segments) |
| CPU efficiency | 5-25% of one core | 60-90% of all cores per chunk |
| Silence handling | Built-in endpointing | Requires explicit VAD gating |
| Accuracy (English) | Good (WER 5-7%) | Good-to-better (WER 5-10% for tiny) |
| Multilingual | Model-dependent | Whisper supports 99 languages |
| Code complexity | Moderate (new recognizer integration) | Lower (reuses existing whisper code) |
| Redundant work | None | ~70% per window |

**Whisper's multilingual advantage is significant.** If the user records meetings
in languages where no good sherpa-onnx streaming model exists, whisper
sliding-window may be the only option.

A reasonable strategy: default to sherpa-onnx streaming for supported languages,
fall back to whisper sliding-window for others.

## Interaction with Roadmap Phases

### Phase 1 (Watch-folder)

No interaction. Watch-folder processes completed files — captioning is a
live-recording feature.

### Phase 2 (Network daemon)

Strong synergy. Caption events broadcast over the network let remote clients
display live captions — the primary use case for "closed captioning in a
meeting." The caption event is small (~100 bytes) and low-frequency (~1-5/sec),
so network overhead is negligible.

### Phase 3 (Multi-client sessions)

Caption events need `session_id` scoping, same as all other events. Each
recording session gets its own streaming ASR instance. Memory scales linearly:
2 concurrent sessions = ~400-800 MB for captioning, still well within 16 GB.

## Risks and Open Questions

### Accuracy expectations

Streaming ASR is inherently less accurate than batch transcription with larger
models. Users should understand that live captions are approximate — the final
transcript (produced post-recording with whisper small/medium) will be more
accurate. This is the same model used by YouTube Live, Zoom, and Teams: live
captions are "good enough," final transcript is authoritative.

### CPU contention during recording

Audio capture (PipeWire) is real-time priority. Streaming ASR inference is
CPU-intensive. On a 2-core system, captioning could starve the capture thread.
Mitigations:
- Cap inference thread count (e.g., 2 threads max for captioning, regardless of
  core count)
- Use `nice`/`SCHED_BATCH` for the captioning thread
- Monitor capture buffer health — if samples are being dropped, reduce
  captioning frequency or disable it

### Model availability

Sherpa-onnx streaming models are well-maintained for English, Chinese, and
several other languages. Coverage is narrower than whisper's 99 languages.
The model zoo should be audited for target languages before committing to
Approach A for a specific deployment.

### Testing without live audio

Unit testing streaming ASR requires feeding pre-recorded audio at simulated
real-time speed. A test harness that reads a WAV file and delivers chunks on a
timer would exercise the full pipeline without requiring microphone access.

## Implementation Effort Estimate

| Component | Complexity | Dependencies |
|---|---|---|
| Audio callback in capture classes | Low | None — additive change |
| Ring buffer for captioning thread | Low | None |
| Sherpa-onnx streaming recognizer wrapper | Medium | sherpa-onnx (already vendored) |
| Caption IPC events + broadcast | Low | Existing broadcast infra |
| Model management for streaming models | Low | Existing model manager |
| Tray caption display | Medium | GTK text rendering |
| Whisper sliding-window fallback | Medium | Existing whisper + VAD code |
| Deduplication / overlap handling (whisper) | Medium-High | Novel logic |

The sherpa-onnx streaming path (Approach A) is roughly a week of focused work
for someone familiar with the codebase. The whisper fallback (Approach B) adds
another few days, mostly for the deduplication logic.

## Conclusion

Live captioning on CPU-only hardware with 16 GB RAM is feasible and
architecturally clean. The key insight is that sherpa-onnx — already a project
dependency — provides streaming ASR models designed exactly for this use case.
The main infrastructure gap is an audio callback in the capture layer, which is
a small, additive change that doesn't disturb the existing batch pipeline.

The feature fits naturally into the roadmap: it is independent of Phase 1
(watch-folder), synergizes with Phase 2 (network broadcast of captions), and
scales with Phase 3 (per-session captioning instances). If accepted, it could
slot in as a Phase 2 companion feature, since caption events are most valuable
when broadcast to remote clients.
