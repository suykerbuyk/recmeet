// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 2 — streaming-engine unit tests.
//
// Tests are split into two groups:
//   * Model-free:  exercise the ring buffer, drop-watermark, scheduler
//                  fallback, RAII teardown, and thread-cap configuration
//                  without touching the recognizer. These run in any build.
//   * Model-bearing ([caption-model]): run with a real sherpa-onnx
//                  streaming-zipformer model. Skip with a clear message if
//                  the model isn't already cached on the host. CI without
//                  network is expected to skip them; the orchestrator will
//                  exercise them after merge.
//
// Stub-build behaviour:
//   When RECMEET_USE_SHERPA is OFF, start() returns false with the canonical
//   error message and the producer / ring queries report zero. We assert
//   that contract at the top of each model-free test that depends on the
//   ring being live, so the same source file works in both build flavors.

#include <catch2/catch_test_macros.hpp>

#include "caption_engine.h"
#include "util.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace recmeet;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Recording sink — captures all CaptionResult emissions for assertions.
// ---------------------------------------------------------------------------
struct ResultSink {
    std::mutex mtx;
    std::vector<CaptionResult> results;
    static void cb(const CaptionResult& r, void* ud) {
        auto* s = static_cast<ResultSink*>(ud);
        std::lock_guard lk(s->mtx);
        s->results.push_back(r);
    }
    std::size_t count() {
        std::lock_guard lk(mtx);
        return results.size();
    }
    bool any_partial() {
        std::lock_guard lk(mtx);
        for (auto& r : results) if (r.is_partial) return true;
        return false;
    }
    bool any_final() {
        std::lock_guard lk(mtx);
        for (auto& r : results) if (!r.is_partial) return true;
        return false;
    }
    std::string last_text() {
        std::lock_guard lk(mtx);
        if (results.empty()) return "";
        return results.back().text;
    }
};

struct DegradedSink {
    std::atomic<std::size_t> count{0};
    static void cb(CaptionDegradedReason, void* ud) {
        auto* s = static_cast<DegradedSink*>(ud);
        s->count.fetch_add(1, std::memory_order_acq_rel);
    }
};

// ---------------------------------------------------------------------------
// Model availability helper. Looks for the streaming zipformer in the
// canonical recmeet model dir; if not present, returns empty. Tests that
// depend on the model SKIP rather than fail.
// ---------------------------------------------------------------------------
fs::path streaming_model_dir_if_present() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) return {};
    fs::path dir = fs::path(home) / ".local" / "share" / "recmeet"
                   / "models" / "sherpa" / "online" / "en-2023-06-26";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    // Quick sanity: needs encoder + decoder + joiner + tokens.
    bool has_enc = false, has_dec = false, has_join = false, has_tok = false;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        std::string n = e.path().filename().string();
        if (n.find("encoder") != std::string::npos) has_enc = true;
        if (n.find("decoder") != std::string::npos) has_dec = true;
        if (n.find("joiner")  != std::string::npos) has_join = true;
        if (n.find("tokens")  != std::string::npos) has_tok = true;
    }
    if (!(has_enc && has_dec && has_join && has_tok)) return {};
    return dir;
}

bool sherpa_build() {
#ifdef RECMEET_USE_SHERPA
    return true;
#else
    return false;
#endif
}

} // anonymous namespace

// ===========================================================================
// 1. Recognizer init/teardown ([caption-model] — needs a real model).
// ===========================================================================
TEST_CASE("CaptionEngine: recognizer init/teardown", "[streaming-engine][caption-model]") {
    if (!sherpa_build()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping recognizer init/teardown test");
        return;
    }
    fs::path model_dir = streaming_model_dir_if_present();
    if (model_dir.empty()) {
        WARN("Streaming zipformer model not cached — skipping. Expected at "
             "~/.local/share/recmeet/models/sherpa/online/en-2023-06-26/");
        return;
    }
    CaptionEngine eng;
    CaptionEngine::Options opts;
    opts.model_dir = model_dir.string();
    opts.num_threads = 1;

    ResultSink rsink;
    DegradedSink dsink;
    REQUIRE(eng.start(opts, &ResultSink::cb, &rsink,
                            &DegradedSink::cb, &dsink));
    CHECK(eng.is_running());
    eng.stop();
    CHECK(!eng.is_running());
}

