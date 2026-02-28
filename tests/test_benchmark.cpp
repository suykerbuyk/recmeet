// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "diarize.h"
#include "transcribe.h"
#include "vad.h"
#include "summarize.h"
#include "model_manager.h"
#include "audio_file.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace recmeet;

namespace {

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
fs::path find_project_root() {
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
std::string strip_reference_transcript(const std::string& md) {
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

/// Lowercase, strip punctuation (keep apostrophes), split into words.
std::vector<std::string> tokenize_words(const std::string& text) {
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
double compute_wer(const std::vector<std::string>& ref,
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

} // anonymous namespace


// ---------------------------------------------------------------------------
// Benchmark tests — tagged [benchmark], SKIP when models/files are absent
// ---------------------------------------------------------------------------

TEST_CASE("Transcribe debate audio with whisper base", "[benchmark]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path audio_path = root / "assets" / "biden_trump_debate_2020.wav";
    fs::path ref_path   = root / "assets" / "biden_trump_debate_2020.md";

    if (!fs::exists(audio_path))
        SKIP("Reference audio not found: " + audio_path.string());
    if (!fs::exists(ref_path))
        SKIP("Reference transcript not found: " + ref_path.string());
    if (!is_whisper_model_cached("base"))
        SKIP("Whisper base model not cached — run: ./build/recmeet --model base --help");

    // Load reference transcript
    std::ifstream ref_file(ref_path);
    std::string ref_md((std::istreambuf_iterator<char>(ref_file)),
                        std::istreambuf_iterator<char>());
    std::string ref_text = strip_reference_transcript(ref_md);
    auto ref_words = tokenize_words(ref_text);
    REQUIRE(!ref_words.empty());

    // Transcribe
    fs::path model_path = ensure_whisper_model("base");

    auto t0 = std::chrono::steady_clock::now();
    auto result = transcribe(model_path, audio_path, "en");
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    // Collect hypothesis text from segments (raw text, no timestamps)
    std::string hyp_text;
    for (const auto& seg : result.segments)
        hyp_text += seg.text + " ";
    auto hyp_words = tokenize_words(hyp_text);

    REQUIRE(!hyp_words.empty());

    double wer = compute_wer(ref_words, hyp_words);
    fprintf(stderr, "\n[benchmark] Whisper base transcription:\n");
    fprintf(stderr, "  Reference words: %zu\n", ref_words.size());
    fprintf(stderr, "  Hypothesis words: %zu\n", hyp_words.size());
    fprintf(stderr, "  WER: %.1f%%\n", wer * 100.0);
    fprintf(stderr, "  Time: %.1fs\n", secs);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "\n      \"test\": \"whisper_transcription\","
        "\n      \"model\": \"base\","
        "\n      \"wer\": %.4f,"
        "\n      \"ref_words\": %zu,"
        "\n      \"hyp_words\": %zu,"
        "\n      \"segments\": %zu,"
        "\n      \"time_secs\": %.1f",
        wer, ref_words.size(), hyp_words.size(), result.segments.size(), secs);
    BenchmarkResults::add(buf);

    CHECK(wer < 0.40);
}

TEST_CASE("Summarize reference transcript with Grok API", "[benchmark]") {
    const char* api_key = std::getenv("XAI_API_KEY");
    if (!api_key || std::string(api_key).empty())
        SKIP("XAI_API_KEY not set");

    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path ref_path = root / "assets" / "biden_trump_debate_2020.md";
    if (!fs::exists(ref_path))
        SKIP("Reference transcript not found: " + ref_path.string());

    std::ifstream ref_file(ref_path);
    std::string transcript((std::istreambuf_iterator<char>(ref_file)),
                            std::istreambuf_iterator<char>());
    REQUIRE(!transcript.empty());

    // Grok API has a large context window — no truncation needed
    std::string api_url = "https://api.x.ai/v1/chat/completions";
    std::string model = "grok-3";

    // Allow override via environment
    if (const char* m = std::getenv("RECMEET_BENCH_MODEL"))
        model = m;

    auto t0 = std::chrono::steady_clock::now();
    std::string summary = summarize_http(transcript, api_url, api_key, model);
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    fprintf(stderr, "\n[benchmark] Grok API summarization (model: %s):\n", model.c_str());
    fprintf(stderr, "  Transcript size: %zu chars\n", transcript.size());
    fprintf(stderr, "  Summary size: %zu chars\n", summary.size());
    fprintf(stderr, "  Time: %.1fs\n", secs);

    int headings = 0;
    for (const char* h : {"Overview", "Key Points", "Decisions",
                           "Action Items", "Open Questions", "Participants"})
        if (summary.find(h) != std::string::npos) ++headings;

    char buf[512];
    snprintf(buf, sizeof(buf),
        "\n      \"test\": \"grok_api_summarization\","
        "\n      \"model\": \"%s\","
        "\n      \"transcript_chars\": %zu,"
        "\n      \"summary_chars\": %zu,"
        "\n      \"headings_found\": %d,"
        "\n      \"time_secs\": %.1f",
        model.c_str(), transcript.size(), summary.size(), headings, secs);
    BenchmarkResults::add(buf);

    CHECK(!summary.empty());
    CHECK(summary.find("Overview") != std::string::npos);
    CHECK(summary.find("Key Points") != std::string::npos);
    CHECK(summary.find("Decisions") != std::string::npos);
    CHECK(summary.find("Action Items") != std::string::npos);
    CHECK(summary.find("Open Questions") != std::string::npos);
    CHECK(summary.find("Participants") != std::string::npos);
}

#if RECMEET_USE_SHERPA
TEST_CASE("Diarize debate audio with sherpa-onnx", "[benchmark]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path audio_path = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_path))
        SKIP("Reference audio not found: " + audio_path.string());
    if (!is_sherpa_model_cached())
        SKIP("Sherpa diarization models not cached");

    auto t0 = std::chrono::steady_clock::now();
    auto result = diarize(audio_path, 3);  // Biden, Trump, moderator
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    fprintf(stderr, "\n[benchmark] Sherpa-onnx speaker diarization:\n");
    fprintf(stderr, "  Speakers detected: %d\n", result.num_speakers);
    fprintf(stderr, "  Segments: %zu\n", result.segments.size());
    fprintf(stderr, "  Time: %.1fs\n", secs);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "\n      \"test\": \"sherpa_diarization\","
        "\n      \"num_speakers_requested\": 3,"
        "\n      \"num_speakers_detected\": %d,"
        "\n      \"segments\": %zu,"
        "\n      \"time_secs\": %.1f",
        result.num_speakers, result.segments.size(), secs);
    BenchmarkResults::add(buf);

    CHECK(result.num_speakers >= 2);
    CHECK(result.num_speakers <= 5);
    CHECK(!result.segments.empty());
}
#endif

#if RECMEET_USE_SHERPA
TEST_CASE("VAD+Whisper vs plain Whisper transcription", "[benchmark]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path audio_path = root / "assets" / "biden_trump_debate_2020.wav";
    fs::path ref_path   = root / "assets" / "biden_trump_debate_2020.md";

    if (!fs::exists(audio_path))
        SKIP("Reference audio not found: " + audio_path.string());
    if (!fs::exists(ref_path))
        SKIP("Reference transcript not found: " + ref_path.string());
    if (!is_whisper_model_cached("base"))
        SKIP("Whisper base model not cached");
    if (!is_vad_model_cached())
        SKIP("Silero VAD model not cached");

    // Load reference transcript
    std::ifstream ref_file(ref_path);
    std::string ref_md((std::istreambuf_iterator<char>(ref_file)),
                        std::istreambuf_iterator<char>());
    std::string ref_text = strip_reference_transcript(ref_md);
    auto ref_words = tokenize_words(ref_text);
    REQUIRE(!ref_words.empty());

    // Load audio once
    auto samples = read_wav_float(audio_path);
    REQUIRE(!samples.empty());

    fs::path model_path = ensure_whisper_model("base");
    WhisperModel model(model_path);

    // --- Plain whisper (no VAD) ---
    auto t0 = std::chrono::steady_clock::now();
    auto plain_result = transcribe(model, samples.data(), samples.size(), 0.0, "en");
    auto t1 = std::chrono::steady_clock::now();
    double plain_secs = std::chrono::duration<double>(t1 - t0).count();

    std::string plain_text;
    for (const auto& seg : plain_result.segments)
        plain_text += seg.text + " ";
    auto plain_words = tokenize_words(plain_text);
    double plain_wer = compute_wer(ref_words, plain_words);

    // --- VAD + whisper ---
    auto t2 = std::chrono::steady_clock::now();
    VadConfig vad_cfg;
    auto vad_result = detect_speech(samples, vad_cfg);
    auto t3 = std::chrono::steady_clock::now();
    double vad_secs = std::chrono::duration<double>(t3 - t2).count();

    TranscriptResult vad_transcript;
    auto t4 = std::chrono::steady_clock::now();
    for (const auto& seg : vad_result.segments) {
        size_t n = static_cast<size_t>(seg.end_sample - seg.start_sample);
        auto seg_result = transcribe(model, samples.data() + seg.start_sample,
                                     n, seg.start, "en");
        for (auto& s : seg_result.segments)
            vad_transcript.segments.push_back(std::move(s));
    }
    auto t5 = std::chrono::steady_clock::now();
    double vad_transcribe_secs = std::chrono::duration<double>(t5 - t4).count();

    std::string vad_text;
    for (const auto& seg : vad_transcript.segments)
        vad_text += seg.text + " ";
    auto vad_words = tokenize_words(vad_text);
    double vad_wer = compute_wer(ref_words, vad_words);

    fprintf(stderr, "\n[benchmark] VAD+Whisper vs Plain Whisper (base):\n");
    fprintf(stderr, "  Plain:  WER=%.1f%%, time=%.1fs, segments=%zu\n",
            plain_wer * 100.0, plain_secs, plain_result.segments.size());
    fprintf(stderr, "  VAD:    WER=%.1f%%, time=%.1fs (vad=%.1fs + transcribe=%.1fs), "
            "segments=%zu, speech=%.1fs/%.1fs (%.0f%%)\n",
            vad_wer * 100.0, vad_secs + vad_transcribe_secs, vad_secs, vad_transcribe_secs,
            vad_transcript.segments.size(),
            vad_result.total_speech_duration, vad_result.total_audio_duration,
            100.0 * vad_result.total_speech_duration / vad_result.total_audio_duration);

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "\n      \"test\": \"vad_vs_plain_whisper\","
        "\n      \"model\": \"base\","
        "\n      \"plain_wer\": %.4f,"
        "\n      \"plain_time_secs\": %.1f,"
        "\n      \"plain_segments\": %zu,"
        "\n      \"vad_wer\": %.4f,"
        "\n      \"vad_time_secs\": %.1f,"
        "\n      \"vad_detect_secs\": %.1f,"
        "\n      \"vad_transcribe_secs\": %.1f,"
        "\n      \"vad_segments\": %zu,"
        "\n      \"speech_ratio\": %.2f",
        plain_wer, plain_secs, plain_result.segments.size(),
        vad_wer, vad_secs + vad_transcribe_secs, vad_secs, vad_transcribe_secs,
        vad_transcript.segments.size(),
        vad_result.total_speech_duration / vad_result.total_audio_duration);
    BenchmarkResults::add(buf);

    // VAD should not significantly degrade WER
    CHECK(vad_wer < 0.50);
}
#endif

#if RECMEET_USE_LLAMA
TEST_CASE("Summarize reference transcript with local LLM", "[benchmark]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path ref_path = root / "assets" / "biden_trump_debate_2020.md";
    if (!fs::exists(ref_path))
        SKIP("Reference transcript not found: " + ref_path.string());

    // Find LLM model
    fs::path llm_dir = models_dir() / "llama";
    fs::path llm_model;
    if (fs::is_directory(llm_dir)) {
        for (const auto& entry : fs::directory_iterator(llm_dir)) {
            if (entry.path().extension() == ".gguf") {
                llm_model = entry.path();
                break;
            }
        }
    }
    if (llm_model.empty())
        SKIP("No LLM .gguf model found in " + llm_dir.string());

    // Load full reference transcript — summarize_local() handles truncation internally
    std::ifstream ref_file(ref_path);
    std::string transcript((std::istreambuf_iterator<char>(ref_file)),
                            std::istreambuf_iterator<char>());
    REQUIRE(!transcript.empty());

    auto t0 = std::chrono::steady_clock::now();
    std::string summary = summarize_local(transcript, llm_model);
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    fprintf(stderr, "\n[benchmark] Local LLM summarization:\n");
    fprintf(stderr, "  Transcript size: %zu chars\n", transcript.size());
    fprintf(stderr, "  Summary size: %zu chars\n", summary.size());
    fprintf(stderr, "  Time: %.1fs\n", secs);

    int headings = 0;
    for (const char* h : {"Overview", "Key Points", "Decisions",
                           "Action Items", "Open Questions", "Participants"})
        if (summary.find(h) != std::string::npos) ++headings;

    char buf[512];
    snprintf(buf, sizeof(buf),
        "\n      \"test\": \"local_llm_summarization\","
        "\n      \"model\": \"%s\","
        "\n      \"transcript_chars\": %zu,"
        "\n      \"summary_chars\": %zu,"
        "\n      \"headings_found\": %d,"
        "\n      \"time_secs\": %.1f",
        llm_model.filename().c_str(), transcript.size(), summary.size(), headings, secs);
    BenchmarkResults::add(buf);

    CHECK(!summary.empty());
    CHECK(summary.find("Overview") != std::string::npos);
    CHECK(summary.find("Key Points") != std::string::npos);
    CHECK(summary.find("Decisions") != std::string::npos);
    CHECK(summary.find("Action Items") != std::string::npos);
    CHECK(summary.find("Open Questions") != std::string::npos);
    CHECK(summary.find("Participants") != std::string::npos);
}

TEST_CASE("Full pipeline: whisper transcribe then LLM summarize", "[benchmark]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path audio_path = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_path))
        SKIP("Reference audio not found: " + audio_path.string());
    if (!is_whisper_model_cached("base"))
        SKIP("Whisper base model not cached");

    fs::path llm_dir = models_dir() / "llama";
    fs::path llm_model;
    if (fs::is_directory(llm_dir)) {
        for (const auto& entry : fs::directory_iterator(llm_dir)) {
            if (entry.path().extension() == ".gguf") {
                llm_model = entry.path();
                break;
            }
        }
    }
    if (llm_model.empty())
        SKIP("No LLM .gguf model found in " + llm_dir.string());

    // Phase 1: Transcribe
    fs::path model_path = ensure_whisper_model("base");

    auto t0 = std::chrono::steady_clock::now();
    auto result = transcribe(model_path, audio_path, "en");
    auto t1 = std::chrono::steady_clock::now();
    double transcribe_secs = std::chrono::duration<double>(t1 - t0).count();

    std::string transcript_text = result.to_string();
    REQUIRE(!transcript_text.empty());

    // Phase 2: Summarize — summarize_local() handles context-window truncation internally
    auto t2 = std::chrono::steady_clock::now();
    std::string summary = summarize_local(transcript_text, llm_model);
    auto t3 = std::chrono::steady_clock::now();
    double summarize_secs = std::chrono::duration<double>(t3 - t2).count();

    fprintf(stderr, "\n[benchmark] Full pipeline (whisper base + local LLM):\n");
    fprintf(stderr, "  Transcription: %.1fs (%zu segments)\n",
            transcribe_secs, result.segments.size());
    fprintf(stderr, "  Summarization: %.1fs (%zu chars)\n",
            summarize_secs, summary.size());
    fprintf(stderr, "  Total: %.1fs\n", transcribe_secs + summarize_secs);

    int headings = 0;
    for (const char* h : {"Overview", "Key Points", "Decisions",
                           "Action Items", "Open Questions", "Participants"})
        if (summary.find(h) != std::string::npos) ++headings;

    char buf[512];
    snprintf(buf, sizeof(buf),
        "\n      \"test\": \"full_pipeline\","
        "\n      \"whisper_model\": \"base\","
        "\n      \"llm_model\": \"%s\","
        "\n      \"segments\": %zu,"
        "\n      \"summary_chars\": %zu,"
        "\n      \"headings_found\": %d,"
        "\n      \"transcribe_secs\": %.1f,"
        "\n      \"summarize_secs\": %.1f,"
        "\n      \"total_secs\": %.1f",
        llm_model.filename().c_str(), result.segments.size(),
        summary.size(), headings, transcribe_secs, summarize_secs,
        transcribe_secs + summarize_secs);
    BenchmarkResults::add(buf);

    CHECK(!summary.empty());
    CHECK(summary.find("Overview") != std::string::npos);
    CHECK(summary.find("Key Points") != std::string::npos);
    CHECK(summary.find("Decisions") != std::string::npos);
    CHECK(summary.find("Action Items") != std::string::npos);
    CHECK(summary.find("Open Questions") != std::string::npos);
    CHECK(summary.find("Participants") != std::string::npos);
}
#endif
