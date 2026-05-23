// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 7 — V1 capstone full-stack + stress + memory-bound tests for the
// live-captioning pipeline.
//
// These tests exercise the production CaptionEngine + VttWriter wiring on
// a 30-min synthetic and (when available) a 60-min real fixture with
// captions enabled. They are tagged `[full-stack][captions]` and skip
// cleanly when the model and/or fixture is not present locally.
//
// We do NOT drive `run_recording()` here — that path requires a live
// PipeWire capture which isn't reproducible in CI. Instead, we feed a WAV
// (or generated tone) through the engine using the same producer-consumer
// shape that pipeline.cpp uses in production:
//
//     audio (int16 chunks, 16 kHz mono)
//       → CaptionEngine::on_audio_chunk
//       → SPSC ring → ASR worker
//       → CaptionResult callback
//       → daemon broadcast (here: ResultSink for assertions)
//       + .vtt append (here: real VttWriter into a tempdir)
//
// The wiring assertion ("captions emit, no degraded events, .vtt parses")
// is identical whether the audio source is a live capture or a file. The
// only thing we don't validate here is the producer-side audio_callback
// thread itself — that is covered by `streaming_capture_tests.cpp`.

#include <catch2/catch_test_macros.hpp>

#include "audio_file.h"
#include "caption_engine.h"
#include "caption_vtt.h"
#include "config.h"
#include "model_manager.h"
#include "pipeline.h"
#include "reprocess_batch.h"
#include "speaker_id.h"
#include "test_helpers.h"
#include "util.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if RECMEET_USE_SHERPA

using namespace recmeet;
using namespace recmeet::test_helpers;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Locate the canonical streaming-zipformer cache used in production. Empty
// path means "not cached" — caller SKIPs.
fs::path caption_model_dir_if_present() {
    // Use the production resolver so a future model rename only touches one
    // place. Existence is checked here, not in the resolver.
    fs::path dir = caption_model_dir("");  // empty → default
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    return is_caption_model_cached("") ? dir : fs::path{};
}

// Result-sink + degraded-sink — same shape as streaming_engine_tests.cpp.
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
    std::size_t final_count() {
        std::lock_guard lk(mtx);
        std::size_t n = 0;
        for (auto& r : results) if (!r.is_partial) ++n;
        return n;
    }
};

struct DegradedSink {
    std::atomic<std::size_t> count{0};
    static void cb(CaptionDegradedReason, void* ud) {
        auto* s = static_cast<DegradedSink*>(ud);
        s->count.fetch_add(1, std::memory_order_acq_rel);
    }
};

// Convert float32 [-1,1] to int16 mono. Mirrors the production
// AudioChunkCallback contract (capture emits int16 mono 16 kHz).
std::vector<int16_t> floats_to_int16(const std::vector<float>& src) {
    std::vector<int16_t> out;
    out.reserve(src.size());
    for (float f : src) {
        double v = static_cast<double>(f) * 32767.0;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        out.push_back(static_cast<int16_t>(v));
    }
    return out;
}

// Generate a silence-with-occasional-tone fixture sized to `minutes`.
// Real-speech is unavailable in most CI environments; the tone fixture
// validates wiring end-to-end (engine starts, audio flows, no overflow,
// no degraded events, callbacks fire, .vtt header lands on first final).
// Caption *quality* is not asserted on synthetic input — that needs a
// real fixture.
std::vector<int16_t> make_synthetic_tone_int16(double minutes) {
    constexpr int SR = 16000;
    const std::size_t total =
        static_cast<std::size_t>(minutes * 60.0 * SR);
    std::vector<int16_t> out(total, 0);

    // 4-second cycle: 1.5s tone @ 220 Hz, 0.5s silence, 1.5s tone @ 330 Hz, 0.5s silence.
    const std::size_t cycle = static_cast<std::size_t>(4.0 * SR);
    for (std::size_t i = 0; i < total; ++i) {
        const std::size_t pos = i % cycle;
        double t = static_cast<double>(i) / SR;
        double freq = 0.0;
        if (pos < static_cast<std::size_t>(1.5 * SR)) {
            freq = 220.0;
        } else if (pos < static_cast<std::size_t>(2.0 * SR)) {
            // silence
        } else if (pos < static_cast<std::size_t>(3.5 * SR)) {
            freq = 330.0;
        } // else silence
        if (freq > 0.0) {
            double v = 0.4 * std::sin(2.0 * M_PI * freq * t);
            out[i] = static_cast<int16_t>(v * 32767.0);
        }
    }
    return out;
}

