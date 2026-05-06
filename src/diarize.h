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
