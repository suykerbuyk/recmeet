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

// JSON value: string, int64, double, bool, or null (monostate)
using JsonVal = std::variant<std::monostate, bool, int64_t, double, std::string>;
using JsonMap = std::map<std::string, JsonVal>;

// Request: {"id": N, "method": "...", "params": {...}}
struct IpcRequest {
    int64_t id = 0;
    std::string method;
    JsonMap params;
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
struct IpcEvent {
    std::string event;
    JsonMap data;
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

// Emitted by the recording worker once it has successfully wired a
// CaptionEngine into the in-flight recording (whether at record.start or
// mid-recording in response to the `captions.start_engine` verb). The
// tray flips its "engine started" state on this event, NOT on the verb's
// return value, so a single code path handles both startup-time and
// mid-recording wiring.
//
//   {"event":"caption.started","data":{"job_id":N,"timestamp_ms":N}}
IpcEvent make_caption_started_event(int64_t job_id, int64_t ts_ms);

} // namespace recmeet
