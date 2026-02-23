// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "util.h"

#include <string>
#include <vector>

namespace recmeet {

struct TranscriptSegment {
    double start;  // seconds
    double end;
    std::string text;
};

struct TranscriptResult {
    std::vector<TranscriptSegment> segments;
    std::string language;
    float language_prob;

    /// Format as timestamped text: "[MM:SS - MM:SS] text"
    std::string to_string() const;
};

/// Transcribe a WAV file using whisper.cpp.
/// model_path: path to GGUF model file.
/// audio_path: path to WAV file (will be read and converted to float32).
/// language: ISO 639-1 code (e.g. "en") to force; empty = auto-detect.
/// threads: number of CPU threads (0 = use default_thread_count()).
TranscriptResult transcribe(const fs::path& model_path, const fs::path& audio_path,
                            const std::string& language = "", int threads = 0);

} // namespace recmeet
