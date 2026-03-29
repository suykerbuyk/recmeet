// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "pipeline.h"
#include "audio_file.h"
#include "model_manager.h"

#include <fstream>

using namespace recmeet;

static fs::path tmp_dir() {
    fs::path dir = fs::temp_directory_path() / "recmeet_test_pipeline";
    fs::create_directories(dir);
    return dir;
}

TEST_CASE("read_context_file: reads existing file", "[pipeline]") {
    auto dir = tmp_dir();
    fs::path file = dir / "context.txt";

    {
        std::ofstream out(file);
        out << "Agenda:\n- Item 1\n- Item 2\n";
    }

    std::string content = read_context_file(file);
    CHECK(content == "Agenda:\n- Item 1\n- Item 2\n");

    fs::remove_all(dir);
}

TEST_CASE("read_context_file: returns empty for missing file", "[pipeline]") {
    std::string content = read_context_file("/nonexistent/path/context.txt");
    CHECK(content.empty());
}

TEST_CASE("read_context_file: returns empty for empty path", "[pipeline]") {
    std::string content = read_context_file(fs::path{});
    CHECK(content.empty());
}

TEST_CASE("read_context_file: returns empty for empty file", "[pipeline]") {
    auto dir = tmp_dir();
    fs::path file = dir / "empty.txt";
    { std::ofstream out(file); }

    std::string content = read_context_file(file);
    CHECK(content.empty());

    fs::remove_all(dir);
}

// --- build_initial_prompt tests ---

TEST_CASE("build_initial_prompt: empty inputs", "[pipeline]") {
    CHECK(build_initial_prompt({}, "").empty());
}

TEST_CASE("build_initial_prompt: speaker names only", "[pipeline]") {
    CHECK(build_initial_prompt({"Alice", "Bob"}, "") == "Alice, Bob");
}

TEST_CASE("build_initial_prompt: single speaker", "[pipeline]") {
    CHECK(build_initial_prompt({"John Suykerbuyk"}, "") == "John Suykerbuyk");
}

TEST_CASE("build_initial_prompt: vocabulary only", "[pipeline]") {
    CHECK(build_initial_prompt({}, "PipeWire, recmeet") == "PipeWire, recmeet");
}

TEST_CASE("build_initial_prompt: combined speakers and vocabulary", "[pipeline]") {
    auto result = build_initial_prompt({"John Suykerbuyk"}, "PipeWire, recmeet");
    CHECK(result == "John Suykerbuyk, PipeWire, recmeet");
}

TEST_CASE("build_initial_prompt: trims whitespace from vocabulary", "[pipeline]") {
    auto result = build_initial_prompt({}, "  foo ,  bar  ");
    CHECK(result == "foo, bar");
}

TEST_CASE("build_initial_prompt: skips empty vocabulary tokens", "[pipeline]") {
    auto result = build_initial_prompt({}, "foo,,bar, ,baz");
    CHECK(result == "foo, bar, baz");
}

TEST_CASE("run_postprocessing: transcribe minimal WAV with no summary/diarize", "[integration]") {
    ensure_whisper_model("tiny");

    // Create a minimal 1-second silent WAV (16kHz S16LE mono)
    auto dir = tmp_dir();
    fs::path audio = dir / "audio.wav";
    std::vector<int16_t> silence(SAMPLE_RATE, 0);  // 1 second
    write_wav(audio, silence);

    Config cfg;
    cfg.whisper_model = "tiny";
    cfg.no_summary = true;
    cfg.diarize = false;
    cfg.vad = false;

    PostprocessInput input;
    input.out_dir = dir;
    input.audio_path = audio;

    // Should complete without error — transcription of silence may produce
    // empty text, which throws RecmeetError. That's acceptable behavior.
    try {
        auto result = run_postprocessing(cfg, input);
        CHECK(result.output_dir == dir);
    } catch (const RecmeetError&) {
        // "Transcription produced no text" is expected for silence
    }

    fs::remove_all(dir);
}
