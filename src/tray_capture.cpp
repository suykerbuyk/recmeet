// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "tray_capture.h"

#include "json_util.h"
#include "log.h"

#include <sndfile.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace recmeet {
namespace tray_capture {

fs::path default_staging_dir() {
    return client_data_dir() / "staging";
}

std::string format_timestamp(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M", &tm);
    return std::string(buf);
}

fs::path next_staging_wav_path(const fs::path& staging_dir,
                               const std::string& timestamp) {
    fs::path candidate = staging_dir / ("audio_" + timestamp + ".wav");
    if (!fs::exists(candidate)) return candidate;
    for (int i = 1; i <= 10000; ++i) {
        candidate = staging_dir /
            ("audio_" + timestamp + "_" + std::to_string(i) + ".wav");
        if (!fs::exists(candidate)) return candidate;
    }
    throw RecmeetError("tray_capture: could not allocate a unique WAV filename "
                       "under " + staging_dir.string());
}

bool write_wav(const fs::path& path, const std::vector<int16_t>& samples,
               std::string& err_msg) {
    SF_INFO info = {};
    info.samplerate = SAMPLE_RATE;
    info.channels   = CHANNELS;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* sf = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!sf) {
        err_msg = std::string("sf_open failed: ") + sf_strerror(nullptr);
        return false;
    }
    sf_count_t written = sf_write_short(sf, samples.data(),
                                        static_cast<sf_count_t>(samples.size()));
    sf_close(sf);
    if (written != static_cast<sf_count_t>(samples.size())) {
        err_msg = "sf_write_short truncated";
        std::error_code ec;
        fs::remove(path, ec);
        return false;
    }
    return true;
}

fs::path pending_sidecar_path(const fs::path& wav_path) {
    fs::path s = wav_path;
    s.replace_extension(".pending");
    return s;
}

// ---------------------------------------------------------------------------
// Phase D.5 — sidecar v2 schema
// ---------------------------------------------------------------------------
//
// Six top-level scalars + a `context` block, per
// thin-client-recording-server.md lines 376-391. The writer goes through
// `atomic_write_file` so save-for-later is crash-safe (the next tray
// start sees either the full payload or no sidecar at all). The reader
// hand-parses the same shape — the file is always one this writer (or a
// prior version) emitted, so we do not need a general JSON library.

namespace {

void append_string_array(std::string& out, const std::vector<std::string>& v) {
    out += "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ", ";
        out += "\"";
        out += json_escape(v[i]);
        out += "\"";
    }
    out += "]";
}

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

// Parse a quoted JSON string starting at `i` (which must point at the
// opening `"`). On success advances `i` past the closing quote and
// returns true. Correctly handles `\\` followed by `"` via an
// escape-state machine — a naive `s[i-1] != '\\'` check breaks on
// `\\"` sequences because the `\\` is itself an escaped backslash that
// terminates the escape state.
bool parse_quoted(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    size_t start = i;
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
        if (c == '"') break;
        ++i;
    }
    if (i >= s.size()) return false;
    out = unescape_json_string(s.substr(start, i - start));
    ++i;
    return true;
}

// Parse a `"key": <value>` pair. Returns false on shape violation.
// `value_kind`: 's' = string, 'b' = bool, 'a' = array of strings,
// 'o' = nested object. Caller dispatches based on field name.
bool parse_string_pair(const std::string& s, size_t& i,
                       std::string& key, std::string& val) {
    skip_ws(s, i);
    if (!parse_quoted(s, i, key)) return false;
    skip_ws(s, i);
    if (i >= s.size() || s[i] != ':') return false;
    ++i;
    skip_ws(s, i);
    return parse_quoted(s, i, val);
}

bool parse_string_array(const std::string& s, size_t& i,
                        std::vector<std::string>& out) {
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '[') return false;
    ++i;
    while (true) {
        skip_ws(s, i);
        if (i >= s.size()) return false;
        if (s[i] == ']') { ++i; return true; }
        std::string v;
        if (!parse_quoted(s, i, v)) return false;
        out.push_back(std::move(v));
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
    }
}

bool parse_bool(const std::string& s, size_t& i, bool& out) {
    skip_ws(s, i);
    if (i + 4 <= s.size() && s.compare(i, 4, "true") == 0) {
        out = true; i += 4; return true;
    }
    if (i + 5 <= s.size() && s.compare(i, 5, "false") == 0) {
        out = false; i += 5; return true;
    }
    return false;
}

