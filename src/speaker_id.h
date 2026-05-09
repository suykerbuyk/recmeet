// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "diarize.h"

#include <map>
#include <string>
#include <vector>

#if RECMEET_USE_SHERPA
struct SherpaOnnxSpeakerEmbeddingExtractor;
#endif

namespace recmeet {

/// A single enrolled speaker with one or more voiceprint embeddings.
struct SpeakerProfile {
    std::string name;
    std::vector<std::vector<float>> embeddings;  // one per enrollment
    std::string created;   // ISO 8601
    std::string updated;   // ISO 8601
};

/// Load all speaker profiles from a directory of JSON files.
std::vector<SpeakerProfile> load_speaker_db(const fs::path& db_dir);

/// Save a speaker profile to <db_dir>/<name>.json.
void save_speaker(const fs::path& db_dir, const SpeakerProfile& profile);

/// Remove a speaker profile from the database.
bool remove_speaker(const fs::path& db_dir, const std::string& name);

/// Remove all speaker profiles from the database. Returns count removed.
int reset_speakers(const fs::path& db_dir);

/// List enrolled speaker names.
std::vector<std::string> list_speakers(const fs::path& db_dir);

/// Remove a specific embedding from a speaker profile by L2 distance match.
/// Deletes the profile file if no embeddings remain. Returns true if found and removed.
bool remove_embedding(const fs::path& db_dir, const std::string& name,
                      const std::vector<float>& embedding, float epsilon = 1e-6f);

/// Relabel a meeting speaker by cluster_id. Returns true if found and updated.
bool relabel_meeting_speaker(const fs::path& meeting_dir, int cluster_id,
                             const std::string& new_label, float confidence = 1.0f);

/// Per-meeting speaker data persisted as speakers.json alongside audio artifacts.
struct MeetingSpeaker {
    int cluster_id;                         // diarization cluster index
    std::string label;                      // "John" (known) or "Speaker_01" (unknown)
    bool identified;                        // true if matched from speaker DB
    std::vector<float> embedding;           // embedding vector
    float duration_sec;                     // total speaking time (summed from segments)
    float confidence;                       // identification confidence (0 if unknown)
};

/// Save per-meeting speaker data to <meeting_dir>/speakers_<timestamp>.json.
/// If timestamp is empty, falls back to the legacy filename `speakers.json`
/// (used by older fixtures and migration paths).
void save_meeting_speakers(const fs::path& meeting_dir,
                           const std::vector<MeetingSpeaker>& speakers,
                           const std::string& timestamp = "");

/// Load per-meeting speaker data from <meeting_dir>/speakers.json.
std::vector<MeetingSpeaker> load_meeting_speakers(const fs::path& meeting_dir);

/// Result of speaker identification with preserved embeddings.
struct IdentifyResult {
    std::map<int, std::string> names;             // cluster_id → enrolled name (empty if unmatched)
    std::map<int, std::vector<float>> embeddings; // cluster_id → embedding vector
    std::map<int, float> scores;                  // cluster_id → confidence (0.0 if unmatched)
};

#if RECMEET_USE_SHERPA

/// RAII wrapper around `SherpaOnnxSpeakerEmbeddingExtractor`. Loads the
/// embedding model on construction and keeps it loaded across multiple
/// extraction calls. Required by chunked-diarization (T2.1) to avoid
/// reloading ~28 MB of model on every chunk; usable directly by callers
/// that need to extract embeddings for several speakers in sequence.
class SpeakerEmbeddingSession {
public:
    SpeakerEmbeddingSession(const fs::path& model_path, int threads = 0);
    ~SpeakerEmbeddingSession();

    SpeakerEmbeddingSession(const SpeakerEmbeddingSession&) = delete;
    SpeakerEmbeddingSession& operator=(const SpeakerEmbeddingSession&) = delete;
    SpeakerEmbeddingSession(SpeakerEmbeddingSession&& other) noexcept;
    SpeakerEmbeddingSession& operator=(SpeakerEmbeddingSession&& other) noexcept;

    /// Embedding dimension reported by the loaded model. Cached at ctor.
    int dim() const noexcept { return dim_; }

    /// Opaque sherpa handle. Returns nullptr after a move-from.
    const SherpaOnnxSpeakerEmbeddingExtractor* handle() const noexcept { return extractor_; }

private:
    const SherpaOnnxSpeakerEmbeddingExtractor* extractor_ = nullptr;
    int dim_ = 0;
    int threads_ = 0;
};

/// Extract a speaker embedding from audio segments belonging to a specific
/// diarization cluster, reusing a pre-built session. Returns the raw
/// (non-L2-normalized) embedding vector; callers comparing with cosine
/// similarity must normalize first.
std::vector<float> extract_speaker_embedding(
    SpeakerEmbeddingSession& session,
    const float* samples, size_t num_samples,
    const DiarizeResult& diar, int speaker_id);

/// Extract a speaker embedding by constructing a one-shot session from the
/// model path. Preserved for callers that don't need session reuse.
std::vector<float> extract_speaker_embedding(
    const float* samples, size_t num_samples,
    const DiarizeResult& diar, int speaker_id,
    const fs::path& model_path, int threads = 0);

/// Match diarization clusters against enrolled speakers.
/// Returns IdentifyResult with names, embeddings, and scores for all clusters.
/// Embeddings are always extracted; matching is skipped when db is empty.
IdentifyResult identify_speakers(
    const float* samples, size_t num_samples,
    const DiarizeResult& diar,
    const std::vector<SpeakerProfile>& db,
    const fs::path& model_path,
    float threshold = 0.6f,
    int threads = 0);

/// Match pre-computed cluster centroids against enrolled speakers without
/// instantiating an embedding extractor. Bypass entry point for the chunked
/// diarization pipeline (T2.1), which has already extracted one centroid per
/// global cluster during stitching — re-extracting here would re-stream audio
/// through the model and reintroduce the memory peak T2 is meant to avoid.
///
/// `dim` is derived from the first non-empty centroid; centroids whose size
/// differs are passed through as-is (verbatim) into `result.embeddings` but
/// are not registered with the matcher. Centroids are stored verbatim (no
/// normalization) to keep the persisted MeetingSpeaker.embedding format
/// byte-shape-compatible with the legacy single-call path.
IdentifyResult identify_speakers_with_centroids(
    const std::map<int, std::vector<float>>& centroids,
    const std::vector<SpeakerProfile>& db,
    float threshold = 0.6f);

/// Re-identify meeting speakers against current DB using saved embeddings.
/// Speakers with confidence == 1.0 (manually corrected) are preserved.
/// Returns updated speaker list if anything changed, empty if unchanged.
std::vector<MeetingSpeaker> re_identify_meeting(
    const std::vector<MeetingSpeaker>& speakers,
    const std::vector<SpeakerProfile>& db,
    float threshold = 0.6f);
#endif

/// Default speaker database directory.
fs::path default_speaker_db_dir();

/// Current UTC time as ISO 8601 string.
std::string iso_now();

} // namespace recmeet
