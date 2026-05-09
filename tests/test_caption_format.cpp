// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 5.5 — render-time caption normalization tests.
//
// Hermetic — no model, no IPC, no GTK. Validates the pure string-manipulation
// helper used by both the tray overlay and the CLI stderr renderer.

#include <catch2/catch_test_macros.hpp>

#include "caption_format.h"

using namespace recmeet;

TEST_CASE("normalize_caption: empty input", "[caption-format]") {
    REQUIRE(normalize_caption("") == "");
}

TEST_CASE("normalize_caption: single all-caps word", "[caption-format]") {
    REQUIRE(normalize_caption("HELLO") == "Hello");
}

TEST_CASE("normalize_caption: multi-word all-caps", "[caption-format]") {
    REQUIRE(normalize_caption("HELLO WORLD") == "Hello world");
}

TEST_CASE("normalize_caption: multi-sentence with each terminator",
          "[caption-format]") {
    SECTION("period") {
        REQUIRE(normalize_caption("HELLO WORLD. HOW ARE YOU.")
                == "Hello world. How are you.");
    }
    SECTION("exclamation") {
        REQUIRE(normalize_caption("HELLO! BYE.") == "Hello! Bye.");
    }
    SECTION("question") {
        REQUIRE(normalize_caption("HELLO? WHO?") == "Hello? Who?");
    }
    SECTION("mixed") {
        REQUIRE(normalize_caption("HI! HOW ARE YOU? GOOD.")
                == "Hi! How are you? Good.");
    }
}

TEST_CASE("normalize_caption: multi-space preservation", "[caption-format]") {
    // Two spaces between sentences should round-trip verbatim.
    REQUIRE(normalize_caption("HELLO?  WHO?") == "Hello?  Who?");
}

TEST_CASE("normalize_caption: tab and newline as sentence whitespace",
          "[caption-format]") {
    REQUIRE(normalize_caption("HELLO.\tWORLD") == "Hello.\tWorld");
    REQUIRE(normalize_caption("HELLO.\nWORLD") == "Hello.\nWorld");
}

TEST_CASE("normalize_caption: embedded period (FILE.TXT)", "[caption-format]") {
    // Period followed by alpha (no whitespace) must NOT trigger
    // capitalization.
    REQUIRE(normalize_caption("FILE.TXT") == "File.txt");
    REQUIRE(normalize_caption("HELLO FILE.TXT WORLD")
            == "Hello file.txt world");
}

TEST_CASE("normalize_caption: idempotency", "[caption-format]") {
    auto once = normalize_caption("HELLO WORLD. HOW ARE YOU?");
    auto twice = normalize_caption(once);
    REQUIRE(once == twice);
    REQUIRE(once == "Hello world. How are you?");
}

TEST_CASE("normalize_caption: mixed-case input lowercases body",
          "[caption-format]") {
    // A mixed-case input is lowercased then re-capitalized at sentence
    // boundaries — the function is NOT a no-op on naturally-cased input,
    // it's idempotent w.r.t. its own output.
    REQUIRE(normalize_caption("Hello World") == "Hello world");
    REQUIRE(normalize_caption("HeLLo WORLD. hOw aRE yoU?")
            == "Hello world. How are you?");
}

TEST_CASE("normalize_caption: leading whitespace preserved", "[caption-format]") {
    // Leading whitespace before the first alpha — alpha still capitalized.
    REQUIRE(normalize_caption("   HELLO") == "   Hello");
}

TEST_CASE("normalize_caption: trailing terminator no whitespace",
          "[caption-format]") {
    // Trailing `.` with nothing after — round-trip clean.
    REQUIRE(normalize_caption("HELLO.") == "Hello.");
}

TEST_CASE("normalize_caption: non-ASCII bytes pass through", "[caption-format]") {
    // The model emits ASCII for English, but the function is documented to
    // pass UTF-8 bytes verbatim. Smoke-check: a UTF-8 multibyte char in the
    // middle of an all-caps run shouldn't crash or re-case the bytes.
    std::string in = "HELLO\xc3\xa9 WORLD";   // "HELLOé WORLD"
    std::string out = normalize_caption(in);
    REQUIRE(out == "Hello\xc3\xa9 world");
}

// ---------------------------------------------------------------------------
// CLI formatter tests
// ---------------------------------------------------------------------------

TEST_CASE("format_caption_for_cli: partial vs final indicator",
          "[caption-format]") {
    REQUIRE(format_caption_for_cli("HELLO", true,  true)
            == "[captions] (partial) Hello");
    REQUIRE(format_caption_for_cli("HELLO", false, true)
            == "[captions] (final)   Hello");
}

TEST_CASE("format_caption_for_cli: normalize=false passes raw",
          "[caption-format]") {
    REQUIRE(format_caption_for_cli("HELLO WORLD", false, false)
            == "[captions] (final)   HELLO WORLD");
}

TEST_CASE("format_caption_degraded_for_cli: prefix shape",
          "[caption-format]") {
    REQUIRE(format_caption_degraded_for_cli("buffer_overrun")
            == "[captions] degraded: buffer_overrun");
}
