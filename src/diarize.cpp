// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "diarize.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>

#if RECMEET_USE_SHERPA
#include "audio_file.h"
#include "model_manager.h"
#include "speaker_id.h"
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

// ---------------------------------------------------------------------------
// T2.1 — chunked diarization with stitching
// ---------------------------------------------------------------------------
//
// Stitching algorithm pinned by plan rev 7 (`postprocess-memory-containment.md`,
// section "Stitching algorithm"). Centroids are stored **raw** (non-L2-
// normalized) end-to-end so that:
//   - Persisted MeetingSpeaker.embedding stays byte-format-compatible with
//     the legacy single-call path (H1, line 16).
//   - Cosine similarity is computed transiently in step 5 and step 8 by
//     L2-normalizing both operands at comparison time only (line 397, 409).
//
// Step 7 emits each owned segment at its full global extent (no trim-to-core)
// so sherpa's per-chunk VAD jitter cannot drop boundary speech (M-1', line
// 405). Step 8's greedy-merge ends with a deterministic ascending-current-ID
// compaction so global IDs are 0..N-1 contiguous (M-2', line 421).

namespace {

constexpr float kStitchEps = 1e-9f;

float l2_norm(const std::vector<float>& v) {
    double sum_sq = 0.0;
    for (float x : v) sum_sq += static_cast<double>(x) * static_cast<double>(x);
    return static_cast<float>(std::sqrt(sum_sq));
}

float cosine_sim_raw(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    double dot = 0.0;
    double norm_a_sq = 0.0;
    double norm_b_sq = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        double ai = static_cast<double>(a[i]);
        double bi = static_cast<double>(b[i]);
        dot += ai * bi;
        norm_a_sq += ai * ai;
        norm_b_sq += bi * bi;
    }
    double denom = std::sqrt(norm_a_sq) * std::sqrt(norm_b_sq);
    if (denom < static_cast<double>(kStitchEps)) return 0.0f;
    return static_cast<float>(dot / denom);
}

// Sample-weighted mean of raw vectors. count_a + count_b > 0 by construction.
std::vector<float> weighted_mean_raw(
    const std::vector<float>& a, long count_a,
    const std::vector<float>& b, long count_b) {
    std::vector<float> out(a.size(), 0.0f);
    double total = static_cast<double>(count_a + count_b);
    if (total <= 0.0) return out;
    for (size_t i = 0; i < a.size(); ++i) {
        double na = static_cast<double>(a[i]) * static_cast<double>(count_a);
        double nb = static_cast<double>(b[i]) * static_cast<double>(count_b);
        out[i] = static_cast<float>((na + nb) / total);
    }
    return out;
}

struct GlobalEntry {
    int id;                       // current global ID (post-merge, pre-compaction)
    std::vector<float> raw;       // raw running-mean centroid
    long sample_count;            // total samples contributed to this centroid
};

// Returns a list of unique chunk-local speaker IDs in ascending order.
std::vector<int> unique_local_ids(const DiarizeResult& chunk) {
    std::set<int> uniq;
    for (const auto& s : chunk.segments) uniq.insert(s.speaker);
    return std::vector<int>(uniq.begin(), uniq.end());
}

// Sum of segment durations (samples) for a chunk-local speaker.
long chunk_local_sample_count(const DiarizeResult& chunk, int local_sid) {
    double dur_sec = 0.0;
    for (const auto& s : chunk.segments) {
        if (s.speaker != local_sid) continue;
        if (s.end > s.start) dur_sec += (s.end - s.start);
    }
    long n = static_cast<long>(dur_sec * static_cast<double>(SAMPLE_RATE));
    return n > 0 ? n : 1;  // guarantee positive weight even for zero-length seg
}

} // namespace

