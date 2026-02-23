// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "transcribe.h"
#include "util.h"

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
/// Run speaker diarization on a WAV file using sherpa-onnx.
/// num_speakers: 0 = auto-detect, >0 = force N clusters.
/// threads: number of CPU threads (0 = use default_thread_count()).
DiarizeResult diarize(const fs::path& audio_path, int num_speakers = 0, int threads = 0,
                      float threshold = 1.18f);
#endif

/// Merge speaker labels into transcript segments by timestamp overlap.
/// For each transcript segment, finds the diarization segment with maximum
/// temporal overlap and prepends "Speaker_XX: " to the text.
std::vector<TranscriptSegment> merge_speakers(
    const std::vector<TranscriptSegment>& transcript,
    const DiarizeResult& diarization);

/// Format a 0-based speaker ID as "Speaker_01", "Speaker_02", etc.
std::string format_speaker(int speaker_id);

} // namespace recmeet
