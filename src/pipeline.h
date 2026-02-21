#pragma once

#include "util.h"
#include "config.h"

#include <functional>
#include <string>

namespace recmeet {

struct PipelineResult {
    fs::path transcript_path;
    fs::path summary_path;   // empty if no summary
    fs::path obsidian_path;  // empty if obsidian disabled
    fs::path output_dir;
};

/// Phase callback — called with phase name: "recording", "transcribing", "summarizing", "complete".
using PhaseCallback = std::function<void(const std::string&)>;

/// Run the full pipeline: record → validate → mix → transcribe → summarize → obsidian output.
PipelineResult run_pipeline(const Config& cfg, StopToken& stop, PhaseCallback on_phase = nullptr);

} // namespace recmeet
