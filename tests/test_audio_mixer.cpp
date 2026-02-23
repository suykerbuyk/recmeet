// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "audio_mixer.h"

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
