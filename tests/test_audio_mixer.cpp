// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "audio_mixer.h"
#include "util.h"

using namespace recmeet;

TEST_CASE("mix_audio: equal-length streams", "[mixer]") {
    std::vector<int16_t> a = {100, 200, 300, 400};
    std::vector<int16_t> b = {500, 600, 700, 800};
    auto out = mix_audio(a, b);

    REQUIRE(out.size() == 4);
    CHECK(out[0] == 300);  // (100+500)/2
    CHECK(out[1] == 400);  // (200+600)/2
    CHECK(out[2] == 500);  // (300+700)/2
    CHECK(out[3] == 600);  // (400+800)/2
}

TEST_CASE("mix_audio: different-length streams zero-pads shorter", "[mixer]") {
    std::vector<int16_t> a = {1000, 2000};
    std::vector<int16_t> b = {3000, 4000, 5000, 6000};
    auto out = mix_audio(a, b);

    REQUIRE(out.size() == 4);
    CHECK(out[0] == 2000);  // (1000+3000)/2
    CHECK(out[1] == 3000);  // (2000+4000)/2
    CHECK(out[2] == 2500);  // (0+5000)/2
    CHECK(out[3] == 3000);  // (0+6000)/2
}

TEST_CASE("mix_audio: empty inputs", "[mixer]") {
    SECTION("both empty") {
        auto out = mix_audio({}, {});
        REQUIRE(out.empty());
    }

    SECTION("one empty") {
        std::vector<int16_t> a = {1000, -1000};
        auto out = mix_audio(a, {});
        REQUIRE(out.size() == 2);
        CHECK(out[0] == 500);   // (1000+0)/2
        CHECK(out[1] == -500);  // (-1000+0)/2
    }
}

TEST_CASE("mix_audio: clamps to int16 range", "[mixer]") {
    // Two max-positive values should clamp, not overflow
    std::vector<int16_t> a = {32767};
    std::vector<int16_t> b = {32767};
    auto out = mix_audio(a, b);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 32767);  // (32767+32767)/2 = 32767, no clamp needed

    // But verify the average is correct for large values
    std::vector<int16_t> c = {32000};
    std::vector<int16_t> d = {32000};
    auto out2 = mix_audio(c, d);
    CHECK(out2[0] == 32000);
}

TEST_CASE("mix_audio: negative samples", "[mixer]") {
    std::vector<int16_t> a = {-10000, -20000};
    std::vector<int16_t> b = {-5000, -10000};
    auto out = mix_audio(a, b);

    REQUIRE(out.size() == 2);
    CHECK(out[0] == -7500);   // (-10000+-5000)/2
    CHECK(out[1] == -15000);  // (-20000+-10000)/2
}

TEST_CASE("mix_audio: silence + signal passes signal through halved", "[mixer]") {
    std::vector<int16_t> signal = {10000, -10000, 5000};
    std::vector<int16_t> silence = {0, 0, 0};
    auto out = mix_audio(signal, silence);

    REQUIRE(out.size() == 3);
    CHECK(out[0] == 5000);
    CHECK(out[1] == -5000);
    CHECK(out[2] == 2500);
}

// ---------------------------------------------------------------------------
// v2-dual-source-recording Phase 1.0 — offline mix-at-Stop contract.
//
// V1 (live_recording.cpp:370-389) drains two COMPLETE buffers at Stop and
// calls mix_audio() exactly once — a single bounded zero-pad, zero
// cumulative drift. These hermetic tests pin that contract (the tray's
// dual-mode Stop path will mix identically) and exercise the capture-side
// validation gate that replaces V1's disk-reading validate_audio() in the
// tray, which cannot link recmeet_core.
// ---------------------------------------------------------------------------

TEST_CASE("dual mix-at-stop: two full buffers mixed once, length = longer",
          "[tray][dual]") {
    // Simulate the offline mix: mic ran slightly longer than monitor (the
    // monitor source started a few samples late). One mix call, shorter
    // stream zero-padded to the longer length — no per-tick re-padding.
    std::vector<int16_t> mic     = {1000, 2000, 3000, 4000, 5000};
    std::vector<int16_t> monitor = {  -0, 1000, 2000};  // 2 samples shorter

    auto mixed = mix_audio(mic, monitor);

    REQUIRE(mixed.size() == 5);          // length of the longer (mic) buffer
    CHECK(mixed[0] == 500);              // (1000 +    0) / 2
    CHECK(mixed[1] == 1500);             // (2000 + 1000) / 2
    CHECK(mixed[2] == 2500);             // (3000 + 2000) / 2
    CHECK(mixed[3] == 2000);             // (4000 +    0) / 2  monitor zero-padded
    CHECK(mixed[4] == 2500);             // (5000 +    0) / 2  monitor zero-padded
}

