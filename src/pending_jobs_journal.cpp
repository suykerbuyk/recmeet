// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "pending_jobs_journal.h"

#include "json_util.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace recmeet {

namespace {

// Hand-parser local to the journal. We do not use a general JSON library
// because every entry was written by THIS class (or a prior version of
// it), so we can rely on the exact serializer output shape. The parser
// extracts string-valued fields by name and an integer-valued
// `submitted_at_unix` field; unknown keys are silently skipped, which is
// the additive-extension property the .h comment promises.

void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
}

std::string unescape_json_string(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char nc = s[i + 1];
            switch (nc) {
                case '"':  out += '"';  ++i; continue;
                case '\\': out += '\\'; ++i; continue;
                case 'n':  out += '\n'; ++i; continue;
                case 'r':  out += '\r'; ++i; continue;
                case 't':  out += '\t'; ++i; continue;
                default: break;
            }
        }
        out += c;
    }
    return out;
}

// Scan past a JSON-encoded string starting at `i` (which points at the
// first content byte, AFTER the opening quote). On success advances `i`
// to point at the closing `"` and returns the start..end-1 substring of
// raw (still-escaped) bytes via `start_out` + `end_out`. Returns false
// on unterminated input. Correctly handles the classic
// `\\"` corner case by tracking whether the previous byte was an
// active escape opener (rather than a naive `s[i-1] != '\\'` check that
// breaks on `\\\\` and `\\"` boundaries).
bool scan_json_string(const std::string& s, size_t& i, size_t& end_out) {
    bool escape = false;
    while (i < s.size()) {
        char c = s[i];
        if (escape) {
            escape = false;
            ++i;
            continue;
        }
        if (c == '\\') {
            escape = true;
            ++i;
            continue;
        }
        if (c == '"') { end_out = i; return true; }
        ++i;
    }
    return false;
}

// Parse one `"key" : <value>` pair starting at index `i` inside an
// object body. `i` is advanced past the value. Returns false on any
// shape violation; the caller treats that as end-of-object.
bool parse_pair(const std::string& s, size_t& i,
                std::string& out_key, std::string& out_val_string,
                bool& out_val_is_string, int64_t& out_val_int) {
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    size_t kstart = i;
    size_t kend = 0;
    if (!scan_json_string(s, i, kend)) return false;
    out_key = unescape_json_string(s.substr(kstart, kend - kstart));
    ++i;  // closing quote
    skip_ws(s, i);
    if (i >= s.size() || s[i] != ':') return false;
    ++i;
    skip_ws(s, i);
    if (i >= s.size()) return false;
    if (s[i] == '"') {
        ++i;
        size_t vstart = i;
        size_t vend = 0;
        if (!scan_json_string(s, i, vend)) return false;
        out_val_string = unescape_json_string(s.substr(vstart, vend - vstart));
        out_val_is_string = true;
        ++i;  // closing quote
    } else if (s[i] == '-' || std::isdigit(static_cast<unsigned char>(s[i]))) {
        out_val_is_string = false;
        int sign = 1;
        if (s[i] == '-') { sign = -1; ++i; }
        int64_t v = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            v = v * 10 + (s[i] - '0');
            ++i;
        }
        out_val_int = sign * v;
    } else {
        return false;
    }
    return true;
}