DiarizeChunkedResult stitch_chunks(
    const std::vector<DiarizeResult>& chunk_results,
    const std::vector<std::map<int, std::vector<float>>>& chunk_centroids,
    const std::vector<ChunkExtents>& extents,
    const DiarizeChunkConfig& cfg,
    int num_speakers) {

    if (chunk_results.size() != chunk_centroids.size()
        || chunk_results.size() != extents.size())
        throw RecmeetError("stitch_chunks: chunk_results / chunk_centroids / "
                           "extents size mismatch");

    DiarizeChunkedResult out;

    // Global registry: ordered by current ID. Entries hold raw centroids and
    // their accumulated sample counts. id_seq increments for each new speaker
    // discovered; merging reuses the smaller pre-merge ID per H3 (line 17) /
    // step 8 (line 415).
    std::vector<GlobalEntry> globals;
    int id_seq = 0;

    // Per-chunk local-id -> global-id map, captured during step 5 so that
    // step 7 can rewrite emitted segments without recomputing the match.
    std::vector<std::map<int, int>> local_to_global(chunk_results.size());

    // Step 3-5-6: assign each chunk's local centroids to globals and update
    // running mean.
    for (size_t i = 0; i < chunk_results.size(); ++i) {
        const auto& centroids_i = chunk_centroids[i];

        // Iterate local IDs in ascending order for determinism.
        std::vector<int> local_ids;
        local_ids.reserve(centroids_i.size());
        for (const auto& kv : centroids_i) local_ids.push_back(kv.first);
        std::sort(local_ids.begin(), local_ids.end());

        for (int local_sid : local_ids) {
            const auto& local_raw = centroids_i.at(local_sid);
            if (local_raw.empty()) continue;  // skip degenerate centroid

            long local_count = chunk_local_sample_count(chunk_results[i], local_sid);

            // Step 5: nearest global by cosine similarity. Scan in ascending
            // global-ID order; ties resolve to the smaller ID.
            int best_id = -1;
            float best_sim = -1.0f;
            for (const auto& g : globals) {
                float sim = cosine_sim_raw(local_raw, g.raw);
                if (sim > best_sim) {
                    best_sim = sim;
                    best_id = g.id;
                }
            }

            if (best_id >= 0 && best_sim >= cfg.stitch_threshold) {
                // Step 6: weighted-mean update of matched global.
                for (auto& g : globals) {
                    if (g.id != best_id) continue;
                    g.raw = weighted_mean_raw(g.raw, g.sample_count,
                                              local_raw, local_count);
                    g.sample_count += local_count;
                    break;
                }
                local_to_global[i][local_sid] = best_id;
            } else {
                GlobalEntry ge;
                ge.id = id_seq++;
                ge.raw = local_raw;
                ge.sample_count = local_count;
                globals.push_back(ge);
                local_to_global[i][local_sid] = ge.id;
            }
        }
    }

    // Step 7: rewrite segment timestamps to global coords + filter by core
    // ownership. Midpoint-in-core emits at full extent (M-1' line 405);
    // boundary midpoints owned by lower-indexed chunk (`<` upper bound).
    for (size_t i = 0; i < chunk_results.size(); ++i) {
        const auto& chunk = chunk_results[i];
        const auto& ext = extents[i];
        const auto& l2g = local_to_global[i];

        for (const auto& seg : chunk.segments) {
            double global_start = ext.offset_sec + seg.start;
            double global_end   = ext.offset_sec + seg.end;
            double global_mid   = 0.5 * (global_start + global_end);

            // Half-open interval: [core_start, core_end). Mid exactly on the
            // upper boundary belongs to the next chunk (or no chunk if last).
            bool in_core =
                global_mid >= ext.core_start_sec && global_mid <  ext.core_end_sec;
            if (!in_core) continue;

            int gid;
            auto it = l2g.find(seg.speaker);
            if (it == l2g.end()) {
                // Defensive: a chunk segment whose local speaker has no
                // centroid (empty embedding). Skip rather than guess.
                continue;
            }
            gid = it->second;

            DiarizeSegment out_seg;
            out_seg.start = global_start;
            out_seg.end   = global_end;
            out_seg.speaker = gid;
            out.diar.segments.push_back(out_seg);
        }
    }

    // Step 8: post-stitch greedy-merge enforcing `num_speakers` count limit.
    // Merge pair with highest pairwise cosine similarity; tie-break to the
    // smaller min(A.id, B.id). Sample-weighted mean of raw vectors. Rewrite
    // already-emitted segments. After the loop: ascending-current-ID
    // compaction to 0..N-1 (M-2' line 421).
    if (num_speakers > 0) {
        while (static_cast<int>(globals.size()) > num_speakers) {
            int best_a_idx = -1, best_b_idx = -1;
            float best_sim = -2.0f;
            int best_min_id = std::numeric_limits<int>::max();

            for (size_t a = 0; a < globals.size(); ++a) {
                for (size_t b = a + 1; b < globals.size(); ++b) {
                    float sim = cosine_sim_raw(globals[a].raw, globals[b].raw);
                    int min_id = std::min(globals[a].id, globals[b].id);
                    bool take = false;
                    if (sim > best_sim) {
                        take = true;
                    } else if (sim == best_sim && min_id < best_min_id) {
                        take = true;
                    }
                    if (take) {
                        best_sim = sim;
                        best_min_id = min_id;
                        best_a_idx = static_cast<int>(a);
                        best_b_idx = static_cast<int>(b);
                    }
                }
            }
            if (best_a_idx < 0 || best_b_idx < 0) break;

            int id_a = globals[best_a_idx].id;
            int id_b = globals[best_b_idx].id;
            int merged_id = std::min(id_a, id_b);
            int dropped_id = std::max(id_a, id_b);

            std::vector<float> merged_raw = weighted_mean_raw(
                globals[best_a_idx].raw, globals[best_a_idx].sample_count,
                globals[best_b_idx].raw, globals[best_b_idx].sample_count);
            long merged_count = globals[best_a_idx].sample_count
                              + globals[best_b_idx].sample_count;

            // Replace the entry whose id == merged_id; erase the dropped one.
            int keep_idx = (id_a == merged_id) ? best_a_idx : best_b_idx;
            int drop_idx = (id_a == merged_id) ? best_b_idx : best_a_idx;
            globals[keep_idx].raw = std::move(merged_raw);
            globals[keep_idx].sample_count = merged_count;
            // Erase higher-index first to keep the other index valid.
            if (drop_idx > keep_idx) {
                globals.erase(globals.begin() + drop_idx);
            } else {
                globals.erase(globals.begin() + drop_idx);
            }

            // Rewrite emitted segments: dropped_id -> merged_id.
            for (auto& seg : out.diar.segments) {
                if (seg.speaker == dropped_id) seg.speaker = merged_id;
            }
        }
    }

    // Step 8 finale: deterministic ID compaction to 0..N-1 contiguous in
    // ascending current-ID order.
    std::vector<GlobalEntry> sorted_globals = globals;
    std::sort(sorted_globals.begin(), sorted_globals.end(),
              [](const GlobalEntry& x, const GlobalEntry& y) {
                  return x.id < y.id;
              });
    std::map<int, int> remap;
    for (size_t k = 0; k < sorted_globals.size(); ++k) {
        remap[sorted_globals[k].id] = static_cast<int>(k);
    }
    for (auto& seg : out.diar.segments) {
        auto it = remap.find(seg.speaker);
        if (it != remap.end()) seg.speaker = it->second;
    }
    for (size_t k = 0; k < sorted_globals.size(); ++k) {
        out.centroids[static_cast<int>(k)] = std::move(sorted_globals[k].raw);
    }
    out.diar.num_speakers = static_cast<int>(sorted_globals.size());

    // Re-sort emitted segments by start time so downstream merge_speakers and
    // any time-ordered consumers see a clean sequence.
    std::stable_sort(out.diar.segments.begin(), out.diar.segments.end(),
                     [](const DiarizeSegment& x, const DiarizeSegment& y) {
                         return x.start < y.start;
                     });

    return out;
}

