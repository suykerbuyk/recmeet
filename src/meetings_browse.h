// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

// Phase E.6.1 — server-side meeting discovery helper.
//
// `discover_meetings(meetings_root)` walks the top-level meeting directories
// under `meetings_root`, samples each one for the three artifact-presence
// flags the WebUI cares about (`audio`, `speakers.json`, `Meeting_*.md`),
// and returns one `MeetingInfo` per dir sorted by mtime descending. The
// helper is colocated with the `meetings.list` IPC verb (daemon.cpp) rather
// than folded into meeting_index.cpp — meeting_index.cpp is a strict
// `meeting_id → path` map and a different concern. The helper is pure
// std::filesystem and does NOT pull in any ML symbols, which matters for
// the daemon's binary-slim invariant.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace recmeet {

namespace fs = std::filesystem;

/// Wire shape for the `meetings.list` IPC response. `meeting_id` is
/// optional — null/absent for pre-C.11 legacy meetings whose `context.json`
/// lacks a `meeting_id` field. The WebUI uses this nullability to hide
/// mutation buttons (relabel / reprocess / enroll) on legacy meetings.
struct MeetingInfo {
    std::string name;                          ///< dir basename (operator-readable)
    std::optional<std::string> meeting_id;     ///< canonical lowercase UUID v4, or nullopt
    bool has_audio = false;
    bool has_speakers = false;
    bool has_summary = false;
    std::string mtime_iso;                     ///< ISO 8601 UTC, e.g. "2026-05-20T12:34:56Z"
};

/// Walk top-level meeting directories under `meetings_root` and produce one
/// `MeetingInfo` per directory.
///
/// * `name` = dir basename
/// * `meeting_id` = optional, populated from `context.json` via
///   `load_meeting_id(dir)` when present and valid; nullopt otherwise
/// * `has_audio` = true when `find_audio_file(dir)` returns a non-empty path
/// * `has_speakers` = true when `find_speakers_file(dir)` returns a non-empty path
/// * `has_summary` = true when a `Meeting_*.md` exists in the dir
/// * `mtime_iso` = directory mtime formatted as `%Y-%m-%dT%H:%M:%SZ` (UTC)
///
/// The returned vector is sorted by directory mtime descending (most recent
/// first). A missing or non-directory `meetings_root` is not an error;
/// returns an empty vector.
std::vector<MeetingInfo> discover_meetings(const fs::path& meetings_root);

} // namespace recmeet
