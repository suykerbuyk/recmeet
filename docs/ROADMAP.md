# Roadmap

This document charts future capabilities for recmeet, ordered by implementation effort and standalone value. Each section describes user-facing value, what the current codebase already provides, and the technical delta required.

See [LIVE-CAPTIONING-PLAN.md](LIVE-CAPTIONING-PLAN.md) for the full feasibility study behind Phase 2b.

## Phase 1: Watch-Folder Processing

### What it enables

Drop an audio file (WAV, FLAC, MP3) into a designated folder and have it automatically transcribed, diarized, and summarized — no CLI invocation or tray interaction required. Useful for processing recordings from external devices, voice memos, or batch workflows.

### What exists today

- `run_postprocessing()` already accepts an arbitrary `PostprocessInput.audio_path` — it does not assume the audio came from a live capture.
- `--reprocess` proves the pipeline works on pre-existing files: it loads audio, transcribes, and summarizes without any recording phase.
- The daemon's state machine cleanly separates recording from postprocessing; a watch-folder job only needs the postprocessing half.

### Technical delta

- **inotify watcher.** A new thread (or integration into the poll loop via the self-pipe pattern) monitors a configured directory for `IN_CLOSE_WRITE` events on supported file extensions.
- **Job submission.** On detecting a new file, the watcher submits a postprocessing job through the same `compare_exchange(Idle -> Postprocessing)` gate used by `record.start`.
- **Queue vs reject.** The daemon currently allows only one job at a time. Two options:
  - *Reject-when-busy:* move the file to an `error/` or `pending/` subfolder; the user retries later.
  - *Queue:* maintain a FIFO of pending files, drain them sequentially. This is more useful but adds state that must survive daemon restarts.
- **Config surface.** New config keys: `watch_dir` (path), `watch_enabled` (bool), `watch_extensions` (list).
- **Events.** Reuse existing `state.changed`, `phase`, and `job.complete` events — no new event types needed since the postprocessing pipeline is identical.

### Open questions

- Should the watch folder support recursive subdirectory monitoring?
- How to handle partial writes (large files being copied in)? `IN_CLOSE_WRITE` handles this for local writes, but NFS/CIFS may need a settle-time heuristic.
- Should processed files be moved to a `done/` subfolder, deleted, or left in place with a sidecar marker?

## Phase 2: Network Daemon

### What it enables

Run `recmeet-daemon` on a headless machine (home server, NUC) and control it from tray clients on other machines over the LAN or a Tailscale network. Audio capture still happens on the daemon host; clients get real-time status and retrieve results remotely.

### What exists today

- The NDJSON wire protocol has no Unix-specific features — it is length-agnostic, newline-delimited JSON over a stream socket. Switching the transport layer does not require protocol changes.
- `IpcServer` already supports multiple concurrent clients via `poll()` and a client fd map.
- `broadcast()` sends events to all connected clients, so progress reporting works for N clients out of the box.
- `state.changed` and `phase` events give clients enough information to render live status.

### Technical delta

- **TCP listener.** `ipc_server.cpp` hardcodes `AF_UNIX` for socket creation, bind, and accept. Add a parallel `AF_INET` / `AF_INET6` listener (or replace the Unix socket when running in network mode). Both listeners can coexist in the same poll loop.
- **Authentication.** The Unix socket relies on filesystem permissions (`0700` directory). TCP needs explicit auth:
  - *Simplest:* pre-shared key (PSK) sent as the first message after connect, validated before the client enters the poll set.
  - *Better:* TLS with mutual certificate verification (mTLS), or rely on Tailscale's identity layer (connections from the tailnet are already authenticated).
- **Client identity.** Today clients are anonymous fd numbers. Network mode needs:
  - A client registration step (hostname, display name) stored in the client map.
  - Per-client filtering for events (e.g., a client only cares about jobs it started).
- **Result retrieval.** `job.complete` currently reports `note_path` and `output_dir` as local filesystem paths. Remote clients need a way to fetch results:
  - *Option A:* Add a `results.get` method that streams file contents over the IPC channel.
  - *Option B:* Serve an HTTP endpoint for file downloads (adds a dependency but is more natural for large files).
  - *Option C:* Rely on shared filesystem (NFS, Syncthing, Tailscale file sharing) and keep paths as-is.
