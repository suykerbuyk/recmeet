// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "util.h"

#include <string>
#include <vector>

struct whisper_context;  // forward-declare to avoid exposing whisper.h

namespace recmeet {

// ---------------------------------------------------------------------------
// WhisperModel — RAII wrapper for a loaded whisper.cpp model
// ---------------------------------------------------------------------------

class WhisperModel {
public:
    /// Load a GGUF model from disk.  Throws RecmeetError on failure.
    explicit WhisperModel(const fs::path& model_path);
    ~WhisperModel();

    WhisperModel(const WhisperModel&) = delete;
    WhisperModel& operator=(const WhisperModel&) = delete;

    WhisperModel(WhisperModel&& other) noexcept;
    WhisperModel& operator=(WhisperModel&& other) noexcept;

    whisper_context* get() const { return ctx_; }
    const fs::path& path() const { return path_; }

private:
    whisper_context* ctx_ = nullptr;
    fs::path path_;
};

// ---------------------------------------------------------------------------
// Transcript types
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Transcription functions
// ---------------------------------------------------------------------------

/// Transcribe a WAV file using a pre-loaded whisper model.
/// Prefer this overload when transcribing multiple files to avoid reloading.
TranscriptResult transcribe(WhisperModel& model, const fs::path& audio_path,
                            const std::string& language = "", int threads = 0);

/// Convenience: load the model, transcribe, then free.
/// Equivalent to constructing a temporary WhisperModel and calling the above.
TranscriptResult transcribe(const fs::path& model_path, const fs::path& audio_path,
                            const std::string& language = "", int threads = 0);

} // namespace recmeet
