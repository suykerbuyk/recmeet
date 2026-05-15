// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.4 — artifact enumeration implementation. See fetch_artifacts.h for
// the filter policy.

#include "fetch_artifacts.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace recmeet {

namespace {

// Lowercase the trailing extension (including the leading dot) so the
// allowlist match is case-insensitive. `Meeting.MD` and `Meeting.md` are
// the same artifact from the wire's perspective.
std::string lowercase_extension(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = filename.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    return ext;
}

// Lowercase a string. Used for filename comparisons (`speakers.json` vs
// `Speakers.JSON`).
std::string lowercase(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(c));
    return out;
}

// Audio extensions written by the upload manager (`audio.<ext>`) and the
// legacy record.start path (`audio_<ts>.wav`, `mic.wav`, `monitor.wav`).
bool is_audio_extension(const std::string& ext) {
    return ext == ".wav" || ext == ".flac" || ext == ".mp3"
        || ext == ".m4a" || ext == ".ogg";
}

// Speaker DB filenames (`speakers.json`, `speakers_<ts>.json`) — server-
// resident per the v2 architecture, never shipped to the client.
bool is_speakers_db_file(const std::string& filename_lower) {
    if (filename_lower == "speakers.json") return true;
    static const std::string kPrefix = "speakers_";
    if (filename_lower.size() > kPrefix.size()
        && filename_lower.compare(0, kPrefix.size(), kPrefix) == 0
        && lowercase_extension(filename_lower) == ".json")
        return true;
    return false;
}

// Context input files (`context.json`, `context_<ts>.json`) — these are
// inputs the operator/client supplies, not outputs of the postprocess
// pipeline. Excluding them keeps the artifact set tight.
bool is_context_input_file(const std::string& filename_lower) {
    if (filename_lower == "context.json") return true;
    static const std::string kPrefix = "context_";
    if (filename_lower.size() > kPrefix.size()
        && filename_lower.compare(0, kPrefix.size(), kPrefix) == 0
        && lowercase_extension(filename_lower) == ".json")
        return true;
    return false;
}

} // anonymous namespace

bool should_include_artifact(const std::string& filename,
                             bool is_regular_file) {
    if (!is_regular_file) return false;
    if (filename.empty() || filename[0] == '.') return false;  // hidden / dotfile

    const std::string lower = lowercase(filename);

    // Explicit blocklist for things that happen to share an allowlisted
    // extension. `.json` is NOT on the allowlist today, but if it ever
    // becomes one, these two paths must still be rejected.
    if (is_speakers_db_file(lower)) return false;
    if (is_context_input_file(lower)) return false;

    const std::string ext = lowercase_extension(lower);

    // Audio: always rejected. The upload-driven C.2 path stages `audio.<ext>`
    // in `out_dir`, and the legacy record.start path writes `audio_<ts>.wav`
    // / `mic.wav` / `monitor.wav`. The client supplied the audio (uploads)
    // or owns the recording (capture) — re-shipping it is wasted bytes.
    if (is_audio_extension(ext)) return false;

    // The allowlist. Narrow on purpose — see fetch_artifacts.h policy comment.
    if (ext == ".md") return true;
    if (ext == ".vtt") return true;

    return false;
}

std::string content_type_for(const std::string& filename) {
    const std::string ext = lowercase_extension(lowercase(filename));
    if (ext == ".md")  return "text/markdown";
    if (ext == ".vtt") return "text/vtt";
    // Defensive fallback. should_include_artifact() guarantees we never reach
    // here for an included file, but a future allowlist extension can land
    // by simply adding the case above; the fallback keeps the wire shape
    // well-formed regardless.
    return "application/octet-stream";
}

std::vector<ArtifactInfo> enumerate_artifacts(const fs::path& out_dir,
                                              std::string* out_error) {
    std::vector<ArtifactInfo> result;

    std::error_code ec;
    if (!fs::exists(out_dir, ec) || ec) {
        if (out_error) {
            *out_error = "out_dir does not exist: " + out_dir.string();
            if (ec) *out_error += " (" + ec.message() + ")";
        }
        return result;
    }
    if (!fs::is_directory(out_dir, ec) || ec) {
        if (out_error) {
            *out_error = "out_dir is not a directory: " + out_dir.string();
            if (ec) *out_error += " (" + ec.message() + ")";
        }
        return result;
    }

    // Non-recursive iteration: artifacts live flat in `out_dir`. Subdirs
    // (e.g. `year/month/` if note_dir overrides into a hierarchy) are
    // intentionally not descended — `Job::input.out_dir` for an upload-
    // driven job IS the staging dir, and the note lands there directly.
    fs::directory_iterator it(out_dir, ec);
    if (ec) {
        if (out_error)
            *out_error = "cannot enumerate out_dir " + out_dir.string()
                       + ": " + ec.message();
        return result;
    }

    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;  // tolerate transient enumeration errors
        const fs::directory_entry& entry = *it;
        const std::string name = entry.path().filename().string();

        std::error_code stat_ec;
        const bool regular = entry.is_regular_file(stat_ec);
        if (stat_ec) continue;

        if (!should_include_artifact(name, regular)) continue;

        ArtifactInfo info;
        info.path = entry.path();
        info.name = name;
        std::error_code size_ec;
        const auto sz = fs::file_size(entry.path(), size_ec);
        info.size = size_ec ? 0 : static_cast<int64_t>(sz);
        info.content_type = content_type_for(name);
        result.push_back(std::move(info));
    }

    // Deterministic wire order: lexicographic by basename. The wire shape
    // is `artifacts[]` in the metadata + N `0x02` frames in the SAME order;
    // the client demultiplexes by counting `0x02` frames against the array,
    // so the order is contractual. Sorting on the server side means two
    // runs against the same directory produce the same `artifacts[]`.
    std::sort(result.begin(), result.end(),
              [](const ArtifactInfo& a, const ArtifactInfo& b) {
                  return a.name < b.name;
              });
    return result;
}

} // namespace recmeet
