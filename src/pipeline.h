// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "util.h"
#include "config.h"

#include <functional>
#include <string>
#include <vector>

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

/// Progress callback — called with (phase_name, percent 0-100).
using ProgressCallback = std::function<void(const std::string&, int)>;

/// Compute weighted progress across VAD segments (for testing).
int vad_weighted_progress(size_t seg_index, int seg_percent, const std::vector<size_t>& seg_samples);

/// Build whisper initial_prompt from speaker names and vocabulary hints.
std::string build_initial_prompt(const std::vector<std::string>& speaker_names,
                                 const std::string& vocabulary);

/// Read entire file as string. Returns empty if missing or unreadable.
std::string read_context_file(const fs::path& path);

/// Save inline context as context.json in the meeting output directory.
void save_meeting_context(const fs::path& out_dir, const std::string& context_inline,
                          const fs::path& context_file = {});

/// Load context string from context.json in a meeting directory.
/// Returns empty if file doesn't exist.
std::string load_meeting_context(const fs::path& out_dir);

/// Record audio. Phase: "recording". For --reprocess, resolves paths only.
PostprocessInput run_recording(const Config& cfg, StopToken& stop,
                               PhaseCallback on_phase = nullptr);

/// Transcribe + diarize + summarize + note.
/// Phases: "transcribing", "diarizing", "summarizing", "complete".
PipelineResult run_postprocessing(const Config& cfg, const PostprocessInput& input,
                                  PhaseCallback on_phase = nullptr,
                                  ProgressCallback on_progress = nullptr,
                                  StopToken* stop = nullptr);

/// Run the full pipeline: record → validate → mix → transcribe → summarize → note output.
PipelineResult run_pipeline(const Config& cfg, StopToken& stop, PhaseCallback on_phase = nullptr);

} // namespace recmeet
