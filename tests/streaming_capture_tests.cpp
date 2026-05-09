// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 1 — streaming audio-capture callback unit tests.
//
// These tests exercise the AudioChunkCallback wiring on PipeWireCapture
// and PulseMonitorCapture without actually opening PipeWire / PulseAudio.
// Both classes expose a test-only `_inject_for_test(samples, n)` hook that
// mirrors the body of their respective capture loops (lock + buffer append
// + callback dispatch), so we can drive that exact code path in isolation.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "audio_capture.h"
#include "audio_file.h"
#include "audio_monitor.h"
#include "util.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <thread>
#include <vector>

#include <sndfile.h>

using namespace recmeet;

namespace {

// Minimal sink the C-style callback can write into. Atomics so set/unset
// races (Test #3) are well-defined; vector access is single-threaded in
// these tests so a plain std::vector behind atomic counters is fine.
struct CaptureSink {
    std::atomic<std::size_t> chunks{0};
    std::atomic<std::size_t> total_samples{0};
    // First sample of every chunk, captured for ordering checks. Guarded
    // implicitly by the single-threaded test driver.
    std::vector<int16_t> first_sample_per_chunk;
    // Userdata sentinel — checked to confirm the wiring forwards it intact.
    int sentinel = 0;
};

void on_chunk(const int16_t* samples, std::size_t n, void* userdata) {
    auto* sink = static_cast<CaptureSink*>(userdata);
    sink->chunks.fetch_add(1, std::memory_order_relaxed);
    sink->total_samples.fetch_add(n, std::memory_order_relaxed);
    if (n > 0)
        sink->first_sample_per_chunk.push_back(samples[0]);
}

// Generate a deterministic chunk of samples — strictly-increasing values so
// ordering bugs surface as out-of-order first-samples.
std::vector<int16_t> make_chunk(std::size_t n, int16_t start) {
    std::vector<int16_t> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<int16_t>(start + static_cast<int16_t>(i));
    return out;
}

namespace fs = std::filesystem;

fs::path tmp_dir() {
    fs::path dir = fs::temp_directory_path() / "recmeet_streaming_test";
    fs::create_directories(dir);
    return dir;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Callback fires on each appended chunk (count matches insert count).
// ---------------------------------------------------------------------------

TEST_CASE("PipeWireCapture: callback fires once per inserted chunk", "[streaming-capture]") {
    PipeWireCapture cap("test-source");
    CaptureSink sink;
    sink.sentinel = 0xC0DE;
    cap.set_audio_callback(&on_chunk, &sink);

    auto chunk = make_chunk(160, 1000);
    cap._inject_for_test(chunk.data(), chunk.size());

    CHECK(sink.chunks.load() == 1);
    CHECK(sink.total_samples.load() == 160);
    REQUIRE(sink.first_sample_per_chunk.size() == 1);
    CHECK(sink.first_sample_per_chunk[0] == 1000);
    CHECK(sink.sentinel == 0xC0DE);
}

TEST_CASE("PulseMonitorCapture: callback fires once per inserted chunk", "[streaming-capture]") {
    PulseMonitorCapture cap("test.monitor");
    CaptureSink sink;
    cap.set_audio_callback(&on_chunk, &sink);

    auto chunk = make_chunk(160, 500);
    cap._inject_for_test(chunk.data(), chunk.size());

    CHECK(sink.chunks.load() == 1);
    CHECK(sink.total_samples.load() == 160);
    REQUIRE(sink.first_sample_per_chunk.size() == 1);
    CHECK(sink.first_sample_per_chunk[0] == 500);
}

// ---------------------------------------------------------------------------
// 2. No-callback (cb == nullptr) behaviour is byte-identical to current
//    drain() — regression guard for the batch path. We feed identical chunks
//    through one capture instance with no callback installed and one with a
//    callback installed; drain() output must match exactly. This catches the
//    "callback steals samples" failure mode at the same time.
// ---------------------------------------------------------------------------

TEST_CASE("Batch path is byte-identical with and without a callback installed",
          "[streaming-capture]") {
    auto chunk_a = make_chunk(160, 0);
    auto chunk_b = make_chunk(240, 1000);
    auto chunk_c = make_chunk( 80, 2000);

    PipeWireCapture batch_only("test-source");
    batch_only._inject_for_test(chunk_a.data(), chunk_a.size());
    batch_only._inject_for_test(chunk_b.data(), chunk_b.size());
    batch_only._inject_for_test(chunk_c.data(), chunk_c.size());
    auto baseline = batch_only.drain();

    PipeWireCapture with_cb("test-source");
    CaptureSink sink;
    with_cb.set_audio_callback(&on_chunk, &sink);
    with_cb._inject_for_test(chunk_a.data(), chunk_a.size());
    with_cb._inject_for_test(chunk_b.data(), chunk_b.size());
    with_cb._inject_for_test(chunk_c.data(), chunk_c.size());
    auto with_cb_buf = with_cb.drain();

    REQUIRE(baseline.size() == with_cb_buf.size());
    CHECK(baseline == with_cb_buf);
    // Callback also saw everything.
    CHECK(sink.chunks.load() == 3);
    CHECK(sink.total_samples.load() == chunk_a.size() + chunk_b.size() + chunk_c.size());
}

// ---------------------------------------------------------------------------
// 3. set_audio_callback(cb, ud) then set_audio_callback(nullptr, nullptr):
//    the callback stops firing.
// ---------------------------------------------------------------------------

TEST_CASE("set_audio_callback(nullptr) stops further callback fires",
          "[streaming-capture]") {
    PipeWireCapture cap("test-source");
    CaptureSink sink;
    cap.set_audio_callback(&on_chunk, &sink);

    auto chunk = make_chunk(160, 0);
    cap._inject_for_test(chunk.data(), chunk.size());
    REQUIRE(sink.chunks.load() == 1);

    cap.set_audio_callback(nullptr, nullptr);
    cap._inject_for_test(chunk.data(), chunk.size());
    cap._inject_for_test(chunk.data(), chunk.size());

    // Callback count should not have advanced; samples still hit the buffer.
    CHECK(sink.chunks.load() == 1);
    auto drained = cap.drain();
    CHECK(drained.size() == 3 * chunk.size());
}

TEST_CASE("PulseMonitorCapture: set_audio_callback(nullptr) stops further fires",
          "[streaming-capture]") {
    PulseMonitorCapture cap("test.monitor");
    CaptureSink sink;
    cap.set_audio_callback(&on_chunk, &sink);

    auto chunk = make_chunk(160, 0);
    cap._inject_for_test(chunk.data(), chunk.size());
    REQUIRE(sink.chunks.load() == 1);

    cap.set_audio_callback(nullptr, nullptr);
    cap._inject_for_test(chunk.data(), chunk.size());

    CHECK(sink.chunks.load() == 1);
    auto drained = cap.drain();
    CHECK(drained.size() == 2 * chunk.size());
}

// ---------------------------------------------------------------------------
// 4. Multi-chunk delivery: feed N chunks, callback receives N invocations
//    with matching sample counts and in-order first-samples.
// ---------------------------------------------------------------------------

TEST_CASE("Multi-chunk delivery preserves count, total samples, and order",
          "[streaming-capture]") {
    PipeWireCapture cap("test-source");
    CaptureSink sink;
    cap.set_audio_callback(&on_chunk, &sink);

    constexpr int N_CHUNKS = 25;
    constexpr std::size_t CHUNK_N = 160;  // 10ms @ 16kHz

    std::size_t expected_total = 0;
    for (int i = 0; i < N_CHUNKS; ++i) {
        auto chunk = make_chunk(CHUNK_N, static_cast<int16_t>(i * 100));
        cap._inject_for_test(chunk.data(), chunk.size());
        expected_total += chunk.size();
    }

    CHECK(sink.chunks.load() == N_CHUNKS);
    CHECK(sink.total_samples.load() == expected_total);
    REQUIRE(sink.first_sample_per_chunk.size() == N_CHUNKS);
    for (int i = 0; i < N_CHUNKS; ++i) {
        CHECK(sink.first_sample_per_chunk[i] == static_cast<int16_t>(i * 100));
    }
}

// ---------------------------------------------------------------------------
// 5. WAV-replay harness: write a small generated WAV fixture, read it back
//    in chunks at simulated real-time, and feed it through the callback.
//    Assert chunk-count and total-sample-count round-trip end-to-end.
//
// "Simulated real-time" here means we sleep a tiny amount between chunks so
// the test isn't a pure tight loop — enough to exercise the timing-tolerant
// shape of the capture path without actually waiting wall-clock seconds. The
// fixture is short (250ms of audio) so the test stays under a few hundred
// milliseconds even in CI.
// ---------------------------------------------------------------------------

TEST_CASE("WAV-replay harness round-trips chunk count and sample count",
          "[streaming-capture]") {
    auto dir = tmp_dir();
    fs::path wav = dir / "streaming_replay.wav";

    // Generate 250ms of a 440Hz sine wave at 16kHz (4000 samples total).
    constexpr std::size_t TOTAL_SAMPLES = SAMPLE_RATE / 4;
    std::vector<int16_t> samples(TOTAL_SAMPLES);
    for (std::size_t i = 0; i < TOTAL_SAMPLES; ++i)
        samples[i] = static_cast<int16_t>(
            16000 * std::sin(2.0 * M_PI * 440.0 * static_cast<double>(i) / SAMPLE_RATE));

    write_wav(wav, samples);
    REQUIRE(fs::exists(wav));

    // Read back via libsndfile in 10ms chunks (160 samples). At a real
    // 16kHz capture this would be one PipeWire callback per chunk.
    constexpr std::size_t CHUNK_SAMPLES = SAMPLE_RATE / 100;  // 160
    SF_INFO info{};
    SNDFILE* sf = sf_open(wav.string().c_str(), SFM_READ, &info);
    REQUIRE(sf != nullptr);
    REQUIRE(info.samplerate == SAMPLE_RATE);
    REQUIRE(info.channels == CHANNELS);

    PipeWireCapture cap("test-source");
    CaptureSink sink;
    cap.set_audio_callback(&on_chunk, &sink);

    std::vector<int16_t> chunk(CHUNK_SAMPLES);
    std::size_t fed_total = 0;
    std::size_t fed_chunks = 0;
    while (true) {
        sf_count_t got = sf_read_short(sf, chunk.data(), static_cast<sf_count_t>(CHUNK_SAMPLES));
        if (got <= 0) break;
        cap._inject_for_test(chunk.data(), static_cast<std::size_t>(got));
        fed_total += static_cast<std::size_t>(got);
        ++fed_chunks;
        // Simulated real-time pacing — short enough not to slow CI.
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    sf_close(sf);

    CHECK(fed_total == TOTAL_SAMPLES);
    CHECK(sink.chunks.load() == fed_chunks);
    CHECK(sink.total_samples.load() == fed_total);

    // drain() also returns the full audio, byte-identical to what we fed.
    auto drained = cap.drain();
    REQUIRE(drained.size() == TOTAL_SAMPLES);
    CHECK(drained == samples);

    fs::remove(wav);
}

// ---------------------------------------------------------------------------
// 6. Drain round-trip: with callback set + unset, drain() still returns the
//    full accumulated buffer with no sample loss. This is the explicit
//    regression guard for the "callback steals samples" failure mode called
//    out in the plan.
// ---------------------------------------------------------------------------

TEST_CASE("drain() returns full buffer regardless of callback state (no sample loss)",
          "[streaming-capture]") {
    PipeWireCapture cap("test-source");
    CaptureSink sink;

    auto chunk_a = make_chunk(160, 1);
    auto chunk_b = make_chunk(160, 161);
    auto chunk_c = make_chunk(160, 321);
    auto chunk_d = make_chunk(160, 481);

    // No callback yet.
    cap._inject_for_test(chunk_a.data(), chunk_a.size());

    // Install callback.
    cap.set_audio_callback(&on_chunk, &sink);
    cap._inject_for_test(chunk_b.data(), chunk_b.size());
    cap._inject_for_test(chunk_c.data(), chunk_c.size());

    // Unset callback.
    cap.set_audio_callback(nullptr, nullptr);
    cap._inject_for_test(chunk_d.data(), chunk_d.size());

    // Callback only saw the two middle chunks.
    CHECK(sink.chunks.load() == 2);
    CHECK(sink.total_samples.load() == chunk_b.size() + chunk_c.size());

    // drain() returns ALL samples in order — full audio fidelity preserved.
    auto drained = cap.drain();
    REQUIRE(drained.size() ==
            chunk_a.size() + chunk_b.size() + chunk_c.size() + chunk_d.size());

    std::vector<int16_t> expected;
    expected.reserve(drained.size());
    expected.insert(expected.end(), chunk_a.begin(), chunk_a.end());
    expected.insert(expected.end(), chunk_b.begin(), chunk_b.end());
    expected.insert(expected.end(), chunk_c.begin(), chunk_c.end());
    expected.insert(expected.end(), chunk_d.begin(), chunk_d.end());
    CHECK(drained == expected);
}
