// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "reconnect_backoff.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace recmeet {

int next_nominal_backoff(int current_nominal, int cap_secs) {
    if (current_nominal <= 0) return std::min(1, cap_secs > 0 ? cap_secs : 1);
    int next = current_nominal * 2;
    if (cap_secs > 0 && next > cap_secs) next = cap_secs;
    return next;
}

int jittered_delay_secs(int nominal_secs,
                        double jitter_fraction,
                        std::mt19937& rng,
                        int cap_secs) {
    if (nominal_secs <= 0) nominal_secs = 1;
    if (jitter_fraction < 0.0) jitter_fraction = 0.0;
    if (jitter_fraction >= 1.0) jitter_fraction = 0.999;
    if (cap_secs <= 0) cap_secs = nominal_secs;

    // Uniform draw in [-jitter_fraction, +jitter_fraction]. The double
    // distribution is uniform over the full range; the bound construction
    // matches the AWS-style "equal jitter" pattern but symmetric
    // (±fraction rather than 0..fraction) — symmetry is the spec.
    std::uniform_real_distribution<double> jitter_dist(-jitter_fraction,
                                                       jitter_fraction);
    double draw = jitter_dist(rng);
    double jittered = static_cast<double>(nominal_secs) * (1.0 + draw);

    // Round to integer seconds — the GTK timeout API operates on whole
    // seconds. round-to-nearest is the right policy: floor would bias
    // toward shorter delays, ceil toward longer ones.
    long out = std::lround(jittered);
    if (out < 1) out = 1;
    if (out > cap_secs) out = cap_secs;
    return static_cast<int>(out);
}

JobResyncClassification classify_resynced_job(const std::string& state,
                                              const std::string& kind) {
    JobResyncClassification c;
    // Done → fetch artifacts. Same semantics for every kind: the daemon
    // retains terminal jobs (C.7) so a server-side `done` is always
    // followed by a successful `process.fetch` on the client side.
    if (state == "done") {
        c.action = JobResyncAction::Fetch;
        return c;
    }
    // Failed / cancelled / unknown → user-visible failure. The tray
    // surfaces a dbus notify so the operator sees the loss. The
    // synthetic "unknown" state is what the tray feeds in when
    // `job.status` returns PermissionDenied/InvalidParams for a job_id
    // the local journal expected the server to know about.
    if (state == "failed" || state == "cancelled" || state == "unknown") {
        c.action = JobResyncAction::NotifyFailed;
        // Streaming-session-aborted policy (plan line 362-363): a
        // streaming job that the daemon reports as failed/cancelled/
        // unknown post-reconnect cannot be resumed — the C.10a TCP-
        // drop policy already finalized it server-side. The tray's
        // caller falls back to convergence-principle pattern 2 (batch
        // upload via `process.submit` carrying the same meeting_id).
        if (kind == "streaming") {
            c.streaming_aborted = true;
        }
        return c;
    }
    // Everything else (queued / waiting_on_download / waiting_for_upload
    // / running) is in-flight server-side. The event pump catches up
    // via the next `progress.job` / `job.complete` event; the tray has
    // no immediate action.
    c.action = JobResyncAction::Monitor;
    return c;
}

