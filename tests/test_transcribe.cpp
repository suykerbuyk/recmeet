// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "transcribe.h"

#include <type_traits>

using namespace recmeet;

TEST_CASE("to_string: empty segments returns empty string", "[transcribe]") {
    TranscriptResult result;
    CHECK(result.to_string().empty());
}

TEST_CASE("to_string: single segment formats correctly", "[transcribe]") {
    TranscriptResult result;
    result.segments.push_back({65.0, 125.0, "Hello"});
    CHECK(result.to_string() == "[01:05 - 02:05] Hello\n");
}

TEST_CASE("to_string: multiple segments joined with newlines", "[transcribe]") {
    TranscriptResult result;
    result.segments.push_back({0.0, 5.0, "First"});
    result.segments.push_back({5.0, 10.0, "Second"});
    result.segments.push_back({10.0, 15.0, "Third"});

    std::string expected =
        "[00:00 - 00:05] First\n"
        "[00:05 - 00:10] Second\n"
        "[00:10 - 00:15] Third\n";
    CHECK(result.to_string() == expected);
}

TEST_CASE("to_string: zero-second timestamp", "[transcribe]") {
    TranscriptResult result;
    result.segments.push_back({0.0, 0.0, "Start"});
    CHECK(result.to_string() == "[00:00 - 00:00] Start\n");
}

TEST_CASE("to_string: large timestamps overflow minutes", "[transcribe]") {
    TranscriptResult result;
    result.segments.push_back({3661.0, 3722.0, "Late"});
    CHECK(result.to_string() == "[61:01 - 62:02] Late\n");
}

TEST_CASE("to_string: fractional seconds truncated", "[transcribe]") {
    TranscriptResult result;
    result.segments.push_back({1.9, 2.1, "Hi"});
    CHECK(result.to_string() == "[00:01 - 00:02] Hi\n");
}

// ---------------------------------------------------------------------------
// WhisperModel
// ---------------------------------------------------------------------------

TEST_CASE("WhisperModel: invalid path throws RecmeetError", "[transcribe]") {
    CHECK_THROWS_AS(WhisperModel("/no/such/model.gguf"), RecmeetError);
}

TEST_CASE("WhisperModel: error message includes model path", "[transcribe]") {
    try {
        WhisperModel("/no/such/model.gguf");
        FAIL("Expected RecmeetError");
    } catch (const RecmeetError& e) {
        CHECK(std::string(e.what()).find("/no/such/model.gguf") != std::string::npos);
    }
}

TEST_CASE("WhisperModel: non-copyable, noexcept-movable", "[transcribe]") {
    static_assert(std::is_nothrow_move_constructible_v<WhisperModel>);
    static_assert(std::is_nothrow_move_assignable_v<WhisperModel>);
    static_assert(!std::is_copy_constructible_v<WhisperModel>);
    static_assert(!std::is_copy_assignable_v<WhisperModel>);
}
