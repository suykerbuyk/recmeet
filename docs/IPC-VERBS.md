# IPC Verb Reference (V2)

This is the per-verb specification for the `recmeet` V2 thin-client wire
surface introduced across Phases A and C of the
`feat/v2-thin-client` work. The **frame layer** (1-byte discriminators,
length prefix, state-machine reader) is documented in
[`IPC-WIRE-PROTOCOL.md`](IPC-WIRE-PROTOCOL.md); the **architectural
overview** lives in [`ARCHITECTURE.md`](ARCHITECTURE.md). This document
defines the JSON shapes — request, response, errors, event payloads —
that ride on top of the framed transport. The wire was bumped to
`IPC_PROTOCOL_VERSION = 3` in Phase C.9 to retire the legacy
live-recording verbs and the `recording` boolean on `state.changed`.

## Conventions

### Reading a verb entry

Every verb in this document carries the same eight sections:

1. **Purpose** — one or two sentences on what the verb does.
2. **Phase** — sub-phase tag of `thin-client-recording-server` (e.g.
   `A.6`, `C.2`, `C.10b`). Useful when tracing history through the
   commit log.
3. **Wire frame** — frame type that carries the request and the
   response. NDJSON `0x00` unless otherwise noted. Follow-up binary
   frames (`0x01` upload, `0x02` artifact, `0x03` streaming audio) are
   called out per verb.
4. **Request** — JSON shape with field types. Field nesting is
   documented inline; nested objects are encoded as JSON-formatted
   string values when serialized through the `JsonMap` type
   (see "JSON encoding" below).
5. **Response** — JSON shape with field types. `ok: true` is the
   convention for ack-only responses.
6. **Errors** — common error-code conditions for this verb. The full
   error-code table is in the next section.
7. **Example flow** — multi-step exchanges when the verb is part of a
   sequence (most prominently `process.submit` + `0x01` frames +
   `progress.job` + `process.fetch`).
8. **Code refs** — `file:line` pointers into the daemon's handler
   registration. Verified against the current `HEAD` at write time.

### Frame envelopes

Every verb in this document is exchanged inside an NDJSON `0x00`
frame: the frame body is one JSON object terminated by `\n`. Request,
response, and error envelopes follow the JSON-RPC-like shape from
`src/ipc_protocol.h`:

```
Request   : {"id": <int64>, "method": "<verb>", "params": {...}}
Response  : {"id": <int64>, "result": {...}}
Error     : {"id": <int64>, "error": {"code": <int>, "message": "<...>"}}
Event     : {"event": "<name>", "data": {...}, "client_id": "<opt>"}
```

The `id` field is a client-chosen monotonic correlation token; the
daemon echoes it on the matching response or error. Events do not
correlate to a request and have no `id`.

### JSON encoding (`JsonMap` / `JsonVal`)

The C++ value type `JsonMap = std::map<std::string, JsonVal>` is
**flat**: `JsonVal` is a `std::variant` over `string`, `int64`,
`double`, `bool`, and `null` (see `src/ipc_protocol.h:69-70`).
Nested objects and arrays are encoded as JSON-formatted **string**
values inside that map. Both the daemon and client parse those
substrings back into structured form on the receive side. Verb
documentation below shows the *logical* JSON shape; the wire bytes
serialize nested fields as escaped substrings.

For example, `process.submit { credentials: { provider: "openai" } }`
ships as:

```
{"id":7,"method":"process.submit","params":{"credentials":"{\"provider\":\"openai\"}"}}
```

The daemon re-parses the `credentials` substring as a JSON object
during handler dispatch (see the `pull_nested` helper in
`src/daemon.cpp` around line 1643).

### State and phase vocabulary

Job **state** names are lowercase strings produced by
`job_state_name()` in `src/job_queue.cpp:29`. The full set:

- `queued`
- `waiting_on_download`
- `waiting_for_upload`
- `running`
- `done`
- `failed`
- `cancelled`

Job **kind** names are lowercase strings produced by
`job_kind_name()`: `postprocess`, `streaming`, `model_download`.

Job **phase** values (the human-readable lifecycle markers carried on
`progress.job` events) include `queued`, `uploading`,
`downloading_model`, `resumed`, `transcribing`, `diarizing`,
`identifying`, `summarizing`, `streaming`, `finalizing`, `complete`,
`done`, `failed`, `cancelled`. The set is not closed — subprocess-emitted
phases (transcribing, diarizing, identifying, summarizing) appear on
the `phase` event rather than `progress.job`.

### Error codes

`IpcErrorCode` (`src/ipc_protocol.h:120`) defines:

| Code     | Name               | Meaning                                                                  |
|----------|--------------------|--------------------------------------------------------------------------|
| `-32600` | `InvalidRequest`   | Malformed envelope — not a parseable request.                            |
| `-32601` | `MethodNotFound`   | Verb not registered.                                                     |
| `-32602` | `InvalidParams`    | Params parsed but failed semantic validation (missing/out-of-range).     |
| `-32603` | `InternalError`    | Daemon-side failure (subsystem unavailable, file I/O error, etc.).       |
| `1`      | `AlreadyRecording` | Reserved; not surfaced by V2 verbs.                                      |
| `2`      | `NotRecording`     | Reserved; not surfaced by V2 verbs.                                      |
| `3`      | `Busy`             | Resource slot occupied — retry later.                                    |
| `4`      | `PermissionDenied` | Operator policy or ownership check refused the request; retry won't help.|
| `5`      | `JobNotReady`      | Lifecycle state forbids the operation (e.g. fetch on a non-Done job).    |

### Authentication and ownership

Every accepted connection is stamped with a server-issued `client_id`
that the daemon attaches to each `IpcRequest` *before* dispatch
(see `src/ipc_protocol.h:74-87`). The `client_id` is never on the
wire as part of a request — clients never set it. Job-ownership
checks (`process.fetch`, `process.cancel`, `job.status`,
`enroll.finalize`) compare the request's stamped `client_id` against
the job's recorded `client_id` (set at enqueue time via the C.7
binding) and return `PermissionDenied` on mismatch.

## Auth and session (Phase A)

### `auth.ok`

**Purpose** — server-emitted handshake frame confirming the
connection has authenticated and is ready for steady-state traffic.

**Phase** — A.1 (PSK gate), A.4 (carries `client_id`),
A.5 (carries `protocol_version`).

**Wire frame** — NDJSON `0x00`. Server → client only. Not a
request/response pair; the frame has no `id`.

**Frame shape**:

```json
{
  "type": "auth.ok",
  "client_id": "<server-issued id>",
  "protocol_version": 3
}
```

- `client_id` (string) — the per-connection id the daemon will stamp
  on every incoming request from this client.
- `protocol_version` (int) — `IPC_PROTOCOL_VERSION` from
  `src/ipc_protocol.h:66`. A client that reads a mismatched version
  closes the connection (see `verify_auth_ok_and_capture()` in
  `src/ipc_client.cpp`).

**Delivery semantics**:

- **Unix socket** — synthesized at accept time. The kernel-enforced
  peer credentials are trusted; no PSK challenge is exchanged.
  Emission site: `src/ipc_server.cpp:531`.
- **TCP socket** — emitted only after the client's first frame is a
  well-formed `{"type":"auth.token","token":"<psk>"}` that matches
  the configured PSK. Emission site: `src/ipc_server.cpp:615`. On
  mismatch the server sends an `auth.error` frame instead and closes
  the connection.

**Errors** — auth failures produce an `auth.error` frame:

```json
{"type": "auth.error", "reason": "auth_required" | "invalid_token"}
```

**Code refs** — `src/ipc_server.cpp:87` (`make_auth_ok_frame`),
`src/ipc_server.cpp:531` (Unix bypass), `src/ipc_server.cpp:615`
(TCP success), `src/ipc_client.cpp:37` (client-side parser).

### `session.init`

**Purpose** — establish per-client credentials and preferences in one
round trip. The server stores the parsed `SessionCredentials` and
`SessionPreferences` keyed by the request's stamped `client_id`;
subsequent job submissions inherit them via `make_job_config` at the postprocess-enqueue seams.

**Phase** — A.6.

**Wire frame** — NDJSON `0x00`.

**Request**:

```json
{
  "id": 1,
  "method": "session.init",
  "params": {
    "credentials": {
      "provider": "openai",
      "api_key": "sk-...",
      "api_keys": {"openai": "sk-...", "anthropic": "..."}
    },
    "preferences": {
      "output_dir": "/home/u/notes",
      "note_dir": "/home/u/notes",
      "language": "en",
      "vocabulary": "kubernetes, etcd",
      "mic_source": "default",
      "monitor_source": "default",
      "whisper_model": "large-v3",
      "summarization_backend": "http",
      "llm_model": "gpt-4o-mini",
      "captions_enabled": true,
      "caption_latency_ms": 500
    }
  }
}
```

Both `credentials` and `preferences` are optional nested objects. An
omitted field keeps the struct default (see `SessionCredentials` and
`SessionPreferences` in `src/ipc_server.h:34-58`).

**Response**:

```json
{"id": 1, "result": {"ok": true, "session_active": true}}
```

**Errors**:

- `InvalidParams` — `summarization_backend` not in `{"", "http",
  "local"}`, or `caption_latency_ms` outside `[200, 2000]`.
- `InternalError` — request not stamped with a `client_id`
  (defensive; the server stamps every accepted connection).

**Code refs** — `src/daemon.cpp:1656` (handler), `src/ipc_server.h:34`
(`SessionCredentials`), `src/ipc_server.h:46` (`SessionPreferences`),
`src/daemon.cpp:1597` (`validate_prefs_payload`).