- **Bind configuration.** New config keys: `listen_address` (default `127.0.0.1`), `listen_port` (default `0` = disabled), `auth_mode`, `psk` or `tls_cert_path` / `tls_key_path`.

### Design considerations

- Tailscale is the low-friction path: if the daemon binds to `100.x.y.z` (Tailscale IP), the tailnet provides authentication, encryption, and NAT traversal. This avoids implementing TLS entirely for the primary use case.
- The Unix socket should remain the default for local use — zero-config, no auth overhead, systemd socket activation works.
- Firewall/port exposure: binding to `0.0.0.0` without auth would be a security issue. Default to localhost; require explicit opt-in for network listen.

## Phase 2b: Live Captioning

### What it enables

Real-time closed captions during recording, displayed on any connected client — local tray or remote over the network. Captions appear with 200-700 ms latency, giving meeting participants live text output while the recording is still in progress. The final batch transcript (produced post-recording with a larger whisper model) remains the authoritative record; live captions are approximate.

### What exists today

- **sherpa-onnx is already vendored** for VAD and speaker diarization. It also ships streaming (online) ASR models — purpose-built transducer architectures (Zipformer, Conformer) that process audio frame-by-frame and emit tokens incrementally.
- `broadcast()` already pushes events to all connected clients, so caption delivery uses existing infrastructure.
- Audio capture runs on a dedicated thread with real-time priority, producing 16 kHz mono int16 samples — exactly the format streaming ASR models expect.

### Why sherpa-onnx streaming, not whisper

Whisper is an encoder-decoder transformer designed for batch inference. Forcing it into a streaming role via sliding window works but has fundamental drawbacks: redundant computation (~70% overlap per window), hallucination on silence, bursty output, and no cross-window memory. Sherpa-onnx streaming transducers avoid all of these — they maintain internal state across frames and produce incremental output with no redundant work.

A whisper sliding-window fallback (using the `tiny` model, ~0.3-0.8s per 3s chunk on 4-core CPU) remains viable for languages where no sherpa-onnx streaming model exists. See [LIVE-CAPTIONING-PLAN.md](LIVE-CAPTIONING-PLAN.md) for the full comparison.

### Resource budget (CPU-only, 16 GB RAM)

| Component | Memory | CPU |
|---|---|---|
| Streaming ASR model | ~100-200 MB | 5-25% of one core |
| Audio ring buffer (30s) | ~1 MB | negligible |
| Existing daemon overhead | ~50 MB | — |
| **Total** | **~250 MB** | **well under one core** |

This leaves ample headroom for the OS, desktop, and even concurrent batch transcription post-recording.

### Technical delta

- **Audio tap.** Add an `AudioChunkCallback` to `PipeWireCapture` and `PulseMonitorCapture`. In `on_process()` / the read loop, invoke the callback with each chunk of samples alongside the existing buffer accumulation. The batch pipeline is untouched.
- **Streaming ASR engine.** A new component owns a sherpa-onnx online recognizer, consumes audio from a small ring buffer fed by the capture callback, and emits caption events through `broadcast()`.
- **IPC: `caption` event.** `{text, is_partial, timestamp}` — clients replace partial captions in-place and append finalized ones.
- **IPC: `captions.enable` method.** Opt-in toggle per session. Captioning consumes CPU that some users may want reserved.
- **Model management.** Extend the model manager for streaming ASR models (new category alongside `whisper/` and `sherpa/vad/`). Config key: `caption_model`.
- **Client display.** Tray: overlay or tooltip with recent caption text. CLI: optional stderr output during recording.

### Open questions

- Which sherpa-onnx streaming model to recommend as default? The Zipformer transducer family offers the best size/accuracy/speed tradeoff for English.
- Should captions be persisted (written to a `.vtt` or `.srt` file) or treated as ephemeral? Persistence is cheap and useful for accessibility.
- How to handle CPU contention on 2-core systems? Cap inference threads and use `SCHED_BATCH` priority; disable captioning automatically if capture buffer health degrades.