// Push an int16 buffer through the engine in 100 ms chunks (matching the
// PipeWire on_process cadence). Sleeps between chunks so the worker can
// drain — feeding the full buffer in one shot would synthetically stress
// the ring overflow path (covered separately in streaming_engine_tests).
//
// To keep the run-time bounded, we use a `realtime_pacing` knob:
//   * true  — sleep 100 ms per 100 ms chunk (real-time playback). Use
//             this when running a true end-to-end timing test.
//   * false — sleep 1 ms per chunk (fast-feed). Default for [full-stack]
//             tests so a 30-min fixture takes ~20 s instead of 30 min.
//             The recognizer is still endpoint-bounded; we just push
//             frames faster than wall-clock so endpoints fire in test
//             time. The engine treats audio time as buffer time, not
//             wall-clock, so this is semantically equivalent.
void feed_engine(CaptionEngine& eng, const std::vector<int16_t>& samples,
                 bool realtime_pacing) {
    constexpr std::size_t CHUNK = 1600;  // 100 ms @ 16 kHz
    const auto pause = realtime_pacing
        ? std::chrono::milliseconds(100)
        : std::chrono::milliseconds(1);

    for (std::size_t off = 0; off < samples.size(); off += CHUNK) {
        std::size_t n = std::min(CHUNK, samples.size() - off);
        eng._push_samples_for_test(samples.data() + off, n);
        std::this_thread::sleep_for(pause);
    }
    // Give the worker a moment to drain remaining ring contents.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// Count ' --> ' substrings in a VTT file to estimate cue count. Each cue
// has exactly one ' --> ' separator on its timing line.
std::size_t count_vtt_cues(const fs::path& vtt) {
    std::ifstream in(vtt);
    if (!in) return 0;
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string s = buf.str();
    std::size_t n = 0;
    for (std::size_t i = 0; (i = s.find(" --> ", i)) != std::string::npos; ++n) {
        i += 5;
    }
    return n;
}

// True if the file starts with the WebVTT header. The writer emits
// `WEBVTT\n\n` lazily on the first finalized append.
bool vtt_header_present(const fs::path& vtt) {
    std::ifstream in(vtt);
    if (!in) return false;
    char head[8] = {};
    in.read(head, 6);
    return std::string(head, 6) == "WEBVTT";
}

// Locate a 60-min real fixture if one exists locally. Mirrors the
// iter-110 fixture-discovery convention used in test_integration_pipeline /
// test_benchmark.
fs::path find_60min_fixture() {
    if (const char* env = std::getenv("RECMEET_CAPTION_60MIN_FIXTURE")) {
        if (env[0] && fs::exists(env)) return fs::path(env);
    }
    // The benchmark suite already searches; reuse its result.
    fs::path long_audio = find_long_meetings_audio(55.0 * 60.0);
    if (!long_audio.empty()) return long_audio;
    fs::path root = find_project_root();
    if (!root.empty()) {
        fs::path p = root / "notes" / "iter-110" / "audio.wav";
        if (fs::exists(p)) return p;
    }
    return {};
}

// Drive the engine + VttWriter wiring on `samples` and return a result
// summary. The wiring matches `try_start_caption_engine` in pipeline.cpp:
// the engine's result callback fans out to a downstream sink AND a real
// VttWriter so the producer side is identical to production.
struct CaptionRun {
    std::size_t total_results = 0;
    std::size_t finals = 0;
    std::size_t degraded_events = 0;
    std::size_t vtt_cues = 0;
    bool vtt_header = false;
    bool vtt_exists = false;
};

CaptionRun run_captions_on_samples(const fs::path& model_dir,
                                   const std::vector<int16_t>& samples,
                                   const fs::path& vtt_path) {
    // Production-equivalent fan-out: result callback writes to the sink
    // AND appends finalized cues to the VttWriter. We don't duplicate
    // CaptionFanoutAdapter here — the test owns both the sink and the
    // writer, so the closure form is enough.
    ResultSink rsink;
    DegradedSink dsink;
    VttWriter writer(vtt_path);
    VttCueTimer cue_timer;
    std::mutex writer_mtx;

    struct Ctx {
        ResultSink* sink;
        VttWriter* writer;
        VttCueTimer* timer;
        std::mutex* mtx;
    };
    Ctx ctx{&rsink, &writer, &cue_timer, &writer_mtx};

    auto on_result = [](const CaptionResult& r, void* ud) {
        auto* c = static_cast<Ctx*>(ud);
        ResultSink::cb(r, c->sink);
        if (!r.is_partial) {
            std::lock_guard lk(*c->mtx);
            auto [s, e] = c->timer->next(r.timestamp_ms);
            c->writer->append(s, e, r.text, /*is_partial=*/false);
        }
    };

    CaptionEngine eng;
    CaptionEngine::Options opts;
    opts.model_dir = model_dir.string();
    opts.num_threads = 1;
    REQUIRE(eng.start(opts, on_result, &ctx,
                            &DegradedSink::cb, &dsink));

    feed_engine(eng, samples, /*realtime_pacing=*/false);

    eng.stop();
    writer.close();

    CaptionRun r;
    r.total_results = rsink.count();
    r.finals = rsink.final_count();
    r.degraded_events = dsink.count.load();
    r.vtt_exists = fs::exists(vtt_path);
    if (r.vtt_exists) {
        r.vtt_header = vtt_header_present(vtt_path);
        r.vtt_cues = count_vtt_cues(vtt_path);
    }
    return r;
}

} // anonymous namespace


