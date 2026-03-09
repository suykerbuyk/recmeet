// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "speaker_id.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

#if RECMEET_USE_SHERPA
#include <sherpa-onnx/c-api/c-api.h>
#endif

namespace recmeet {

// ---------------------------------------------------------------------------
// Timestamp helper
// ---------------------------------------------------------------------------

std::string iso_now() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

// ---------------------------------------------------------------------------
// Minimal JSON serialization for speaker profiles
// ---------------------------------------------------------------------------

static std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            default:   out += c;
        }
    }
    return out;
}

static std::string serialize_profile(const SpeakerProfile& p) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"name\": \"" << escape_json(p.name) << "\",\n";
    out << "  \"created\": \"" << escape_json(p.created) << "\",\n";
    out << "  \"updated\": \"" << escape_json(p.updated) << "\",\n";
    out << "  \"embeddings\": [\n";
    for (size_t i = 0; i < p.embeddings.size(); ++i) {
        out << "    [";
        for (size_t j = 0; j < p.embeddings[i].size(); ++j) {
            if (j > 0) out << ", ";
            out << p.embeddings[i][j];
        }
        out << "]";
        if (i + 1 < p.embeddings.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

// Minimal JSON parser for speaker profile files
static bool parse_profile(const std::string& json, SpeakerProfile& out) {
    // Extract string field value between quotes after "key": "
    auto get_str = [&](const std::string& key) -> std::string {
        std::string needle = "\"" + key + "\": \"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return "";
        pos += needle.size();
        auto end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    };

    out.name = get_str("name");
    out.created = get_str("created");
    out.updated = get_str("updated");

    if (out.name.empty()) return false;

    // Parse embeddings array: find "embeddings": [ then parse nested arrays
    auto emb_pos = json.find("\"embeddings\":");
    if (emb_pos == std::string::npos) return false;

    auto outer_start = json.find('[', emb_pos);
    if (outer_start == std::string::npos) return false;

    out.embeddings.clear();
    size_t pos = outer_start + 1;

    while (pos < json.size()) {
        // Find next inner array
        auto arr_start = json.find('[', pos);
        if (arr_start == std::string::npos) break;

        // Check we haven't passed the outer closing bracket
        auto outer_end = json.find(']', pos);
        if (outer_end != std::string::npos && outer_end < arr_start) break;

        auto arr_end = json.find(']', arr_start);
        if (arr_end == std::string::npos) break;

        std::string arr_str = json.substr(arr_start + 1, arr_end - arr_start - 1);
        std::vector<float> emb;
        std::istringstream ss(arr_str);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try {
                emb.push_back(std::stof(token));
            } catch (...) {}
        }
        if (!emb.empty())
            out.embeddings.push_back(std::move(emb));

        pos = arr_end + 1;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Speaker database operations
// ---------------------------------------------------------------------------

fs::path default_speaker_db_dir() {
    return data_dir() / "speakers";
}

std::vector<SpeakerProfile> load_speaker_db(const fs::path& db_dir) {
    std::vector<SpeakerProfile> profiles;
    if (!fs::is_directory(db_dir)) return profiles;

    for (const auto& entry : fs::directory_iterator(db_dir)) {
        if (entry.path().extension() != ".json") continue;

        std::ifstream in(entry.path());
        if (!in) continue;

        std::ostringstream buf;
        buf << in.rdbuf();

        SpeakerProfile profile;
        if (parse_profile(buf.str(), profile) && !profile.embeddings.empty())
            profiles.push_back(std::move(profile));
    }

    return profiles;
}

void save_speaker(const fs::path& db_dir, const SpeakerProfile& profile) {
    fs::create_directories(db_dir);
    fs::path path = db_dir / (profile.name + ".json");
    std::ofstream out(path);
    if (!out)
        throw RecmeetError("Cannot write speaker profile: " + path.string());
    out << serialize_profile(profile);
}

bool remove_speaker(const fs::path& db_dir, const std::string& name) {
    fs::path path = db_dir / (name + ".json");
    return fs::remove(path);
}

std::vector<std::string> list_speakers(const fs::path& db_dir) {
    std::vector<std::string> names;
    if (!fs::is_directory(db_dir)) return names;

    for (const auto& entry : fs::directory_iterator(db_dir)) {
        if (entry.path().extension() != ".json") continue;
        names.push_back(entry.path().stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// ---------------------------------------------------------------------------
// Speaker embedding extraction and identification (sherpa-onnx)
// ---------------------------------------------------------------------------

#if RECMEET_USE_SHERPA

std::vector<float> extract_speaker_embedding(
    const float* samples, size_t num_samples,
    const DiarizeResult& diar, int speaker_id,
    const fs::path& model_path, int threads) {

    int t = threads > 0 ? threads : default_thread_count();

    SherpaOnnxSpeakerEmbeddingExtractorConfig cfg{};
    cfg.model = model_path.c_str();
    cfg.num_threads = t;
    cfg.debug = 0;
    cfg.provider = "cpu";

    const auto* extractor = SherpaOnnxCreateSpeakerEmbeddingExtractor(&cfg);
    if (!extractor)
        throw RecmeetError("Failed to create speaker embedding extractor");

    // Feed all segments belonging to this speaker into a single stream
    auto* stream = SherpaOnnxSpeakerEmbeddingExtractorCreateStream(extractor);
    if (!stream) {
        SherpaOnnxDestroySpeakerEmbeddingExtractor(extractor);
        throw RecmeetError("Failed to create embedding extractor stream");
    }

    for (const auto& seg : diar.segments) {
        if (seg.speaker != speaker_id) continue;

        auto start_sample = static_cast<size_t>(seg.start * SAMPLE_RATE);
        auto end_sample = static_cast<size_t>(seg.end * SAMPLE_RATE);
        if (start_sample >= num_samples) continue;
        if (end_sample > num_samples) end_sample = num_samples;

        auto n = static_cast<int32_t>(end_sample - start_sample);
        if (n > 0) {
            SherpaOnnxOnlineStreamAcceptWaveform(stream, SAMPLE_RATE,
                                                  samples + start_sample, n);
        }
    }
    SherpaOnnxOnlineStreamInputFinished(stream);

    std::vector<float> embedding;

    if (SherpaOnnxSpeakerEmbeddingExtractorIsReady(extractor, stream)) {
        int dim = SherpaOnnxSpeakerEmbeddingExtractorDim(extractor);
        const float* raw = SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding(
            extractor, stream);
        if (raw) {
            embedding.assign(raw, raw + dim);
            SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding(raw);
        }
    }

    SherpaOnnxDestroyOnlineStream(stream);
    SherpaOnnxDestroySpeakerEmbeddingExtractor(extractor);

    return embedding;
}

std::map<int, std::string> identify_speakers(
    const float* samples, size_t num_samples,
    const DiarizeResult& diar,
    const std::vector<SpeakerProfile>& db,
    const fs::path& model_path,
    float threshold, int threads) {

    std::map<int, std::string> result;
    if (db.empty() || diar.segments.empty()) return result;

    int t = threads > 0 ? threads : default_thread_count();

    // Create embedding extractor
    SherpaOnnxSpeakerEmbeddingExtractorConfig cfg{};
    cfg.model = model_path.c_str();
    cfg.num_threads = t;
    cfg.debug = 0;
    cfg.provider = "cpu";

    const auto* extractor = SherpaOnnxCreateSpeakerEmbeddingExtractor(&cfg);
    if (!extractor) {
        log_warn("Failed to create embedding extractor for speaker ID");
        return result;
    }

    int dim = SherpaOnnxSpeakerEmbeddingExtractorDim(extractor);

    // Create speaker embedding manager and register enrolled speakers
    const auto* mgr = SherpaOnnxCreateSpeakerEmbeddingManager(dim);
    if (!mgr) {
        SherpaOnnxDestroySpeakerEmbeddingExtractor(extractor);
        log_warn("Failed to create speaker embedding manager");
        return result;
    }

    for (const auto& profile : db) {
        for (const auto& emb : profile.embeddings) {
            if (static_cast<int>(emb.size()) == dim) {
                SherpaOnnxSpeakerEmbeddingManagerAdd(mgr, profile.name.c_str(),
                                                      emb.data());
            }
        }
    }

    // Collect unique speaker IDs from diarization
    std::vector<int> speaker_ids;
    for (const auto& seg : diar.segments) {
        if (std::find(speaker_ids.begin(), speaker_ids.end(), seg.speaker)
                == speaker_ids.end())
            speaker_ids.push_back(seg.speaker);
    }

    // Track which enrolled names have already been matched to prevent
    // two clusters from being assigned the same identity.
    std::vector<std::pair<int, std::string>> candidates;  // {speaker_id, name}
    std::vector<float> scores;

    // Extract embedding for each cluster and search
    for (int sid : speaker_ids) {
        auto* stream = SherpaOnnxSpeakerEmbeddingExtractorCreateStream(extractor);
        if (!stream) continue;

        for (const auto& seg : diar.segments) {
            if (seg.speaker != sid) continue;
            auto start = static_cast<size_t>(seg.start * SAMPLE_RATE);
            auto end = static_cast<size_t>(seg.end * SAMPLE_RATE);
            if (start >= num_samples) continue;
            if (end > num_samples) end = num_samples;
            auto n = static_cast<int32_t>(end - start);
            if (n > 0)
                SherpaOnnxOnlineStreamAcceptWaveform(stream, SAMPLE_RATE,
                                                      samples + start, n);
        }
        SherpaOnnxOnlineStreamInputFinished(stream);

        if (SherpaOnnxSpeakerEmbeddingExtractorIsReady(extractor, stream)) {
            const float* emb = SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding(
                extractor, stream);
            if (emb) {
                const auto* best = SherpaOnnxSpeakerEmbeddingManagerGetBestMatches(
                    mgr, emb, threshold, 1);
                if (best && best->count > 0 && best->matches[0].name) {
                    candidates.push_back({sid, best->matches[0].name});
                    scores.push_back(best->matches[0].score);
                }
                if (best)
                    SherpaOnnxSpeakerEmbeddingManagerFreeBestMatches(best);
                SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding(emb);
            }
        }

        SherpaOnnxDestroyOnlineStream(stream);
    }

    // Resolve conflicts: if multiple clusters match the same name, pick highest score
    // Sort by score descending so highest-confidence matches win
    std::vector<size_t> order(candidates.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return scores[a] > scores[b];
    });

    std::map<std::string, bool> used_names;
    for (size_t idx : order) {
        const auto& name = candidates[idx].second;
        if (used_names.count(name)) continue;  // already assigned to higher-scoring cluster
        result[candidates[idx].first] = name;
        used_names[name] = true;
        log_info("Speaker %d identified as '%s' (score: %.3f)",
                 candidates[idx].first, name.c_str(), scores[idx]);
    }

    SherpaOnnxDestroySpeakerEmbeddingManager(mgr);
    SherpaOnnxDestroySpeakerEmbeddingExtractor(extractor);

    return result;
}

#endif // RECMEET_USE_SHERPA

} // namespace recmeet