## Phase 3: Multi-Client Session Management

### What it enables

Multiple users on the network can each start independent recordings through their own tray clients, with the daemon managing concurrent sessions. Each client sees only its own session's progress, while an admin view shows all active work.

### What exists today

- The daemon tracks state as a single `std::atomic<State>` global — `g_state`.
- `g_worker` is a single `std::thread` — only one job runs at a time.
- `compare_exchange(Idle -> Recording)` is the concurrency gate; a second `record.start` gets error code 1 (`AlreadyRecording`).
- The broadcast mechanism is already per-fd capable — `broadcast()` iterates clients, so targeted sends are a matter of filtering.

### Technical delta

- **Session objects.** Replace the global singleton state with a `Session` struct holding its own state, stop token, worker thread, and originating client id. A `SessionManager` owns a map of active sessions.
- **Resource contention.** Multiple simultaneous recordings compete for:
  - *Audio hardware:* PipeWire allows multiple capture streams, but monitor sources may conflict. Each session needs its own source selection.
  - *GPU/CPU for transcription:* whisper.cpp and llama.cpp are resource-intensive. A semaphore or job queue may be needed to limit concurrent postprocessing.
  - *Disk I/O:* output directories must be namespaced per session.
- **Scoped events.** Introduce a `session_id` field in events. Clients subscribe to their own session's events by default; admin clients can subscribe to all.
- **Protocol additions:**
  - `session.list` — enumerate active sessions (admin).
  - `session.stop {session_id}` — stop a specific session (owner or admin).
  - `record.start` returns `{session_id}` instead of just `{ok}`.
- **Client roles.** Distinguish between regular clients (can only manage their own sessions) and admin clients (can see and control all sessions). Role assignment could be based on auth credentials from Phase 2.

### Open questions

- Is true concurrency (multiple simultaneous recordings) worth the complexity, or is a job queue sufficient? A queue is simpler and avoids hardware contention.
- Should sessions survive daemon restarts? Persisting session state adds significant complexity.
- How should monitor-source conflicts be handled when two sessions want the same audio?

## Cross-Cutting: Progress and Broadcast Enhancements

These improvements benefit all three phases and can be implemented incrementally.

### Granular progress events

The current `phase` event reports coarse stages (recording, transcribing, summarizing). Finer-grained progress would improve the client experience:

- **Transcription progress:** whisper.cpp reports segment-level progress via callback. Emit `progress` events with percentage or segment count.
- **Download progress:** model downloads currently report `downloading` / `complete`. Adding byte counts (`downloaded`, `total`) enables progress bars.
- **Summarization progress:** streaming LLM output (both local and API) could be forwarded to clients for real-time display.

### Event filtering

As the event vocabulary grows (especially with `caption` events from Phase 2b, which fire 1-5 times per second), clients should be able to subscribe to specific event types. A simple `events.subscribe {types: [...]}` method would let the tray ignore download events it doesn't care about and let non-captioning clients skip caption events, reducing noise on slow links (relevant for Phase 2).

### Heartbeat / keepalive

Network connections (Phase 2) need liveness detection. A periodic `ping` / `pong` exchange (or TCP keepalive configuration) would let clients detect daemon unavailability faster than waiting for a read timeout.

## Non-Goals

The following are explicitly out of scope for this roadmap:

- **Web UI.** The tray applet is the primary client. A web dashboard is a separate project.
- **Multi-host audio capture.** Each daemon captures audio from its own host. Distributed recording across machines is not planned.
- **Cloud storage integration.** Results are local files. Syncing to cloud storage (Google Drive, S3) is left to external tools.
- **Mobile clients.** The tray client targets Linux desktops. Mobile apps are out of scope.
- **Real-time transcription streaming to external consumers.** Live captioning (Phase 2b) broadcasts captions over IPC to recmeet clients. Exposing a generic WebSocket or RTMP caption stream for third-party consumers is out of scope.
