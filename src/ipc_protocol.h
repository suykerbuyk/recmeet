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
// Socket path
// ---------------------------------------------------------------------------

std::string default_socket_path();

} // namespace recmeet
