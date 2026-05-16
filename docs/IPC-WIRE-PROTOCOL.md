# IPC Wire Protocol

This is the canonical reference for the bytes on the wire between the
`recmeet` daemon and its clients (CLI, tray, web). It was introduced by
**Phase C.1** of the thin-client-recording-server work and is the
transport substrate every later Phase C sub-phase (binary upload,
artifact download, streaming audio) builds on.

The transport is a bidirectional byte stream — a Unix domain socket
(local) or a TCP socket (remote). Framing is identical on both.

## V2 Verb Reference

See [`IPC-VERBS.md`](IPC-VERBS.md) for per-verb request/response shapes,
error codes, and example flows. This document covers the frame-layer
spec only.

| Verb                       | Phase   | Frame(s)               | Direction         |
|----------------------------|---------|------------------------|-------------------|
| `auth.ok`                  | A.1/4/5 | `0x00`                 | server → client   |
| `session.init`             | A.6     | `0x00`                 | client → server   |
| `session.update_credentials` | A.6   | `0x00`                 | client → server   |
| `session.update_prefs`     | A.6     | `0x00`                 | client → server   |
| `process.submit`           | C.2/8   | `0x00` + `0x01` frames | client → server   |
| `process.submit.cancel`    | C.2     | `0x00`                 | client → server   |
| `process.cancel`           | C.5     | `0x00`                 | client → server   |
| `process.fetch`            | C.4     | `0x00` + `0x02` frames | both              |
| `process.stream`           | C.10a   | `0x00` + `0x03` frames | client → server   |
| `process.stream.cancel`    | C.10a   | `0x00`                 | client → server   |
| `process.stream.commit`    | C.10b   | `0x00`                 | client → server   |
| `job.status`               | C.6     | `0x00`                 | client → server   |
| `job.list`                 | C.6     | `0x00`                 | client → server   |
| `enroll.finalize`          | C.8     | `0x00`                 | client → server   |
| `progress.job` (event)     | C.3     | `0x00`                 | server → client   |
| `state.changed` (event)    | —       | `0x00`                 | server → client   |
| `caption` (event)          | C.10a/b | `0x00`                 | server → client   |
| `caption.degraded` (event) | C.10a/b | `0x00`                 | server → client   |
| `job.complete` (event)     | C.3     | `0x00`                 | server → client   |
| `error` (event)            | —       | `0x00`                 | server → client   |

## Frame format

Every frame on the wire begins with a **1-byte frame-type discriminator**.
The body that follows depends on the discriminator:

```
+------+--------------------------------------------------+
| 0x00 | <JSON object bytes> '\n'                         |   NDJSON line
+------+--------------------------------------------------+
| 0x01 | <4-byte big-endian length N> <N payload bytes>   |   binary upload
+------+--------------------------------------------------+
| 0x02 | <4-byte big-endian length N> <N payload bytes>   |   binary artifact
+------+--------------------------------------------------+
| 0x03 | <4-byte big-endian length N> <N PCM payload bytes>|  streaming audio
+------+--------------------------------------------------+
```

### Discriminators

| Byte   | Name             | Body                                             | Direction (today)        |
|--------|------------------|--------------------------------------------------|--------------------------|
| `0x00` | NDJSON line      | JSON object + `\n`                               | both                     |
| `0x01` | binary upload    | 4-byte BE length + payload                       | client → daemon (C.2)    |
| `0x02` | binary artifact  | 4-byte BE length + payload                       | daemon → client (C.4)    |
| `0x03` | streaming audio  | 4-byte BE length + PCM payload                   | client → daemon (C.10a)  |
| `0x04`+| **reserved**     | —                                                | —                        |

The discriminator set is **open**. `0x04` and above are reserved for
future extensions. A receiver that sees an unknown discriminator MUST
reject it cleanly — tear the connection down with a diagnostic — and MUST
NOT crash or attempt to guess the body shape.

As of Phase C.1, only `0x00` has a consumer. The three binary
discriminators are fully **decode-able** by the framing layer (the
state-machine reader assembles them and hands back the payload), but
there is no handler wired to them yet — a received binary frame is
parsed off the wire and discarded with a debug trace. C.2 / C.4 / C.10a
attach the real upload / download / streaming handlers.

## `0x00` — NDJSON line

This is the historical `recmeet` message format, now explicitly prefixed.
The body is exactly one JSON object followed by a single `\n`. The JSON
object is one of:

- **Request** — `{"id":N,"method":"...","params":{...}}`
- **Response** — `{"id":N,"result":{...}}`
- **Error** — `{"id":N,"error":{"code":N,"message":"..."}}`
- **Event** — `{"event":"...","data":{...}}` (optionally `"client_id":"..."`)
- **Auth** — `{"type":"auth.token",...}` / `{"type":"auth.ok",...}` /
  `{"type":"auth.error",...}`

The JSON object itself never contains a raw newline (the `serialize(...)`
family escapes embedded newlines as `\n`), so the first unescaped `\n`
after the discriminator unambiguously terminates the frame.

## `0x01` / `0x02` / `0x03` — binary frames

A binary frame is `<1-byte type><4-byte big-endian length N><N bytes>`.

- The length is an unsigned 32-bit integer, **big-endian** (network byte
  order). It counts only the payload bytes — not the discriminator and
  not the 4-byte length header itself.
- A length of `0` is legal: a zero-length payload is a valid frame
  (discriminator + 4 zero bytes, no payload).
- The payload is opaque to the framing layer — raw bytes, no
  interpretation. `0x03` payloads are PCM audio; `0x01` / `0x02` payloads
  are whatever the C.2 / C.4 handlers define.

### Length bound

