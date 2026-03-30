// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <cstdint>
#include <string>

namespace recmeet {

/// Parse a JSON string value for a given key from a flat JSON line.
/// Returns empty string on failure. Handles escaped quotes.
inline std::string parse_ndjson_string(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string result;
    for (; pos < json.size(); ++pos) {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            result += json[++pos];
        } else if (json[pos] == '"') {
            break;
        } else {
            result += json[pos];
        }
    }
    return result;
}

/// Parse a JSON integer value for a given key from a flat JSON line.
/// Returns -1 on failure.
inline int64_t parse_ndjson_int(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return -1;
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    try {
        return std::stoll(json.substr(pos));
    } catch (...) {
        return -1;
    }
}

/// Format an NDJSON progress line and write to stdout with fflush.
inline void write_ndjson(const char* event, const char* json_data) {
    fprintf(stdout, "{\"event\":\"%s\",\"data\":%s}\n", event, json_data);
    fflush(stdout);
}

} // namespace recmeet
