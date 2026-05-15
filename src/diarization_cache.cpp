// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "diarization_cache.h"

#include <chrono>
#include <fstream>
#include <sstream>

namespace recmeet {

// ---------------------------------------------------------------------------
// diarization.json artifact reader (subprocess → daemon hand-off).
//
// The companion writer is `write_diarization_artifact` in pipeline.cpp;
// keep both in sync. The format is intentionally minimal — flat JSON with
// a numeric num_speakers and an array of clusters, each with an idx,
// duration_ms, and embedding[] of floats. The parser hand-rolls a tiny
// reader (no JSON library) to keep the subprocess→daemon boundary
// dependency-light and to mirror the existing speakers DB parser in
// speaker_id.cpp.
// ---------------------------------------------------------------------------

std::vector<DiarizationCluster>
DiarizationCache::load_diarization_artifact(const fs::path& path) {
    std::ifstream in(path);
    if (!in)
        throw RecmeetError("diarization_cache: cannot open "
                           + path.string());
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string s = buf.str();

    std::vector<DiarizationCluster> out;

    // Locate the "clusters": [ ... ] array body. Past that we walk over
    // brace-balanced cluster objects.
    auto clusters_pos = s.find("\"clusters\"");
    if (clusters_pos == std::string::npos)
        throw RecmeetError("diarization_cache: 'clusters' key missing in "
                           + path.string());
    auto outer_open = s.find('[', clusters_pos);
    if (outer_open == std::string::npos)
        throw RecmeetError("diarization_cache: clusters array not found");

    size_t pos = outer_open + 1;
    while (pos < s.size()) {
        // Skip whitespace and commas.
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'
                                  || s[pos] == '\n' || s[pos] == ','))
            ++pos;
        if (pos >= s.size() || s[pos] == ']') break;
        if (s[pos] != '{')
            throw RecmeetError("diarization_cache: expected '{' at offset "
                               + std::to_string(pos));
        // Find matching '}'
        size_t obj_start = pos;
        int depth = 1;
        ++pos;
        while (pos < s.size() && depth > 0) {
            if (s[pos] == '{') ++depth;
            else if (s[pos] == '}') --depth;
            else if (s[pos] == '[') {
                // Skip nested array (embedding) to avoid mis-counting braces.
                int adepth = 1;
                ++pos;
                while (pos < s.size() && adepth > 0) {
                    if (s[pos] == '[') ++adepth;
                    else if (s[pos] == ']') --adepth;
                    ++pos;
                }
                continue;
            }
            ++pos;
        }
        if (depth != 0)
            throw RecmeetError("diarization_cache: unbalanced cluster object");
        std::string body = s.substr(obj_start, pos - obj_start);

        DiarizationCluster c;

        // idx
        auto idx_pos = body.find("\"idx\"");
        if (idx_pos == std::string::npos)
            throw RecmeetError("diarization_cache: cluster missing 'idx'");
        auto colon = body.find(':', idx_pos);
        c.idx = std::stoi(body.substr(colon + 1));

        // duration_ms
        auto dur_pos = body.find("\"duration_ms\"");
        if (dur_pos != std::string::npos) {
            auto dcolon = body.find(':', dur_pos);
            c.duration_ms = static_cast<int64_t>(
                std::stoll(body.substr(dcolon + 1)));
        }

        // embedding array
        auto emb_pos = body.find("\"embedding\"");
        if (emb_pos != std::string::npos) {
            auto arr_open = body.find('[', emb_pos);
            auto arr_close = body.find(']', arr_open);
            if (arr_open != std::string::npos
                && arr_close != std::string::npos) {
                std::string arr = body.substr(arr_open + 1,
                                              arr_close - arr_open - 1);
                std::istringstream as(arr);
                std::string tok;
                while (std::getline(as, tok, ',')) {
                    try {
                        c.embedding.push_back(std::stof(tok));
                    } catch (...) {
                        // Skip empty / unparseable tokens (trailing whitespace).
                    }
                }
            }
        }

        out.push_back(std::move(c));
    }

    return out;
}

DiarizationCache::DiarizationCache(int64_t ttl_secs, Clock clock)
    : ttl_secs_(ttl_secs < 0 ? 0 : ttl_secs),
      clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = []() -> int64_t {
            using namespace std::chrono;
            return duration_cast<seconds>(
                       system_clock::now().time_since_epoch())
                .count();
        };
    }
}

int64_t DiarizationCache::now_locked() const {
    return clock_();
}

bool DiarizationCache::expired_locked(const DiarizationCacheEntry& e) const {
    if (ttl_secs_ <= 0) return false;  // 0 = never expire
    return (now_locked() - e.inserted_at) > ttl_secs_;
}

void DiarizationCache::put(int64_t job_id,
                           std::vector<DiarizationCluster> clusters) {
    std::lock_guard<std::mutex> lk(mu_);
    DiarizationCacheEntry e;
    e.job_id = job_id;
    e.clusters = std::move(clusters);
    e.inserted_at = now_locked();
    entries_[job_id] = std::move(e);
}

std::optional<DiarizationCacheEntry> DiarizationCache::get(int64_t job_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(job_id);
    if (it == entries_.end()) return std::nullopt;
    if (expired_locked(it->second)) {
        entries_.erase(it);
        return std::nullopt;
    }
    return it->second;
}

void DiarizationCache::erase(int64_t job_id) {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.erase(job_id);
}

size_t DiarizationCache::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

size_t DiarizationCache::sweep_expired() {
    std::lock_guard<std::mutex> lk(mu_);
    if (ttl_secs_ <= 0) return 0;
    size_t evicted = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (expired_locked(it->second)) {
            it = entries_.erase(it);
            ++evicted;
        } else {
            ++it;
        }
    }
    return evicted;
}

} // namespace recmeet
