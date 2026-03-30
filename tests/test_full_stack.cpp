// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.h"
#include "pipeline.h"
#include "speaker_id.h"
#include "model_manager.h"
#include "config.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace recmeet;
using namespace recmeet::test_helpers;

namespace {

// ---------------------------------------------------------------------------
// Debate context injected into the pipeline
// ---------------------------------------------------------------------------

const char* const kDebateContext =
    "Subject: 2020 Presidential Debate\n"
    "Participants: Joe Biden (Democratic candidate), Donald Trump (Republican incumbent), "
    "Chris Wallace (Fox News moderator)\n"
    "Date: September 29, 2020\n"
    "Topics: Supreme Court nomination, Affordable Care Act, COVID-19 response\n"
    "\n"
    "This is the first of the 2020 presidential debates held at Case Western "
    "Reserve University in Cleveland, Ohio.";

// ---------------------------------------------------------------------------
// Result struct for debate pipeline run
// ---------------------------------------------------------------------------

struct DebateResult {
    PipelineResult pipeline;
    std::string note_content;
    fs::path out_dir;
    std::map<std::string, double> phase_durations;  // phase -> seconds
    bool summarized = false;  // true if summarization was enabled
};

// ---------------------------------------------------------------------------
// run_debate_pipeline — shared helper for TEST_CASEs 1 & 2
// ---------------------------------------------------------------------------

DebateResult run_debate_pipeline(bool use_local_llm, const fs::path& llm_model_path = {}) {
    fs::path root = find_project_root();
    REQUIRE(!root.empty());

    fs::path audio_src = root / "assets" / "biden_trump_debate_2020.wav";
    REQUIRE(fs::exists(audio_src));

    // Create temp directory
    auto out_dir = fs::temp_directory_path() / "recmeet_fullstack_test";
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);

    // Copy audio with timestamped name for resolve_meeting_time()
    fs::path audio_path = out_dir / "audio_2020-09-29_21-00.wav";
    fs::copy_file(audio_src, audio_path);

    // Configure
    Config cfg;
    cfg.whisper_model = "base";
    cfg.language = "en";
    cfg.diarize = true;
    cfg.num_speakers = 3;
    cfg.speaker_id = false;
    cfg.vad = true;
    cfg.context_inline = kDebateContext;
    cfg.output_dir = out_dir;
    cfg.output_dir_explicit = true;
    cfg.note_dir = out_dir;

    if (use_local_llm) {
        cfg.llm_model = llm_model_path.string();
    } else {
        const char* key = std::getenv("XAI_API_KEY");
        if (key && key[0]) {
            cfg.provider = "xai";
            cfg.api_key = key;
        } else {
            cfg.no_summary = true;
        }
    }

    // Phase timing
    std::map<std::string, std::chrono::steady_clock::time_point> phase_starts;
    std::map<std::string, double> phase_durations;
    auto on_phase = [&](const std::string& phase) {
        auto now = std::chrono::steady_clock::now();
        // Close previous phase
        if (!phase_starts.empty()) {
            auto& last = *phase_starts.rbegin();
            phase_durations[last.first] =
                std::chrono::duration<double>(now - last.second).count();
        }
        phase_starts[phase] = now;
    };

    PostprocessInput input;
    input.out_dir = out_dir;
    input.audio_path = audio_path;
    // transcript_text empty — triggers full transcription

    auto result = run_postprocessing(cfg, input, on_phase);

    // Close final phase
    if (!phase_starts.empty()) {
        auto now = std::chrono::steady_clock::now();
        auto& last = *phase_starts.rbegin();
        phase_durations[last.first] =
            std::chrono::duration<double>(now - last.second).count();
    }

    // Read note content
    std::string note_content;
    if (fs::exists(result.note_path)) {
        std::ifstream f(result.note_path);
        note_content.assign(std::istreambuf_iterator<char>(f),
                            std::istreambuf_iterator<char>());
    }

    return {result, note_content, out_dir, phase_durations, !cfg.no_summary};
}

} // anonymous namespace


// ---------------------------------------------------------------------------
// TEST_CASE 1: Full pipeline with API summary
// ---------------------------------------------------------------------------

