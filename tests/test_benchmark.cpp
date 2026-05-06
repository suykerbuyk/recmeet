// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.h"
#include "diarize.h"
#include "speaker_id.h"
#include "transcribe.h"
#include "vad.h"
#include "summarize.h"
#include "model_manager.h"
#include "audio_file.h"
#include "util.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <string>

using namespace recmeet;
using namespace recmeet::test_helpers;


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

TEST_CASE("Diarize debate audio with buffer overload", "[benchmark]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path audio_path = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_path))
        SKIP("Reference audio not found: " + audio_path.string());
    if (!is_sherpa_model_cached())
        SKIP("Sherpa diarization models not cached");

    auto samples = read_wav_float(audio_path);
    REQUIRE(!samples.empty());

    auto t0 = std::chrono::steady_clock::now();
    auto result = diarize(samples.data(), samples.size(), 3);
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    // Also run file-based for comparison
    auto file_result = diarize(audio_path, 3);

    fprintf(stderr, "\n[benchmark] Sherpa-onnx buffer diarization:\n");
    fprintf(stderr, "  Speakers detected: %d\n", result.num_speakers);
    fprintf(stderr, "  Segments: %zu\n", result.segments.size());
    fprintf(stderr, "  Time: %.1fs\n", secs);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "\n      \"test\": \"sherpa_diarization_buffer\","
        "\n      \"num_speakers_requested\": 3,"
        "\n      \"num_speakers_detected\": %d,"
        "\n      \"segments\": %zu,"
        "\n      \"time_secs\": %.1f",
        result.num_speakers, result.segments.size(), secs);
    BenchmarkResults::add(buf);

    CHECK(result.num_speakers >= 2);
    CHECK(result.num_speakers <= 5);
    CHECK(!result.segments.empty());

    // Buffer and file overloads should produce identical results
    CHECK(result.num_speakers == file_result.num_speakers);
    CHECK(result.segments.size() == file_result.segments.size());
}

// ---------------------------------------------------------------------------
// T2.0a regression tests — DiarizeSession + diarize_with_session
// ---------------------------------------------------------------------------

// The legacy `diarize()` wrapper and `diarize_with_session()` must produce
// identical output for the same audio + clustering parameters. Catches
// accidental divergence in the wrapper (parameter passing, defaults).
TEST_CASE("DiarizeSession: wrapper-vs-session parity on debate audio",
          "[benchmark][t2-0a]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path audio_path = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_path))
        SKIP("Reference audio not found: " + audio_path.string());
    if (!is_sherpa_model_cached())
        SKIP("Sherpa diarization models not cached");

    auto samples = read_wav_float(audio_path);
    REQUIRE(!samples.empty());

    // Path A: legacy wrapper.
    auto wrapper_result = diarize(samples.data(), samples.size(),
                                  3, 0, 1.18f);

    // Path B: explicit session.
    DiarizeSession session(0);
    session.set_clustering(3, 1.18f);
    auto session_result = diarize_with_session(session,
                                               samples.data(), samples.size());

    REQUIRE(session_result.num_speakers == wrapper_result.num_speakers);
    REQUIRE(session_result.segments.size() == wrapper_result.segments.size());
    for (size_t i = 0; i < wrapper_result.segments.size(); ++i) {
        CHECK(session_result.segments[i].start ==
              wrapper_result.segments[i].start);
        CHECK(session_result.segments[i].end ==
              wrapper_result.segments[i].end);
        CHECK(session_result.segments[i].speaker ==
              wrapper_result.segments[i].speaker);
    }
}

// Two back-to-back diarize calls on one session must both succeed and produce
// identical output (deterministic, no hidden state mutated by Process). This
// is the structural guarantee that lets T2.1 reuse one session across chunks.
TEST_CASE("DiarizeSession: back-to-back calls deterministic",
          "[benchmark][t2-0a]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path audio_path = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_path))
        SKIP("Reference audio not found: " + audio_path.string());
    if (!is_sherpa_model_cached())
        SKIP("Sherpa diarization models not cached");

    auto samples = read_wav_float(audio_path);
    REQUIRE(!samples.empty());

    DiarizeSession session(0);
    session.set_clustering(3, 1.18f);

    auto first  = diarize_with_session(session, samples.data(), samples.size());
    auto second = diarize_with_session(session, samples.data(), samples.size());

    REQUIRE(first.num_speakers == second.num_speakers);
    REQUIRE(first.segments.size() == second.segments.size());
    for (size_t i = 0; i < first.segments.size(); ++i) {
        CHECK(first.segments[i].start   == second.segments[i].start);
        CHECK(first.segments[i].end     == second.segments[i].end);
        CHECK(first.segments[i].speaker == second.segments[i].speaker);
    }
}

