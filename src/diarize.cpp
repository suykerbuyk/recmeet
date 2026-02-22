#include "diarize.h"

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
    const DiarizeResult& diarization) {

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

        out.text = format_speaker(best_speaker) + ": " + seg.text;
        result.push_back(std::move(out));
    }

    return result;
}

#if RECMEET_USE_SHERPA
DiarizeResult diarize(const fs::path& audio_path, int num_speakers, int threads) {
    auto samples = read_wav_float(audio_path);
    if (samples.empty())
        throw RecmeetError("Cannot read audio for diarization: " + audio_path.string());

    auto model_paths = ensure_sherpa_models();

    // Configure segmentation model
    SherpaOnnxOfflineSpeakerSegmentationPyannoteModelConfig pyannote{};
    pyannote.model = model_paths.segmentation.c_str();

    SherpaOnnxOfflineSpeakerSegmentationModelConfig seg_cfg{};
    seg_cfg.pyannote = pyannote;
    int t = threads > 0 ? threads : default_thread_count();
    seg_cfg.num_threads = t;
    seg_cfg.debug = 0;
    seg_cfg.provider = "cpu";

    // Configure embedding extractor
    SherpaOnnxSpeakerEmbeddingExtractorConfig emb_cfg{};
    emb_cfg.model = model_paths.embedding.c_str();
    emb_cfg.num_threads = t;
    emb_cfg.debug = 0;
    emb_cfg.provider = "cpu";

    // Configure clustering
    SherpaOnnxFastClusteringConfig cluster_cfg{};
    cluster_cfg.num_clusters = num_speakers > 0 ? num_speakers : -1;
    cluster_cfg.threshold = 0.5f;

    // Build top-level config
    SherpaOnnxOfflineSpeakerDiarizationConfig config{};
    config.segmentation = seg_cfg;
    config.embedding = emb_cfg;
    config.clustering = cluster_cfg;
    config.min_duration_on = 0.3f;
    config.min_duration_off = 0.5f;

    const auto* sd = SherpaOnnxCreateOfflineSpeakerDiarization(&config);
    if (!sd)
        throw RecmeetError("Failed to create sherpa-onnx speaker diarization");

    fprintf(stderr, "Diarizing %zu samples (%.1fs)...\n",
            samples.size(), samples.size() / 16000.0);

    const auto* raw_result = SherpaOnnxOfflineSpeakerDiarizationProcess(
        sd, samples.data(), static_cast<int32_t>(samples.size()));

    if (!raw_result) {
        SherpaOnnxDestroyOfflineSpeakerDiarization(sd);
        throw RecmeetError("Speaker diarization processing failed");
    }

    // Extract results
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
    SherpaOnnxDestroyOfflineSpeakerDiarization(sd);

    fprintf(stderr, "Diarization complete: %d speakers, %zu segments\n",
            result.num_speakers, result.segments.size());

    return result;
}
#endif

} // namespace recmeet
