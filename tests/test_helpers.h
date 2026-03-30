// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>
#include <vector>

namespace recmeet {

namespace fs = std::filesystem;
namespace test_helpers {

// ---------------------------------------------------------------------------
// JSON results collector — accumulates entries, writes build/benchmark_results.json at exit
// ---------------------------------------------------------------------------

struct BenchmarkResult {
    std::string json_fragment;  // pre-formatted JSON object body
};

class BenchmarkResults {
public:
    static void add(const std::string& fragment) {
        if (!registered_) {
            std::atexit(write_json);
            registered_ = true;
        }
        entries_.push_back({fragment});
    }

private:
    static void write_json() {
        if (entries_.empty()) return;

        fs::path root = find_root();
        if (root.empty()) return;
        fs::path out_path = root / "build" / "benchmark_results.json";

        // ISO 8601 timestamp
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_r(&t, &tm);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm);

        std::ofstream out(out_path);
        out << "{\n  \"timestamp\": \"" << ts << "\",\n  \"results\": [\n";
        for (size_t i = 0; i < entries_.size(); ++i) {
            out << "    {" << entries_[i].json_fragment << "}";
            if (i + 1 < entries_.size()) out << ",";
            out << "\n";
        }
        out << "  ]\n}\n";

        fprintf(stderr, "\n[benchmark] Results written to %s\n", out_path.c_str());
    }

    static fs::path find_root() {
        fs::path dir = fs::current_path();
        for (int i = 0; i < 10; ++i) {
            if (fs::exists(dir / "CMakeLists.txt") && fs::is_directory(dir / "build"))
                return dir;
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
        return {};
    }

    static inline std::vector<BenchmarkResult> entries_;
    static inline bool registered_ = false;
};

/// Walk up from cwd to find the project root (contains CMakeLists.txt and assets/).
inline fs::path find_project_root() {
    fs::path dir = fs::current_path();
    for (int i = 0; i < 10; ++i) {
        if (fs::exists(dir / "CMakeLists.txt") && fs::is_directory(dir / "assets"))
            return dir;
        if (!dir.has_parent_path() || dir == dir.parent_path())
            break;
        dir = dir.parent_path();
    }
    return {};
}

/// Strip markdown formatting from the reference transcript to plain text.
/// Removes # headers, **SPEAKER:** labels, and [_crosstalk_] markers.
inline std::string strip_reference_transcript(const std::string& md) {
    std::istringstream in(md);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        // Skip header lines
        if (!line.empty() && line[0] == '#')
            continue;

        // Remove [_crosstalk_] markers
        std::string::size_type pos;
        while ((pos = line.find("\\[_crosstalk_\\]")) != std::string::npos)
            line.erase(pos, 15);

        // Strip **SPEAKER:** labels — keep the text after the label
        if (line.size() >= 4 && line[0] == '*' && line[1] == '*') {
            auto close = line.find(":**", 2);
            if (close != std::string::npos) {
                line = line.substr(close + 3);
                // Trim leading space
                if (!line.empty() && line[0] == ' ')
                    line = line.substr(1);
            }
        }

        // Skip empty lines and metadata-only lines
        if (line.empty())
            continue;

        out << line << " ";
    }
    return out.str();
}

/// Strip "[MM:SS - MM:SS] " timestamps and optional "Speaker_XX: " labels
/// from pipeline transcript lines for WER computation.
inline std::string strip_transcript_labels(const std::string& transcript) {
    std::istringstream in(transcript);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        // Strip timestamp prefix: "[MM:SS - MM:SS] "
        if (line.size() > 15 && line[0] == '[') {
            auto close = line.find("] ");
            if (close != std::string::npos)
                line = line.substr(close + 2);
        }
        // Strip optional speaker label: "Speaker_XX: " or "Name: "
        auto colon = line.find(": ");
        if (colon != std::string::npos && colon < 30)
            line = line.substr(colon + 2);
        if (!line.empty())
            out << line << " ";
    }
    return out.str();
}

/// Lowercase, strip punctuation (keep apostrophes), split into words.
inline std::vector<std::string> tokenize_words(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    for (char c : text) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            word += std::tolower(static_cast<unsigned char>(c));
        } else if (c == '\'' && !word.empty()) {
            word += c;
        } else {
            if (!word.empty()) {
                // Trim trailing apostrophe
                if (word.back() == '\'')
                    word.pop_back();
                if (!word.empty())
                    words.push_back(word);
                word.clear();
            }
        }
    }
    if (!word.empty()) {
        if (word.back() == '\'')
            word.pop_back();
        if (!word.empty())
            words.push_back(word);
    }
    return words;
}

/// Word Error Rate via Levenshtein on word vectors, two-row DP (O(n) space).
inline double compute_wer(const std::vector<std::string>& ref,
                          const std::vector<std::string>& hyp) {
    if (ref.empty())
        return hyp.empty() ? 0.0 : 1.0;

    size_t m = ref.size(), n = hyp.size();
    std::vector<size_t> prev(n + 1), curr(n + 1);

    for (size_t j = 0; j <= n; ++j)
        prev[j] = j;

    for (size_t i = 1; i <= m; ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= n; ++j) {
            if (ref[i - 1] == hyp[j - 1]) {
                curr[j] = prev[j - 1];
            } else {
                curr[j] = 1 + std::min({prev[j], curr[j - 1], prev[j - 1]});
            }
        }
        std::swap(prev, curr);
    }

    return static_cast<double>(prev[n]) / static_cast<double>(m);
}

} // namespace test_helpers
} // namespace recmeet