TEST_CASE("Full pipeline: debate audio with API summary", "[full-stack][benchmark]") {
    // Prerequisites
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");
    if (!fs::exists(root / "assets" / "biden_trump_debate_2020.wav"))
        SKIP("Debate audio asset not found");
    if (!is_whisper_model_cached("base"))
        SKIP("Whisper base model not cached");
    if (!is_sherpa_model_cached())
        SKIP("Sherpa diarization models not cached");
    if (!is_vad_model_cached())
        SKIP("VAD model not cached");

    auto dr = run_debate_pipeline(/*use_local_llm=*/false);

    // --- 1. Transcription quality ---
    INFO("Checking transcription quality (WER)");
    REQUIRE(!dr.pipeline.transcript_text.empty());

    std::string stripped = strip_transcript_labels(dr.pipeline.transcript_text);
    auto hyp_words = tokenize_words(stripped);
    REQUIRE(!hyp_words.empty());

    // Load reference transcript
    fs::path ref_path = root / "assets" / "biden_trump_debate_2020.md";
    REQUIRE(fs::exists(ref_path));
    std::ifstream ref_file(ref_path);
    std::string ref_md((std::istreambuf_iterator<char>(ref_file)),
                        std::istreambuf_iterator<char>());
    std::string ref_text = strip_reference_transcript(ref_md);
    auto ref_words = tokenize_words(ref_text);
    REQUIRE(!ref_words.empty());

    double wer = compute_wer(ref_words, hyp_words);
    fprintf(stderr, "\n[full-stack] WER: %.1f%% (ref=%zu, hyp=%zu)\n",
            wer * 100.0, ref_words.size(), hyp_words.size());
    CHECK(wer < 0.45);

    // --- 2. Diarization ---
    INFO("Checking diarization results");
    auto speakers = load_meeting_speakers(dr.out_dir);
    CHECK(speakers.size() >= 2);
    CHECK(speakers.size() <= 5);
    for (const auto& s : speakers) {
        INFO("Speaker: " << s.label << " duration=" << s.duration_sec);
        CHECK(s.duration_sec > 0.0f);
    }

    // --- 3. Summary structure (only when summarization ran) ---
    if (dr.summarized) {
        INFO("Checking summary headings in note");
        // Headings may be inside callout blocks (> ## Overview)
        CHECK(dr.note_content.find("## Overview") != std::string::npos);
        CHECK(dr.note_content.find("## Key Points") != std::string::npos);
        CHECK(dr.note_content.find("## Decisions") != std::string::npos);
        CHECK(dr.note_content.find("## Action Items") != std::string::npos);
        CHECK(dr.note_content.find("## Open Questions") != std::string::npos);
        CHECK(dr.note_content.find("## Participants") != std::string::npos);

        // --- 4. Metadata in frontmatter (AI-derived, requires summary) ---
        INFO("Checking frontmatter metadata");
        CHECK(dr.note_content.find("title:") != std::string::npos);
        bool has_relevant_tag =
            dr.note_content.find("debate") != std::string::npos ||
            dr.note_content.find("politics") != std::string::npos ||
            dr.note_content.find("presidential") != std::string::npos ||
            dr.note_content.find("healthcare") != std::string::npos ||
            dr.note_content.find("supreme-court") != std::string::npos;
        CHECK(has_relevant_tag);
        CHECK(dr.note_content.find("participants:") != std::string::npos);
    } else {
        fprintf(stderr, "  [full-stack] Summarization skipped (no API key)\n");
    }

    // --- 5. Note file structure ---
    INFO("Checking note file structure");
    CHECK(fs::exists(dr.pipeline.note_path));
    CHECK(dr.note_content.find("type: meeting") != std::string::npos);
    CHECK(dr.note_content.find("status: processed") != std::string::npos);
    CHECK(dr.note_content.find("whisper_model: base") != std::string::npos);
    CHECK(dr.note_content.find("date: 2020-09-29") != std::string::npos);

    // --- 6. Context in note ---
    INFO("Checking context presence in note");
    CHECK(dr.note_content.find("Pre-Meeting Context") != std::string::npos);
    CHECK(dr.note_content.find("2020 Presidential Debate") != std::string::npos);

    // --- 7. Context persistence ---
    INFO("Checking context.json persistence");
    CHECK(fs::exists(dr.out_dir / "context.json"));
    std::string loaded_ctx = load_meeting_context(dr.out_dir);
    CHECK(!loaded_ctx.empty());

    // --- 8. Timing metrics ---
    INFO("Recording phase timing metrics");
    for (const auto& [phase, secs] : dr.phase_durations) {
        fprintf(stderr, "  [full-stack] %s: %.1fs\n", phase.c_str(), secs);
    }

    // Write to benchmark results
    std::string json;
    json += "\n      \"test\": \"full_stack_api\",";
    json += "\n      \"wer\": " + std::to_string(wer) + ",";
    json += "\n      \"speakers\": " + std::to_string(speakers.size()) + ",";
    for (const auto& [phase, secs] : dr.phase_durations) {
        json += "\n      \"" + phase + "_sec\": " + std::to_string(secs) + ",";
    }
    // Remove trailing comma
    if (!json.empty() && json.back() == ',')
        json.pop_back();
    BenchmarkResults::add(json);

    // Cleanup
    fs::remove_all(dr.out_dir);
}


// ---------------------------------------------------------------------------
// TEST_CASE 2: Full pipeline with local LLM summary
// ---------------------------------------------------------------------------

