// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "diarize.h"

#include <map>
#include <string>
#include <vector>

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

#if RECMEET_USE_SHERPA

/// Per-meeting speaker data persisted as speakers.json alongside audio artifacts.
struct MeetingSpeaker {
    int cluster_id;                         // diarization cluster index
    std::string label;                      // "John" (known) or "Speaker_01" (unknown)
    bool identified;                        // true if matched from speaker DB
    std::vector<float> embedding;           // embedding vector
    float duration_sec;                     // total speaking time (summed from segments)
    float confidence;                       // identification confidence (0 if unknown)
};

/// Save per-meeting speaker data to <meeting_dir>/speakers.json.
void save_meeting_speakers(const fs::path& meeting_dir,
                           const std::vector<MeetingSpeaker>& speakers);

/// Load per-meeting speaker data from <meeting_dir>/speakers.json.
std::vector<MeetingSpeaker> load_meeting_speakers(const fs::path& meeting_dir);

/// Result of speaker identification with preserved embeddings.
struct IdentifyResult {
    std::map<int, std::string> names;             // cluster_id → enrolled name (empty if unmatched)
    std::map<int, std::vector<float>> embeddings; // cluster_id → embedding vector
    std::map<int, float> scores;                  // cluster_id → confidence (0.0 if unmatched)
};

/// Extract a speaker embedding from audio segments belonging to a specific
/// diarization cluster. Returns a single averaged embedding vector.
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
#endif

/// Default speaker database directory.
fs::path default_speaker_db_dir();

/// Current UTC time as ISO 8601 string.
std::string iso_now();

} // namespace recmeet
