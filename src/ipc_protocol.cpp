// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "ipc_protocol.h"
#include "json_util.h"
#include "util.h"

#include <cstdlib>
#include <unistd.h>

namespace recmeet {

// ---------------------------------------------------------------------------
// JSON value serialization
// ---------------------------------------------------------------------------

std::string serialize_json_val(const JsonVal& val) {
    struct Visitor {
        std::string operator()(std::monostate) const { return "null"; }
        std::string operator()(bool b) const { return b ? "true" : "false"; }
        std::string operator()(int64_t i) const { return std::to_string(i); }
        std::string operator()(double d) const {
            // Avoid trailing zeros but keep at least one decimal
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.6g", d);
            return buf;
        }
        std::string operator()(const std::string& s) const {
            return "\"" + json_escape(s) + "\"";
        }
    };
    return std::visit(Visitor{}, val);
}

std::string serialize_json_map(const JsonMap& map) {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, v] : map) {
        if (!first) out += ",";
        first = false;
        out += "\"" + json_escape(k) + "\":" + serialize_json_val(v);
    }
    out += "}";
    return out;
}

// ---------------------------------------------------------------------------
// Message serialization
// ---------------------------------------------------------------------------

std::string serialize(const IpcRequest& req) {
    std::string out = "{\"id\":" + std::to_string(req.id);
    out += ",\"method\":\"" + json_escape(req.method) + "\"";
    out += ",\"params\":" + serialize_json_map(req.params);
    out += "}";
    return out;
}

std::string serialize(const IpcResponse& resp) {
    std::string out = "{\"id\":" + std::to_string(resp.id);
    out += ",\"result\":" + serialize_json_map(resp.result);
    out += "}";
    return out;
}

std::string serialize(const IpcError& err) {
    std::string out = "{\"id\":" + std::to_string(err.id);
    out += ",\"error\":{\"code\":" + std::to_string(err.code);
    out += ",\"message\":\"" + json_escape(err.message) + "\"}}";
    return out;
}

std::string serialize(const IpcEvent& ev) {
    std::string out = "{\"event\":\"" + json_escape(ev.event) + "\"";
    // Phase A.4: emit `client_id` as a top-level field only when populated.
    // Omitting it on empty keeps the wire compact for the broadcast events
    // (caption / progress / phase) that dominate the byte budget.
    if (!ev.client_id.empty())
        out += ",\"client_id\":\"" + json_escape(ev.client_id) + "\"";
    out += ",\"data\":" + serialize_json_map(ev.data);
    out += "}";
    return out;
}

// ---------------------------------------------------------------------------
// Minimal JSON parser — enough for our NDJSON protocol
// ---------------------------------------------------------------------------

namespace {

// Skip whitespace, return current position
size_t skip_ws(const std::string& s, size_t pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n'))
        ++pos;
    return pos;
}

// Parse a JSON string starting at pos (which must point to '"').
// Returns end position (past closing '"'), or npos on error.
size_t parse_json_string(const std::string& s, size_t pos, std::string& out) {
    if (pos >= s.size() || s[pos] != '"') return std::string::npos;
    ++pos;
    out.clear();
    while (pos < s.size()) {
        char c = s[pos];
        if (c == '"') return pos + 1;
        if (c == '\\' && pos + 1 < s.size()) {
            ++pos;
            switch (s[pos]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case '/':  out += '/';  break;
                default:   out += '\\'; out += s[pos]; break;
            }
        } else {
            out += c;
        }
        ++pos;
    }
    return std::string::npos;
}

// Parse a JSON value starting at pos. Returns end position or npos.
size_t parse_json_value(const std::string& s, size_t pos, JsonVal& out) {
    pos = skip_ws(s, pos);
    if (pos >= s.size()) return std::string::npos;

    char c = s[pos];

    // String
    if (c == '"') {
        std::string str;
        size_t end = parse_json_string(s, pos, str);
        if (end == std::string::npos) return end;
        out = std::move(str);
        return end;
    }

    // true
    if (s.compare(pos, 4, "true") == 0) {
        out = true;
        return pos + 4;
    }

    // false
    if (s.compare(pos, 5, "false") == 0) {
        out = false;
        return pos + 5;
    }

    // null
    if (s.compare(pos, 4, "null") == 0) {
        out = std::monostate{};
        return pos + 4;
    }

    // Number (int or double)
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t start = pos;
        bool is_float = false;
        if (s[pos] == '-') ++pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
        if (pos < s.size() && s[pos] == '.') { is_float = true; ++pos; }
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            is_float = true;
            ++pos;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
        }
        std::string num_str = s.substr(start, pos - start);
        if (is_float)
            out = std::stod(num_str);
        else
            out = static_cast<int64_t>(std::stoll(num_str));
        return pos;
    }

    return std::string::npos;
}

