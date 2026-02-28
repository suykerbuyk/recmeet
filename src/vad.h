// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "util.h"

#include <cstdint>
#include <vector>

namespace recmeet {

struct VadSegment {
    int32_t start_sample;  // index in original audio
    int32_t end_sample;
    double start;          // seconds
    double end;
};

struct VadResult {
    std::vector<VadSegment> segments;
    double total_speech_duration;
    double total_audio_duration;
};

struct VadConfig {
    float threshold = 0.5f;
    float min_silence_duration = 0.5f;
    float min_speech_duration = 0.25f;
    float max_speech_duration = 30.0f;
    int window_size = 512;
};

#if RECMEET_USE_SHERPA
/// Detect speech regions in audio samples using Silero VAD via sherpa-onnx.
/// Takes float32 samples at 16kHz. Returns speech segments with sample-accurate boundaries.
/// threads: number of CPU threads (0 = use default_thread_count()).
VadResult detect_speech(const std::vector<float>& samples,
                        const VadConfig& config = {}, int threads = 0);
#endif

} // namespace recmeet
