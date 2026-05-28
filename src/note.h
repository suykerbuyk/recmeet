// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "util.h"

#include <string>
#include <vector>

namespace recmeet {

struct NoteConfig {
    std::string domain = "general";
    std::vector<std::string> tags;
};

struct MeetingMetadata {
    std::string title;
    std::string description;
    std::vector<std::string> tags;
    std::vector<std::string> participants;
};

struct MeetingData {
    std::string date;              // YYYY-MM-DD
    std::string time;              // HH:MM
    std::string summary_text;      // Markdown summary (from API/LLM)
    std::string transcript_text;   // Full timestamped transcript
    std::string context_text;      // Pre-meeting notes (optional)
    fs::path output_dir;           // Path to raw files
    fs::path note_dir;             // Where to write the note (defaults to output_dir)
    std::vector<std::string> action_items; // Extracted from summary

    // AI-derived metadata
    std::string title;
    std::string description;
    std::vector<std::string> ai_tags;
    std::vector<std::string> participants;
    int duration_seconds = 0;
    std::string whisper_model;
};

/// Write a formatted meeting note to data.output_dir.
/// Returns the path to the created note.
fs::path write_meeting_note(const NoteConfig& config, const MeetingData& data);

/// Extract action items from a summary (lines starting with "- " under "Action Items").
std::vector<std::string> extract_action_items(const std::string& summary);

/// Extract AI-derived metadata (Title, Tags, Description, Participants) from a summary.
MeetingMetadata extract_meeting_metadata(const std::string& summary);

/// Remove the metadata block (Title/Tags/Description lines) from the summary body.
std::string strip_metadata_block(const std::string& summary);

// ---------------------------------------------------------------------------
// note_internal — implementation helpers exported for direct unit-test
// reachability. NOT a public API for production consumers; the only
// non-test caller is `write_meeting_note()` in src/note.cpp.
//
// Convention precedent: this is the first `*_internal` namespace in the
// recmeet/ source tree (verified iter 228 via `git grep
// 'namespace.*_internal' src/`). Subsequent test-only helper exports
// SHOULD follow this shape: a nested `namespace <topic>_internal` inside
// the owning component's public header, with this same one-line comment
// explaining the test-only contract.
// ---------------------------------------------------------------------------
namespace note_internal {

/// Scan `note_parent` for any meeting-note files matching
/// `timestamp_prefix`, migrate legacy un-numbered notes to the `.XX`
/// attempt-counter scheme (mtime-ASC, filename-ASC tiebreak), and return
/// the attempt number the caller's about-to-be-written note should take.
///
/// Self-healing: every call re-scans, so a partial-migration state
/// (e.g. a prior `fs::rename` failed mid-way due to a transient EACCES)
/// is picked up and completed on the next write. The "never gap-fill"
/// invariant is preserved — a failed rename still consumes the slot, so
/// the returned attempt number is always strictly greater than every
/// `.XX` currently on disk.
///
/// Caller contract: `note_parent` MUST already exist (the helper does
/// not create it). On `directory_iterator` failure (permissions/ENOSPC
/// on the directory itself), throws `RecmeetError`. Per-file rename
/// failures during migration are logged via `log_warn` but do not throw.
///
/// `timestamp_prefix` is the `YYYY-MM-DD_HH-MM` substring that
/// `write_meeting_note` composes between `Meeting_` and the optional
/// `_<safe_title>` suffix. The helper regex-escapes it defensively
/// before interpolation.
int next_attempt_and_migrate(const fs::path& note_parent,
                             const std::string& timestamp_prefix);

} // namespace note_internal

} // namespace recmeet
