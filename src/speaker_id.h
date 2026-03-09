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

/// List enrolled speaker names.
std::vector<std::string> list_speakers(const fs::path& db_dir);

#if RECMEET_USE_SHERPA
/// Extract a speaker embedding from audio segments belonging to a specific
/// diarization cluster. Returns a single averaged embedding vector.
std::vector<float> extract_speaker_embedding(
    const float* samples, size_t num_samples,
    const DiarizeResult& diar, int speaker_id,
    const fs::path& model_path, int threads = 0);

/// Match diarization clusters against enrolled speakers.
/// Returns a map from cluster ID to enrolled speaker name.
/// Clusters with no match above the threshold are omitted.
std::map<int, std::string> identify_speakers(
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
