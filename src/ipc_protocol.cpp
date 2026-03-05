// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "ipc_protocol.h"
#include "summarize.h"  // json_escape(), json_extract_string()

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
        std::string data_raw = json_val_as_string(top["data"]);
        if (!data_raw.empty() && data_raw[0] == '{')
            parse_json_object(data_raw, 0, out.event.data);
        return true;
    }

    return false;
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
// Socket path
// ---------------------------------------------------------------------------

std::string default_socket_path() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime)
        return std::string(runtime) + "/recmeet/daemon.sock";
    return "/tmp/recmeet-" + std::to_string(getuid()) + "/daemon.sock";
}

} // namespace recmeet