### `session.update_credentials`

**Purpose** — partial refresh of the per-client credential slot. The
handler snapshots the existing `SessionCredentials`, overlays only
the fields present in the request, and stores the merged result.

**Phase** — A.6.

**Wire frame** — NDJSON `0x00`.

**Request**:

```json
{
  "id": 2,
  "method": "session.update_credentials",
  "params": {
    "provider": "anthropic",
    "api_keys": {"anthropic": "sk-ant-..."}
  }
}
```

Note: the fields here are **flat** on `params` — not nested under a
`credentials` key. Compare with `session.init` which uses the nested
form.

**Response**:

```json
{"id": 2, "result": {"ok": true}}
```

**Errors**:

- `InternalError` — request not stamped with a `client_id`, or the
  session slot is unavailable.

**Code refs** — `src/daemon.cpp:1697`.

### `session.update_prefs`

**Purpose** — partial refresh of the per-client preference slot.
Mirrors `session.update_credentials` but for `SessionPreferences`.
String fields use "non-empty overlays"; the booleans and integer
(`captions_enabled`, `caption_latency_ms`) check key presence
explicitly so an omitted boolean does not implicitly clear it.

**Phase** — A.6.

**Wire frame** — NDJSON `0x00`.

**Request**:

```json
{
  "id": 3,
  "method": "session.update_prefs",
  "params": {
    "caption_latency_ms": 750,
    "summarization_backend": "local"
  }
}
```

Validated fields:

- `caption_latency_ms` — must be in `[200, 2000]`.
- `summarization_backend` — must be `""`, `"http"`, or `"local"`.

**Response**:

```json
{"id": 3, "result": {"ok": true}}
```

**Errors**:

- `InvalidParams` — range violation on `caption_latency_ms` or value
  violation on `summarization_backend`.
- `InternalError` — request not stamped with a `client_id`, or the
  session slot is unavailable.

**Code refs** — `src/daemon.cpp:1722`, validation helper at
`src/daemon.cpp:1597`.

## Job submission (Phase C.2, C.8)

### `process.submit`

**Purpose** — request an upload session for a batch postprocess job
(`mode: "transcribe"`) or a voiceprint enrollment (`mode: "enroll"`).
The daemon reserves a job_id, allocates a staging directory, and
returns an `upload_token` the client must echo back when it follows up
with `0x01` upload frames carrying the audio bytes.

**Phase** — C.2 (transcribe), C.8 (enroll mode).

**Wire frame** — NDJSON `0x00` for the verb itself; follow-up
`0x01` binary upload frames carry the audio payload.

**Request**:

```json
{
  "id": 10,
  "method": "process.submit",
  "params": {
    "audio_size": 16777216,
    "format": "s16le",
    "sample_rate": 16000,
    "channels": 1,
    "context": "Weekly engineering sync, 2026-05-15",
    "mode": "transcribe",
    "enroll_name": ""
  }
}
```

- `audio_size` (int64) — total bytes the client will upload. The
  daemon uses this to size the staging file up front.
- `format` (string) — one of `"s16le"`, `"f32le"`, `"wav"`, `"flac"`,
  `"mp3"`, `"m4a"`, `"ogg"`. Raw PCM is wrapped into WAV via
  libsndfile on the staging side.
- `sample_rate` (int) — Hz; default 16000.
- `channels` (int) — default 1.
- `context` (string) — meeting context forwarded to the summarizer.
- `mode` (string) — `"transcribe"` (default) or `"enroll"` (C.8).
- `enroll_name` (string) — required when `mode == "enroll"`; the
  label the resulting voiceprint will be stored under by
  `enroll.finalize`. Ignored for transcribe mode.

`speaker_hints` is reserved for v2 multi-server and is
accepted-and-ignored (the handler does not read it).

**Response**:

```json
{
  "id": 10,
  "result": {
    "job_id": 47,
    "upload_token": "ut_3b9f...",
    "max_size": 67108864
  }
}
```

- `job_id` (int64) — the reserved postprocess job id. Used by
  `process.fetch`, `process.cancel`, `job.status`, `enroll.finalize`.
- `upload_token` (string) — anti-replay token the client echoes in
  the `0x01` frames. The daemon binds it to the `job_id` and the
  originating `client_id`.
- `max_size` (int64) — server's effective cap on this upload
  (`min(daemon.yaml [ipc] max_upload_bytes, audio_size)`).

**Errors**:

- `InvalidParams` — `audio_size <= 0`, unsupported `format`,
  `mode == "enroll"` with empty `enroll_name`, or `audio_size`
  exceeds the daemon's `max_upload_bytes`.
- `Busy` — no postprocess slot reservation available.
- `InternalError` — upload subsystem unavailable, staging path
  un-writable.