// `set_clustering` must round-trip: forcing a specific cluster count then
// switching back to auto-detect should be honored on each subsequent
// Process call. Validates the SherpaOnnxOfflineSpeakerDiarizationSetConfig
// path the chunked-diarization design relies on.
TEST_CASE("DiarizeSession: set_clustering round-trip",
          "[benchmark][t2-0a]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path audio_path = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_path))
        SKIP("Reference audio not found: " + audio_path.string());
    if (!is_sherpa_model_cached())
        SKIP("Sherpa diarization models not cached");

    auto samples = read_wav_float(audio_path);
    REQUIRE(!samples.empty());

    DiarizeSession session(0);

    // Force exactly 2 clusters.
    session.set_clustering(2, 1.18f);
    auto forced = diarize_with_session(session,
                                       samples.data(), samples.size());
    CHECK(forced.num_speakers == 2);

    // Switch to auto-detect — should yield ≥ 2 speakers on this fixture
    // (debate audio is multi-speaker). Importantly, the session must accept
    // a second SetConfig call without crashing or leaking the prior state.
    session.set_clustering(-1, 1.18f);
    auto autodetect = diarize_with_session(session,
                                           samples.data(), samples.size());
    CHECK(autodetect.num_speakers >= 2);
    CHECK(!autodetect.segments.empty());
}

// ---------------------------------------------------------------------------
// T2.0b regression tests — SpeakerEmbeddingSession + session-overload
// ---------------------------------------------------------------------------

// The legacy `extract_speaker_embedding(samples, ..., model_path)` overload
// must produce a byte-identical embedding to the session-overload — the
// legacy path is now a thin wrapper that constructs a session and forwards.
// Catches accidental divergence in the wrapper (parameter passing, defaults).
TEST_CASE("SpeakerEmbeddingSession: legacy-vs-session parity on debate audio",
          "[benchmark][t2-0b]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path audio_path = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_path))
        SKIP("Reference audio not found: " + audio_path.string());
    if (!is_sherpa_model_cached())
        SKIP("Sherpa diarization models not cached");

    auto samples = read_wav_float(audio_path);
    REQUIRE(!samples.empty());

    // Need a DiarizeResult to drive the segment selection inside extraction.
    auto diar = diarize(samples.data(), samples.size(), 3, 0, 1.18f);
    REQUIRE(!diar.segments.empty());
    REQUIRE(diar.num_speakers >= 1);

    auto model_paths = ensure_sherpa_models();

    // Path A: legacy wrapper.
    auto legacy = extract_speaker_embedding(
        samples.data(), samples.size(), diar, 0, model_paths.embedding);

    // Path B: explicit session.
    SpeakerEmbeddingSession session(model_paths.embedding, 0);
    auto via_session = extract_speaker_embedding(
        session, samples.data(), samples.size(), diar, 0);

    REQUIRE(!legacy.empty());
    REQUIRE(legacy.size() == via_session.size());
    REQUIRE(static_cast<int>(legacy.size()) == session.dim());
    for (size_t i = 0; i < legacy.size(); ++i) {
        CHECK(legacy[i] == via_session[i]);
    }
}

// Two back-to-back extractions on one session must both succeed and produce
// identical output. Structural guarantee that lets T2.1 reuse one embedding
// session across chunks (one extract per chunk-local speaker).
TEST_CASE("SpeakerEmbeddingSession: back-to-back calls deterministic",
          "[benchmark][t2-0b]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path audio_path = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_path))
        SKIP("Reference audio not found: " + audio_path.string());
    if (!is_sherpa_model_cached())
        SKIP("Sherpa diarization models not cached");

    auto samples = read_wav_float(audio_path);
    REQUIRE(!samples.empty());

    auto diar = diarize(samples.data(), samples.size(), 3, 0, 1.18f);
    REQUIRE(!diar.segments.empty());

    auto model_paths = ensure_sherpa_models();
    SpeakerEmbeddingSession session(model_paths.embedding, 0);

    auto first  = extract_speaker_embedding(session, samples.data(), samples.size(), diar, 0);
    auto second = extract_speaker_embedding(session, samples.data(), samples.size(), diar, 0);

    REQUIRE(first.size() == second.size());
    REQUIRE(!first.empty());
    for (size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i] == second[i]);
    }
}

// `ComputeEmbedding` returns *raw* model output, not L2-normalized. T2.1's
// stitching threshold (cosine similarity ≥ 0.6 on unit vectors) only holds
// if callers normalize first — this test codifies the spike finding as an
// executable invariant so a future sherpa upgrade that silently switches to
// pre-normalized output is caught by the test suite.
TEST_CASE("extract_speaker_embedding: raw output not unit-norm",
          "[benchmark][t2-0b]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");

    fs::path audio_path = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_path))
        SKIP("Reference audio not found: " + audio_path.string());
    if (!is_sherpa_model_cached())
        SKIP("Sherpa diarization models not cached");

    auto samples = read_wav_float(audio_path);
    REQUIRE(!samples.empty());

    auto diar = diarize(samples.data(), samples.size(), 3, 0, 1.18f);
    REQUIRE(!diar.segments.empty());

    auto model_paths = ensure_sherpa_models();
    SpeakerEmbeddingSession session(model_paths.embedding, 0);
    auto embedding = extract_speaker_embedding(
        session, samples.data(), samples.size(), diar, 0);

    REQUIRE(!embedding.empty());

    double sum_sq = 0.0;
    for (float v : embedding) sum_sq += static_cast<double>(v) * v;
    double norm = std::sqrt(sum_sq);

    // Raw embeddings observed at norms ~5–25 across speakers; assert clearly
    // distinguishable from unit norm. If sherpa upstream starts pre-normalizing,
    // this fires and T2.1's normalization step can be revisited.
    CHECK(std::abs(norm - 1.0) > 0.01);
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