// Parse the body of one entry object `{ ... }`. `i` points at the `{`
// on entry and is advanced past the `}` on exit.
bool parse_entry(const std::string& s, size_t& i,
                 PendingJobsJournal::Entry& out) {
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    while (true) {
        skip_ws(s, i);
        if (i >= s.size()) return false;
        if (s[i] == '}') { ++i; return true; }

        std::string key, vstr;
        bool vis_str = false;
        int64_t vint = 0;
        if (!parse_pair(s, i, key, vstr, vis_str, vint)) return false;

        if (key == "endpoint" && vis_str)            out.endpoint         = vstr;
        else if (key == "meeting_id" && vis_str)     out.meeting_id       = vstr;
        else if (key == "job_id" && vis_str)         out.job_id           = vstr;
        else if (key == "staging_wav_path" && vis_str) out.staging_wav_path = vstr;
        else if (key == "kind" && vis_str)           out.kind             = vstr;
        else if (key == "slot_kind" && vis_str)      out.slot_kind        = vstr;
        else if (key == "submitted_at_unix" && !vis_str)
            out.submitted_at_unix = vint;
        // unknown keys silently skipped (additive-extension property)

        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
    }
}

std::string serialize_entries(const std::vector<PendingJobsJournal::Entry>& v) {
    std::string out;
    out.reserve(64 + v.size() * 256);
    out += "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ",";
        const auto& e = v[i];
        out += "\n  {";
        out += "\n    \"endpoint\": \"";         out += json_escape(e.endpoint);          out += "\",";
        out += "\n    \"meeting_id\": \"";       out += json_escape(e.meeting_id);        out += "\",";
        out += "\n    \"job_id\": \"";           out += json_escape(e.job_id);            out += "\",";
        out += "\n    \"staging_wav_path\": \""; out += json_escape(e.staging_wav_path);  out += "\",";
        out += "\n    \"kind\": \"";             out += json_escape(e.kind);              out += "\",";
        out += "\n    \"slot_kind\": \"";        out += json_escape(e.slot_kind);         out += "\",";
        out += "\n    \"submitted_at_unix\": ";  out += std::to_string(e.submitted_at_unix);
        out += "\n  }";
    }
    if (!v.empty()) out += "\n";
    out += "]\n";
    return out;
}

} // anonymous namespace

PendingJobsJournal::PendingJobsJournal()
    : path_(data_dir() / "pending_jobs.json") {
}

PendingJobsJournal::PendingJobsJournal(fs::path path)
    : path_(std::move(path)) {
}

std::vector<PendingJobsJournal::Entry> PendingJobsJournal::load() const {
    std::vector<Entry> out;
    std::error_code ec;
    if (!fs::exists(path_, ec) || ec) return out;

    std::ifstream in(path_);
    if (!in) {
        log_warn("[pending_jobs_journal] cannot open %s for read",
                 path_.string().c_str());
        return out;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string body = ss.str();

    size_t i = 0;
    skip_ws(body, i);
    if (i >= body.size() || body[i] != '[') {
        log_warn("[pending_jobs_journal] %s does not look like a JSON array",
                 path_.string().c_str());
        return out;
    }
    ++i;
    while (true) {
        skip_ws(body, i);
        if (i >= body.size()) break;
        if (body[i] == ']') break;
        Entry e;
        if (!parse_entry(body, i, e)) {
            log_warn("[pending_jobs_journal] parse error in %s at offset %zu",
                     path_.string().c_str(), i);
            break;
        }
        out.push_back(std::move(e));
        skip_ws(body, i);
        if (i < body.size() && body[i] == ',') { ++i; continue; }
    }
    return out;
}

void PendingJobsJournal::save(const std::vector<Entry>& entries) const {
    // Mode 0 — journal contents are not secret (no auth material); the
    // open()-time 0600 mask is sufficient. Distinguished from the token
    // store which forces a post-rename chmod to be explicit about the
    // secrecy contract.
    atomic_write_file(path_, serialize_entries(entries), 0);
}

void PendingJobsJournal::append(const Entry& entry) const {
    auto v = load();
    v.push_back(entry);
    save(v);
}

void PendingJobsJournal::remove_by_job_id(const std::string& job_id) const {
    if (job_id.empty()) return;  // defensive — never remove all entries
    auto v = load();
    auto orig = v.size();
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const Entry& e) { return e.job_id == job_id; }),
            v.end());
    if (v.size() != orig) save(v);
}

} // namespace recmeet
