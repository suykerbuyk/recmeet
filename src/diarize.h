// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "transcribe.h"
#include "util.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

#if RECMEET_USE_SHERPA
#include "model_manager.h"
struct SherpaOnnxOfflineSpeakerDiarization;
#endif

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

/// RAII wrapper around `SherpaOnnxOfflineSpeakerDiarization`. Loads the
/// pyannote segmentation and embedding models on construction and keeps them
/// loaded across multiple diarize calls — `set_clustering()` mutates only the
/// cheap clustering parameters (rebuilds the FastClustering object only).
/// Required by chunked-diarization (T2.1) to avoid reloading ~45 MB of models
/// on every chunk; usable directly by callers that need session reuse.
class DiarizeSession {
public:
    explicit DiarizeSession(int threads = 0);
    ~DiarizeSession();

    DiarizeSession(const DiarizeSession&) = delete;
    DiarizeSession& operator=(const DiarizeSession&) = delete;
    DiarizeSession(DiarizeSession&& other) noexcept;
    DiarizeSession& operator=(DiarizeSession&& other) noexcept;

    /// Update the clustering parameters used by subsequent diarize calls.
    /// num_clusters: -1 = auto-detect; >0 = force N clusters.
    /// threshold: clustering distance threshold (sherpa default 1.18).
    /// Cheap — only the FastClustering object is rebuilt; embedding and
    /// segmentation models stay loaded.
    void set_clustering(int num_clusters, float threshold);

    /// Opaque sherpa handle used by `diarize_with_session`. Returns nullptr
    /// after a move-from.
    const SherpaOnnxOfflineSpeakerDiarization* handle() const noexcept { return sd_; }

private:
    const SherpaOnnxOfflineSpeakerDiarization* sd_ = nullptr;
    SherpaModelPaths model_paths_;
    int threads_ = 0;
};

/// Run speaker diarization on a pre-loaded audio buffer (16kHz float32 mono)
/// using a pre-built session. Caller must invoke `session.set_clustering()`
/// at least once before this call to set the desired num_clusters/threshold.
DiarizeResult diarize_with_session(DiarizeSession& session,
                                   const float* samples, size_t num_samples,
                                   DiarizeProgressCallback on_progress = nullptr);

// ---------------------------------------------------------------------------
// T2.1 — chunked diarization with stitching
// ---------------------------------------------------------------------------

/// Configuration for chunked diarization. Defaults sized to keep each chunk's
/// peak working set well under the iter-110 ~10 GB single-call boundary while
/// still giving each chunk enough audio to produce well-separated clusters.
struct DiarizeChunkConfig {
    float chunk_minutes = 15.0f;
    float overlap_seconds = 30.0f;
    /// Cosine-similarity floor for stitching chunk-local centroids into the
    /// global registry. Matches the SherpaOnnxSpeakerEmbeddingManager metric
    /// (Q4 spike): 0.6 on L2-normalized unit vectors.
    float stitch_threshold = 0.6f;
    /// Cosine-similarity floor for the post-stitch unified greedy-merge loop
    /// (`apply_collapse`, Phase B.1/B.3). Pairs at or above this similarity
    /// are auto-merged regardless of the ceiling; pairs below this similarity
    /// are merged ONLY when the ceiling (`target_speakers`) is already
    /// exceeded. Phase A.2 chose 0.55 empirically (M-4 floor).
    float collapse_threshold = 0.55f;

    /// Phase A instrumentation: when non-empty, `stitch_chunks` writes a JSON
    /// artifact at the end of stitching containing all global centroids, the
    /// full pairwise cosine-similarity matrix, the per-chunk local→global
    /// map, and sample-count weights. Auto-suffixed with `meeting_timestamp`
    /// (M-1). Reusable across this bug class; the empty-string check is
    /// negligible on the hot path.
    std::string debug_dump_centroids_path;
    /// Used to disambiguate concurrent reprocesses (M-1). Inserted into the
    /// dump filename before the extension, e.g. PATH = `/tmp/x.json` and
    /// meeting_timestamp = `2026-05-18_09-36` writes `/tmp/x_2026-05-18_09-36.json`.
    /// Empty timestamp = no suffix (used by the dump-format unit test).
    std::string meeting_timestamp;
};