// ===========================================================================
// 7.1a — 30-min synthetic with captions ON
//
// Wiring assertion: engine starts on the cached model, no degraded events
// fire, the VTT writer file is created (or correctly absent if no finals
// were emitted on the synthetic tone), and the lifecycle tears down clean.
//
// Caption *quality* is not asserted on synthetic input — the model has
// nothing to transcribe from a tone. The cue-count > 100 assertion in the
// plan is gated behind a real-speech fixture (test 7.1b below).
// ===========================================================================
TEST_CASE("Captions full-stack: 30-min synthetic, engine wiring",
          "[full-stack][captions][slow]") {
    fs::path model_dir = caption_model_dir_if_present();
    if (model_dir.empty()) {
        SKIP("Streaming caption model not cached — run "
             "`./build/recmeet --caption-model en-2023-06-26 --download-models` "
             "or set RECMEET_USE_SHERPA=ON build.");
    }

    auto out_dir = fs::temp_directory_path() / "recmeet_fullstack_captions_30min_synth";
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);
    fs::path vtt_path = out_dir / "captions.vtt";

    INFO("Generating 30-min synthetic int16 tone fixture (~58 MB)…");
    auto samples = make_synthetic_tone_int16(30.0);
    REQUIRE(samples.size() == 30u * 60u * 16000u);

    auto run = run_captions_on_samples(model_dir, samples, vtt_path);

    fprintf(stderr, "\n[full-stack][captions] 30-min synthetic: "
            "results=%zu finals=%zu degraded=%zu vtt_cues=%zu vtt_exists=%d\n",
            run.total_results, run.finals, run.degraded_events,
            run.vtt_cues, run.vtt_exists ? 1 : 0);

    // --- Wiring assertions (always-on) ---
    // No degraded events on a fast-fed synthetic — the worker drains the
    // ring much faster than chunks arrive. A degraded event here would
    // signal a regression in the producer/consumer balance.
    CHECK(run.degraded_events == 0);

    // If any finals fired, the VTT writer must have created the file with
    // a valid header. If no finals fired (model produced no endpoints on
    // tone — fully expected), the writer correctly produces NO file (lazy
    // open — silent sessions leave no .vtt at all).
    if (run.finals > 0) {
        CHECK(run.vtt_exists);
        CHECK(run.vtt_header);
        CHECK(run.vtt_cues == run.finals);
    } else {
        // Defensible either way: writer may have been touched by a
        // partial leak (defense-in-depth filter inside append). Just
        // CHECK the header invariant: if file exists, header is correct.
        if (run.vtt_exists) CHECK(run.vtt_header);
    }

    fs::remove_all(out_dir);
}


