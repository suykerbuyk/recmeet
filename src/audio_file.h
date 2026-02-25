// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "util.h"

#include <cstdint>
#include <string>
#include <vector>

namespace recmeet {

/// Write S16LE mono 16kHz samples to a WAV file using libsndfile.
void write_wav(const fs::path& path, const std::vector<int16_t>& samples);

/// Read a WAV file and return float32 samples normalized to [-1, 1].
/// This is the format whisper.cpp expects.
std::vector<float> read_wav_float(const fs::path& path);

/// Validate a WAV file exists and has minimum duration.
/// Returns duration in seconds. Throws AudioValidationError on failure.
double validate_audio(const fs::path& path, double min_duration = 1.0,
                      const std::string& label = "Audio");

/// Return audio duration in seconds (truncated). Returns 0 on any error.
int get_audio_duration_seconds(const fs::path& path);

} // namespace recmeet