**Example flow** — transcribe a 16 MiB WAV:

```
client → process.submit { audio_size=16777216, format="wav", ... }
client ← { job_id: 47, upload_token: "ut_3b9f...", max_size: 16777216 }

client → 0x01 frames (each <= max_binary_frame_bytes) carrying the WAV
client ← progress.job { job_id: 47, phase: "uploading",
                        bytes_received: ..., bytes_total: 16777216 }
client ← progress.job { job_id: 47, phase: "transcribing" }
client ← phase { job_id: 47, name: "diarizing" }
...
client ← job.complete { job_id: 47, note_path: "...", output_dir: "..." }

client → process.fetch { job_id: 47 }
client ← { job_id: 47, artifacts: [...], total_size: ... }
client ← 0x02 frame (artifact[0])
client ← 0x02 frame (artifact[1])
...
```

**Example flow** — enroll a speaker (two-step):

```
client → process.submit { mode: "enroll", enroll_name: "Alice", ... }
client ← { job_id: 51, upload_token: "ut_...", max_size: ... }
client → 0x01 frames (audio)
client ← job.complete { job_id: 51, enroll_mode: true,
                        speakers: [{idx:0, duration_ms:...}, ...] }

# Client picks `target_speaker` from speakers[]
client → enroll.finalize { job_id: 51, target_speaker: 0,
                           enroll_name: "Alice" }
client ← { ok: true, enroll_name: "Alice", embedding_count: 1 }
```

**Code refs** — `src/daemon.cpp:1953`, `SubmitRequest` at
`src/upload_session.h:94`.

### `process.submit.cancel`

**Purpose** — abort an in-flight upload session before its `0x01`
frames have been fully consumed. Equivalent to closing the TCP
connection mid-upload but explicit and idempotent.

**Phase** — C.2.

**Wire frame** — NDJSON `0x00`.

**Request**:

```json
{
  "id": 11,
  "method": "process.submit.cancel",
  "params": {"upload_token": "ut_3b9f..."}
}
```

**Response**:

```json
{"id": 11, "result": {"ok": true}}
```

**Errors**:

- `InvalidParams` — missing `upload_token`, or the token is unknown
  or not owned by the requesting client.
- `InternalError` — upload subsystem unavailable.

**Code refs** — `src/daemon.cpp:2017`.

## Job processing and lifecycle (Phase C.3, C.5, C.6)

### `process.cancel`

**Purpose** — unified cancellation by `job_id`. Works for any
non-terminal job regardless of kind (postprocess, streaming,
model_download) or state (queued, waiting_on_download,
waiting_for_upload, running). The handler dispatches to the right
teardown path internally.

**Phase** — C.5.

**Wire frame** — NDJSON `0x00`.

**Request**:

```json
{
  "id": 20,
  "method": "process.cancel",
  "params": {"job_id": 47}
}
```

**Response**:

```json
{"id": 20, "result": {"ok": true}}
```

Cancellation also triggers a `state.changed` broadcast as a side
effect.

**Dispatch table** (see `src/daemon.cpp:2154` for the source):

| Kind            | State                   | Action                                                                                   |
|-----------------|-------------------------|------------------------------------------------------------------------------------------|
| `postprocess`   | `queued`                | `g_jobs->cancel(job_id)` — lazy FIFO removal at next dequeue.                            |
| `postprocess`   | `waiting_on_download`   | `g_jobs->cancel(job_id)` — `finish_download` skips re-arming.                            |
| `postprocess`   | `waiting_for_upload`    | `g_uploads->cancel_by_job_id(job_id)` + `g_jobs->cancel(job_id)`.                        |
| `postprocess`   | `running`               | `g_jobs->cancel(job_id)` + `g_pp_stop.request()` + `kill(child, SIGTERM)`.               |
| `streaming`     | any non-terminal        | `g_streaming->cancel_by_job_id(job_id)` (engine + WAV + slot).                           |
| `model_download`| any non-terminal        | `g_jobs->cancel(job_id)` — download worker observes via `finish_download`.               |

**Errors**:

- `InvalidParams` — missing/non-positive `job_id`, unknown `job_id`,
  or job is already in a terminal state (`done` / `failed` /
  `cancelled`). Idempotent on terminal jobs in the sense that the
  state does not change, but the error is reported.
- `PermissionDenied` — the job is not owned by the requesting
  `client_id`.
- `InternalError` — job queue unavailable.

**Code refs** — `src/daemon.cpp:2088`.

### `job.status`

**Purpose** — read-only single-job snapshot. Surfaces the JobQueue
registry to clients without exposing the internal `PostprocessInput`
or `Config` (those carry secrets and are deliberately not on the
wire).

**Phase** — C.6.

**Wire frame** — NDJSON `0x00`.

**Request**:

```json
{
  "id": 30,
  "method": "job.status",
  "params": {"job_id": 47}
}
```