TEST_CASE("validate_min_duration: accepts a buffer meeting the floor",
          "[tray][dual]") {
    // 1.5s of 16kHz mono samples clears a 1.0s floor; returns the duration.
    std::vector<int16_t> samples(static_cast<size_t>(SAMPLE_RATE * 1.5), 0);
    double dur = validate_min_duration(samples, 1.0, "Mic audio");
    CHECK(dur == Catch::Approx(1.5));
}

TEST_CASE("validate_min_duration: throws on a too-short buffer",
          "[tray][dual]") {
    // 0.5s buffer fails a 1.0s floor — this is the fatal-mic / non-fatal-
    // monitor gate the tray's Stop path wraps in try/catch for fallback.
    std::vector<int16_t> samples(static_cast<size_t>(SAMPLE_RATE * 0.5), 0);
    CHECK_THROWS_AS(validate_min_duration(samples, 1.0, "Monitor audio"),
                    AudioValidationError);
}

TEST_CASE("validate_min_duration: empty buffer is too short", "[tray][dual]") {
    CHECK_THROWS_AS(validate_min_duration({}, 1.0, "Mic audio"),
                    AudioValidationError);
}

// ---------------------------------------------------------------------------
// finalize_dual_mix — the Stop-time mix-vs-fallback decision the tray's
// stop_capture delegates to. Monitor clears the gate → mix; monitor
// silent/short → stage mic-only (V1 non-fatal degradation).
// ---------------------------------------------------------------------------

TEST_CASE("finalize_dual_mix: healthy monitor is mixed in", "[tray][dual]") {
    // Both buffers >= 1.0s at 16kHz. Expect a single mix, length = longer.
    std::vector<int16_t> mic(static_cast<size_t>(SAMPLE_RATE * 1.2), 1000);
    std::vector<int16_t> mon(static_cast<size_t>(SAMPLE_RATE * 1.0), 3000);

    auto res = finalize_dual_mix(mic, mon, 1.0);

    CHECK(res.mixed);
    REQUIRE(res.audio.size() == mic.size());      // mic is the longer stream
    CHECK(res.audio[0] == 2000);                  // (1000 + 3000) / 2
    // Tail past the monitor's end: monitor zero-padded → mic halved.
    CHECK(res.audio.back() == 500);               // (1000 + 0) / 2
}

TEST_CASE("finalize_dual_mix: short monitor degrades to mic-only",
          "[tray][dual]") {
    // Monitor is 0.5s (< 1.0s floor) — dropped; mic staged unchanged.
    std::vector<int16_t> mic(static_cast<size_t>(SAMPLE_RATE * 2.0), 1234);
    std::vector<int16_t> mon(static_cast<size_t>(SAMPLE_RATE * 0.5), 9999);

    auto res = finalize_dual_mix(mic, mon, 1.0);

    CHECK_FALSE(res.mixed);
    REQUIRE(res.audio.size() == mic.size());
    CHECK(res.audio[0] == 1234);                  // mic untouched, not halved
}

TEST_CASE("finalize_dual_mix: empty monitor degrades to mic-only",
          "[tray][dual]") {
    std::vector<int16_t> mic(static_cast<size_t>(SAMPLE_RATE * 1.5), 777);

    auto res = finalize_dual_mix(mic, {}, 1.0);

    CHECK_FALSE(res.mixed);
    REQUIRE(res.audio.size() == mic.size());
    CHECK(res.audio[0] == 777);
}

TEST_CASE("finalize_dual_mix: monitor exactly at the floor is mixed",
          "[tray][dual]") {
    // Boundary: duration == min_monitor_duration passes (>= semantics).
    std::vector<int16_t> mic(SAMPLE_RATE, 2000);
    std::vector<int16_t> mon(SAMPLE_RATE, 2000);  // exactly 1.0s

    auto res = finalize_dual_mix(mic, mon, 1.0);

    CHECK(res.mixed);
    REQUIRE(res.audio.size() == static_cast<size_t>(SAMPLE_RATE));
    CHECK(res.audio[0] == 2000);                  // (2000 + 2000) / 2
}
