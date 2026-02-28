// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "vad.h"
#include "log.h"

#if RECMEET_USE_SHERPA
#include "model_manager.h"
#include <sherpa-onnx/c-api/c-api.h>
#endif

namespace recmeet {

#if RECMEET_USE_SHERPA
VadResult detect_speech(const std::vector<float>& samples,
                        const VadConfig& config, int threads) {
    if (samples.empty())
        throw RecmeetError("Cannot run VAD on empty audio");

    fs::path model_path = ensure_vad_model();

    int t = threads > 0 ? threads : default_thread_count();

    SherpaOnnxSileroVadModelConfig silero{};
    silero.model = model_path.c_str();
    silero.threshold = config.threshold;
    silero.min_silence_duration = config.min_silence_duration;
    silero.min_speech_duration = config.min_speech_duration;
    silero.max_speech_duration = config.max_speech_duration;
    silero.window_size = config.window_size;

    SherpaOnnxVadModelConfig vad_cfg{};
    vad_cfg.silero_vad = silero;
    vad_cfg.sample_rate = SAMPLE_RATE;
    vad_cfg.num_threads = t;
    vad_cfg.provider = "cpu";
    vad_cfg.debug = 0;

    const auto* vad = SherpaOnnxCreateVoiceActivityDetector(&vad_cfg, 30.0f);
    if (!vad)
        throw RecmeetError("Failed to create sherpa-onnx VAD");

    // Feed audio in window_size chunks
    int32_t total = static_cast<int32_t>(samples.size());
    int32_t ws = config.window_size;

    for (int32_t offset = 0; offset + ws <= total; offset += ws) {
        SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad, samples.data() + offset, ws);
    }

    // Flush remaining audio
    SherpaOnnxVoiceActivityDetectorFlush(vad);

    // Extract speech segments
    VadResult result;
    result.total_audio_duration = static_cast<double>(total) / SAMPLE_RATE;
    result.total_speech_duration = 0.0;

    while (!SherpaOnnxVoiceActivityDetectorEmpty(vad)) {
        const auto* seg = SherpaOnnxVoiceActivityDetectorFront(vad);
        if (seg) {
            int32_t end_sample = seg->start + seg->n;
            double start_sec = static_cast<double>(seg->start) / SAMPLE_RATE;
            double end_sec = static_cast<double>(end_sample) / SAMPLE_RATE;

            result.segments.push_back({
                seg->start,
                end_sample,
                start_sec,
                end_sec
            });
            result.total_speech_duration += end_sec - start_sec;

            SherpaOnnxDestroySpeechSegment(seg);
        }
        SherpaOnnxVoiceActivityDetectorPop(vad);
    }

    SherpaOnnxDestroyVoiceActivityDetector(vad);

    log_info("VAD: %.1fs speech in %.1fs audio (%zu segments, %.0f%% speech)",
            result.total_speech_duration, result.total_audio_duration,
            result.segments.size(),
            result.total_audio_duration > 0
                ? 100.0 * result.total_speech_duration / result.total_audio_duration
                : 0.0);

    return result;
}
#endif

} // namespace recmeet