A binary frame's declared length is bounded by a transport-level cap
(`kDefaultMaxBinaryFrameBytes`, **16 MiB** by default). The receiver
checks the 4-byte length header *before* buffering any payload bytes, so
a hostile peer cannot make the reader allocate an arbitrary amount of
memory just by declaring a huge length. A frame whose declared length
exceeds the cap is a terminal protocol error — the connection is closed.

This cap is **independent** of the NDJSON line cap (`max_message_bytes`,
8 MiB default): NDJSON frames are newline-delimited and bounded by the
line cap; binary frames are length-prefixed and bounded by the binary
cap. Phase C.1 ships the binary cap as a named constant plus a
`set_max_binary_frame_bytes()` setter seam on `IpcServer` and
`FrameReader`. **C.2 / C.10a** will wire it to a configurable
`[ipc] max_upload_bytes` key in `daemon.yaml`.

## The state-machine reader

Each connection owns a `FrameReader` (`src/ipc_protocol.{h,cpp}`). Raw
socket bytes are appended via `feed()`; complete frames are pulled out
via `next()`, which returns one of:

- `Ok` — a complete frame is in the out-parameter.
- `NeedMore` — no complete frame yet; feed more bytes and retry.
- `BadDiscriminator` — byte 0 of a frame was an unknown type (`0x04`+).
  Terminal: the caller closes the connection.
- `FrameTooLarge` — a binary frame's declared length exceeded the cap.
  Terminal.

Internally the reader is a four-state machine over the byte stream:

```
        ┌──────────────┐  byte == 0x00         ┌──────────┐  '\n' seen
        │  AtBoundary  │──────────────────────▶│  Ndjson  │────────────┐
        │ (next byte = │  byte == 0x01/02/03   └──────────┘            │
        │ discriminator)│─────────┐                  ▲                 │
        └──────────────┘          │                  │  emit Ndjson    │
               ▲                  ▼                  │     Frame       │
               │           ┌────────────┐  4 bytes   │                 │
               │           │ BinaryLen  │  decoded   │                 │
               │           │ (accumulate│────────┐   │                 │
               │           │  4-byte BE)│        │   │                 │
               │           └────────────┘        ▼   │                 │
               │                          ┌──────────┐  N bytes seen   │
               │  emit Binary Frame        │  Binary  │────────────────┐│
               └───────────────────────────│(accumulate│               ││
                                           │  N bytes) │               ││
                                           └──────────┘                ││
               ┌────────────────────────────────────────────────────────┘│
               └─────────────────────────────────────────────────────────┘
```

- **`AtBoundary`** — the reader is between frames. The next byte is read
  as a discriminator. `0x00` → `Ndjson`; `0x01`/`0x02`/`0x03` →
  `BinaryLen`; anything else → `BadDiscriminator` (terminal).
- **`Ndjson`** — accumulating a JSON line. When the first `\n` is found,
  the line (without the `\n`) is emitted as a `Frame` and the reader
  returns to `AtBoundary`.
- **`BinaryLen`** — accumulating the 4-byte big-endian length header.
  Once 4 bytes are buffered the length is decoded; if it exceeds the cap
  the reader returns `FrameTooLarge` (terminal), otherwise it moves to
  `Binary`.
- **`Binary`** — accumulating exactly `N` payload bytes. When `N` bytes
  are buffered the payload is emitted as a `Frame` and the reader returns
  to `AtBoundary`.

### Partial reads

Every state holds its accumulator in the reader's internal buffer, so a
frame split across any number of `read()` / `poll()` calls is handled
transparently: each `feed()` appends, each `next()` makes as much
progress as the buffered bytes allow and returns `NeedMore` when it runs
out. No frame data is ever lost across the boundary of a partial read.

### Interleaving NDJSON with in-flight binary frames

Because every frame is self-delimiting and the reader returns to
`AtBoundary` after each complete frame, an NDJSON command can be fully
received and dispatched **in between** two binary frames of an in-flight
stream. Concretely: a client can send a `0x00` NDJSON `process.cancel`
while a `0x03` audio stream is flowing — the daemon's reader assembles
the cancel frame, dispatches it, then resumes assembling the next audio
frame. There is no "binary mode lock" that blocks NDJSON traffic.

(Note: a *single* binary frame is still atomic — the reader will not
interleave bytes of two frames. Interleaving happens at the *frame*
granularity, between complete frames, which is the granularity a
streaming protocol cares about.)

## Handshake / version interaction

Phase C.1 is a **breaking** wire change: a pre-C.1 (`protocol_version` 1)
peer emits raw `{...}\n` with no discriminator byte, which a C.1
(`protocol_version` 2) peer would misparse — byte 0 (`{`, `0x7B`) is in
the reserved discriminator range and would trip `BadDiscriminator`.

The Phase A.5 `protocol_version` handshake is what makes the break safe.
The daemon stamps `IPC_PROTOCOL_VERSION` (now **2**) into the `auth.ok`
frame. The client parses it in `verify_auth_ok_and_capture()` and closes
the connection on a mismatch (or a missing field — treated as v0) before
any non-auth frame is exchanged. So a stale peer fails the handshake
cleanly instead of misframing live traffic.

The auth handshake itself rides the framed transport: `auth.token`,
`auth.ok`, and `auth.error` are all `0x00` NDJSON frames. The client's
connect-time blocking reader (`read_one_line_blocking`) strips the
`0x00` prefix directly, since it runs before the steady-state
`FrameReader` path takes over.

## Versioning rule

Bump `IPC_PROTOCOL_VERSION` (`src/ipc_protocol.h`) whenever the wire
contract changes in a way the daemon and client must agree on — a new
auth field, a new frame shape, a new discriminator with required
semantics. All in-tree clients ship with the daemon as a unit, so
backwards compatibility is bounded; there are no third-party clients on
the wire.
