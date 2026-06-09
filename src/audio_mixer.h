// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace recmeet {

/// Mix two S16LE mono streams by averaging samples.
/// If streams differ in length, the shorter one is zero-padded.
std::vector<int16_t> mix_audio(const std::vector<int16_t>& a,
                                const std::vector<int16_t>& b);

/// Validate an in-memory S16LE 16kHz mono capture buffer has at least
/// `min_duration` seconds of samples. Returns the buffer duration in
/// seconds. Throws `AudioValidationError` if too short.
///
/// This is the capture-side counterpart to `validate_audio()` (which reads
/// a WAV from disk and lives in `recmeet_core`). The tray's offline
/// mix-at-Stop path validates the monitor buffer (non-fatal, falls back to
/// mic-only) before writing the authoritative WAV, and `recmeet-client`
/// cannot link `recmeet_core` — hence this buffer-level check lives here in
/// `recmeet_capture`. Mirrors V1's `validate_audio(path, 1.0, label)` gate
/// in `live_recording.cpp`.
double validate_min_duration(const std::vector<int16_t>& samples,
                             double min_duration,
                             const std::string& label);

/// Result of `finalize_dual_mix`: the authoritative mono buffer to stage at
/// Stop, and whether the monitor stream was actually mixed in.
struct DualMixResult {
    std::vector<int16_t> audio;
    bool mixed = false;
};

/// Decide the authoritative staging buffer for a dual-source recording's
/// Stop. If `mon` clears `min_monitor_duration` seconds, the result is the
/// offline `mix_audio()` of mic+monitor with `mixed=true`; otherwise the
/// monitor is dropped as silent/broken and the mic buffer is staged
/// unchanged with `mixed=false` — V1's non-fatal monitor degradation
/// (`live_recording.cpp:382-397`). A mic-only recording (empty `mon`)
/// always yields `mixed=false`. Pure: no I/O or logging, so the caller owns
/// the user-facing warn/info messages.
DualMixResult finalize_dual_mix(std::vector<int16_t> mic,
                                std::vector<int16_t> mon,
                                double min_monitor_duration);

} // namespace recmeet
