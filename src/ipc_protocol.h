// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace recmeet {

// ---------------------------------------------------------------------------
// IPC message types for daemon ↔ client communication
//
// Phase C.1 wire format: every frame on the wire begins with a 1-byte
// frame-type discriminator. See `docs/IPC-WIRE-PROTOCOL.md` for the full
// spec. The four discriminators are:
//
//   0x00  NDJSON line     — a JSON object followed by '\n' (the historical
//                           format, now explicitly prefixed). Carries
//                           requests / responses / errors / events / auth.
//   0x01  binary upload   — 4-byte big-endian length + raw payload bytes.
//   0x02  binary artifact — 4-byte big-endian length + raw payload bytes.
//   0x03  streaming audio — 4-byte big-endian length + PCM payload bytes.
//
// `0x04+` is a reserved range for future extensions. The state-machine
// reader rejects an unknown discriminator cleanly (it does NOT crash):
// the connection is torn down with a protocol-violation diagnostic.
//
// The discriminator set is open and the binary discriminators are
// decode-able but have no consumer yet — Phase C.2 / C.7 / C.10a wire the
// actual upload / download / streaming handlers on top of this substrate.
// ---------------------------------------------------------------------------

// Phase A.5: wire protocol version. Stamped by the daemon in every
// `auth.ok` frame ("`{"type":"auth.ok","client_id":"...","protocol_version":N}`").
// The client reads it on connect; a mismatch (or missing field — treated
// as v0) closes the connection and surfaces `IpcClient::protocol_mismatch()`.
// All in-tree clients (tray, cli, future thin-client tray) ship with the
// daemon as a unit so backwards-compat is bounded — no third-party clients
// on the wire today. Bump this when the wire contract changes in a way the
// daemon and client must agree on (new auth fields, new frame shape).
//
// `inline constexpr` keeps the symbol header-only with external linkage so
// tests in another TU can reference the same value without a separate .cpp
// definition.
//
// Phase C.1: bumped 1 → 2. C.1 is a BREAKING wire change — it introduces
// the 1-byte frame-type discriminator at every frame boundary. A v1 peer
// emits raw `{...}\n` with no prefix; a v2 peer would misparse byte 0 as
// a discriminator. The A.5 `protocol_version` handshake is what makes the
// break safe: a stale peer fails `verify_auth_ok_and_capture()` and the
// connection is closed before any non-auth frame is exchanged.
inline constexpr int IPC_PROTOCOL_VERSION = 2;

// JSON value: string, int64, double, bool, or null (monostate)
using JsonVal = std::variant<std::monostate, bool, int64_t, double, std::string>;
using JsonMap = std::map<std::string, JsonVal>;

// Request: {"id": N, "method": "...", "params": {...}}
//
// Phase A.6: `client_id` is a server-side stamped field — populated by the
// IPC server immediately before invoking the registered handler, from the
// `ClientState::client_id` of the connection that delivered the line. It
// is NOT serialized onto the wire by `serialize(IpcRequest)`, and clients
// never set it. Handlers that need to know the originating client (e.g.
// `session.init`, `session.update_credentials`, `session.update_prefs`,
// and any later handler whose state lives in the per-client session slot)
// read it from this field instead of reaching into the server.
struct IpcRequest {
    int64_t id = 0;
    std::string method;
    JsonMap params;
    std::string client_id;  // server-stamped, never on the wire
};

// Success response: {"id": N, "result": {...}}
struct IpcResponse {
    int64_t id = 0;
    JsonMap result;
};

// Error response: {"id": N, "error": {"code": N, "message": "..."}}
struct IpcError {
    int64_t id = 0;
    int code = 0;
    std::string message;
};

// Server→client event push: {"event": "...", "data": {...}}
//
// Phase A.4: `client_id` is an optional routing tag carried as a top-level
// JSON field, NOT nested under `data`. When non-empty the serializer emits
// `"client_id":"..."`; when empty the field is omitted entirely (no
// `"client_id":null`) so high-frequency event payloads (caption ticks at
// ~10 Hz, progress) stay compact on the wire. The field is purely
// informational at this stage — actual per-`client_id` routing on the
// server runs through `IpcServer::send_to_client()`, not through the
// `IpcEvent` payload. Phase C.7's JobQueue is what populates this from a
// `job_id → client_id` binding; A.4 only lands the plumbing.
struct IpcEvent {
    std::string event;
    JsonMap data;
    std::string client_id;  // empty → omitted from wire
};

