// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 5.1 / 5.4 — render-state machine tests.
//
// Validates the GUI-free CaptionRenderState. The actual GtkWindow / GtkLabel
// construction is exercised manually (`./build/recmeet-tray`) and is not
// unit-tested here — known gap, documented in the Phase 5 deliverable notes.

#include <catch2/catch_test_macros.hpp>

#include "caption_format.h"

#include <chrono>

using namespace recmeet;
using Clock = CaptionRenderState::Clock;

namespace {

// Anchor a synthetic "now" so tests don't rely on real time. We advance
// `now` manually between calls.
Clock::time_point t0() {
    static const auto base = Clock::now();
    return base;
}

} // namespace

TEST_CASE("CaptionRenderState: initial empty state", "[caption-render]") {
    CaptionRenderState st;
    REQUIRE_FALSE(st.has_content());
    REQUIRE(st.line_count() == 0);
    REQUIRE(st.to_label_markup() == "");
    REQUIRE_FALSE(st.tick(t0()));   // never updated → never auto-hide
}

TEST_CASE("CaptionRenderState: first partial renders italic",
          "[caption-render]") {
    CaptionRenderState st;
    st.update("HELLO WORLD", true, true, t0());
    REQUIRE(st.has_content());
    REQUIRE(st.line_count() == 1);
    REQUIRE(st.to_label_markup() == "<i>Hello world</i>");
}

TEST_CASE("CaptionRenderState: partial → final transition",
          "[caption-render]") {
    CaptionRenderState st;
    auto t = t0();
    st.update("HELLO WORLD", true, true, t);
    REQUIRE(st.to_label_markup() == "<i>Hello world</i>");

    // Finalize: replaces the trailing partial with a final.
    st.update("HELLO WORLD.", false, true, t + std::chrono::milliseconds(100));
    REQUIRE(st.line_count() == 1);
    REQUIRE(st.to_label_markup() == "Hello world.");

    // Next partial appears below as italics.
    st.update("HOW ARE", true, true, t + std::chrono::milliseconds(200));
    REQUIRE(st.line_count() == 2);
    REQUIRE(st.to_label_markup() == "Hello world.\n<i>How are</i>");
}

TEST_CASE("CaptionRenderState: caps history at max_lines, partial untouched",
          "[caption-render]") {
    CaptionRenderState st(/*max_lines=*/3);
    auto t = t0();

    // 3 finals + 1 partial.
    st.update("ONE.",  false, true, t);
    st.update("TWO.",  false, true, t + std::chrono::milliseconds(10));
    st.update("THREE.", false, true, t + std::chrono::milliseconds(20));
    st.update("FOUR",  true,  true, t + std::chrono::milliseconds(30));

    REQUIRE(st.line_count() == 4);
    REQUIRE(st.to_label_markup() == "One.\nTwo.\nThree.\n<i>Four</i>");

    // Add a fourth final → oldest final dropped, partial (now promoted)
    // followed by a fresh slot.
    st.update("FOUR.", false, true, t + std::chrono::milliseconds(40));
    REQUIRE(st.line_count() == 3);
    REQUIRE(st.to_label_markup() == "Two.\nThree.\nFour.");
}

TEST_CASE("CaptionRenderState: auto-hide after 5s on finalized line",
          "[caption-render]") {
    CaptionRenderState st(3, std::chrono::seconds(5));
    auto t = t0();

    st.update("HELLO.", false, true, t);
    REQUIRE_FALSE(st.tick(t));                                  // 0s
    REQUIRE_FALSE(st.tick(t + std::chrono::seconds(4)));        // <5s
    REQUIRE(st.tick(t + std::chrono::seconds(5)));              // ==5s, hide
    REQUIRE(st.tick(t + std::chrono::seconds(60)));             // long after
}

TEST_CASE("CaptionRenderState: no auto-hide while partial is live",
          "[caption-render]") {
    CaptionRenderState st(3, std::chrono::seconds(5));
    auto t = t0();
    st.update("HELLO WORLD", true, true, t);   // partial
    // Even after long elapsed time, do NOT hide while we're mid-sentence.
    REQUIRE_FALSE(st.tick(t + std::chrono::seconds(60)));
}

TEST_CASE("CaptionRenderState: clear() resets visible content",
          "[caption-render]") {
    CaptionRenderState st;
    st.update("HELLO.", false, true, t0());
    REQUIRE(st.has_content());
    st.clear();
    REQUIRE_FALSE(st.has_content());
    REQUIRE(st.to_label_markup() == "");

    // After clear, an update should re-populate without leftover state.
    st.update("HI", true, true, t0() + std::chrono::milliseconds(100));
    REQUIRE(st.line_count() == 1);
    REQUIRE(st.to_label_markup() == "<i>Hi</i>");
}

TEST_CASE("CaptionRenderState: degraded marker rendered into markup",
          "[caption-render]") {
    CaptionRenderState st;
    auto t = t0();
    st.update("HELLO.", false, true, t);
    st.degraded("buffer_overrun", t);

    auto markup = st.to_label_markup();
    REQUIRE(markup.find("Hello.") != std::string::npos);
    REQUIRE(markup.find("captions falling behind: buffer_overrun")
            != std::string::npos);

    // degraded_active reflects the time window.
    REQUIRE(st.degraded_active(t));
    REQUIRE(st.degraded_active(t + std::chrono::milliseconds(1500)));
    REQUIRE_FALSE(st.degraded_active(t + std::chrono::seconds(3)));
}

TEST_CASE("CaptionRenderState: pango markup escapes special chars",
          "[caption-render]") {
    CaptionRenderState st;
    // The model only emits letters + spaces, but defense-in-depth: a stray
    // '<' must be escaped so we don't break GTK markup.
    st.update("A < B & C", true, false, t0());     // apply_normalize=false
    auto markup = st.to_label_markup();
    REQUIRE(markup.find("A &lt; B &amp; C") != std::string::npos);
    REQUIRE(markup.find('<') != std::string::npos);  // <i> tag still present
}

TEST_CASE("CaptionRenderState: normalize disabled passes raw to render",
          "[caption-render]") {
    CaptionRenderState st;
    st.update("HELLO WORLD", false, /*apply_normalize=*/false, t0());
    REQUIRE(st.to_label_markup() == "HELLO WORLD");
}