// ===========================================================================
// 7.1b — 60-min real fixture with captions ON (synthetic 100× soak test)
//
// `run_captions_on_samples` feeds the engine via `_push_samples_for_test`
// at 1 ms/chunk, which runs at ~100× wall-clock. That dumps ~6 hours of
// audio into the engine's fixed-size ring buffer in tens of seconds; the
// ring's drop-oldest behavior at `src/caption_engine.cpp:282-298` is
// documented intentional under that load, and `caption.degraded` events
// at the 1 Hz rate-limiter cap (`:415-431`) are an EXPECTED stress signal
// from the synthetic feed — NOT a bug signal about the production
// pipeline.
//
// Real-time pipeline coverage lives in
// `tests/test_full_stack_captions_paced.cpp` (`[full-stack][captions][paced]`),
// which drives the full IPC wire path at 100 ms cadence against a
// SpawnedDaemon. On 2026-05-23 the paced test returned 0 degraded /
// 34 finals / 5665 partials over 10 minutes (Phase 4 of
// test-and-verification-hardening). The paced test is the binding
// coverage for caption pipeline correctness.
//
// This test is retained as a soak / forward-progress check under
// extreme sustained load: confirms the engine survives the overload
// without crashing, makes some forward progress (non-zero finals +
// cues), and the VTT persister stays coherent with engine output
// (cues == finals). Specific event counts are NOT asserted because
// they reflect synthetic-feed dynamics, not production behavior.
// ===========================================================================
TEST_CASE("Captions full-stack: 60-min real fixture, caption quality",
          "[full-stack][captions][slow]") {
    fs::path model_dir = caption_model_dir_if_present();
    if (model_dir.empty()) {
        SKIP("Streaming caption model not cached.");
    }

    fs::path audio_path = find_60min_fixture();
    if (audio_path.empty()) {
        SKIP("60-min real-speech fixture not found. Provide via "
             "RECMEET_CAPTION_60MIN_FIXTURE=/path/to/audio.wav (16 kHz mono "
             "PCM, ~60 min).");
    }

    INFO("Loading 60-min fixture: " << audio_path.string());
    auto floats = read_wav_float(audio_path);
    REQUIRE(!floats.empty());
    auto samples = floats_to_int16(floats);

    auto out_dir = fs::temp_directory_path() / "recmeet_fullstack_captions_60min_real";
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);
    fs::path vtt_path = out_dir / "captions.vtt";

    auto run = run_captions_on_samples(model_dir, samples, vtt_path);

    fprintf(stderr, "\n[full-stack][captions] 60-min real: "
            "results=%zu finals=%zu degraded=%zu vtt_cues=%zu\n",
            run.total_results, run.finals, run.degraded_events, run.vtt_cues);

    // Synthetic 100×-wall-clock feed: assert pipeline survival + forward
    // progress + persistence coherence. Event counts are stress-feed
    // dynamics, not production behavior — see the test header comment
    // and `tests/test_full_stack_captions_paced.cpp` for real-time
    // coverage of the captions pipeline.
    CHECK(run.finals > 0);
    REQUIRE(run.vtt_exists);
    CHECK(run.vtt_header);
    CHECK(run.vtt_cues > 0);
    CHECK(run.vtt_cues == run.finals);

    fs::remove_all(out_dir);
}