// Standard error codes
enum class IpcErrorCode : int {
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams  = -32602,
    InternalError  = -32603,
    AlreadyRecording = 1,
    NotRecording     = 2,
    Busy             = 3,
};

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string serialize(const IpcRequest& req);
std::string serialize(const IpcResponse& resp);
std::string serialize(const IpcError& err);
std::string serialize(const IpcEvent& ev);

// ---------------------------------------------------------------------------
// Parsing — returns true on success, false on malformed input.
// Caller determines message type by checking which fields are present:
//   - has "method" → IpcRequest
//   - has "result" → IpcResponse
//   - has "error"  → IpcError
//   - has "event"  → IpcEvent
// ---------------------------------------------------------------------------

enum class IpcMessageType { Request, Response, Error, Event, Unknown };

struct IpcMessage {
    IpcMessageType type = IpcMessageType::Unknown;
    IpcRequest request;
    IpcResponse response;
    IpcError error;
    IpcEvent event;
};

bool parse_ipc_message(const std::string& line, IpcMessage& out);

// ---------------------------------------------------------------------------
// Phase C.1: framed wire protocol
//
// Every frame on the wire is `<1-byte type><body>`. The body shape depends
// on the type:
//   - FrameType::Ndjson  : body is a JSON object terminated by '\n'.
//   - FrameType::Binary* : body is `<4-byte big-endian length N><N bytes>`.
//
// The encode helpers below produce a complete wire frame (prefix + body).
// The `FrameReader` state machine on the receiving side consumes an
// arbitrary byte stream and yields fully-assembled `Frame`s, transparently
// handling partial reads (a frame split across multiple read() calls) and
// NDJSON frames interleaved with in-flight binary frames.
// ---------------------------------------------------------------------------

enum class FrameType : uint8_t {
    Ndjson         = 0x00,  // JSON object + '\n'
    BinaryUpload   = 0x01,  // 4-byte BE length + payload
    BinaryArtifact = 0x02,  // 4-byte BE length + payload
    StreamAudio    = 0x03,  // 4-byte BE length + PCM payload
    // 0x04+ reserved — FrameReader rejects unknown discriminators cleanly.
};

// A fully-assembled inbound frame produced by FrameReader::next().
// For an Ndjson frame, `payload` holds the JSON line WITHOUT the trailing
// '\n' (so it feeds straight into `parse_ipc_message`). For a binary frame
// it holds exactly the declared-length payload bytes.
struct Frame {
    FrameType   type = FrameType::Ndjson;
    std::string payload;
};

// Default upper bound on a single binary frame's declared length, in bytes
// (16 MiB). This is the C.1 transport-level cap — independent of the
// NDJSON `max_message_bytes` line cap, since binary frames have a length
// prefix rather than a newline terminator. A frame whose 4-byte BE length
// header exceeds this is rejected before any payload bytes are buffered,
// so a hostile peer cannot make the reader allocate an arbitrary amount of
// memory by declaring a huge length. C.2 / C.10a will wire this to a
// configurable `[ipc] max_upload_bytes` daemon.yaml key; C.1 ships the
// named constant and a setter seam only.
inline constexpr size_t kDefaultMaxBinaryFrameBytes = 16 * 1024 * 1024;

// Encode a complete NDJSON wire frame: `0x00` + `json` + '\n'.
// `json` must be a single JSON object with no embedded newline (the
// `serialize(...)` family already guarantees this).
std::string frame_ndjson(const std::string& json);

// Encode a complete binary wire frame: `<type>` + `<4-byte BE length>` +
// `payload`. `type` must be one of the binary discriminators; passing
// FrameType::Ndjson is a caller error (use frame_ndjson instead).
std::string frame_binary(FrameType type, const std::string& payload);

// Outcome of FrameReader::next().
enum class FrameStatus {
    Ok,             // a complete frame was produced in `out`
    NeedMore,       // no complete frame yet — feed more bytes and retry
    BadDiscriminator,  // byte 0 of a frame was an unknown type (0x04+)
    FrameTooLarge,  // a binary frame's declared length exceeds the cap
};

// Per-connection inbound framing state machine.
//
// Usage: append raw socket bytes via `feed()`, then call `next()` in a
// loop until it returns `NeedMore` — each `Ok` return hands back one
// fully-assembled `Frame`. A non-Ok / non-NeedMore status is terminal for
// the connection: the caller closes the fd.
//
// The state machine has two modes:
//   - Ndjson : accumulating a JSON line until '\n'.
//   - Binary : accumulating `expected_len` payload bytes after having
//              decoded the 4-byte big-endian length header.
// Between frames it is "at a frame boundary" and the next byte is read as
// a discriminator. Because each frame is self-delimiting, an NDJSON
// command can be fully received and dispatched in between two binary
// frames of an in-flight stream — interleaving falls out naturally.
class FrameReader {
public:
    explicit FrameReader(size_t max_binary_frame_bytes = kDefaultMaxBinaryFrameBytes)
        : max_binary_frame_bytes_(max_binary_frame_bytes) {}