**Response**:

```json
{
  "id": 30,
  "result": {
    "job_id": 47,
    "kind": "postprocess",
    "state": "running",
    "client_id": "cl_a3b9...",
    "model_id": "",
    "error": ""
  }
}
```

- `kind` — `"postprocess"`, `"streaming"`, or `"model_download"`.
- `state` — one of the seven state names (see "State and phase
  vocabulary" above).
- `model_id` — populated only for `model_download` jobs; empty
  string otherwise.
- `error` — empty unless `state == "failed"`.

The C.7 registry retains terminal jobs, so a `Done` / `Failed` /
`Cancelled` job_id remains queryable for the lifetime of the daemon.

**Errors**:

- `InvalidParams` — missing/non-positive `job_id` or unknown
  `job_id`.
- `PermissionDenied` — job is not owned by the requesting client.
- `InternalError` — job queue unavailable.

**Code refs** — `src/daemon.cpp:2495`.

### `job.list`

**Purpose** — enumerate every job owned by the requesting client,
across all kinds and lifecycle states (including terminal jobs).
Used by the tray on restart to re-sync.

**Phase** — C.6.

**Wire frame** — NDJSON `0x00`.

**Request**:

```json
{"id": 31, "method": "job.list", "params": {}}
```

Server-side scoping is by the request's stamped `client_id`; the
verb takes no params.

**Response**:

```json
{
  "id": 31,
  "result": {
    "jobs": [
      {"job_id": 12, "kind": "postprocess", "state": "done",
       "client_id": "cl_a3b9...", "model_id": "", "error": ""},
      {"job_id": 47, "kind": "postprocess", "state": "running",
       "client_id": "cl_a3b9...", "model_id": "", "error": ""}
    ],
    "count": 2
  }
}
```

Ordering is ascending `job_id` (the `std::map` iteration order of
`list_by_client`). The `jobs[]` element shape exactly matches
`job.status`'s flat fields — the daemon shares a
`serialize_job_object` helper between the two verbs so the field set
cannot drift.

**Errors**:

- `InternalError` — job queue unavailable.

**Code refs** — `src/daemon.cpp:2566`, helper at
`src/daemon.cpp:2476`.

## Artifact download (Phase C.4)

### `process.fetch`

**Purpose** — download all artifacts of a completed postprocess job.
Returns a metadata response naming the artifacts, followed by one
`0x02` binary artifact frame per entry, in the same order as
`artifacts[]`.

**Phase** — C.4.

**Wire frame** — NDJSON `0x00` for the metadata response;
follow-up `0x02` binary artifact frames carry the bytes.

**Request**:

```json
{
  "id": 40,
  "method": "process.fetch",
  "params": {"job_id": 47}
}
```

**Response (metadata)**:

```json
{
  "id": 40,
  "result": {
    "job_id": 47,
    "artifacts": [
      {"name": "transcript.md", "size": 12453, "content_type": "text/markdown"},
      {"name": "summary.md",    "size":  4129, "content_type": "text/markdown"},
      {"name": "diarization.json", "size": 8311, "content_type": "application/json"}
    ],
    "total_size": 24893
  }
}
```

After the metadata response, the daemon `post()`s the binary
fan-out onto the poll thread: one `0x02` frame per `artifacts[]`
entry, in the same order. Clients write each frame to
`output_dir / artifacts[i].name`.

**Artifact filtering** — only files in the job's `out_dir`
top-level are eligible. The following are excluded:

- Audio files (`.wav`, `.mp3`, `.flac`, `.m4a`, `.ogg`).
- `speakers.json` (server-side index, not a per-job artifact).
- `context.json` (input, not output).
- Hidden files (leading dot).
- Files inside nested subdirectories.

**Errors**:

- `InvalidParams` — missing/non-positive `job_id` or unknown
  `job_id`.
- `PermissionDenied` — job is not owned by the requesting client.
- `JobNotReady` — job is not in `done` state. Includes the
  current state in the message.
- `InternalError` — `out_dir` missing, unreadable, or any
  artifact exceeds the binary-frame cap (16 MiB default; raise
  `[ipc] max_upload_bytes` to fix).

**Example flow** — see the `process.submit` example above for the
end-to-end submit → fetch sequence.

**Code refs** — `src/daemon.cpp:2254`, artifact enumeration in
`src/fetch_artifacts.cpp`.

## Streaming (Phase C.10a, C.10b)

### `process.stream`

**Purpose** — open a live-audio streaming session. The daemon
allocates a streaming job, mints a `stream_token`, and stands up a
disk-backed temp WAV that all `0x03` audio frames are appended to in
real time. A `CaptionEngine` consumes the same stream and emits
`caption` events. Streaming sessions are **mandatorily disk-backed**
— there is no in-memory buffering mode.

**Phase** — C.10a.

**Wire frame** — NDJSON `0x00` for the verb itself; follow-up
`0x03` streaming-audio frames carry raw PCM payloads.

**Request**:

```json
{
  "id": 50,
  "method": "process.stream",
  "params": {
    "format": "s16le",
    "sample_rate": 16000,
    "channels": 1,
    "context": "Standup, 2026-05-15",
    "language": "en",
    "captions_enabled": true,
    "latency_budget_ms": 500
  }
}
```

Defaults come from `StreamRequest` (`src/streaming_session.h:86`):
`format="s16le"`, `sample_rate=16000`, `channels=1`,
`language="en"`, `captions_enabled=true`, `latency_budget_ms=500`.

- `language` — English-only guard: any non-`"en"` value is rejected
  with `InvalidParams`.
- `latency_budget_ms` — must be in `[200, 2000]`. Drives the
  caption emit cadence.

`speaker_hints` is reserved for v2 multi-server and is
accepted-and-ignored.

**Response**:

```json
{
  "id": 50,
  "result": {
    "job_id": 60,
    "stream_token": "st_a17c..."
  }
}
```

- `job_id` (int64) — the streaming job id. Use with
  `process.cancel` and `job.status`. **NB**: this is the streaming
  job's id, distinct from any postprocess job_id minted by
  `process.stream.commit`.
- `stream_token` (string) — opaque token the client echoes when
  sending `0x03` audio frames.

**Errors**:

- `InvalidParams` — non-English `language`, `latency_budget_ms`
  out of range, unsupported `format`.
- `Busy` — streaming slot already occupied.
- `InternalError` — streaming subsystem unavailable.

**Code refs** — `src/daemon.cpp:1799`, `StreamRequest` at
`src/streaming_session.h:86`.

### `process.stream.cancel`

**Purpose** — abort an in-flight streaming session by token.
Discards the buffered audio, unlinks the temp WAV, and releases the
streaming slot. Idempotent on already-closed sessions (returns
`InvalidParams` "unknown stream_token").

**Phase** — C.10a.

**Wire frame** — NDJSON `0x00`.

**Request**:

```json
{
  "id": 51,
  "method": "process.stream.cancel",
  "params": {"stream_token": "st_a17c..."}
}
```

**Response**:

```json
{"id": 51, "result": {"ok": true}}
```

Cancellation also triggers a `state.changed` broadcast.

**Errors**:

- `InvalidParams` — missing `stream_token`, or the token is
  unknown or not owned by the requesting client.
- `InternalError` — streaming subsystem unavailable.

**Code refs** — `src/daemon.cpp:1862`.

### `process.stream.commit`

**Purpose** — finalize a streaming session: flush the caption
engine, close and persist the temp WAV, and enqueue a fresh
**postprocess job** that runs the full transcribe + diarize +
summarize pipeline on the captured audio. Releases the streaming
slot and allocates a postprocess slot. The response carries the
**new postprocess job_id** — the client monitors it via `job.status`
+ `progress.job` and pulls artifacts via `process.fetch` exactly as
for a `process.submit` job.

**Phase** — C.10b.

**Wire frame** — NDJSON `0x00`.

**Request**:

```json
{
  "id": 52,
  "method": "process.stream.commit",
  "params": {"stream_token": "st_a17c..."}
}
```

**Response**:

```json
{
  "id": 52,
  "result": {
    "job_id": 61,
    "ok": true
  }
}
```

`job_id` here is the **postprocess** job_id (not the streaming
job's id from `process.stream`). The streaming `Config` snapshot
taken at `process.stream` time (per-client preferences frozen at
session-start) is what the postprocess job runs against — a
concurrent `config.reload` between stream-start and commit does not
race the in-flight session.

**Errors**:

- `InvalidParams` — missing `stream_token`, unknown token, or
  session not in a committable state (the daemon's validation chain
  rejects narrowest-first; see comment block at
  `src/daemon.cpp:1907`).
- `PermissionDenied` — token belongs to a different client.
- `InternalError` — streaming subsystem unavailable; WAV close
  failure; postprocess enqueue failure.

**Example flow** — live captions → notes:

```
client → process.stream { language: "en", captions_enabled: true, ... }
client ← { job_id: 60, stream_token: "st_a17c..." }

client → 0x03 frame (PCM s16le) ...
client ← caption { job_id: 60, text: "HELLO WORLD", is_partial: true, ... }
client → 0x03 frame ...
client ← caption { job_id: 60, text: "HELLO WORLD", is_partial: false, ... }
...

client → process.stream.commit { stream_token: "st_a17c..." }
client ← { job_id: 61, ok: true }                # NEW postprocess job
client ← progress.job { job_id: 61, phase: "transcribing" }
...
client ← job.complete { job_id: 61, note_path: "...", output_dir: "..." }
client → process.fetch { job_id: 61 }
```

**Code refs** — `src/daemon.cpp:1907`.

## Voiceprint enrollment (Phase C.8)

### `enroll.finalize`

**Purpose** — second step of the two-step voiceprint enrollment
flow. Loads the diarization result from `DiarizationCache` (24 h
default TTL), extracts the embedding for the chosen cluster, and
appends it to the named speaker profile on disk.

**Phase** — C.8.

**Wire frame** — NDJSON `0x00`.

**Request**:

```json
{
  "id": 70,
  "method": "enroll.finalize",
  "params": {
    "job_id": 51,
    "target_speaker": 0,
    "enroll_name": "Alice"
  }
}
```

- `job_id` (int64) — either a `mode: "enroll"` job from
  `process.submit`, OR an existing completed postprocess job whose
  diarization is still cached.
- `target_speaker` (int64) — zero-based cluster index from the
  `speakers[]` payload on the `job.complete` event (flow (a)) or
  from a prior `diarization.json` (flow (b)).
- `enroll_name` (string) — required and non-empty. The
  user-visible label the embedding will be stored under (one
  `SpeakerProfile` may carry multiple embeddings under the same
  name, accumulated across enrollments).

**Response**:

```json
{
  "id": 70,
  "result": {
    "ok": true,
    "enroll_name": "Alice",
    "embedding_count": 1
  }
}
```

`embedding_count` is the post-append count of embeddings stored
under `enroll_name`.

**Two flows**:

- **(a) Fresh enrollment audio** — client calls `process.submit`
  with `mode: "enroll"`, uploads the audio, waits for the
  `job.complete` event whose `enroll_mode: true` payload carries
  `speakers: [{idx, duration_ms}, ...]`. Client picks the target
  cluster, calls `enroll.finalize`.
- **(b) Reuse existing diarization** — client passes the `job_id`
  of a previously-completed postprocess job. The diarization is
  re-loaded from `DiarizationCache` (24 h TTL).

**Errors**:

- `InvalidParams` — missing `job_id`, `target_speaker`, or
  `enroll_name`; non-positive `job_id`; negative `target_speaker`;
  empty `enroll_name`; unknown `job_id`; `target_speaker` out of
  cluster range; diarization no longer cached (TTL expired —
  message includes the TTL).
- `PermissionDenied` — job is not owned by the requesting client.
- `JobNotReady` — job state is not `done`.
- `InternalError` — DiarizationCache subsystem unavailable; cluster
  has no cached embedding (subprocess skipped extraction); speakers
  DB I/O failure.

**Code refs** — `src/daemon.cpp:2643`. DiarizationCache lives in
`src/diarization_cache.cpp`. SpeakerProfile load/save is in
`src/api_models.cpp`.

## Events (daemon → client)

All events ride on the NDJSON `0x00` frame as `{"event": "...",
"data": {...}}` envelopes. Phase C.3 routes events whose owning
job_id is known to the originating client only (via
`send_to_client`); an empty / unbindable `client_id` falls back to
`broadcast` so the event still becomes visible.

### `progress.job`

**Purpose** — phase transition or progress tick for a job. Emitted
from the `JobQueue::set_job_event_sink` hook and from the upload
manager's progress sink.

**Routing** — Phase C.3. To the job's originating `client_id` via
`send_to_client`. Falls back to `broadcast` on empty `client_id`.

**Payload (lifecycle transitions)**:

```json
{
  "event": "progress.job",
  "data": {
    "job_id": 47,
    "kind": "postprocess",
    "phase": "downloading_model",
    "model": "whisper/large-v3",
    "error": ""
  }
}
```

**Payload (upload progress)**:

```json
{
  "event": "progress.job",
  "data": {
    "job_id": 47,
    "kind": "postprocess",
    "phase": "uploading",
    "bytes_received": 8388608,
    "bytes_total": 16777216
  }
}
```

**Phase values** — `downloading_model`, `resumed`, `uploading`,
`done`, `failed`, `cancelled`. (Subprocess phases —
`transcribing`, `diarizing`, `identifying`, `summarizing` — ride
the separate `phase` event, not `progress.job`.)

**Code refs** — `src/daemon.cpp:1255` (lifecycle sink),
`src/daemon.cpp:1379` (upload progress sink).

### `state.changed`

**Purpose** — global daemon state snapshot. Broadcast to every
authed client whenever a job transitions, an upload session
opens/closes, or a streaming session opens/closes.

**Routing** — broadcast (not per-client).

**Payload**:

```json
{
  "event": "state.changed",
  "data": {
    "state": "idle",
    "postprocessing": false,
    "downloading": false,
    "streaming": false
  }
}
```

- `state` — composite state string (`"idle"`, `"postprocessing"`,
  `"downloading"`, `"streaming"`, and composite combinations).
  Phase C.9 removed the `"recording"` / `"reprocessing"` /
  `"recording+postprocessing"` / `"reprocessing+postprocessing"`
  names.
- `postprocessing` (bool) — a postprocess job is running.
- `downloading` (bool) — a model_download job is running.
- `streaming` (bool) — a streaming session is live.

V2 wire bump (`IPC_PROTOCOL_VERSION = 3`) removed the legacy
`recording` boolean — the daemon no longer captures audio.

**Code refs** — `src/daemon.cpp:162` (`broadcast_state`),
`src/daemon.cpp:173` (`broadcast_state_inline`).

### `caption` / `caption.degraded`

**Purpose** — `caption` carries an in-progress or finalized
transcription tick from a streaming job. `caption.degraded`
signals that the streaming engine has dropped into a degraded
mode (buffer overrun, latency over budget, model fallback).

**Routing** — Phase C.10b. To the streaming session's owning
`client_id` only (C.10a originally broadcast; C.10b tightened to
routed).

**`caption` payload**:

```json
{
  "event": "caption",
  "data": {
    "job_id": 60,
    "text": "HELLO WORLD",
    "is_partial": true,
    "timestamp_ms": 1247
  }
}
```

- `text` — recognizer's raw hypothesis (uppercase for the
  `en-2023-06-26` streaming zipformer; rendering normalization is
  out of scope for this layer).
- `is_partial` — `true` for in-progress hypotheses; `false` for a
  finalized segment.
- `timestamp_ms` — monotonic wall-clock since the caption engine
  started. Treat as an ordering hint, not an absolute meeting
  time.

**`caption.degraded` payload**:

```json
{
  "event": "caption.degraded",
  "data": {
    "job_id": 60,
    "reason": "buffer_overrun",
    "timestamp_ms": 1300
  }
}
```

**Code refs** — `src/ipc_protocol.cpp:527`
(`make_caption_event`), `src/ipc_protocol.cpp:540`
(`make_caption_degraded_event`), `src/daemon.cpp:1304` (sink
wiring), `src/streaming_session.cpp` for the producer side.

### `job.complete`

**Purpose** — terminal status with the full result payload.
Distinct from `progress.job { phase: "done" }`: `job.complete`
carries the artifact pointers (transcribe mode) or
`speakers[]` (enroll mode), where `progress.job` is the bare
lifecycle marker.

**Routing** — Phase C.3. To the job's originating `client_id`
via `send_to_client`. Falls back to `broadcast` on empty
`client_id`.

**Payload (transcribe mode)**:

```json
{
  "event": "job.complete",
  "data": {
    "job_id": 47,
    "batch_job": false,
    "note_path": "/home/u/notes/2026-05-15-standup.md",
    "output_dir": "/home/u/notes/2026-05-15-standup"
  }
}
```

**Payload (enroll mode)**:

```json
{
  "event": "job.complete",
  "data": {
    "job_id": 51,
    "batch_job": false,
    "enroll_mode": true,
    "speakers": [
      {"idx": 0, "duration_ms": 4500},
      {"idx": 1, "duration_ms": 2100}
    ]
  }
}
```

`enroll_mode: true` is the discriminator a thin client switches on
to decide whether to render artifact links (transcribe) or a
speaker-picker UI (enroll).

**Code refs** — `src/daemon.cpp:817`.

### `error`

**Purpose** — server-side error frame for failures that are not
naturally tied to a specific in-flight request. Used by background
worker failures and protocol-violation reports.

**Routing** — to the originator's `client_id` when known; broadcast
otherwise.

**Payload**:

```json
{
  "event": "error",
  "data": {
    "code": -32603,
    "message": "Processing crashed (signal 11: Segmentation fault)",
    "job_id": 47
  }
}
```

Error codes follow the `IpcErrorCode` table above.

## See also

- [`IPC-WIRE-PROTOCOL.md`](IPC-WIRE-PROTOCOL.md) — frame-layer spec
  (discriminators, length prefix, state machine, partial reads).
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — architectural overview of
  the daemon, the upload/streaming managers, and the JobQueue.
- [`COMPONENT-DIAGRAMS.md`](COMPONENT-DIAGRAMS.md) — visual
  reference for the daemon's subsystem boundaries.
- `src/ipc_protocol.h` — wire-level constants
  (`IPC_PROTOCOL_VERSION`, `FrameType`,
  `kDefaultMaxBinaryFrameBytes`, `IpcErrorCode`).
- `src/ipc_server.h` — server public API
  (`SessionCredentials`, `SessionPreferences`, `MethodHandler`,
  `BinaryFrameHandler`).
- `src/ipc_client.h` — client public API the in-tree thin client
  uses to drive these verbs.
- `src/job_queue.h` / `src/job_queue.cpp` — `Job`, `JobKind`,
  `JobState`, and the `job_state_name` / `job_kind_name` helpers
  that produce the lowercase wire names.