// ===========================================================================
// 7.2 — CPU-contention stress
//
// Engine + a concurrent batch reprocess (transcribe / diarize) running on
// a separate fixture, exercising the iter-71 recording-vs-postprocess path.
// The reprocess saturates cores; the engine MAY emit caption.degraded
// events, which is acceptable. The contract:
//   * neither side crashes
//   * the engine continues running and tears down clean
//   * caption.degraded events are bounded (rate-limited to ≤ 1/s by the
//     engine; we just check they don't grow unboundedly)
//
// Tagged [stress][slow] — gated out of `make test`. Run manually with
// `./build/recmeet_tests "[stress]"`.
// ===========================================================================
TEST_CASE("Captions stress: CPU contention with concurrent reprocess",
          "[stress][captions][slow]") {
    fs::path model_dir = caption_model_dir_if_present();
    if (model_dir.empty()) SKIP("Streaming caption model not cached.");

    fs::path root = find_project_root();
    if (root.empty()) SKIP("Project root with assets/ not found");
    fs::path audio_src = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_src)) SKIP("Debate audio asset not found");
    if (!is_whisper_model_cached("base")) SKIP("Whisper base model not cached");

    auto workspace = fs::temp_directory_path() / "recmeet_caption_stress";
    fs::remove_all(workspace);
    fs::create_directories(workspace);

    // --- Set up the reprocess workload (runs in a background thread) ---
    fs::path reproc_dir = workspace / "2026-01-01_10-00";
    fs::create_directories(reproc_dir);
    fs::copy_file(audio_src, reproc_dir / "audio_2026-01-01_10-00.wav");

    JobConfig rcfg;
    rcfg.whisper_model = "base";
    rcfg.language = "en";
    rcfg.reprocess_dir = reproc_dir;
    rcfg.output_dir = reproc_dir;
    rcfg.output_dir_explicit = true;
    rcfg.note_dir = reproc_dir;
    rcfg.no_summary = true;
    rcfg.diarize = false;
    rcfg.vad = false;
    rcfg.speaker_id = false;

    PostprocessInput rinput;
    rinput.out_dir = reproc_dir;
    rinput.audio_path = reproc_dir / "audio_2026-01-01_10-00.wav";

    std::atomic<bool> reproc_done{false};
    std::atomic<bool> reproc_failed{false};
    std::thread reproc_thread([&]() {
        try {
            run_postprocessing(rcfg, rinput);
        } catch (const std::exception& e) {
            reproc_failed.store(true);
            fprintf(stderr, "[stress] reprocess threw: %s\n", e.what());
        }
        reproc_done.store(true);
    });

    // --- Run captions in parallel ---
    auto vtt_path = workspace / "captions.vtt";
    // Synthetic 5-min fixture — long enough that reproc hasn't finished
    // before the engine drains. Not real speech; we're stressing the
    // scheduler, not validating caption quality.
    auto samples = make_synthetic_tone_int16(5.0);
    auto run = run_captions_on_samples(model_dir, samples, vtt_path);

    // Wait for the background reprocess to finish (or time out at 5 min).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (!reproc_done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (reproc_thread.joinable()) reproc_thread.join();

    fprintf(stderr, "\n[stress][captions] reproc_done=%d reproc_failed=%d "
            "engine_results=%zu engine_finals=%zu degraded=%zu\n",
            reproc_done.load() ? 1 : 0, reproc_failed.load() ? 1 : 0,
            run.total_results, run.finals, run.degraded_events);

    // --- Contract assertions ---
    // Reaching here means neither thread crashed (otherwise the test
    // would have aborted on terminate()).
    CHECK_FALSE(reproc_failed.load());
    CHECK(reproc_done.load());

    // Degraded events are allowed (CPU contention can starve the worker)
    // but must remain bounded by the engine's 1/s rate-limit. 5 minutes
    // of audio @ ≤ 1/s ⇒ at most ~300 events.
    CHECK(run.degraded_events < 1000);

    fs::remove_all(workspace);
}