    // Append raw bytes received from the socket.
    void feed(const char* data, size_t n) { buf_.append(data, n); }
    void feed(const std::string& data) { buf_.append(data); }

    // Try to extract the next complete frame. See FrameStatus.
    FrameStatus next(Frame& out);

    // Bytes currently buffered but not yet assembled into a frame. Used by
    // the server's slowloris guard: an NDJSON line that grows past the
    // line cap without a '\n' is a protocol abuse signal.
    size_t buffered() const { return buf_.size(); }

    // True when the reader is mid-frame (a discriminator has been consumed
    // but the frame body is not yet complete). At a frame boundary this is
    // false. Exposed mainly for the line-cap guard and for tests.
    bool in_frame() const { return state_ != State::AtBoundary; }

    // The line cap applies only to NDJSON bodies; expose the current mode
    // so the caller applies the right cap to the right byte stream.
    bool in_ndjson_frame() const { return state_ == State::Ndjson; }

    void set_max_binary_frame_bytes(size_t n) { max_binary_frame_bytes_ = n; }
    size_t max_binary_frame_bytes() const { return max_binary_frame_bytes_; }

private:
    enum class State {
        AtBoundary,  // next byte is a frame-type discriminator
        Ndjson,      // accumulating a JSON line until '\n'
        BinaryLen,   // accumulating the 4-byte big-endian length header
        Binary,      // accumulating `expected_len_` payload bytes
    };

    State       state_ = State::AtBoundary;
    std::string buf_;                 // unconsumed raw bytes
    FrameType   cur_type_ = FrameType::Ndjson;  // type of the in-flight frame
    uint32_t    expected_len_ = 0;    // declared payload length (binary modes)
    size_t      max_binary_frame_bytes_;
};

// ---------------------------------------------------------------------------
// JsonVal helpers
// ---------------------------------------------------------------------------

std::string serialize_json_val(const JsonVal& val);
std::string serialize_json_map(const JsonMap& map);

std::string json_val_as_string(const JsonVal& val, const std::string& def = "");
int64_t json_val_as_int(const JsonVal& val, int64_t def = 0);
double json_val_as_double(const JsonVal& val, double def = 0.0);
bool json_val_as_bool(const JsonVal& val, bool def = false);

// ---------------------------------------------------------------------------
// Socket path / address
// ---------------------------------------------------------------------------

std::string default_socket_path();

// Transport type for IPC connections
enum class IpcTransport { Unix, Tcp };

// Parsed IPC address — either a Unix socket path or a TCP host:port
struct IpcAddress {
    IpcTransport transport = IpcTransport::Unix;
    std::string host;          // TCP only
    uint16_t port = 0;         // TCP only
    std::string socket_path;   // Unix only
};

// Parse an address string into an IpcAddress.
// Heuristic: "host:port" (digits after last colon, 1-65535) → TCP,
//            otherwise → Unix socket path.
// Empty string → default Unix socket path.
// Returns false on invalid input (e.g. port out of range, bare IPv6).
bool parse_ipc_address(const std::string& addr, IpcAddress& out);

// Default address (Unix socket at default_socket_path()).
IpcAddress default_ipc_address();

// ---------------------------------------------------------------------------
// Caption event helpers (Phase 3)
//
// The streaming caption engine emits `CaptionResult` and degraded signals on
// its own worker thread. These helpers build IPC events with a stable wire
// shape so daemon and clients agree on the field set:
//
//   {"event":"caption","data":{"job_id":N,"text":"...","is_partial":true|false,"timestamp_ms":N}}
//   {"event":"caption.degraded","data":{"job_id":N,"reason":"buffer_overrun","timestamp_ms":N}}
//
// `text` is the recognizer's raw hypothesis (ALL-CAPS for the en-2023-06-26
// streaming zipformer); rendering normalization is Phase 5's job.
// `timestamp_ms` is wall-clock since the caption engine started; clients
// treat it as a monotonic ordering hint, not an absolute meeting time.
// ---------------------------------------------------------------------------

IpcEvent make_caption_event(int64_t job_id,
                            const std::string& text,
                            bool is_partial,
                            int64_t timestamp_ms);

IpcEvent make_caption_degraded_event(int64_t job_id,
                                     const std::string& reason,
                                     int64_t timestamp_ms);

} // namespace recmeet