/// Result of chunked diarization. `centroids` maps each global speaker ID
/// (0..N-1 contiguous after compaction) to the **raw** (non-L2-normalized)
/// running-mean embedding for that speaker. The chunked pipeline feeds this
/// directly into `identify_speakers_with_centroids` to skip a second
/// extractor pass over the audio.
struct DiarizeChunkedResult {
    DiarizeResult diar;
    std::map<int, std::vector<float>> centroids;
};

/// Per-chunk PCM extents and core regions. Pinned exactly per the plan's
/// "Stitching algorithm" step 1. Cores tile [0, total_seconds] with no gaps;
/// PCM extends overlap_seconds/2 past each side of the core (clipped at audio
/// boundaries for first/last chunk). The core determines which segments the
/// chunk owns at emission time; PCM regions outside the core inform stitching
/// only. Exposed in the public header so the stitch helper is unit-testable
/// with handcrafted DiarizeResult inputs (rev 7 L-4').
struct ChunkExtents {
    size_t pcm_start_samples;   // inclusive
    size_t pcm_end_samples;     // exclusive
    double core_start_sec;
    double core_end_sec;
    double offset_sec;          // == pcm_start_samples / SAMPLE_RATE
};

/// Stitch a sequence of per-chunk DiarizeResult objects into one global
/// DiarizeChunkedResult. Each `chunk_results[i]` carries chunk-local segment
/// times (relative to the chunk PCM start) and chunk-local speaker IDs.
/// `chunk_centroids[i]` maps each chunk-local speaker ID present in
/// `chunk_results[i].segments` to its raw (non-normalized) embedding vector.
/// `extents[i]` gives the chunk's PCM/core layout in global coordinates.
///
/// Returns a DiarizeChunkedResult whose segment timestamps are in **global**
/// coordinates and whose speaker IDs and centroid keys are the **compacted**
/// global IDs (0..N-1 contiguous). Implements:
///   - on-demand cosine matching against the global registry (raw vectors,
///     L2-normalized transiently for the dot product);
///   - sample-weighted running-mean update on raw centroids;
///   - midpoint-in-core ownership with full-extent emit (no trim);
///   - post-stitch unified greedy-merge (`apply_collapse`) enforcing both
///     `target_speakers` as a hard cap and `cfg.collapse_threshold` as the
///     auto-merge quality floor;
///   - deterministic ascending-current-ID compaction.
///
/// Sample counts are derived from chunk-local segment durations in samples
/// (so two chunks contributing the same speaker for 5 s and 10 s produce a
/// 10-weighted-vs-5-weighted running mean). Exposed for unit tests; the
/// in-process orchestrator `diarize_chunked` calls this same helper.
///
/// `target_speakers`: hard cap on the global-speaker count for the merge
/// loop. Phase B semantics (rename from `num_speakers` to surface the shift):
/// the unified greedy-merge loop in `apply_collapse` ALWAYS runs, with
/// `target_speakers` as the ceiling. `target_speakers == 0` (legacy
/// "auto-detect, no cap") is honored by treating it as "no ceiling":
/// merges only happen when the threshold gate fires. Callers wanting the
/// pre-Phase-B "no collapse" behavior should pass 0; callers wanting a
/// hard cap should pass the resolved target (Phase B.2 precedence chain).
DiarizeChunkedResult stitch_chunks(
    const std::vector<DiarizeResult>& chunk_results,
    const std::vector<std::map<int, std::vector<float>>>& chunk_centroids,
    const std::vector<ChunkExtents>& extents,
    const DiarizeChunkConfig& cfg,
    int target_speakers);