DiarizeChunkedResult diarize_chunked(
    const float* samples, size_t num_samples,
    int num_speakers, int threads, float threshold,
    const DiarizeChunkConfig& chunk_cfg,
    DiarizeProgressCallback on_progress) {

    if (!samples || num_samples == 0)
        throw RecmeetError("diarize_chunked: empty audio buffer");

    // M-5' validation: positive spacing with ≥ 60 s of core.
    const float chunk_sec = chunk_cfg.chunk_minutes * 60.0f;
    if (chunk_sec <= chunk_cfg.overlap_seconds + 60.0f) {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
            "diarize_chunked: invalid chunk config — chunk %.2f s must exceed "
            "overlap %.2f s + 60 s minimum core (got chunk_minutes=%.3f, "
            "overlap_seconds=%.3f)",
            chunk_sec, chunk_cfg.overlap_seconds,
            chunk_cfg.chunk_minutes, chunk_cfg.overlap_seconds);
        throw RecmeetError(msg);
    }

    const double total_seconds = static_cast<double>(num_samples)
                                / static_cast<double>(SAMPLE_RATE);
    const double spacing_sec = static_cast<double>(chunk_sec)
                             - static_cast<double>(chunk_cfg.overlap_seconds);
    // We've validated chunk_sec > overlap + 60, so spacing_sec > 60 > 0.

    const double half_overlap = static_cast<double>(chunk_cfg.overlap_seconds) * 0.5;

    int N = 1;
    if (total_seconds > static_cast<double>(chunk_cfg.overlap_seconds)) {
        double n_d = std::ceil(
            (total_seconds - static_cast<double>(chunk_cfg.overlap_seconds))
            / spacing_sec);
        N = static_cast<int>(n_d);
        if (N < 1) N = 1;
    }

    // Build chunk extents (L-1' — non-owning views into the caller's PCM).
    std::vector<ChunkExtents> extents;
    extents.reserve(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        ChunkExtents ext;
        double pcm_start_sec = std::max(0.0,
            static_cast<double>(i) * spacing_sec - half_overlap);
        double pcm_end_sec = std::min(total_seconds,
            static_cast<double>(i + 1) * spacing_sec + half_overlap);
        if (pcm_end_sec <= pcm_start_sec) continue;

        ext.pcm_start_samples = static_cast<size_t>(
            pcm_start_sec * static_cast<double>(SAMPLE_RATE));
        ext.pcm_end_samples = static_cast<size_t>(
            pcm_end_sec * static_cast<double>(SAMPLE_RATE));
        if (ext.pcm_end_samples > num_samples) ext.pcm_end_samples = num_samples;
        if (ext.pcm_start_samples >= ext.pcm_end_samples) continue;

        ext.core_start_sec = (i == 0) ? 0.0 : (static_cast<double>(i) * spacing_sec);
        ext.core_end_sec = (i == N - 1) ? total_seconds
                                        : (static_cast<double>(i + 1) * spacing_sec);
        ext.offset_sec = static_cast<double>(ext.pcm_start_samples)
                       / static_cast<double>(SAMPLE_RATE);
        extents.push_back(ext);
    }

    if (extents.empty())
        throw RecmeetError("diarize_chunked: failed to build any chunk extents");

    log_info("diarize_chunked: %zu chunks, total=%.1fs, spacing=%.1fs, overlap=%.1fs",
             extents.size(), total_seconds, spacing_sec,
             static_cast<double>(chunk_cfg.overlap_seconds));

    // Single shared sessions reused across chunks (T2.0).
    DiarizeSession diar_session(threads);
    auto model_paths = ensure_sherpa_models();
    SpeakerEmbeddingSession emb_session(model_paths.embedding, threads);

    std::vector<DiarizeResult> chunk_results;
    chunk_results.reserve(extents.size());
    std::vector<std::map<int, std::vector<float>>> chunk_centroids;
    chunk_centroids.reserve(extents.size());

    // Track total embedding extractions for progress denominator. We don't
    // know the per-chunk speaker count up front so denominator is rough but
    // monotone; granularity matters more than absolute accuracy here.
    int extractions_done = 0;
    int extractions_estimate = std::max<int>(1, static_cast<int>(extents.size()) * 2);

    for (size_t i = 0; i < extents.size(); ++i) {
        const auto& ext = extents[i];

        // Per-chunk -1 (auto-detect) per Q1/C1 resolution (line 361).
        diar_session.set_clustering(-1, threshold);

        // L-1': non-owning view into the caller's buffer.
        const float* chunk_pcm = samples + ext.pcm_start_samples;
        size_t chunk_n = ext.pcm_end_samples - ext.pcm_start_samples;

        DiarizeResult cr = diarize_with_session(diar_session, chunk_pcm, chunk_n,
                                                /*progress*/nullptr);

        // Step 3c-d: extract one raw centroid per chunk-local speaker.
        std::map<int, std::vector<float>> centroids_i;
        for (int local_sid : unique_local_ids(cr)) {
            // Step 3e: emit a sub-chunk progress tick so the watchdog sees
            // activity through the long extraction phase.
            if (on_progress) {
                int overall = static_cast<int>(
                    100.0 * (static_cast<double>(extractions_done)
                             / static_cast<double>(extractions_estimate)));
                if (overall < 0) overall = 0;
                if (overall > 99) overall = 99;
                on_progress(overall, 100);
            }

            std::vector<float> raw = extract_speaker_embedding(
                emb_session, chunk_pcm, chunk_n, cr, local_sid);
            ++extractions_done;
            if (extractions_done > extractions_estimate)
                extractions_estimate = extractions_done + 1;

            if (!raw.empty()) centroids_i[local_sid] = std::move(raw);
        }

        chunk_results.push_back(std::move(cr));
        chunk_centroids.push_back(std::move(centroids_i));
    }

    // Final progress tick at 100 %.
    if (on_progress) on_progress(100, 100);

    return stitch_chunks(chunk_results, chunk_centroids, extents,
                         chunk_cfg, num_speakers);
}
#endif

} // namespace recmeet