namespace {

// Skip whitespace at `pos` in `s`. Updates `pos` in place. Returns
// true if there are still characters to read after the skip.
bool skip_ws(const std::string& s, std::size_t& pos) {
    while (pos < s.size()) {
        char c = s[pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return true;
        ++pos;
    }
    return false;
}

// Extract the next JSON string literal at `pos` (which must point at
// the opening quote). Updates `pos` past the closing quote. Returns
// the unescaped string. Escape handling: \\, \", \n, \t, \r — the
// daemon's `json_escape` only emits the basic set so we mirror it.
// Defensive: a malformed string (no closing quote) returns whatever
// was scanned so far and parks `pos` at end-of-input.
std::string scan_string(const std::string& s, std::size_t& pos) {
    std::string out;
    if (pos >= s.size() || s[pos] != '"') return out;
    ++pos;  // opening quote
    while (pos < s.size()) {
        char c = s[pos];
        if (c == '\\' && pos + 1 < s.size()) {
            char nc = s[pos + 1];
            switch (nc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                default:   out += nc;   break;  // permissive fallback
            }
            pos += 2;
            continue;
        }
        if (c == '"') { ++pos; return out; }
        out += c;
        ++pos;
    }
    return out;
}

// Extract a signed integer at `pos`. Updates `pos` past the digits.
// Leading sign accepted. Returns 0 if no digits are read.
int64_t scan_int(const std::string& s, std::size_t& pos) {
    int sign = 1;
    if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) {
        if (s[pos] == '-') sign = -1;
        ++pos;
    }
    int64_t val = 0;
    bool any = false;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        val = val * 10 + (s[pos] - '0');
        ++pos;
        any = true;
    }
    return any ? sign * val : 0;
}

// Parse one object {...} into `out`. `pos` points at the opening `{`
// and is advanced past the matching `}`. Tolerates unknown keys
// (skipped) so the daemon adding new fields does not break the client.
void parse_one_job_object(const std::string& s,
                          std::size_t& pos,
                          ParsedJobListEntry& out) {
    if (pos >= s.size() || s[pos] != '{') return;
    ++pos;  // opening brace
    while (pos < s.size()) {
        if (!skip_ws(s, pos)) return;
        char c = s[pos];
        if (c == '}') { ++pos; return; }
        if (c == ',') { ++pos; continue; }
        if (c != '"') { ++pos; continue; }  // skip noise

        std::string key = scan_string(s, pos);
        if (!skip_ws(s, pos)) return;
        if (s[pos] != ':') { ++pos; continue; }
        ++pos;
        if (!skip_ws(s, pos)) return;

        // String-valued keys vs int-valued. The daemon's
        // serialize_job_object emits the seven fields with fixed
        // types: job_id + progress are integers, the rest are strings.
        if (key == "job_id") {
            out.job_id = scan_int(s, pos);
        } else if (key == "progress") {
            out.progress = static_cast<int>(scan_int(s, pos));
        } else if (key == "kind") {
            out.kind = scan_string(s, pos);
        } else if (key == "state") {
            out.state = scan_string(s, pos);
        } else if (key == "meeting_id") {
            out.meeting_id = scan_string(s, pos);
        } else if (key == "phase") {
            out.phase = scan_string(s, pos);
        } else if (key == "error") {
            out.error = scan_string(s, pos);
        } else {
            // Unknown / unconsumed key — skip its value. We rely on
            // the daemon never embedding nested arrays/objects in a
            // job-object element (the C.6 contract), so the value is
            // a primitive: string, integer, true/false, or null.
            if (s[pos] == '"') {
                (void)scan_string(s, pos);
            } else {
                // Read until the next , } or end.
                while (pos < s.size() && s[pos] != ',' && s[pos] != '}') ++pos;
            }
        }
    }
}

} // anonymous namespace

std::vector<ParsedJobListEntry>
parse_job_list_jobs(const std::string& jobs_array_json) {
    std::vector<ParsedJobListEntry> out;
    std::size_t pos = 0;
    if (!skip_ws(jobs_array_json, pos)) return out;
    if (jobs_array_json[pos] != '[') return out;
    ++pos;
    while (pos < jobs_array_json.size()) {
        if (!skip_ws(jobs_array_json, pos)) break;
        char c = jobs_array_json[pos];
        if (c == ']') { ++pos; break; }
        if (c == ',') { ++pos; continue; }
        if (c == '{') {
            ParsedJobListEntry e;
            parse_one_job_object(jobs_array_json, pos, e);
            out.push_back(std::move(e));
            continue;
        }
        // Unrecognized — skip one character defensively.
        ++pos;
    }
    return out;
}

} // namespace recmeet