/// Run chunked diarization on a pre-loaded audio buffer (16kHz float32 mono).
/// Slices the audio into overlapping chunks of `cfg.chunk_minutes` width with
/// `cfg.overlap_seconds` of shared audio between adjacent chunks, runs one
/// `diarize_with_session` pass per chunk (single shared DiarizeSession +
/// SpeakerEmbeddingSession), then stitches per-chunk speaker IDs into a
/// global registry via `stitch_chunks`.
///
/// `target_speakers`: hard cap on the global-speaker count enforced by the
/// unified `apply_collapse` loop. 0 = no ceiling (auto-detect, post-stitch
/// merges only happen when `cfg.collapse_threshold` fires). >0 = the unified
/// loop force-merges past the floor until count <= target. Renamed from
/// `num_speakers` (Phase B.4) to flag the semantic shift from "0 = skip
/// merge, else cap" to "always-on unified loop, target = hard cap".
/// `threshold`: per-chunk clustering threshold (sherpa default 1.18); distinct
/// from `cfg.stitch_threshold` and from `cfg.collapse_threshold`.
///
/// Throws `RecmeetError` if `cfg.chunk_minutes * 60 <= cfg.overlap_seconds + 60`
/// (M-5'). Emits one `on_progress("diarizing", overall_pct)` call per
/// `extract_speaker_embedding` invocation (M-3'/L-4' progress granularity).
DiarizeChunkedResult diarize_chunked(
    const float* samples, size_t num_samples,
    int target_speakers, int threads, float threshold,
    const DiarizeChunkConfig& chunk_cfg,
    DiarizeProgressCallback on_progress = nullptr);

/// Run speaker diarization on a pre-loaded audio buffer (16kHz float32 mono).
/// `num_speakers` here is forwarded to sherpa's clustering: 0 = auto-detect,
/// >0 = force N clusters via `set_clustering(num_speakers)`. **Distinct
/// semantics from `diarize_chunked`'s `target_speakers`** — that parameter
/// controls our post-stitch unified merge loop; this one is the sherpa-side
/// FastClustering knob. Renaming both to the same name would falsely imply
/// they're interchangeable; the parameter on this entry point stays
/// `num_speakers` (Phase B.4 design note).
/// threads: number of CPU threads (0 = use default_thread_count()).
/// on_progress: optional callback fired per chunk during embedding extraction.
DiarizeResult diarize(const float* samples, size_t num_samples,
                      int num_speakers = 0, int threads = 0,
                      float threshold = 1.18f,
                      DiarizeProgressCallback on_progress = nullptr);

/// Run speaker diarization on a WAV file using sherpa-onnx.
/// Convenience wrapper — reads the file and delegates to the buffer overload.
/// `num_speakers` is sherpa's FastClustering knob (see buffer overload).
DiarizeResult diarize(const fs::path& audio_path, int num_speakers = 0, int threads = 0,
                      float threshold = 1.18f,
                      DiarizeProgressCallback on_progress = nullptr);

