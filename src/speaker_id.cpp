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

int reset_speakers(const fs::path& db_dir) {
    if (!fs::is_directory(db_dir)) return 0;
    int count = 0;
    for (const auto& entry : fs::directory_iterator(db_dir)) {
        if (entry.path().extension() == ".json") {
            fs::remove(entry.path());
            ++count;
        }
    }
    return count;
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

bool remove_embedding(const fs::path& db_dir, const std::string& name,
                      const std::vector<float>& embedding, float epsilon) {
    if (embedding.empty()) return false;

    auto profiles = load_speaker_db(db_dir);
    SpeakerProfile* found = nullptr;
    for (auto& p : profiles) {
        if (p.name == name) { found = &p; break; }
    }
    if (!found) return false;

    // Find embedding by L2 distance
    float threshold = epsilon * epsilon * static_cast<float>(embedding.size());
    auto it = std::find_if(found->embeddings.begin(), found->embeddings.end(),
        [&](const std::vector<float>& e) {
            if (e.size() != embedding.size()) return false;
            float dist2 = 0.0f;
            for (size_t i = 0; i < e.size(); ++i) {
                float d = e[i] - embedding[i];
                dist2 += d * d;
            }
            return dist2 < threshold;
        });
    if (it == found->embeddings.end()) return false;

    found->embeddings.erase(it);

    if (found->embeddings.empty()) {
        remove_speaker(db_dir, name);
    } else {
        found->updated = iso_now();
        save_speaker(db_dir, *found);
    }
    return true;
}

bool relabel_meeting_speaker(const fs::path& meeting_dir, int cluster_id,
                             const std::string& new_label, float confidence) {
    auto speakers = load_meeting_speakers(meeting_dir);
    if (speakers.empty()) return false;

    bool found = false;
    for (auto& s : speakers) {
        if (s.cluster_id == cluster_id) {
            s.label = new_label;
            s.identified = true;
            s.confidence = confidence;
            found = true;
            break;
        }
    }
    if (!found) return false;

    save_meeting_speakers(meeting_dir, speakers);
    return true;
}

// ---------------------------------------------------------------------------
// Per-meeting speaker data (speakers.json)
// ---------------------------------------------------------------------------

static std::string serialize_meeting_speakers(const std::vector<MeetingSpeaker>& speakers) {
    std::ostringstream out;
    out << "{\n  \"speakers\": [\n";
    for (size_t i = 0; i < speakers.size(); ++i) {
        const auto& s = speakers[i];
        out << "    {\n";
        out << "      \"cluster_id\": " << s.cluster_id << ",\n";
        out << "      \"label\": \"" << escape_json(s.label) << "\",\n";
        out << "      \"identified\": " << (s.identified ? "true" : "false") << ",\n";
        out << "      \"duration_sec\": " << s.duration_sec << ",\n";
        out << "      \"confidence\": " << s.confidence << ",\n";
        out << "      \"embedding\": [";
        for (size_t j = 0; j < s.embedding.size(); ++j) {
            if (j > 0) out << ", ";
            out << s.embedding[j];
        }
        out << "]\n    }";
        if (i + 1 < speakers.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

static bool parse_meeting_speakers(const std::string& json, std::vector<MeetingSpeaker>& out) {
    out.clear();

    // Find "speakers": [ array
    auto arr_pos = json.find("\"speakers\":");
    if (arr_pos == std::string::npos) return false;

    auto outer_start = json.find('[', arr_pos);
    if (outer_start == std::string::npos) return false;

    // Find matching outer ]
    int depth = 1;
    size_t outer_end = outer_start + 1;
    while (outer_end < json.size() && depth > 0) {
        if (json[outer_end] == '[') ++depth;
        else if (json[outer_end] == ']') --depth;
        ++outer_end;
    }
    if (depth != 0) return false;
    --outer_end; // point at the ]

    // Parse each object between { and }
    size_t pos = outer_start + 1;
    while (pos < outer_end) {
        auto obj_start = json.find('{', pos);
        if (obj_start == std::string::npos || obj_start >= outer_end) break;

        // Find matching }
        int od = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < json.size() && od > 0) {
            if (json[obj_end] == '{') ++od;
            else if (json[obj_end] == '}') --od;
            ++obj_end;
        }
        --obj_end;

        std::string obj = json.substr(obj_start, obj_end - obj_start + 1);

        MeetingSpeaker s{};

        // Parse cluster_id
        auto cid_pos = obj.find("\"cluster_id\":");
        if (cid_pos != std::string::npos) {
            auto val_start = cid_pos + 13;
            while (val_start < obj.size() && (obj[val_start] == ' ' || obj[val_start] == '\t'))
                ++val_start;
            s.cluster_id = std::atoi(obj.c_str() + val_start);
        }

        // Parse label
        auto lbl_pos = obj.find("\"label\": \"");
        if (lbl_pos != std::string::npos) {
            auto val_start = lbl_pos + 10;
            auto val_end = obj.find('"', val_start);
            if (val_end != std::string::npos)
                s.label = obj.substr(val_start, val_end - val_start);
        }

        // Parse identified
        auto id_pos = obj.find("\"identified\":");
        if (id_pos != std::string::npos)
            s.identified = obj.find("true", id_pos) < obj.find("false", id_pos);

        // Parse duration_sec
        auto dur_pos = obj.find("\"duration_sec\":");
        if (dur_pos != std::string::npos) {
            auto val_start = dur_pos + 15;
            while (val_start < obj.size() && (obj[val_start] == ' ' || obj[val_start] == '\t'))
                ++val_start;
            s.duration_sec = std::strtof(obj.c_str() + val_start, nullptr);
        }

        // Parse confidence
        auto conf_pos = obj.find("\"confidence\":");
        if (conf_pos != std::string::npos) {
            auto val_start = conf_pos + 13;
            while (val_start < obj.size() && (obj[val_start] == ' ' || obj[val_start] == '\t'))
                ++val_start;
            s.confidence = std::strtof(obj.c_str() + val_start, nullptr);
        }

        // Parse embedding array
        auto emb_pos = obj.find("\"embedding\":");
        if (emb_pos != std::string::npos) {
            auto emb_start = obj.find('[', emb_pos);
            auto emb_end = obj.find(']', emb_start);
            if (emb_start != std::string::npos && emb_end != std::string::npos) {
                std::string emb_str = obj.substr(emb_start + 1, emb_end - emb_start - 1);
                std::istringstream ss(emb_str);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    try {
                        s.embedding.push_back(std::stof(token));
                    } catch (...) {}
                }
            }
        }

        out.push_back(std::move(s));
        pos = obj_end + 1;
    }

    return true;
}

void save_meeting_speakers(const fs::path& meeting_dir,
                           const std::vector<MeetingSpeaker>& speakers) {
    fs::create_directories(meeting_dir);
    fs::path path = meeting_dir / "speakers.json";
    std::ofstream out(path);
    if (!out)
        throw RecmeetError("Cannot write speakers.json: " + path.string());
    out << serialize_meeting_speakers(speakers);
}

std::vector<MeetingSpeaker> load_meeting_speakers(const fs::path& meeting_dir) {
    fs::path path = meeting_dir / "speakers.json";
    if (!fs::exists(path)) return {};

    std::ifstream in(path);
    if (!in) return {};

    std::ostringstream buf;
    buf << in.rdbuf();

    std::vector<MeetingSpeaker> speakers;
    parse_meeting_speakers(buf.str(), speakers);
    return speakers;
}

// ---------------------------------------------------------------------------
// Speaker embedding extraction and identification (sherpa-onnx)
// ---------------------------------------------------------------------------

#if RECMEET_USE_SHERPA

std::vector<float> extract_speaker_embedding(
    const float* samples, size_t num_samples,
    const DiarizeResult& diar, int speaker_id,
    const fs::path& model_path, int threads) {

    int t = std::min(threads > 0 ? threads : default_thread_count(), 4);

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

IdentifyResult identify_speakers(
    const float* samples, size_t num_samples,
    const DiarizeResult& diar,
    const std::vector<SpeakerProfile>& db,
    const fs::path& model_path,
    float threshold, int threads) {

    IdentifyResult result;
    if (diar.segments.empty()) return result;

    int t = std::min(threads > 0 ? threads : default_thread_count(), 4);

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

    // Create speaker embedding manager and register enrolled speakers (if any)
    const SherpaOnnxSpeakerEmbeddingManager* mgr = nullptr;
    if (!db.empty()) {
        mgr = SherpaOnnxCreateSpeakerEmbeddingManager(dim);
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
    }

    // Collect unique speaker IDs from diarization
    std::vector<int> speaker_ids;
    for (const auto& seg : diar.segments) {
        if (std::find(speaker_ids.begin(), speaker_ids.end(), seg.speaker)
                == speaker_ids.end())
            speaker_ids.push_back(seg.speaker);
    }

    // Track candidates for conflict resolution
    std::vector<std::pair<int, std::string>> candidates;  // {speaker_id, name}
    std::vector<float> candidate_scores;

    // Extract embedding for each cluster and optionally match
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
                // Preserve embedding before destroying the raw pointer
                result.embeddings[sid] = std::vector<float>(emb, emb + dim);
                result.scores[sid] = 0.0f;

                // Match against enrolled speakers if DB is available
                if (mgr) {
                    const auto* best = SherpaOnnxSpeakerEmbeddingManagerGetBestMatches(
                        mgr, emb, threshold, 1);
                    if (best && best->count > 0 && best->matches[0].name) {
                        candidates.push_back({sid, best->matches[0].name});
                        candidate_scores.push_back(best->matches[0].score);
                    }
                    if (best)
                        SherpaOnnxSpeakerEmbeddingManagerFreeBestMatches(best);
                }

                SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding(emb);
            }
        }

        SherpaOnnxDestroyOnlineStream(stream);
    }

    // Resolve conflicts: if multiple clusters match the same name, pick highest score
    std::vector<size_t> order(candidates.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return candidate_scores[a] > candidate_scores[b];
    });

    std::map<std::string, bool> used_names;
    for (size_t idx : order) {
        const auto& name = candidates[idx].second;
        if (used_names.count(name)) continue;
        int sid = candidates[idx].first;
        result.names[sid] = name;
        result.scores[sid] = candidate_scores[idx];
        used_names[name] = true;
        log_info("Speaker %d identified as '%s' (score: %.3f)",
                 sid, name.c_str(), candidate_scores[idx]);
    }

    if (mgr)
        SherpaOnnxDestroySpeakerEmbeddingManager(mgr);
    SherpaOnnxDestroySpeakerEmbeddingExtractor(extractor);

    return result;
}