// Parse a JSON object into a flat map. Does not recurse into nested objects;
// nested objects/arrays are stored as their raw JSON string.
size_t parse_json_object(const std::string& s, size_t pos, JsonMap& out) {
    pos = skip_ws(s, pos);
    if (pos >= s.size() || s[pos] != '{') return std::string::npos;
    ++pos;
    pos = skip_ws(s, pos);
    if (pos < s.size() && s[pos] == '}') return pos + 1;

    while (pos < s.size()) {
        pos = skip_ws(s, pos);
        std::string key;
        pos = parse_json_string(s, pos, key);
        if (pos == std::string::npos) return pos;

        pos = skip_ws(s, pos);
        if (pos >= s.size() || s[pos] != ':') return std::string::npos;
        ++pos;
        pos = skip_ws(s, pos);

        // Check for nested object or array — store as raw string
        if (pos < s.size() && (s[pos] == '{' || s[pos] == '[')) {
            size_t start = pos;
            int depth = 1;
            char open = s[pos], close = (open == '{') ? '}' : ']';
            ++pos;
            bool in_str = false;
            while (pos < s.size() && depth > 0) {
                if (in_str) {
                    if (s[pos] == '\\') { ++pos; }
                    else if (s[pos] == '"') in_str = false;
                } else {
                    if (s[pos] == '"') in_str = true;
                    else if (s[pos] == open) ++depth;
                    else if (s[pos] == close) --depth;
                }
                ++pos;
            }
            out[key] = s.substr(start, pos - start);
        } else {
            JsonVal val;
            pos = parse_json_value(s, pos, val);
            if (pos == std::string::npos) return pos;
            out[key] = std::move(val);
        }

        pos = skip_ws(s, pos);
        if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
        if (pos < s.size() && s[pos] == '}') return pos + 1;
        return std::string::npos;
    }
    return std::string::npos;
}

