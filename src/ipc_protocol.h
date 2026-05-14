// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>

namespace recmeet {

// ---------------------------------------------------------------------------
// IPC message types for daemon ↔ client communication
// Wire format: newline-delimited JSON (NDJSON)
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
inline constexpr int IPC_PROTOCOL_VERSION = 1;

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