#if RECMEET_USE_LLAMA
TEST_CASE("Full pipeline: debate audio with local LLM summary", "[full-stack][benchmark]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");
    if (!fs::exists(root / "assets" / "biden_trump_debate_2020.wav"))
        SKIP("Debate audio asset not found");
    if (!is_whisper_model_cached("base"))
        SKIP("Whisper base model not cached");
    if (!is_sherpa_model_cached())
        SKIP("Sherpa diarization models not cached");
    if (!is_vad_model_cached())
        SKIP("VAD model not cached");

    // Find a .gguf model file
    fs::path models_dir = fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".") /
                          ".cache" / "recmeet" / "models";
    fs::path gguf_path;
    if (fs::exists(models_dir)) {
        for (const auto& entry : fs::directory_iterator(models_dir)) {
            if (entry.path().extension() == ".gguf") {
                gguf_path = entry.path();
                break;
            }
        }
    }
    if (gguf_path.empty())
        SKIP("No .gguf model found in " + models_dir.string());

    auto dr = run_debate_pipeline(/*use_local_llm=*/true, gguf_path);

    // Same assertions as TEST_CASE 1
    INFO("Checking transcription quality (WER)");
    REQUIRE(!dr.pipeline.transcript_text.empty());

    std::string stripped = strip_transcript_labels(dr.pipeline.transcript_text);
    auto hyp_words = tokenize_words(stripped);
    REQUIRE(!hyp_words.empty());

    fs::path ref_path = root / "assets" / "biden_trump_debate_2020.md";
    REQUIRE(fs::exists(ref_path));
    std::ifstream ref_file(ref_path);
    std::string ref_md((std::istreambuf_iterator<char>(ref_file)),
                        std::istreambuf_iterator<char>());
    std::string ref_text = strip_reference_transcript(ref_md);
    auto ref_words = tokenize_words(ref_text);
    REQUIRE(!ref_words.empty());

    double wer = compute_wer(ref_words, hyp_words);
    fprintf(stderr, "\n[full-stack] Local LLM WER: %.1f%% (ref=%zu, hyp=%zu)\n",
            wer * 100.0, ref_words.size(), hyp_words.size());
    CHECK(wer < 0.45);

    auto speakers = load_meeting_speakers(dr.out_dir);
    CHECK(speakers.size() >= 2);
    CHECK(speakers.size() <= 5);

    CHECK(dr.note_content.find("## Overview") != std::string::npos);
    CHECK(fs::exists(dr.pipeline.note_path));
    CHECK(dr.note_content.find("type: meeting") != std::string::npos);
    CHECK(dr.note_content.find("Pre-Meeting Context") != std::string::npos);
    CHECK(dr.note_content.find("2020 Presidential Debate") != std::string::npos);

    CHECK(fs::exists(dr.out_dir / "context.json"));
    CHECK(!load_meeting_context(dr.out_dir).empty());

    for (const auto& [phase, secs] : dr.phase_durations) {
        fprintf(stderr, "  [full-stack] %s: %.1fs\n", phase.c_str(), secs);
    }

    std::string json;
    json += "\n      \"test\": \"full_stack_local_llm\",";
    json += "\n      \"wer\": " + std::to_string(wer) + ",";
    json += "\n      \"speakers\": " + std::to_string(speakers.size());
    BenchmarkResults::add(json);

    fs::remove_all(dr.out_dir);
}
#endif // RECMEET_USE_LLAMA


// ---------------------------------------------------------------------------
// TEST_CASE 3: Reprocess with context.json fallback (lightweight, no summary)
// ---------------------------------------------------------------------------

TEST_CASE("Reprocess with context.json fallback", "[full-stack]") {
    fs::path root = find_project_root();
    if (root.empty())
        SKIP("Project root with assets/ not found");
    if (!fs::exists(root / "assets" / "biden_trump_debate_2020.wav"))
        SKIP("Debate audio asset not found");
    if (!is_whisper_model_cached("base"))
        SKIP("Whisper base model not cached");

    // Create temp directory
    auto out_dir = fs::temp_directory_path() / "recmeet_reprocess_test";
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);

    // Copy audio
    fs::path audio_src = root / "assets" / "biden_trump_debate_2020.wav";
    fs::path audio_path = out_dir / "audio_2020-09-29_21-00.wav";
    fs::copy_file(audio_src, audio_path);

    // Write context.json (simulating a prior recording session)
    save_meeting_context(out_dir, "Reprocess test context");

    // Configure for reprocess — minimal pipeline
    Config cfg;
    cfg.whisper_model = "base";
    cfg.language = "en";
    cfg.reprocess_dir = out_dir;
    cfg.output_dir = out_dir;
    cfg.output_dir_explicit = true;
    cfg.note_dir = out_dir;
    cfg.no_summary = true;
    cfg.diarize = false;
    cfg.vad = false;
    cfg.speaker_id = false;
    cfg.context_inline = "";  // empty — should pick up context.json

    PostprocessInput input;
    input.out_dir = out_dir;
    input.audio_path = audio_path;

    auto result = run_postprocessing(cfg, input);

    // Read note
    REQUIRE(fs::exists(result.note_path));
    std::ifstream f(result.note_path);
    std::string note((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    INFO("Note content:\n" << note.substr(0, 500));

    CHECK(note.find("Pre-Meeting Context") != std::string::npos);
    CHECK(note.find("Reprocess test context") != std::string::npos);

    // Verify transcript_text is populated
    CHECK(!result.transcript_text.empty());

    // Cleanup
    fs::remove_all(out_dir);
}
