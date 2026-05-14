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

/// Write the `.pending` sidecar for a "Save for later" disposition.
/// The shape is intentionally minimal — D.5 will tighten the schema:
///   {
///     "wav_path":         "<absolute path>",
///     "timestamp":        "YYYY-MM-DD_HH-MM",
///     "mic_source":       "<pulse/pipewire source name>",
///     "captions_enabled": <bool>
///   }
/// Returns false if the file could not be opened or fully written.
bool write_pending_sidecar(const fs::path& wav_path,
                           const std::string& timestamp,
                           const std::string& mic_source,
                           bool captions_enabled);

/// Companion of `write_pending_sidecar`: derive the sidecar path
/// for a given staging WAV. Pure: `audio_X.wav` → `audio_X.pending`.
fs::path pending_sidecar_path(const fs::path& wav_path);

} // namespace tray_capture
} // namespace recmeet
