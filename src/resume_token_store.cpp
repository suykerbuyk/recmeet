// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "resume_token_store.h"

#include "json_util.h"
#include "log.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace recmeet {

ResumeTokenStore::ResumeTokenStore()
    : path_(state_dir() / "session.tokens.json") {
}

ResumeTokenStore::ResumeTokenStore(fs::path path)
    : path_(std::move(path)) {
}

void ResumeTokenStore::reload() {
    loaded_ = false;
    entries_.clear();
}

// Parse the persisted JSON object `{"<addr>": "<token>", ...}`. The format
// is intentionally minimal — flat string-to-string map — and we hand-parse
// rather than dragging a JSON library across the IPC boundary. The file is
// always one this process (or a prior `recmeet-client`) wrote, so we don't
// have to tolerate adversarial input. Malformed contents are treated as an
// empty map; the next `put` will rewrite the file from scratch.
void ResumeTokenStore::load_from_disk() {
    loaded_ = true;
    entries_.clear();

    std::error_code ec;
    if (!fs::exists(path_, ec) || ec) return;

    std::ifstream in(path_);
    if (!in) {
        log_warn("[resume_token_store] cannot open %s for read",
                 path_.string().c_str());
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string body = ss.str();

    // Strip the outer braces. A defensive check — if the file has been
    // hand-edited or the prior writer crashed mid-write somehow the byte
    // contract still requires `{ ... }`.
    size_t lb = body.find('{');
    size_t rb = body.rfind('}');
    if (lb == std::string::npos || rb == std::string::npos || rb <= lb) {
        log_warn("[resume_token_store] %s does not look like a JSON object",
                 path_.string().c_str());
        return;
    }

    // Walk key/value pairs. Each pair is `"<key>"\s*:\s*"<value>"` with
    // optional trailing comma. Quotes inside the strings are escaped per
    // the writer (`json_escape`); we unescape the basic cases (`\\` and
    // `\"`) — server addresses and tokens don't contain control chars in
    // practice but we honor the same escape grammar the writer emits so
    // round-trip is exact.
    auto unescape = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '\\' && i + 1 < s.size()) {
                char nc = s[i + 1];
                if (nc == '"' || nc == '\\') { out += nc; ++i; continue; }
            }
            out += c;
        }
        return out;
    };

    // Scan past one JSON string literal — escape-aware.
    auto scan_string = [&](size_t& i) -> bool {
        bool escape = false;
        while (i < rb) {
            char c = body[i];
            if (escape) { escape = false; ++i; continue; }
            if (c == '\\') { escape = true; ++i; continue; }
            if (c == '"') return true;
            ++i;
        }
        return false;
    };

    size_t i = lb + 1;
    while (i < rb) {
        // Skip whitespace / commas
        while (i < rb && (body[i] == ' ' || body[i] == '\t' ||
                          body[i] == '\n' || body[i] == '\r' ||
                          body[i] == ','))
            ++i;
        if (i >= rb) break;
        if (body[i] != '"') break;
        size_t kstart = ++i;
        if (!scan_string(i)) break;
        std::string key = unescape(body.substr(kstart, i - kstart));
        ++i;  // closing quote
        // Skip whitespace and colon
        while (i < rb && (body[i] == ' ' || body[i] == '\t' ||
                          body[i] == '\n' || body[i] == '\r'))
            ++i;
        if (i >= rb || body[i] != ':') break;
        ++i;
        while (i < rb && (body[i] == ' ' || body[i] == '\t' ||
                          body[i] == '\n' || body[i] == '\r'))
            ++i;
        if (i >= rb || body[i] != '"') break;
        size_t vstart = ++i;
        if (!scan_string(i)) break;
        std::string val = unescape(body.substr(vstart, i - vstart));
        ++i;  // closing quote
        entries_[key] = val;
    }
}

std::string ResumeTokenStore::serialize() const {
    std::string out;
    out.reserve(64 + entries_.size() * 96);
    out += "{";
    bool first = true;
    for (const auto& [k, v] : entries_) {
        if (!first) out += ",";
        first = false;
        out += "\n  \"";
        out += json_escape(k);
        out += "\": \"";
        out += json_escape(v);
        out += "\"";
    }
    if (!entries_.empty()) out += "\n";
    out += "}\n";
    return out;
}

std::optional<std::string>
ResumeTokenStore::get(const std::string& server_address) {
    if (!loaded_) load_from_disk();
    auto it = entries_.find(server_address);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

void ResumeTokenStore::put(const std::string& server_address,
                           const std::string& token) {
    if (!loaded_) load_from_disk();
    auto it = entries_.find(server_address);
    if (it != entries_.end() && it->second == token) return;  // idempotent

    entries_[server_address] = token;
    // Mode 0600 — the token is server-side auth material for the
    // (client_id, session) tuple; D.5 plan checklist item #5/#6
    // explicitly calls this out as a secrecy requirement.
    atomic_write_file(path_, serialize(), 0600);
}

void ResumeTokenStore::erase(const std::string& server_address) {
    if (!loaded_) load_from_disk();
    auto it = entries_.find(server_address);
    if (it == entries_.end()) return;  // idempotent
    entries_.erase(it);
    atomic_write_file(path_, serialize(), 0600);
}

} // namespace recmeet
