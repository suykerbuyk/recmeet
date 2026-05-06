// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "diarize.h"
#include "log.h"

#include <algorithm>
#include <cstdio>

#if RECMEET_USE_SHERPA
#include "audio_file.h"
#include "model_manager.h"
#include <sherpa-onnx/c-api/c-api.h>
#endif

namespace recmeet {

std::string format_speaker(int speaker_id) {
    char buf[16];
    snprintf(buf, sizeof(buf), "Speaker_%02d", speaker_id + 1);
    return buf;
}

std::vector<TranscriptSegment> merge_speakers(
    const std::vector<TranscriptSegment>& transcript,
    const DiarizeResult& diarization,
    const std::map<int, std::string>& speaker_names) {

    std::vector<TranscriptSegment> result;
    result.reserve(transcript.size());

    for (const auto& seg : transcript) {
        TranscriptSegment out = seg;

        // Find diarization segment with maximum temporal overlap
        int best_speaker = 0;
        double best_overlap = 0.0;

        for (const auto& d : diarization.segments) {
            double overlap = std::min(seg.end, d.end) - std::max(seg.start, d.start);
            if (overlap > best_overlap) {
                best_overlap = overlap;
                best_speaker = d.speaker;
            }
        }

        // Use enrolled name if available, otherwise fall back to Speaker_XX
        auto it = speaker_names.find(best_speaker);
        std::string name = (it != speaker_names.end()) ? it->second
                                                       : format_speaker(best_speaker);
        out.text = name + ": " + seg.text;
        result.push_back(std::move(out));
    }

    return result;
}

#if RECMEET_USE_SHERPA
namespace {

// Build a full SherpaOnnxOfflineSpeakerDiarizationConfig. The returned struct
// holds non-owning C-string pointers into `model_paths`; caller must ensure
// `model_paths` outlives any sherpa call that reads the config.
SherpaOnnxOfflineSpeakerDiarizationConfig
make_diarize_config(const SherpaModelPaths& model_paths, int threads,
                    int num_clusters, float threshold) {
    SherpaOnnxOfflineSpeakerSegmentationPyannoteModelConfig pyannote{};
    pyannote.model = model_paths.segmentation.c_str();

    SherpaOnnxOfflineSpeakerSegmentationModelConfig seg_cfg{};
    seg_cfg.pyannote = pyannote;
    int t = std::min(threads > 0 ? threads : default_thread_count(), 4);
    seg_cfg.num_threads = t;
    seg_cfg.debug = 0;
    seg_cfg.provider = "cpu";

    SherpaOnnxSpeakerEmbeddingExtractorConfig emb_cfg{};
    emb_cfg.model = model_paths.embedding.c_str();
    emb_cfg.num_threads = t;
    emb_cfg.debug = 0;
    emb_cfg.provider = "cpu";

    SherpaOnnxFastClusteringConfig cluster_cfg{};
    cluster_cfg.num_clusters = num_clusters;
    cluster_cfg.threshold = threshold;

    SherpaOnnxOfflineSpeakerDiarizationConfig config{};
    config.segmentation = seg_cfg;
    config.embedding = emb_cfg;
    config.clustering = cluster_cfg;
    config.min_duration_on = 0.3f;
    config.min_duration_off = 0.5f;
    return config;
}

} // namespace

DiarizeSession::DiarizeSession(int threads) : threads_(threads) {
    model_paths_ = ensure_sherpa_models();
    // Initial clustering is sentinel auto-detect at the sherpa default
    // threshold; callers must invoke set_clustering() before each Process to
    // pin per-call params. We build with sane defaults so the session is
    // immediately usable for tests that exercise the construction path alone.
    auto config = make_diarize_config(model_paths_, threads_, -1, 1.18f);
    log_debug("DiarizeSession: creating sherpa diarization (threads=%d)", threads_);
    sd_ = SherpaOnnxCreateOfflineSpeakerDiarization(&config);
    if (!sd_)
        throw RecmeetError("Failed to create sherpa-onnx speaker diarization");
}

DiarizeSession::~DiarizeSession() {
    if (sd_) SherpaOnnxDestroyOfflineSpeakerDiarization(sd_);
}

DiarizeSession::DiarizeSession(DiarizeSession&& other) noexcept
    : sd_(other.sd_),
      model_paths_(std::move(other.model_paths_)),
      threads_(other.threads_) {
    other.sd_ = nullptr;
}

DiarizeSession& DiarizeSession::operator=(DiarizeSession&& other) noexcept {
    if (this != &other) {
        if (sd_) SherpaOnnxDestroyOfflineSpeakerDiarization(sd_);
        sd_ = other.sd_;
        model_paths_ = std::move(other.model_paths_);
        threads_ = other.threads_;
        other.sd_ = nullptr;
    }
    return *this;
}

void DiarizeSession::set_clustering(int num_clusters, float threshold) {
    if (!sd_)
        throw RecmeetError("DiarizeSession::set_clustering on moved-from session");
    auto config = make_diarize_config(model_paths_, threads_, num_clusters, threshold);
    SherpaOnnxOfflineSpeakerDiarizationSetConfig(sd_, &config);
    log_debug("DiarizeSession::set_clustering(num_clusters=%d, threshold=%.3f)",
              num_clusters, threshold);
}

DiarizeResult diarize_with_session(DiarizeSession& session,
                                   const float* samples, size_t num_samples,
                                   DiarizeProgressCallback on_progress) {
    log_debug("diarize_with_session: ENTER (samples=%zu)", num_samples);
    if (!samples || num_samples == 0)
        throw RecmeetError("Cannot diarize: empty audio buffer");
    if (!session.handle())
        throw RecmeetError("diarize_with_session: empty (moved-from) session");

    log_info("Diarizing %zu samples (%.1fs)...",
             num_samples, num_samples / 16000.0);

    const SherpaOnnxOfflineSpeakerDiarizationResult* raw_result;
    if (on_progress) {
        auto c_callback = [](int32_t done, int32_t total, void* arg) -> int32_t {
            auto* fn = static_cast<DiarizeProgressCallback*>(arg);
            (*fn)(done, total);
            return 0;
        };
        log_debug("diarize_with_session: calling ProcessWithCallback (this may deadlock)...");
        raw_result = SherpaOnnxOfflineSpeakerDiarizationProcessWithCallback(
            session.handle(), samples, static_cast<int32_t>(num_samples),
            c_callback, &on_progress);
    } else {
        raw_result = SherpaOnnxOfflineSpeakerDiarizationProcess(
            session.handle(), samples, static_cast<int32_t>(num_samples));
    }

    log_debug("diarize_with_session: Process returned (result=%p)", (void*)raw_result);
    if (!raw_result)
        throw RecmeetError("Speaker diarization processing failed");

    DiarizeResult result;
    result.num_speakers = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSpeakers(raw_result);
    int32_t num_segments = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments(raw_result);

    const auto* sorted = SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime(raw_result);
    if (sorted) {
        result.segments.reserve(num_segments);
        for (int32_t i = 0; i < num_segments; ++i) {
            result.segments.push_back({
                static_cast<double>(sorted[i].start),
                static_cast<double>(sorted[i].end),
                sorted[i].speaker
            });
        }
        SherpaOnnxOfflineSpeakerDiarizationDestroySegment(sorted);
    }

    SherpaOnnxOfflineSpeakerDiarizationDestroyResult(raw_result);

    log_debug("diarize_with_session: extracted %zu segments from %d speakers",
              result.segments.size(), result.num_speakers);
    log_info("Diarization complete: %d speakers, %zu segments",
             result.num_speakers, result.segments.size());
    log_debug("diarize_with_session: EXIT (%zu segments)", result.segments.size());

    return result;
}

DiarizeResult diarize(const float* samples, size_t num_samples,
                      int num_speakers, int threads, float threshold,
                      DiarizeProgressCallback on_progress) {
    log_debug("diarize: ENTER (samples=%zu, speakers=%d, threads=%d)",
              num_samples, num_speakers, threads);
    if (!samples || num_samples == 0)
        throw RecmeetError("Cannot diarize: empty audio buffer");
    DiarizeSession session(threads);
    session.set_clustering(num_speakers > 0 ? num_speakers : -1, threshold);
    return diarize_with_session(session, samples, num_samples, std::move(on_progress));
}

DiarizeResult diarize(const fs::path& audio_path, int num_speakers, int threads,
                      float threshold, DiarizeProgressCallback on_progress) {
    auto samples = read_wav_float(audio_path);
    if (samples.empty())
        throw RecmeetError("Cannot read audio for diarization: " + audio_path.string());
    return diarize(samples.data(), samples.size(), num_speakers, threads, threshold,
                   std::move(on_progress));
}
#endif

} // namespace recmeet
