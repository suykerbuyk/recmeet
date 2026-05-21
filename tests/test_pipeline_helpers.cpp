// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "pipeline.h"
#include "audio_file.h"
#include "model_manager.h"
#include "test_tmpdir.h"

#include <fstream>

using namespace recmeet;

static fs::path tmp_dir() {
    fs::path dir = recmeet::test::tmp_path("recmeet_test_pipeline");
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

// --- save/load meeting context tests ---

TEST_CASE("save_meeting_context + load_meeting_context round-trip", "[pipeline]") {
    auto dir = tmp_dir();

    save_meeting_context(dir, "Subject: Standup\nParticipants: Alice, Bob");
    std::string loaded = load_meeting_context(dir);
    CHECK(loaded == "Subject: Standup\nParticipants: Alice, Bob");

    fs::remove_all(dir);
}

TEST_CASE("save_meeting_context with context_file", "[pipeline]") {
    auto dir = tmp_dir();

    save_meeting_context(dir, "inline notes", "/tmp/agenda.txt");
    std::string loaded = load_meeting_context(dir);
    CHECK(loaded == "inline notes");

    // Verify context.json exists (legacy filename written when timestamp empty)
    CHECK(fs::exists(dir / "context.json"));

    fs::remove_all(dir);
}

TEST_CASE("save_meeting_context: timestamp arg writes per-instance file", "[pipeline]") {
    auto dir = tmp_dir();

    save_meeting_context(dir, "Standup notes", {}, "2026-05-07_10-30");

    // Per-instance file is written; legacy filename is NOT.
    CHECK(fs::exists(dir / "context_2026-05-07_10-30.json"));
    CHECK_FALSE(fs::exists(dir / "context.json"));

    // load_meeting_context resolves the per-instance file.
    CHECK(load_meeting_context(dir) == "Standup notes");

    fs::remove_all(dir);
}

TEST_CASE("load_meeting_context: prefers per-instance over legacy", "[pipeline]") {
    auto dir = tmp_dir();

    // Write legacy first.
    save_meeting_context(dir, "older legacy context");
    REQUIRE(fs::exists(dir / "context.json"));

    // Write per-instance with newer content.
    save_meeting_context(dir, "newer per-instance context", {}, "2026-05-07_10-30");
    REQUIRE(fs::exists(dir / "context_2026-05-07_10-30.json"));

    // load_meeting_context (which uses find_context_file) prefers the per-instance file.
    CHECK(load_meeting_context(dir) == "newer per-instance context");

    fs::remove_all(dir);
}

TEST_CASE("load_meeting_context: returns empty for missing dir", "[pipeline]") {
    CHECK(load_meeting_context("/nonexistent/dir").empty());
}

TEST_CASE("save_meeting_context: no-op when both empty", "[pipeline]") {
    auto dir = tmp_dir();
    save_meeting_context(dir, "");
    CHECK_FALSE(fs::exists(dir / "context.json"));
    fs::remove_all(dir);
}

TEST_CASE("save_meeting_context: special characters round-trip", "[pipeline]") {
    auto dir = tmp_dir();
    std::string ctx = "Agenda: \"Q1 Review\"\nBudget: $1,000\\item";
    save_meeting_context(dir, ctx);
    CHECK(load_meeting_context(dir) == ctx);
    fs::remove_all(dir);
}

TEST_CASE("load_meeting_context: returns empty for malformed JSON", "[pipeline]") {
    auto dir = tmp_dir();
    {
        std::ofstream out(dir / "context.json");
        out << "not valid json";
    }
    CHECK(load_meeting_context(dir).empty());
    fs::remove_all(dir);
}

TEST_CASE("load_meeting_context: returns empty when context key missing", "[pipeline]") {
    auto dir = tmp_dir();
    {
        std::ofstream out(dir / "context.json");
        out << "{\"other_key\":\"value\"}";
    }
    CHECK(load_meeting_context(dir).empty());
    fs::remove_all(dir);
}

// --- Phase C.11 — meeting_id round-trip in context.json -------------------

TEST_CASE("save_meeting_context: omits meeting_id field when empty (v1 shape preserved)",
          "[pipeline][c11]") {
    auto dir = tmp_dir();
    save_meeting_context(dir, "hello", {}, "", /*meeting_id=*/"");

    std::ifstream in(dir / "context.json");
    std::ostringstream buf; buf << in.rdbuf();
    // v1 readers must see byte-for-byte the same shape on the no-id path.
    CHECK(buf.str() == "{\"context\":\"hello\",\"context_file\":\"\"}");

    fs::remove_all(dir);
}

TEST_CASE("save_meeting_context + load_meeting_id round-trip", "[pipeline][c11]") {
    auto dir = tmp_dir();
    const std::string id = "12345678-1234-4567-89ab-1234567890ab"; // valid UUID v4

    save_meeting_context(dir, "ctx body", {}, "", id);
    CHECK(load_meeting_id(dir) == id);
    // Context text still loads cleanly alongside.
    CHECK(load_meeting_context(dir) == "ctx body");

    fs::remove_all(dir);
}

TEST_CASE("save_meeting_context: persists meeting_id when only id is set "
          "(no inline context, no context_file)",
          "[pipeline][c11]") {
    auto dir = tmp_dir();
    const std::string id = "abcdef01-2345-4678-9abc-def012345678";

    save_meeting_context(dir, "", {}, "", id);
    REQUIRE(fs::exists(dir / "context.json"));
    CHECK(load_meeting_id(dir) == id);
    CHECK(load_meeting_context(dir).empty());

    fs::remove_all(dir);
}

TEST_CASE("load_meeting_id: returns empty for v1-written context.json "
          "(no meeting_id field)",
          "[pipeline][c11]") {
    auto dir = tmp_dir();
    save_meeting_context(dir, "v1 context"); // no id passed
    CHECK(load_meeting_id(dir).empty());
    // Context still loads.
    CHECK(load_meeting_context(dir) == "v1 context");
    fs::remove_all(dir);
}

TEST_CASE("load_meeting_id: rejects a malformed stored value rather than poison the index",
          "[pipeline][c11]") {
    auto dir = tmp_dir();
    {
        std::ofstream out(dir / "context.json");
        // Wrong layout — not a UUID v4. Must NOT propagate into the index.
        out << "{\"context\":\"\",\"context_file\":\"\","
               "\"meeting_id\":\"not-a-real-uuid\"}";
    }
    CHECK(load_meeting_id(dir).empty());
    fs::remove_all(dir);
}

TEST_CASE("load_meeting_id: returns empty for missing dir", "[pipeline][c11]") {
    CHECK(load_meeting_id("/nonexistent/dir").empty());
}

TEST_CASE("save_meeting_context: timestamp + meeting_id together write per-instance file",
          "[pipeline][c11]") {
    auto dir = tmp_dir();
    const std::string id = "00000000-0000-4000-8000-000000000001";

    save_meeting_context(dir, "ctx", {}, "2026-05-16_12-34", id);

    CHECK(fs::exists(dir / "context_2026-05-16_12-34.json"));
    CHECK_FALSE(fs::exists(dir / "context.json"));
    CHECK(load_meeting_id(dir) == id);
    CHECK(load_meeting_context(dir) == "ctx");

    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Phase B.0 — `resolve_context_text` precedence
// ---------------------------------------------------------------------------

TEST_CASE("resolve_context_text: inline only returns inline",
          "[pipeline][context-resolve]") {
    auto dir = tmp_dir();
    JobConfig cfg;
    cfg.context_inline = "Participants: Alice, Bob";
    CHECK(resolve_context_text(cfg, dir) == "Participants: Alice, Bob");
    fs::remove_all(dir);
}

TEST_CASE("resolve_context_text: inline + file concatenated with separator",
          "[pipeline][context-resolve]") {
    auto dir = tmp_dir();
    fs::path file = dir / "agenda.txt";
    {
        std::ofstream out(file);
        out << "Agenda:\n- Item 1\n";
    }
    JobConfig cfg;
    cfg.context_inline = "Subject: Standup";
    cfg.context_file = file;
    std::string got = resolve_context_text(cfg, dir);
    // Pre-Phase-B summarizer-prep semantics: file appended with "\n\n"
    // when inline is non-empty.
    CHECK(got == "Subject: Standup\n\nAgenda:\n- Item 1\n");
    fs::remove_all(dir);
}

TEST_CASE("resolve_context_text: reprocess with saved context.json (no inline)",
          "[pipeline][context-resolve]") {
    auto dir = tmp_dir();
    save_meeting_context(dir, "Saved: Q1 review notes");
    REQUIRE(fs::exists(dir / "context.json"));

    JobConfig cfg;
    cfg.reprocess_dir = dir;  // marks this as a reprocess

    CHECK(resolve_context_text(cfg, dir) == "Saved: Q1 review notes");
    fs::remove_all(dir);
}

TEST_CASE("resolve_context_text: inline overrides saved context.json",
          "[pipeline][context-resolve]") {
    auto dir = tmp_dir();
    save_meeting_context(dir, "Saved on-disk content");
    REQUIRE(fs::exists(dir / "context.json"));

    JobConfig cfg;
    cfg.context_inline = "Fresh inline content";
    cfg.reprocess_dir = dir;  // reprocess + inline together

    // Inline wins; saved context.json is NOT consulted when inline non-empty.
    CHECK(resolve_context_text(cfg, dir) == "Fresh inline content");
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Phase B.2 — `resolve_target_speakers` precedence chain
// ---------------------------------------------------------------------------

TEST_CASE("resolve_target_speakers: --num-speakers wins over context",
          "[pipeline][target-speakers]") {
    const char* source = nullptr;
    int target = resolve_target_speakers(
        /*cli_num_speakers=*/3,
        /*context_speaker_count=*/4,
        /*max_auto_speakers=*/8,
        &source);
    CHECK(target == 3);
    REQUIRE(source != nullptr);
    CHECK(std::string(source) == "--num-speakers");
}

TEST_CASE("resolve_target_speakers: context wins when --num-speakers is 0",
          "[pipeline][target-speakers]") {
    const char* source = nullptr;
    int target = resolve_target_speakers(
        /*cli_num_speakers=*/0,
        /*context_speaker_count=*/4,
        /*max_auto_speakers=*/8,
        &source);
    CHECK(target == 4);
    REQUIRE(source != nullptr);
    CHECK(std::string(source) == "context");
}

TEST_CASE("resolve_target_speakers: max_auto fallback when both 0",
          "[pipeline][target-speakers]") {
    const char* source = nullptr;
    int target = resolve_target_speakers(
        /*cli_num_speakers=*/0,
        /*context_speaker_count=*/0,
        /*max_auto_speakers=*/8,
        &source);
    CHECK(target == 8);
    REQUIRE(source != nullptr);
    CHECK(std::string(source) == "max_auto");
}

TEST_CASE("resolve_target_speakers: max_auto override honored",
          "[pipeline][target-speakers]") {
    const char* source = nullptr;
    int target = resolve_target_speakers(
        /*cli_num_speakers=*/0,
        /*context_speaker_count=*/0,
        /*max_auto_speakers=*/12,
        &source);
    CHECK(target == 12);
    CHECK(std::string(source) == "max_auto");
}

TEST_CASE("resolve_target_speakers: source_out can be omitted",
          "[pipeline][target-speakers]") {
    CHECK(resolve_target_speakers(3, 4, 8, nullptr) == 3);
    CHECK(resolve_target_speakers(0, 4, 8, nullptr) == 4);
    CHECK(resolve_target_speakers(0, 0, 8, nullptr) == 8);
}

TEST_CASE("parse_context_participants: Phase B stub returns 0",
          "[pipeline][target-speakers]") {
    // The Phase C parser body is not yet wired (this brief landed Phase B).
    // The stub returns 0 so the precedence chain falls through to max_auto.
    // Phase C's regression coverage will land actual-parse tests.
    CHECK(parse_context_participants("") == 0);
    CHECK(parse_context_participants("Participants: Alice, Bob") == 0);
}

TEST_CASE("run_postprocessing: transcribe minimal WAV with no summary/diarize", "[integration]") {
    ensure_whisper_model("tiny");

    // Create a minimal 1-second silent WAV (16kHz S16LE mono)
    auto dir = tmp_dir();
    fs::path audio = dir / "audio.wav";
    std::vector<int16_t> silence(SAMPLE_RATE, 0);  // 1 second
    write_wav(audio, silence);

    JobConfig cfg;
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
