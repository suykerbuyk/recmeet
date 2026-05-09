// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "util.h"
#include "config.h"
#include "caption_engine.h"  // CaptionResult / CaptionDegradedReason callback typedefs

#include <functional>
#include <string>
#include <vector>

namespace recmeet {

/// Hooks used by the daemon to broadcast `caption` and `caption.degraded`
/// IPC events out of the recording worker. When `cfg.captions_enabled` is
/// true and the build supports streaming captions (RECMEET_USE_SHERPA=ON),
/// `run_recording` instantiates a CaptionEngine and forwards its emissions
/// through these hooks. When the build is sherpa-OFF or the engine fails to
/// start, `on_engine_error` (if set) is invoked once with a one-shot
/// "captions degraded" reason and recording continues unaffected.
///
/// All hooks may run on the engine's worker thread; the daemon is expected
/// to marshal them onto the IPC poll thread (e.g. via IpcServer::post()).
struct CaptionHooks {
    CaptionResultCallback   on_result   = nullptr;
    void*                   result_ud   = nullptr;
    CaptionDegradedCallback on_degraded = nullptr;
    void*                   degraded_ud = nullptr;

    /// Optional one-shot signal that the engine itself failed to come up
    /// (no-sherpa stub, missing model, etc.). Distinct from `on_degraded`
    /// because it never fires from the engine's worker — the daemon
    /// invokes it inline from the recording worker before the recording
    /// loop begins.
    using EngineErrorCallback = void(*)(const std::string& message, void* userdata);
    EngineErrorCallback     on_engine_error = nullptr;
    void*                   engine_error_ud = nullptr;
};

struct PipelineResult {
    fs::path note_path;
    fs::path output_dir;
    std::string transcript_text;  ///< Raw timestamped transcript (empty if transcription skipped).
};

/// Input for post-processing phase (output of recording phase).
struct PostprocessInput {
    fs::path out_dir;
    fs::path audio_path;
    std::string transcript_text;  ///< If empty, postprocessing will transcribe from audio_path.
    std::string timestamp;  ///< YYYY-MM-DD_HH-MM form. Empty if dir/audio doesn't match the canonical pattern.
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

/// Save inline context as context_<timestamp>.json in the meeting output directory.
/// If timestamp is empty, falls back to the legacy filename `context.json`.
void save_meeting_context(const fs::path& out_dir, const std::string& context_inline,
                          const fs::path& context_file = {},
                          const std::string& timestamp = "");

/// Load context string from context.json in a meeting directory.
/// Returns empty if file doesn't exist.
std::string load_meeting_context(const fs::path& out_dir);

/// Record audio. Phase: "recording". For --reprocess, resolves paths only.
///
/// `caption_hooks` (Phase 3) is consulted only when `cfg.captions_enabled`
/// is true and a real recording is being performed (i.e. not reprocess).
/// Pass nullptr (default) for the existing pre-Phase-3 behaviour.
PostprocessInput run_recording(const Config& cfg, StopToken& stop,
                               PhaseCallback on_phase = nullptr,
                               const CaptionHooks* caption_hooks = nullptr);

/// Resolve a streaming caption model directory. If `name` is non-empty it
/// names a subdir under `~/.local/share/recmeet/models/sherpa/online/`;
/// otherwise falls back to "en-2023-06-26" (the Phase-0 pinned default).
/// Returns the absolute path; existence is NOT checked here — the engine's
/// `start()` resolves files inside and reports a clear error if missing.
fs::path resolve_caption_model_dir(const std::string& name);

/// Transcribe + diarize + summarize + note.
/// Phases: "transcribing", "diarizing", "summarizing", "complete".
PipelineResult run_postprocessing(const Config& cfg, const PostprocessInput& input,
                                  PhaseCallback on_phase = nullptr,
                                  ProgressCallback on_progress = nullptr,
                                  StopToken* stop = nullptr);

/// Run the full pipeline: record → validate → mix → transcribe → summarize → note output.
PipelineResult run_pipeline(const Config& cfg, StopToken& stop, PhaseCallback on_phase = nullptr);

} // namespace recmeet