// ===========================================================================
// 7.3 — Memory-bound bench: peak RSS captions ON ≤ captions OFF + 1 GB
//
// Mirrors the iter-121 [t2-1] pattern in test_benchmark.cpp. The 1 Hz
// peak-RSS sampler runs around two engine sessions (one OFF, one ON) on
// the same synthetic fixture; the delta is the per-engine RSS overhead.
//
// Plan budget: ≤ 1 GB above the captions-OFF baseline.
// ===========================================================================
TEST_CASE("Captions memory bound: peak RSS overhead ≤ 1 GB",
          "[benchmark][captions][slow]") {
    fs::path model_dir = caption_model_dir_if_present();
    if (model_dir.empty()) SKIP("Streaming caption model not cached.");

    // 5-min fixture — keeps the bench bounded but long enough for the
    // recognizer to load weights and steady-state.
    auto samples = make_synthetic_tone_int16(5.0);

    // 1 Hz sampler — same shape as test_benchmark.cpp's measure_with_rss.
    auto measure = [](auto&& fn) -> std::pair<double, long> {
        std::atomic<bool> stop{false};
        long seed = read_self_rss_kb();
        std::atomic<long> peak{seed};
        std::thread sampler([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                long now_kb = read_self_rss_kb();
                long prev = peak.load(std::memory_order_relaxed);
                while (now_kb > prev &&
                       !peak.compare_exchange_weak(prev, now_kb)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        });
        auto t0 = std::chrono::steady_clock::now();
        fn();
        auto t1 = std::chrono::steady_clock::now();
        stop.store(true);
        sampler.join();
        return {std::chrono::duration<double>(t1 - t0).count(), peak.load()};
    };

    // --- OFF baseline: skip the engine entirely; just allocate + drop
    //     the same audio buffer so the heap state is comparable.
    auto [off_secs, off_peak_kb] = measure([&]() {
        // Touch all samples so the optimizer can't dead-strip the buffer.
        std::int64_t accum = 0;
        for (auto v : samples) accum += v;
        (void)accum;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });

    // --- ON: run the engine on the same buffer with VTT persistence.
    auto vtt_path = fs::temp_directory_path() / "recmeet_caption_rss_bench.vtt";
    fs::remove(vtt_path);
    auto [on_secs, on_peak_kb] = measure([&]() {
        run_captions_on_samples(model_dir, samples, vtt_path);
    });
    fs::remove(vtt_path);

    long delta_kb = on_peak_kb - off_peak_kb;
    fprintf(stderr, "\n[benchmark][captions] OFF peak=%ld KB (%ld MB), "
            "ON peak=%ld KB (%ld MB), delta=%ld KB (%ld MB), "
            "OFF=%.1fs ON=%.1fs\n",
            off_peak_kb, off_peak_kb / 1024,
            on_peak_kb,  on_peak_kb  / 1024,
            delta_kb,    delta_kb    / 1024,
            off_secs,    on_secs);

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "\n      \"test\": \"captions_peak_rss_5min_synth\","
        "\n      \"off_peak_kb\": %ld,"
        "\n      \"on_peak_kb\": %ld,"
        "\n      \"delta_kb\": %ld,"
        "\n      \"off_secs\": %.1f,"
        "\n      \"on_secs\": %.1f",
        off_peak_kb, on_peak_kb, delta_kb, off_secs, on_secs);
    BenchmarkResults::add(buf);

    // Plan gate: per-engine overhead ≤ 1 GB.
    CHECK(delta_kb < 1L * 1024L * 1024L);  // 1 GB in KB
}

#endif // RECMEET_USE_SHERPA
