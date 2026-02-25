// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio_file.h"

#include <cmath>
#include <fstream>
#include <sndfile.h>

using namespace recmeet;
using Catch::Matchers::WithinAbs;

static fs::path tmp_dir() {
    fs::path dir = fs::temp_directory_path() / "recmeet_test";
    fs::create_directories(dir);
    return dir;
}

TEST_CASE("write_wav + read_wav_float round-trip", "[audio_file]") {
    auto dir = tmp_dir();
    fs::path wav = dir / "roundtrip.wav";

    // Generate a 1-second 440Hz sine wave
    std::vector<int16_t> samples(SAMPLE_RATE);
    for (int i = 0; i < SAMPLE_RATE; ++i)
        samples[i] = static_cast<int16_t>(16000 * std::sin(2.0 * M_PI * 440.0 * i / SAMPLE_RATE));

    write_wav(wav, samples);
    REQUIRE(fs::exists(wav));
    REQUIRE(fs::file_size(wav) > 44);  // WAV header is 44 bytes

    auto floats = read_wav_float(wav);
    REQUIRE(floats.size() == SAMPLE_RATE);

    // Verify round-trip preserves values (int16 -> float32 -> compare)
    // float values should be in [-1, 1], proportional to int16 values
    for (int i = 0; i < 100; ++i) {
        double expected = samples[i] / 32768.0;
        CHECK_THAT(floats[i], WithinAbs(expected, 0.001));
    }

    fs::remove(wav);
}

TEST_CASE("validate_audio: valid file", "[audio_file]") {
    auto dir = tmp_dir();
    fs::path wav = dir / "valid.wav";

    // 2 seconds of silence
    std::vector<int16_t> samples(SAMPLE_RATE * 2, 0);
    write_wav(wav, samples);

    double dur = validate_audio(wav, 1.0, "Test");
    CHECK_THAT(dur, WithinAbs(2.0, 0.1));

    fs::remove(wav);
}

TEST_CASE("validate_audio: too short", "[audio_file]") {
    auto dir = tmp_dir();
    fs::path wav = dir / "short.wav";

    // 0.1 seconds
    std::vector<int16_t> samples(SAMPLE_RATE / 10, 0);
    write_wav(wav, samples);

    CHECK_THROWS_AS(validate_audio(wav, 1.0, "Test"), AudioValidationError);

    fs::remove(wav);
}

TEST_CASE("validate_audio: missing file", "[audio_file]") {
    CHECK_THROWS_AS(validate_audio("/nonexistent/path.wav"), AudioValidationError);
}

TEST_CASE("validate_audio: empty file", "[audio_file]") {
    auto dir = tmp_dir();
    fs::path wav = dir / "empty.wav";

    // Create a zero-byte file
    { std::ofstream(wav); }

    CHECK_THROWS_AS(validate_audio(wav), AudioValidationError);

    fs::remove(wav);
}

TEST_CASE("read_wav_float: handles stereo by downmixing", "[audio_file]") {
    auto dir = tmp_dir();
    fs::path wav = dir / "stereo.wav";

    // Write a stereo WAV file directly using libsndfile
    SF_INFO info = {};
    info.samplerate = SAMPLE_RATE;
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* sf = sf_open(wav.c_str(), SFM_WRITE, &info);
    REQUIRE(sf != nullptr);

    // Write 100 frames of stereo: left=0.5, right=-0.5
    // After downmix, each frame should be ~0.0
    int n_frames = 100;
    std::vector<int16_t> stereo_samples(n_frames * 2);
    for (int i = 0; i < n_frames; ++i) {
        stereo_samples[i * 2]     = 16000;  // left
        stereo_samples[i * 2 + 1] = -16000; // right
    }
    sf_write_short(sf, stereo_samples.data(), stereo_samples.size());
    sf_close(sf);

    auto mono = read_wav_float(wav);
    REQUIRE(mono.size() == static_cast<size_t>(n_frames));

    // Downmixed: (16000 + -16000) / 2 / 32768 ≈ 0.0
    for (int i = 0; i < n_frames; ++i) {
        CHECK_THAT(mono[i], WithinAbs(0.0, 0.01));
    }

    fs::remove(wav);
}

TEST_CASE("write_wav: throws on invalid directory", "[audio_file]") {
    std::vector<int16_t> samples(100, 0);
    CHECK_THROWS_AS(write_wav("/nonexistent/dir/test.wav", samples), RecmeetError);
}

TEST_CASE("get_audio_duration_seconds: returns correct duration", "[audio_file]") {
    auto dir = tmp_dir();
    fs::path wav = dir / "duration.wav";

    // 5 seconds of silence
    std::vector<int16_t> samples(SAMPLE_RATE * 5, 0);
    write_wav(wav, samples);

    CHECK(get_audio_duration_seconds(wav) == 5);

    fs::remove(wav);
}

TEST_CASE("get_audio_duration_seconds: missing file returns 0", "[audio_file]") {
    CHECK(get_audio_duration_seconds("/nonexistent/path.wav") == 0);
}

TEST_CASE("write_wav: empty samples creates valid header-only file", "[audio_file]") {
    auto dir = tmp_dir();
    fs::path wav = dir / "empty_samples.wav";

    write_wav(wav, {});
    REQUIRE(fs::exists(wav));
    // libsndfile writes a valid header even for 0 frames
    CHECK(fs::file_size(wav) > 0);

    fs::remove(wav);
}
