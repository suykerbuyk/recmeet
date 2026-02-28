// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "vad.h"

#include <cmath>

using namespace recmeet;

// ---------------------------------------------------------------------------
// Unit tests — no model needed
// ---------------------------------------------------------------------------

TEST_CASE("VadConfig: default values", "[vad]") {
    VadConfig cfg;
    CHECK(cfg.threshold == 0.5f);
    CHECK(cfg.min_silence_duration == 0.5f);
    CHECK(cfg.min_speech_duration == 0.25f);
    CHECK(cfg.max_speech_duration == 30.0f);
    CHECK(cfg.window_size == 512);
}

TEST_CASE("VadSegment: time calculation from samples", "[vad]") {
    VadSegment seg;
    seg.start_sample = 16000;   // 1.0s at 16kHz
    seg.end_sample = 32000;     // 2.0s at 16kHz
    seg.start = static_cast<double>(seg.start_sample) / SAMPLE_RATE;
    seg.end = static_cast<double>(seg.end_sample) / SAMPLE_RATE;

    CHECK(seg.start == 1.0);
    CHECK(seg.end == 2.0);
}

TEST_CASE("VadResult: speech ratio calculation", "[vad]") {
    VadResult result;
    result.total_audio_duration = 60.0;
    result.total_speech_duration = 30.0;

    double ratio = result.total_speech_duration / result.total_audio_duration;
    CHECK(ratio == 0.5);
}

TEST_CASE("VadResult: empty segments", "[vad]") {
    VadResult result;
    result.total_audio_duration = 10.0;
    result.total_speech_duration = 0.0;
    CHECK(result.segments.empty());
    CHECK(result.total_speech_duration == 0.0);
}

// ---------------------------------------------------------------------------
// Integration tests — require RECMEET_USE_SHERPA and cached VAD model
// ---------------------------------------------------------------------------

#if RECMEET_USE_SHERPA
#include "model_manager.h"
#include "audio_file.h"

TEST_CASE("detect_speech: throws on empty audio", "[vad]") {
    std::vector<float> empty;
    CHECK_THROWS_AS(detect_speech(empty), RecmeetError);
}

TEST_CASE("detect_speech: silence produces no segments", "[vad][integration]") {
    if (!is_vad_model_cached())
        SKIP("Silero VAD model not cached");

    // 5 seconds of silence
    std::vector<float> silence(SAMPLE_RATE * 5, 0.0f);
    auto result = detect_speech(silence);

    CHECK(result.segments.empty());
    CHECK(result.total_speech_duration == 0.0);
    CHECK(result.total_audio_duration > 0.0);
}

TEST_CASE("detect_speech: pure tone is not detected as speech", "[vad][integration]") {
    if (!is_vad_model_cached())
        SKIP("Silero VAD model not cached");

    // 3 seconds of 440Hz sine wave — Silero VAD is speech-trained,
    // so a pure tone should not trigger speech detection.
    std::vector<float> tone(SAMPLE_RATE * 3);
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = 0.5f * std::sin(2.0f * 3.14159f * 440.0f * i / SAMPLE_RATE);

    auto result = detect_speech(tone);

    CHECK(result.segments.empty());
    CHECK(result.total_audio_duration > 0.0);
}

TEST_CASE("detect_speech: real audio produces speech segments", "[vad][integration]") {
    if (!is_vad_model_cached())
        SKIP("Silero VAD model not cached");

    // Find the benchmark audio file
    fs::path dir = fs::current_path();
    for (int i = 0; i < 10; ++i) {
        if (fs::exists(dir / "assets" / "biden_trump_debate_2020.wav"))
            break;
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    fs::path audio = dir / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio))
        SKIP("Reference audio not found: " + audio.string());

    auto samples = read_wav_float(audio);
    REQUIRE(!samples.empty());

    auto result = detect_speech(samples);

    CHECK(!result.segments.empty());
    CHECK(result.total_speech_duration > 0.0);
    // Speech should be a significant portion of a debate recording
    double ratio = result.total_speech_duration / result.total_audio_duration;
    CHECK(ratio > 0.3);
    CHECK(ratio < 1.0);
}
#endif
