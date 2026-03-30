// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "transcribe.h"
#include "util.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace recmeet {

struct DiarizeSegment {
    double start;  // seconds
    double end;
    int speaker;   // 0-based
};

struct DiarizeResult {
    std::vector<DiarizeSegment> segments;
    int num_speakers = 0;
};

#if RECMEET_USE_SHERPA
/// Progress callback for diarization: (num_processed_chunks, num_total_chunks).
using DiarizeProgressCallback = std::function<void(int, int)>;

/// Run speaker diarization on a pre-loaded audio buffer (16kHz float32 mono).
/// num_speakers: 0 = auto-detect, >0 = force N clusters.
/// threads: number of CPU threads (0 = use default_thread_count()).
/// on_progress: optional callback fired per chunk during embedding extraction.
DiarizeResult diarize(const float* samples, size_t num_samples,
                      int num_speakers = 0, int threads = 0,
                      float threshold = 1.18f,
                      DiarizeProgressCallback on_progress = nullptr);

/// Run speaker diarization on a WAV file using sherpa-onnx.
/// Convenience wrapper — reads the file and delegates to the buffer overload.
DiarizeResult diarize(const fs::path& audio_path, int num_speakers = 0, int threads = 0,
                      float threshold = 1.18f,
                      DiarizeProgressCallback on_progress = nullptr);
#endif

/// Merge speaker labels into transcript segments by timestamp overlap.
/// For each transcript segment, finds the diarization segment with maximum
/// temporal overlap and prepends the speaker name to the text.
/// If speaker_names is provided, uses enrolled names for matching clusters;
/// unmatched clusters fall back to "Speaker_XX" format.
std::vector<TranscriptSegment> merge_speakers(
    const std::vector<TranscriptSegment>& transcript,
    const DiarizeResult& diarization,
    const std::map<int, std::string>& speaker_names = {});

/// Format a 0-based speaker ID as "Speaker_01", "Speaker_02", etc.
std::string format_speaker(int speaker_id);

} // namespace recmeet