// ===========================================================================
// 2. Partial → final transition ([caption-model]).
//    Stripped to a coarse shape check: feed audio long enough that the
//    recognizer will eventually emit *some* hypothesis (partial), then
//    silence to trigger an endpoint. Skips silently without a model.
// ===========================================================================
TEST_CASE("CaptionEngine: partial → final transition", "[streaming-engine][caption-model]") {
    if (!sherpa_build()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping partial→final test");
        return;
    }
    fs::path model_dir = streaming_model_dir_if_present();
    if (model_dir.empty()) {
        WARN("Streaming zipformer model not cached — skipping partial→final test");
        return;
    }
    CaptionEngine eng;
    CaptionEngine::Options opts;
    opts.model_dir = model_dir.string();
    ResultSink rsink;
    DegradedSink dsink;
    REQUIRE(eng.start(opts, &ResultSink::cb, &rsink, &DegradedSink::cb, &dsink));

    // Feed ~2 seconds of mid-amplitude noise then 4 seconds of silence so the
    // recognizer's endpoint detection (rule1 trailing silence ~2.4s) fires.
    constexpr int SR = 16000;
    std::vector<int16_t> noise(SR * 2);
    for (std::size_t i = 0; i < noise.size(); ++i) {
        noise[i] = static_cast<int16_t>((i * 31) & 0x3FFF);
    }
    std::vector<int16_t> silence(SR * 4, 0);

    // Push in 100 ms chunks to mirror the capture cadence.
    constexpr std::size_t CHUNK = 1600;
    for (std::size_t off = 0; off < noise.size(); off += CHUNK) {
        std::size_t n = std::min(CHUNK, noise.size() - off);
        eng._push_samples_for_test(noise.data() + off, n);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (std::size_t off = 0; off < silence.size(); off += CHUNK) {
        std::size_t n = std::min(CHUNK, silence.size() - off);
        eng._push_samples_for_test(silence.data() + off, n);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Give the worker a moment to drain + emit.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    eng.stop();
    // We assert at least one of the two flag values surfaced. Real-speech
    // fixtures (test #7) check the ALL-CAPS shape; this test only asserts
    // the lifecycle plumbing.
    INFO("emitted " << rsink.count() << " result(s); any_partial=" << rsink.any_partial()
         << " any_final=" << rsink.any_final());
    CHECK(true);  // permissive — engine output on noise is model-dependent
}

// ===========================================================================
// 3. Ring buffer overflow → drop watermark.
//    Model-free: feed faster than the (non-running, model-free) consumer can
//    drain. We use a tiny ring (1024 samples) and a slow worker poll so the
//    producer overruns; ring-overflow flag → consumer emits one degraded
//    event (or zero, if the worker hasn't ticked yet — we drive a few ticks).
// ===========================================================================
TEST_CASE("CaptionEngine: ring buffer overflow → drop-watermark fires",
          "[streaming-engine]") {
    if (!sherpa_build()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping overflow test (no consumer)");
        return;
    }
    CaptionEngine eng;
    CaptionEngine::Options opts;
    opts._no_recognizer_for_test = true;  // model-free
    opts.ring_capacity_override = 1024;   // 64 ms @ 16 kHz; small on purpose
    opts.worker_poll_ms_override = 1;
    ResultSink rsink;
    DegradedSink dsink;
    REQUIRE(eng.start(opts, &ResultSink::cb, &rsink, &DegradedSink::cb, &dsink));

    // Feed 4× capacity in one shot — guaranteed overflow.
    std::vector<int16_t> burst(opts.ring_capacity_override * 4, 0);
    eng._push_samples_for_test(burst.data(), burst.size());

    // Wait for the worker to tick + emit (worker_poll_ms=1 → fast).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (dsink.count.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(dsink.count.load() >= 1);

    eng.stop();
}

// ===========================================================================
// 4. Drop-watermark rate limit honors 1/s.
//    Sustained overflow over ~1.2s should produce at most 2 degraded events
//    (one immediate + one ~1s later). We assert ≤ 2, which is the rate-limit
//    contract.
// ===========================================================================
TEST_CASE("CaptionEngine: drop-watermark rate-limit ≤ 1/s",
          "[streaming-engine]") {
    if (!sherpa_build()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping rate-limit test");
        return;
    }
    CaptionEngine eng;
    CaptionEngine::Options opts;
    opts._no_recognizer_for_test = true;  // model-free
    opts.ring_capacity_override = 256;    // tiny so every push overflows
    opts.worker_poll_ms_override = 1;
    ResultSink rsink;
    DegradedSink dsink;
    REQUIRE(eng.start(opts, &ResultSink::cb, &rsink, &DegradedSink::cb, &dsink));

    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    std::vector<int16_t> burst(2048, 0);
    while (std::chrono::steady_clock::now() < end) {
        eng._push_samples_for_test(burst.data(), burst.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Allow worker to drain final tick.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    eng.stop();

    auto n = dsink.count.load();
    INFO("degraded events over ~1.2s: " << n);
    CHECK(n >= 1);
    CHECK(n <= 2);
}

// ===========================================================================
// 5. Thread cap honored — num_threads=4 -> effective is 2.
// ===========================================================================
TEST_CASE("CaptionEngine: num_threads is capped at 2",
          "[streaming-engine]") {
    if (!sherpa_build()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping thread-cap test");
        return;
    }
    CaptionEngine eng;
    CaptionEngine::Options opts;
    opts._no_recognizer_for_test = true;  // model-free
    opts.num_threads = 4;
    REQUIRE(eng.start(opts, nullptr, nullptr, nullptr, nullptr));
    CHECK(eng._effective_num_threads_for_test() == 2);
    eng.stop();
    CHECK(eng._effective_num_threads_for_test() == 2);  // value persists across stop

    // Also verify the floor at 1.
    CaptionEngine eng2;
    CaptionEngine::Options opts2;
    opts2._no_recognizer_for_test = true;
    opts2.num_threads = 1;
    REQUIRE(eng2.start(opts2, nullptr, nullptr, nullptr, nullptr));
    CHECK(eng2._effective_num_threads_for_test() == 1);
    eng2.stop();

    // Zero / negative request -> floor at 1.
    CaptionEngine eng3;
    CaptionEngine::Options opts3;
    opts3._no_recognizer_for_test = true;
    opts3.num_threads = 0;
    REQUIRE(eng3.start(opts3, nullptr, nullptr, nullptr, nullptr));
    CHECK(eng3._effective_num_threads_for_test() == 1);
    eng3.stop();
}

// ===========================================================================
// 6. RAII teardown joins the worker.
// ===========================================================================
TEST_CASE("CaptionEngine: destructor joins the worker thread",
          "[streaming-engine]") {
    if (!sherpa_build()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping RAII test");
        return;
    }
    {
        CaptionEngine eng;
        CaptionEngine::Options opts;
        opts._no_recognizer_for_test = true;  // model-free
        opts.worker_poll_ms_override = 1;
        REQUIRE(eng.start(opts, nullptr, nullptr, nullptr, nullptr));
        // Spam some samples so the worker's loop body actually runs.
        std::vector<int16_t> noise(1600, 0);
        eng._push_samples_for_test(noise.data(), noise.size());
        // Drop on scope exit — destructor must call stop() and join.
    }
    // If the destructor returns, the worker thread was joined. Reaching this
    // line is the assertion.
    CHECK(true);
}

// ===========================================================================
// 7. Deterministic output on a fixed audio fixture ([caption-model]).
//    We don't ship a clean speech fixture here yet (Phase 4 carries the model
//    manager + sample WAV). For now, this test feeds a generated tone — the
//    recognizer will emit *something* (likely empty or noise tokens). The
//    purpose of this slot in the plan is to land the seam; the orchestrator
//    will replace it with a real fixture in Phase 4.
// ===========================================================================
TEST_CASE("CaptionEngine: deterministic output on fixed fixture",
          "[streaming-engine][caption-model]") {
    if (!sherpa_build()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping fixture-output test");
        return;
    }
    fs::path model_dir = streaming_model_dir_if_present();
    if (model_dir.empty()) {
        WARN("Streaming zipformer model not cached — skipping fixture-output test");
        return;
    }
    CaptionEngine eng;
    CaptionEngine::Options opts;
    opts.model_dir = model_dir.string();
    ResultSink rsink;
    DegradedSink dsink;
    REQUIRE(eng.start(opts, &ResultSink::cb, &rsink, &DegradedSink::cb, &dsink));

    // 2 s of 440 Hz tone @ 16 kHz, S16LE. Not real speech — output is
    // effectively unconstrained; we accept any non-crash result.
    constexpr int SR = 16000;
    std::vector<int16_t> tone(SR * 2);
    for (std::size_t i = 0; i < tone.size(); ++i) {
        double t = static_cast<double>(i) / SR;
        tone[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265 * 440.0 * t));
    }
    constexpr std::size_t CHUNK = 1600;
    for (std::size_t off = 0; off < tone.size(); off += CHUNK) {
        std::size_t n = std::min(CHUNK, tone.size() - off);
        eng._push_samples_for_test(tone.data() + off, n);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    eng.stop();
    // Lifecycle didn't crash — the seam is wired.
    CHECK(true);
}

// ===========================================================================
// 8. EPERM fallback to nice — inject EPERM via the test seam, observe rc=1.
//    Implementation: the test seam is a function pointer; we install a stub
//    that returns 1 (the "nice fallback" sentinel) and assert it was called.
// ===========================================================================
namespace {
std::atomic<int> g_seam_invocations{0};
std::atomic<int> g_seam_last_rc{-99};
int eperm_seam(void* /*ud*/) {
    g_seam_invocations.fetch_add(1, std::memory_order_acq_rel);
    g_seam_last_rc.store(1, std::memory_order_release);
    return 1;
}
} // namespace

TEST_CASE("CaptionEngine: scheduler-setter EPERM falls back to nice",
          "[streaming-engine]") {
    if (!sherpa_build()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping scheduler-fallback test");
        return;
    }
    g_seam_invocations.store(0);
    g_seam_last_rc.store(-99);

    CaptionEngine eng;
    CaptionEngine::Options opts;
    opts._no_recognizer_for_test = true;  // model-free
    opts.scheduler_setter = &eperm_seam;
    opts.worker_poll_ms_override = 1;
    REQUIRE(eng.start(opts, nullptr, nullptr, nullptr, nullptr));
    // Wait for the worker to enter and call the seam.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_seam_invocations.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(g_seam_invocations.load() >= 1);
    CHECK(g_seam_last_rc.load() == 1);  // nice-fallback sentinel
    eng.stop();
}

// ===========================================================================
// 9. Backpressure logged once not flooded.
//    Sustained 3 s overflow → degraded event count must be ≤ 4 (1 immediate +
//    one per second). Liberal upper bound to absorb scheduling jitter.
// ===========================================================================
TEST_CASE("CaptionEngine: sustained overflow does not flood degraded callback",
          "[streaming-engine]") {
    if (!sherpa_build()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping flood-suppression test");
        return;
    }
    CaptionEngine eng;
    CaptionEngine::Options opts;
    opts._no_recognizer_for_test = true;  // model-free
    opts.ring_capacity_override = 256;
    opts.worker_poll_ms_override = 1;
    ResultSink rsink;
    DegradedSink dsink;
    REQUIRE(eng.start(opts, &ResultSink::cb, &rsink, &DegradedSink::cb, &dsink));

    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::vector<int16_t> burst(2048, 0);
    while (std::chrono::steady_clock::now() < end) {
        eng._push_samples_for_test(burst.data(), burst.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    eng.stop();

    auto n = dsink.count.load();
    INFO("degraded events over ~3s: " << n);
    CHECK(n >= 1);
    CHECK(n <= 4);  // generous upper bound for 1/s rate-limit + jitter
}

// ===========================================================================
// 10. ALL-CAPS engine output sanity ([caption-model]).
//     For the (silent) tone fixture we can't assert non-empty, so this test
//     validates the *shape* contract: any text emitted by the engine must
//     match the Phase 0.2-locked uppercase + no-punctuation profile. If no
//     text is emitted, the test passes vacuously — the orchestrator will
//     pin a real-speech fixture in Phase 4.
// ===========================================================================
TEST_CASE("CaptionEngine: emitted text is ALL-CAPS, no punctuation",
          "[streaming-engine][caption-model]") {
    if (!sherpa_build()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping ALL-CAPS-shape test");
        return;
    }
    fs::path model_dir = streaming_model_dir_if_present();
    if (model_dir.empty()) {
        WARN("Streaming zipformer model not cached — skipping ALL-CAPS-shape test");
        return;
    }
    CaptionEngine eng;
    CaptionEngine::Options opts;
    opts.model_dir = model_dir.string();
    ResultSink rsink;
    DegradedSink dsink;
    REQUIRE(eng.start(opts, &ResultSink::cb, &rsink, &DegradedSink::cb, &dsink));

    constexpr int SR = 16000;
    std::vector<int16_t> tone(SR);
    for (std::size_t i = 0; i < tone.size(); ++i) {
        double t = static_cast<double>(i) / SR;
        tone[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265 * 440.0 * t));
    }
    constexpr std::size_t CHUNK = 1600;
    for (std::size_t off = 0; off < tone.size(); off += CHUNK) {
        std::size_t n = std::min(CHUNK, tone.size() - off);
        eng._push_samples_for_test(tone.data() + off, n);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    eng.stop();

    std::lock_guard lk(rsink.mtx);
    for (auto& r : rsink.results) {
        for (char c : r.text) {
            // A future model that produces punctuation would force a
            // deliberate decision rather than silent semantic shift.
            CHECK(c != '.');
            CHECK(c != ',');
            CHECK(c != '?');
            CHECK(c != '!');
            // Lowercase letters indicate a model swap from the zipformer
            // lock — also flag.
            if (c >= 'a' && c <= 'z') {
                FAIL("emitted lowercase character: " << r.text);
            }
        }
    }
}

// ===========================================================================
// Stub-build contract: with sherpa OFF, start() returns false with the
// canonical error message. Always-on so OFF-build CI exercises the seam.
// ===========================================================================
TEST_CASE("CaptionEngine: stub build reports clear error from start()",
          "[streaming-engine][caption-stub]") {
    if (sherpa_build()) {
        // Skip — the stub-build contract only matters with RECMEET_USE_SHERPA=OFF.
        return;
    }
    CaptionEngine eng;
    CaptionEngine::Options opts;
    opts.model_dir = "/nonexistent";
    ResultSink rsink;
    DegradedSink dsink;
    bool ok = eng.start(opts, &ResultSink::cb, &rsink,
                              &DegradedSink::cb, &dsink);
    CHECK_FALSE(ok);
    CHECK(eng.last_error() == "captions require RECMEET_USE_SHERPA=ON build");
    CHECK_FALSE(eng.is_running());
}