std::vector<MeetingSpeaker> re_identify_meeting(
    const std::vector<MeetingSpeaker>& speakers,
    const std::vector<SpeakerProfile>& db,
    float threshold) {

    if (speakers.empty() || db.empty()) return {};

    // Infer embedding dimension from first non-empty meeting embedding
    int dim = 0;
    for (const auto& s : speakers) {
        if (!s.embedding.empty()) {
            dim = static_cast<int>(s.embedding.size());
            break;
        }
    }
    if (dim == 0) return {};

    // Create manager and register DB embeddings
    const auto* mgr = SherpaOnnxCreateSpeakerEmbeddingManager(dim);
    if (!mgr) return {};

    for (const auto& profile : db) {
        for (const auto& emb : profile.embeddings) {
            if (static_cast<int>(emb.size()) == dim)
                SherpaOnnxSpeakerEmbeddingManagerAdd(mgr, profile.name.c_str(), emb.data());
        }
    }

    // Collect candidates: match each non-manual speaker against DB
    struct Candidate {
        size_t index;      // index into speakers
        std::string name;
        float score;
    };
    std::vector<Candidate> candidates;

    for (size_t i = 0; i < speakers.size(); ++i) {
        const auto& s = speakers[i];
        // Skip manually corrected speakers
        if (s.confidence == 1.0f) continue;
        // Skip speakers with no/mismatched embedding
        if (s.embedding.empty() || static_cast<int>(s.embedding.size()) != dim) continue;

        const auto* best = SherpaOnnxSpeakerEmbeddingManagerGetBestMatches(
            mgr, s.embedding.data(), threshold, 1);
        if (best && best->count > 0 && best->matches[0].name) {
            candidates.push_back({i, best->matches[0].name, best->matches[0].score});
        }
        if (best)
            SherpaOnnxSpeakerEmbeddingManagerFreeBestMatches(best);
    }

    SherpaOnnxDestroySpeakerEmbeddingManager(mgr);

    // Conflict resolution: sort by score descending, assign greedily
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    std::map<std::string, size_t> assigned_names;  // name -> speaker index
    std::map<size_t, Candidate*> assigned_speakers; // speaker index -> candidate

    for (auto& c : candidates) {
        if (assigned_names.count(c.name)) continue;
        assigned_names[c.name] = c.index;
        assigned_speakers[c.index] = &c;
    }

    // Build updated list and check for changes
    auto updated = speakers;
    bool changed = false;

    for (size_t i = 0; i < updated.size(); ++i) {
        auto& s = updated[i];
        if (s.confidence == 1.0f) continue;  // manual — preserve
        if (s.embedding.empty() || static_cast<int>(s.embedding.size()) != dim) continue;

        auto it = assigned_speakers.find(i);
        if (it != assigned_speakers.end()) {
            // Matched a profile
            if (s.label != it->second->name || !s.identified ||
                s.confidence != it->second->score) {
                s.label = it->second->name;
                s.identified = true;
                s.confidence = it->second->score;
                changed = true;
            }
        } else {
            // No match — reset if was previously identified
            if (s.identified) {
                s.label = format_speaker(s.cluster_id);
                s.identified = false;
                s.confidence = 0.0f;
                changed = true;
            }
        }
    }

    return changed ? updated : std::vector<MeetingSpeaker>{};
}

#endif // RECMEET_USE_SHERPA

} // namespace recmeet
