// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "util.h"

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

// Phase B.2 / B.3 — tray-side capture helpers split out so the
// hermetic units (WAV writing, .pending sidecar, staging path
// resolution) can be exercised by unit tests without booting GTK or
// PipeWire. The full state machine + dialog continue to live in
// `tray.cpp` because they touch GtkWidget* / global tray state.

namespace recmeet {
namespace tray_capture {

/// Default staging directory for tray-side recordings. Concrete
/// value: ~/.local/share/recmeet/staging/ (via data_dir()).
fs::path default_staging_dir();

/// Format the canonical "YYYY-MM-DD_HH-MM" timestamp matching the
/// daemon-side audio_<ts>.wav convention. Uses localtime_r — the
/// stamp is operator-facing, not a wire-protocol field.
std::string format_timestamp(std::time_t t);

/// Build the absolute path for a new staging WAV. If a file with the
/// minute-precision name already exists, the helper appends a
/// `_N` collision suffix (mirrors the daemon's iter-60 convention).
/// Throws RecmeetError if no unique slot is found within 10000 tries
/// (an event so improbable it's a clear caller-side bug — e.g. the
/// dir is unwritable).
fs::path next_staging_wav_path(const fs::path& staging_dir,
                               const std::string& timestamp);

/// Write a 16 kHz mono PCM WAV via libsndfile. Returns true on full
/// write, false on any sf_open / sf_write_short failure; on failure
/// `err_msg` is populated with the libsndfile error string.
/// Removes a partially-written file before returning false so the
/// caller never observes a half-written WAV on disk.
bool write_wav(const fs::path& path, const std::vector<int16_t>& samples,
               std::string& err_msg);

/// Phase D.5 — context payload carried inline on the sidecar v2 schema.
/// Mirrors the structured fields the operator entered in the recording
/// context dialog (subject / participants / notes / language /
/// vocabulary). On restart the resume submenu re-presents the submit
/// flow with these values pre-populated so the operator does not have
/// to re-key meeting metadata.
struct PendingSidecarContext {
    std::string subject;
    std::vector<std::string> participants;
    std::string notes;
    std::string language;
    std::vector<std::string> vocabulary;
};

/// Phase D.5 — full sidecar v2 payload. Six top-level scalars + a
/// `context` block. Schema per thin-client-recording-server.md lines
/// 376-391.
struct PendingSidecarV2 {
    std::string meeting_id;        ///< canonical lowercase UUID v4
    std::string wav_path;          ///< absolute path to the staging WAV
    std::string timestamp;         ///< YYYY-MM-DD_HH-MM
    std::string mic_source;        ///< pulse / pipewire source name
    bool captions_enabled = false;
    PendingSidecarContext context;
};

/// Phase D.5 — sidecar v2 atomic writer. Replaces the v1 4-field writer.
/// Uses `util::atomic_write_file` (write-tmp + `fsync` + `rename` +
/// `fsync(dir)`) so a crash mid-save-for-later cannot corrupt the
/// sidecar — the next tray start sees either the v2 payload in full or
/// no sidecar at all (and the WAV becomes eligible for D.6 eviction).
/// Throws `RecmeetError` on filesystem failure; the caller logs and
/// falls back to a discard-or-retry policy.
void write_pending_sidecar_v2(const PendingSidecarV2& payload);

/// Phase D.5 — sidecar v2 reader. Returns the parsed payload; on a
/// missing file, malformed JSON, or schema-version mismatch returns an
/// empty `PendingSidecarV2{}` (all fields default) — the caller treats
/// that as "skip this sidecar" and leaves the file on disk for forensic
/// inspection.
PendingSidecarV2 read_pending_sidecar(const fs::path& sidecar_path);

/// Companion of `write_pending_sidecar_v2`: derive the sidecar path
/// for a given staging WAV. Pure: `audio_X.wav` → `audio_X.pending`.
fs::path pending_sidecar_path(const fs::path& wav_path);

/// Phase D.5 — eviction-contract probe. Returns `true` when the staging
/// WAV at `wav_path` is "protected" (i.e. has a sibling `.pending`
/// sidecar) and therefore must NOT be evicted by D.6's disk-budget
/// sweep. D.5 only exposes the probe — the actual eviction sweep lands
/// in D.6. The test for this contract is the [d5][eviction-contract]
/// assertion.
bool is_sidecar_protected(const fs::path& wav_path);

} // namespace tray_capture
} // namespace recmeet
