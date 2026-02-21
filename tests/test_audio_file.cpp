#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio_file.h"

#include <cmath>
#include <fstream>

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

TEST_CASE("write_wav: empty samples creates valid header-only file", "[audio_file]") {
    auto dir = tmp_dir();
    fs::path wav = dir / "empty_samples.wav";

    write_wav(wav, {});
    REQUIRE(fs::exists(wav));
    // libsndfile writes a valid header even for 0 frames
    CHECK(fs::file_size(wav) > 0);

    fs::remove(wav);
}