/// Phase B.3 — shared post-stitch collapse helper.
///
/// Owns the unified greedy-merge loop (B.1) plus the deterministic
/// `0..N-1` compaction (former Step-8 finale). Both code paths (long-audio
/// `stitch_chunks` and short-audio post-`diarize()`) call this single helper
/// so the segment-rewrite-and-compaction sequence has one owner.
///
/// Inputs:
///   - `diar_inout.segments[i].speaker` will be rewritten in place by the
///     accumulated merge map + the final 0..N-1 compaction.
///   - `globals` is the chunked-stitch registry (or, for short-audio, a
///     synthetic-globals view of `diar_inout`'s clusters). Merged in place
///     by the unified loop; the post-loop survivors define the compaction
///     order (ascending current-ID).
///   - `target_speakers`: hard cap on final speaker count. 0 = no ceiling.
///   - `collapse_threshold`: cosine-similarity floor for auto-merges
///     (Phase A.2 chose 0.55f).
///
/// Returns: `CollapseResult{centroids, num_speakers}`. The `centroids` map
/// is keyed by the new contiguous `0..N-1` IDs. Caller assigns this back
/// to its own centroid storage (`DiarizeChunkedResult::centroids`, etc.).
///
/// Termination (per loop iteration):
///   - `ceiling_hit  = (target_speakers > 0 && globals.size() > target_speakers)`
///   - `threshold_ok = (best_sim >= collapse_threshold)`
///   - `if (!ceiling_hit && !threshold_ok) break;`
///
/// When `ceiling_hit` is true the merge proceeds regardless of `best_sim` —
/// the cap is the hard mechanism that forces a count. The threshold sets the
/// floor on auto-merge quality only when we are at or below the cap.
struct CollapseResult {
    std::map<int, std::vector<float>> centroids;
    int num_speakers = 0;
};

/// One entry in the global-speaker registry used by `apply_collapse` and the
/// chunked-stitch pipeline. Hoisted to the header (Phase B.3) so the
/// short-audio adapter can construct a globals vector that's passable to
/// `apply_collapse`. Centroids are stored RAW (non-L2-normalized) end-to-end
/// to preserve byte-format compatibility with the legacy persistence path.
struct GlobalEntry {
    int id = 0;                   // current global ID (post-merge, pre-compaction)
    std::vector<float> raw;       // raw running-mean centroid
    long sample_count = 0;        // total samples contributed to this centroid
};

CollapseResult apply_collapse(
    DiarizeResult& diar_inout,
    std::vector<GlobalEntry>& globals,
    int target_speakers,
    float collapse_threshold);

/// Phase B.3 — short-audio adapter. Builds a `std::vector<GlobalEntry>` from
/// a sherpa-clustering `DiarizeResult` by extracting one centroid per unique
/// cluster ID via `extract_speaker_embedding` over the cluster's segment
/// audio. Sample-count weights are derived from total segment-duration
/// samples per cluster ID. The returned globals can be passed directly to
/// `apply_collapse`. Used by the pipeline's short-audio post-diarize wiring.
std::vector<GlobalEntry> build_short_audio_globals(
    const float* samples, size_t num_samples,
    const DiarizeResult& diar, int threads);

/// Phase B.3 — short-audio dump helper. Mirrors the centroid/sample-count
/// vectors stored in `globals` into the JSON-dump arrays for the
/// `dump_centroids_json` instrumentation call site in the short-audio path.
void copy_globals_for_dump(const std::vector<GlobalEntry>& globals,
                           std::vector<std::vector<float>>& centroids_out,
                           std::vector<long>& sample_counts_out);
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

/// Write a centroid-dump JSON artifact at `path` (Phase A instrumentation).
/// If `path` is empty, no file is written. If `meeting_timestamp` is non-empty
/// it is inserted before the extension to prevent concurrent-reprocess
/// collisions (M-1). The dump payload is documented inline in the
/// implementation; the format is stable enough for
/// `scripts/diarize_threshold_analysis.py` to consume.
///
/// `centroids[i]` is the raw (non-L2-normalized) running-mean embedding for
/// global ID `i`. `sample_counts[i]` is the cumulative sample-count weight for
/// global ID `i` (used by the sample-weighted mean update). `local_to_global`
/// is a per-chunk map `chunk_index → (local_id → global_id)`; pass an empty
/// vector for the short-audio path (no chunking).
void dump_centroids_json(
    const std::string& path,
    const std::string& meeting_timestamp,
    const std::vector<std::vector<float>>& centroids,
    const std::vector<long>& sample_counts,
    const std::vector<std::map<int, int>>& local_to_global,
    const std::string& source_label);

} // namespace recmeet
