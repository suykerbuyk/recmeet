// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "util.h"
#include "config.h"

#include <functional>
#include <string>

namespace recmeet {

struct PipelineResult {
    fs::path note_path;
    fs::path output_dir;
};

/// Input for post-processing phase (output of recording phase).
struct PostprocessInput {
    fs::path out_dir;
    fs::path audio_path;
    std::string transcript_text;  ///< If empty, postprocessing will transcribe from audio_path.
};

/// Phase callback — called with phase name: "recording", "transcribing", "diarizing", "summarizing", "complete".
using PhaseCallback = std::function<void(const std::string&)>;

/// Read entire file as string. Returns empty if missing or unreadable.
std::string read_context_file(const fs::path& path);

/// Record audio. Phase: "recording". For --reprocess, resolves paths only.
PostprocessInput run_recording(const Config& cfg, StopToken& stop,
                               PhaseCallback on_phase = nullptr);

/// Transcribe + diarize + summarize + note. Runs to completion (no StopToken).
/// Phases: "transcribing", "diarizing", "summarizing", "complete".
PipelineResult run_postprocessing(const Config& cfg, const PostprocessInput& input,
                                  PhaseCallback on_phase = nullptr);

/// Run the full pipeline: record → validate → mix → transcribe → summarize → note output.
PipelineResult run_pipeline(const Config& cfg, StopToken& stop, PhaseCallback on_phase = nullptr);

} // namespace recmeet