bool parse_context_block(const std::string& s, size_t& i,
                         PendingSidecarContext& out) {
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    while (true) {
        skip_ws(s, i);
        if (i >= s.size()) return false;
        if (s[i] == '}') { ++i; return true; }
        std::string key;
        if (!parse_quoted(s, i, key)) return false;
        skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        skip_ws(s, i);
        if (i >= s.size()) return false;
        if (s[i] == '"') {
            std::string v;
            if (!parse_quoted(s, i, v)) return false;
            if      (key == "subject")  out.subject  = v;
            else if (key == "notes")    out.notes    = v;
            else if (key == "language") out.language = v;
            // unknown string keys silently skipped
        } else if (s[i] == '[') {
            std::vector<std::string> v;
            if (!parse_string_array(s, i, v)) return false;
            if      (key == "participants") out.participants = std::move(v);
            else if (key == "vocabulary")   out.vocabulary   = std::move(v);
        } else {
            return false;
        }
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
    }
}

} // anonymous namespace

void write_pending_sidecar_v2(const PendingSidecarV2& p) {
    std::string out;
    out.reserve(512);
    out += "{\n";
    out += "  \"meeting_id\": \"";  out += json_escape(p.meeting_id);   out += "\",\n";
    out += "  \"wav_path\": \"";    out += json_escape(p.wav_path);     out += "\",\n";
    out += "  \"timestamp\": \"";   out += json_escape(p.timestamp);    out += "\",\n";
    out += "  \"mic_source\": \""; out += json_escape(p.mic_source);   out += "\",\n";
    out += "  \"captions_enabled\": ";
    out += (p.captions_enabled ? "true" : "false");
    out += ",\n";
    out += "  \"context\": {\n";
    out += "    \"subject\": \"";  out += json_escape(p.context.subject); out += "\",\n";
    out += "    \"participants\": ";
    append_string_array(out, p.context.participants);
    out += ",\n";
    out += "    \"notes\": \"";    out += json_escape(p.context.notes);    out += "\",\n";
    out += "    \"language\": \""; out += json_escape(p.context.language); out += "\",\n";
    out += "    \"vocabulary\": ";
    append_string_array(out, p.context.vocabulary);
    out += "\n  }\n";
    out += "}\n";

    fs::path sidecar = pending_sidecar_path(p.wav_path);
    atomic_write_file(sidecar, out, 0);
}

PendingSidecarV2 read_pending_sidecar(const fs::path& sidecar_path) {
    PendingSidecarV2 out;
    std::error_code ec;
    if (!fs::exists(sidecar_path, ec) || ec) return out;

    std::ifstream in(sidecar_path);
    if (!in) {
        log_warn("[tray_capture] cannot open sidecar %s for read",
                 sidecar_path.string().c_str());
        return out;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string body = ss.str();

    size_t i = 0;
    skip_ws(body, i);
    if (i >= body.size() || body[i] != '{') {
        log_warn("[tray_capture] sidecar %s is not a JSON object",
                 sidecar_path.string().c_str());
        return PendingSidecarV2{};
    }
    ++i;
    while (true) {
        skip_ws(body, i);
        if (i >= body.size()) {
            log_warn("[tray_capture] sidecar %s truncated",
                     sidecar_path.string().c_str());
            return PendingSidecarV2{};
        }
        if (body[i] == '}') { ++i; break; }

        std::string key;
        if (!parse_quoted(body, i, key)) {
            log_warn("[tray_capture] sidecar %s malformed key",
                     sidecar_path.string().c_str());
            return PendingSidecarV2{};
        }
        skip_ws(body, i);
        if (i >= body.size() || body[i] != ':') return PendingSidecarV2{};
        ++i;
        skip_ws(body, i);
        if (i >= body.size()) return PendingSidecarV2{};

        if (body[i] == '"') {
            std::string v;
            if (!parse_quoted(body, i, v)) return PendingSidecarV2{};
            if      (key == "meeting_id") out.meeting_id = v;
            else if (key == "wav_path")   out.wav_path   = v;
            else if (key == "timestamp")  out.timestamp  = v;
            else if (key == "mic_source") out.mic_source = v;
        } else if (body[i] == 't' || body[i] == 'f') {
            bool bv;
            if (!parse_bool(body, i, bv)) return PendingSidecarV2{};
            if (key == "captions_enabled") out.captions_enabled = bv;
        } else if (body[i] == '{') {
            PendingSidecarContext ctx;
            if (!parse_context_block(body, i, ctx)) return PendingSidecarV2{};
            if (key == "context") out.context = std::move(ctx);
        } else {
            log_warn("[tray_capture] sidecar %s unexpected value type",
                     sidecar_path.string().c_str());
            return PendingSidecarV2{};
        }
        skip_ws(body, i);
        if (i < body.size() && body[i] == ',') { ++i; continue; }
    }
    return out;
}

bool is_sidecar_protected(const fs::path& wav_path) {
    fs::path sidecar = pending_sidecar_path(wav_path);
    std::error_code ec;
    return fs::exists(sidecar, ec) && !ec;
}

} // namespace tray_capture
} // namespace recmeet