// Parse a nested error object: {"code": N, "message": "..."}
bool parse_error_object(const std::string& raw, int& code, std::string& message) {
    JsonMap map;
    if (parse_json_object(raw, 0, map) == std::string::npos) return false;
    code = static_cast<int>(json_val_as_int(map["code"]));
    message = json_val_as_string(map["message"]);
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public parse
// ---------------------------------------------------------------------------

bool parse_ipc_message(const std::string& line, IpcMessage& out) {
    JsonMap top;
    if (parse_json_object(line, 0, top) == std::string::npos)
        return false;

    // Determine type by which keys are present
    bool has_method = top.count("method");
    bool has_result = top.count("result");
    bool has_error  = top.count("error");
    bool has_event  = top.count("event");

    if (has_method) {
        out.type = IpcMessageType::Request;
        out.request.id = json_val_as_int(top["id"]);
        out.request.method = json_val_as_string(top["method"]);
        // Parse params sub-object if present
        std::string params_raw = json_val_as_string(top["params"]);
        if (!params_raw.empty() && params_raw[0] == '{')
            parse_json_object(params_raw, 0, out.request.params);
        return true;
    }

    if (has_result) {
        out.type = IpcMessageType::Response;
        out.response.id = json_val_as_int(top["id"]);
        std::string result_raw = json_val_as_string(top["result"]);
        if (!result_raw.empty() && result_raw[0] == '{')
            parse_json_object(result_raw, 0, out.response.result);
        return true;
    }

    if (has_error) {
        out.type = IpcMessageType::Error;
        out.error.id = json_val_as_int(top["id"]);
        std::string err_raw = json_val_as_string(top["error"]);
        if (!err_raw.empty())
            parse_error_object(err_raw, out.error.code, out.error.message);
        return true;
    }

    if (has_event) {
        out.type = IpcMessageType::Event;
        out.event.event = json_val_as_string(top["event"]);
        // Phase A.4: top-level `client_id` is optional; when absent the
        // event is unrouted (broadcast / global). Absence vs empty string
        // are equivalent on the wire — both produce an empty client_id.
        if (top.count("client_id"))
            out.event.client_id = json_val_as_string(top["client_id"]);
        std::string data_raw = json_val_as_string(top["data"]);
        if (!data_raw.empty() && data_raw[0] == '{')
            parse_json_object(data_raw, 0, out.event.data);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Phase C.1: framed wire protocol — encode helpers + state-machine reader
// ---------------------------------------------------------------------------

namespace {

// Big-endian encode a 32-bit length into the four bytes pointed at by `p`.
// Hand-rolled rather than htonl() so the encoder has no <arpa/inet.h>
// dependency and the byte order is unambiguous at the call site.
void put_be32(char* p, uint32_t v) {
    p[0] = static_cast<char>((v >> 24) & 0xFF);
    p[1] = static_cast<char>((v >> 16) & 0xFF);
    p[2] = static_cast<char>((v >> 8) & 0xFF);
    p[3] = static_cast<char>(v & 0xFF);
}

// Big-endian decode a 32-bit length from four bytes.
uint32_t get_be32(const char* p) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24)
         | (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16)
         | (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8)
         | (static_cast<uint32_t>(static_cast<unsigned char>(p[3])));
}

// True for the three binary discriminators (length-prefixed frames).
bool is_binary_frame_type(FrameType t) {
    return t == FrameType::BinaryUpload
        || t == FrameType::BinaryArtifact
        || t == FrameType::StreamAudio;
}

// True for any discriminator the C.1 state machine knows how to decode.
// 0x04+ is the reserved range and is intentionally NOT recognized here.
bool is_known_frame_type(uint8_t b) {
    return b == static_cast<uint8_t>(FrameType::Ndjson)
        || b == static_cast<uint8_t>(FrameType::BinaryUpload)
        || b == static_cast<uint8_t>(FrameType::BinaryArtifact)
        || b == static_cast<uint8_t>(FrameType::StreamAudio);
}

} // anonymous namespace

std::string frame_ndjson(const std::string& json) {
    std::string out;
    out.reserve(json.size() + 2);
    out += static_cast<char>(FrameType::Ndjson);
    out += json;
    out += '\n';
    return out;
}

std::string frame_binary(FrameType type, const std::string& payload) {
    // Caller error to frame NDJSON through here — but degrade gracefully
    // rather than crash: treat a misuse as an upload frame so the wire
    // still carries a well-formed length-prefixed body.
    if (!is_binary_frame_type(type))
        type = FrameType::BinaryUpload;
    std::string out;
    out.reserve(payload.size() + 5);
    out += static_cast<char>(type);
    char lenbuf[4];
    put_be32(lenbuf, static_cast<uint32_t>(payload.size()));
    out.append(lenbuf, 4);
    out += payload;
    return out;
}

FrameStatus FrameReader::next(Frame& out) {
    for (;;) {
        switch (state_) {
            case State::AtBoundary: {
                if (buf_.empty()) return FrameStatus::NeedMore;
                const uint8_t disc = static_cast<uint8_t>(buf_[0]);
                if (!is_known_frame_type(disc)) {
                    // Unknown discriminator (0x04+ reserved range, or
                    // garbage). Terminal — caller closes the connection.
                    // We do NOT consume the byte; leaving it lets a
                    // diagnostic dump the offending prefix if desired.
                    return FrameStatus::BadDiscriminator;
                }
                cur_type_ = static_cast<FrameType>(disc);
                buf_.erase(0, 1);  // consume the discriminator
                if (cur_type_ == FrameType::Ndjson) {
                    state_ = State::Ndjson;
                } else {
                    state_ = State::BinaryLen;
                }
                continue;  // re-enter the loop in the new state
            }

            case State::Ndjson: {
                size_t nl = buf_.find('\n');
                if (nl == std::string::npos) return FrameStatus::NeedMore;
                out.type    = FrameType::Ndjson;
                out.payload = buf_.substr(0, nl);  // line without the '\n'
                buf_.erase(0, nl + 1);
                state_ = State::AtBoundary;
                return FrameStatus::Ok;
            }

            case State::BinaryLen: {
                if (buf_.size() < 4) return FrameStatus::NeedMore;
                expected_len_ = get_be32(buf_.data());
                buf_.erase(0, 4);
                if (expected_len_ > max_binary_frame_bytes_) {
                    // Reject BEFORE buffering any payload bytes — a hostile
                    // peer cannot make the reader allocate arbitrarily by
                    // declaring a huge length. Terminal for the connection.
                    return FrameStatus::FrameTooLarge;
                }
                state_ = State::Binary;
                continue;
            }

            case State::Binary: {
                if (buf_.size() < expected_len_) return FrameStatus::NeedMore;
                out.type    = cur_type_;
                out.payload = buf_.substr(0, expected_len_);
                buf_.erase(0, expected_len_);
                state_ = State::AtBoundary;
                return FrameStatus::Ok;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// JsonVal helpers
// ---------------------------------------------------------------------------

std::string json_val_as_string(const JsonVal& val, const std::string& def) {
    if (auto* s = std::get_if<std::string>(&val)) return *s;
    return def;
}

int64_t json_val_as_int(const JsonVal& val, int64_t def) {
    if (auto* i = std::get_if<int64_t>(&val)) return *i;
    if (auto* d = std::get_if<double>(&val)) return static_cast<int64_t>(*d);
    return def;
}

double json_val_as_double(const JsonVal& val, double def) {
    if (auto* d = std::get_if<double>(&val)) return *d;
    if (auto* i = std::get_if<int64_t>(&val)) return static_cast<double>(*i);
    return def;
}

bool json_val_as_bool(const JsonVal& val, bool def) {
    if (auto* b = std::get_if<bool>(&val)) return *b;
    return def;
}

// ---------------------------------------------------------------------------
// Socket path / address
// ---------------------------------------------------------------------------

std::string default_socket_path() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime)
        return std::string(runtime) + "/recmeet/daemon.sock";
    return "/tmp/recmeet-" + std::to_string(getuid()) + "/daemon.sock";
}

bool parse_ipc_address(const std::string& addr, IpcAddress& out) {
    if (addr.empty()) {
        out = default_ipc_address();
        return true;
    }

    // Reject bare IPv6 addresses (multiple colons without bracket notation).
    // Bracket notation [host]:port is reserved for future IPv6 support.
    size_t first_colon = addr.find(':');
    size_t last_colon = addr.rfind(':');
    if (first_colon != last_colon) {
        // Multiple colons — only valid if this were IPv6, which we don't support
        return false;
    }

    // Check for host:port pattern: last colon followed by all digits (1-65535)
    if (last_colon != std::string::npos && last_colon + 1 < addr.size()) {
        std::string port_str = addr.substr(last_colon + 1);
        bool all_digits = true;
        for (char c : port_str) {
            if (c < '0' || c > '9') { all_digits = false; break; }
        }
        if (all_digits && !port_str.empty()) {
            std::string host = addr.substr(0, last_colon);
            if (host.empty()) return false;  // ":9090" with no host
            unsigned long port_val = std::stoul(port_str);
            if (port_val < 1 || port_val > 65535) return false;
            out.transport = IpcTransport::Tcp;
            out.host = host;
            out.port = static_cast<uint16_t>(port_val);
            out.socket_path.clear();
            return true;
        }
    }

    // Otherwise treat as Unix socket path
    out.transport = IpcTransport::Unix;
    out.socket_path = addr;
    out.host.clear();
    out.port = 0;
    return true;
}

IpcAddress default_ipc_address() {
    IpcAddress addr;
    addr.transport = IpcTransport::Unix;
    addr.socket_path = server_socket_path();
    return addr;
}

// ---------------------------------------------------------------------------
// Caption event builders
// ---------------------------------------------------------------------------

IpcEvent make_caption_event(int64_t job_id,
                            const std::string& text,
                            bool is_partial,
                            int64_t timestamp_ms) {
    IpcEvent ev;
    ev.event = "caption";
    ev.data["job_id"]       = job_id;
    ev.data["text"]         = text;
    ev.data["is_partial"]   = is_partial;
    ev.data["timestamp_ms"] = timestamp_ms;
    return ev;
}

IpcEvent make_caption_degraded_event(int64_t job_id,
                                     const std::string& reason,
                                     int64_t timestamp_ms) {
    IpcEvent ev;
    ev.event = "caption.degraded";
    ev.data["job_id"]       = job_id;
    ev.data["reason"]       = reason;
    ev.data["timestamp_ms"] = timestamp_ms;
    return ev;
}

} // namespace recmeet
