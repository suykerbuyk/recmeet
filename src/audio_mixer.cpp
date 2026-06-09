// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "audio_mixer.h"
#include "util.h"

#include <algorithm>
#include <cmath>

namespace recmeet {

std::vector<int16_t> mix_audio(const std::vector<int16_t>& a,
                                const std::vector<int16_t>& b) {
    size_t len = std::max(a.size(), b.size());
    std::vector<int16_t> out(len);

    for (size_t i = 0; i < len; ++i) {
        int32_t sa = (i < a.size()) ? a[i] : 0;
        int32_t sb = (i < b.size()) ? b[i] : 0;
        // Average the two streams, clamp to int16 range
        int32_t mixed = (sa + sb) / 2;
        out[i] = static_cast<int16_t>(std::clamp(mixed, (int32_t)-32768, (int32_t)32767));
    }
    return out;
}

double validate_min_duration(const std::vector<int16_t>& samples,
                             double min_duration,
                             const std::string& label) {
    double duration = static_cast<double>(samples.size()) / SAMPLE_RATE;
    if (duration < min_duration)
        throw AudioValidationError(label + " too short (" +
                                   std::to_string(duration) + "s).");
    return duration;
}

DualMixResult finalize_dual_mix(std::vector<int16_t> mic,
                                std::vector<int16_t> mon,
                                double min_monitor_duration) {
    // Non-fatal monitor gate (V1 live_recording.cpp:386-397): a monitor that
    // fails the minimum-duration check is dropped and we stage mic-only.
    try {
        validate_min_duration(mon, min_monitor_duration, "Monitor audio");
    } catch (const AudioValidationError&) {
        return DualMixResult{std::move(mic), false};
    }
    return DualMixResult{mix_audio(mic, mon), true};
}

} // namespace recmeet
