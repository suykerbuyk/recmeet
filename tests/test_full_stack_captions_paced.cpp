// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 4 step 2 — captions real-time-paced wire-pump discriminator.
//
// Purpose: distinguish whether the 60-min captions test's 248-degraded
// count is a test-shape artifact (1ms/chunk feed at 100x wall-clock
// overflowing the engine ring by design, see caption_engine.cpp:282-298)
// or a producer-side scheduling bug in the live wire path.
//
// Shape (mirrors test_full_stack_live.cpp's SpawnedDaemon pattern but
// targets the streaming verbs `process.stream` + `process.stream.commit`
// rather than the batch `process.submit`):
//
//   1. SKIP gates: project root + assets/biden_trump_debate_2020.wav +
//      whisper tiny + sherpa streaming-zipformer en-2023-06-26 cached.
//   2. SpawnedDaemon over Unix socket (summary off, diarization off).
//   3. IpcClient connect + session.init(output_dir, note_dir).
//   4. process.stream → get stream_token (s16le mono 16kHz, captions on,
//      meeting_id minted as canonical UUID v4).
//   5. Worker thread feeds 100ms PCM chunks (1600 samples = 3200 bytes)
//      at 100ms cadence using steady_clock + sleep_until (NOT sleep_for —
//      drift accumulates over 6000 ticks). Loops the ~15-min fixture as
//      needed to reach >=10 minutes wall-clock.
//   6. Main thread polls for events via IpcClient::read_events and
//      buckets caption.degraded by reason, counts caption finals/partials.
//   7. Worker sends process.stream.commit at end-of-input; main thread
//      then drains events until the commit response arrives.
//   8. INFO-log all counts; REQUIRE only that the test ran to completion
//      without IPC errors.
//
// The operator reads the numbers from the test runner output to decide
// whether the captions regression is real (non-trivial degraded events
// at real-time pacing) or a test-shape artifact (zero or near-zero
// degraded events).
//
// Tag layout [full-stack][captions][paced]: kept distinct from the
// existing [full-stack][captions] so the synthetic-feed test stays the
// historical baseline.

#include <catch2/catch_test_macros.hpp>

#include "full_stack_helpers.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "json_util.h"
#include "model_manager.h"
#include "test_helpers.h"
#include "test_progress_phase.h"
#include "test_tmpdir.h"

#include "audio_file.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if RECMEET_USE_SHERPA

namespace recmeet {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// Mint a canonical lowercase UUID v4 — required for the process.stream
// `meeting_id` field (the daemon-side handler validates via
// is_valid_meeting_id() at daemon_handlers.cpp:391). Same shape as the
// helper in test_full_stack_live.cpp.
std::string make_uuid_v4() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t a = dist(gen);
    uint64_t b = dist(gen);
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>((a >> 32) & 0xFFFFFFFFULL),
                  static_cast<unsigned>((a >> 16) & 0xFFFFULL),
                  static_cast<unsigned>(a & 0xFFFFULL),
                  static_cast<unsigned>((b >> 48) & 0xFFFFULL),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return std::string(buf, 36);
}

// Write daemon.yaml pinning meetings_root + disabling summary/diarization.
// Same shape as test_full_stack_live.cpp's write_daemon_yaml — duplicated
// per the established no-cross-test-helper pattern.
void write_daemon_yaml(const fs::path& xdg_config_dir,
                       const fs::path& meetings_root) {
    fs::path cfg_dir = xdg_config_dir / "recmeet";
    fs::create_directories(cfg_dir);
    std::ofstream cfg(cfg_dir / "daemon.yaml");
    REQUIRE(cfg.is_open());
    cfg << "# recmeet full-stack captions-paced test config\n"
        << "summary:\n"
        << "  disabled: true\n"
        << "diarization:\n"
        << "  enabled: false\n"
        << "server:\n"
        << "  meetings_root: \"" << meetings_root.string() << "\"\n";
}

// Convert float [-1,1] → int16 mono. Same as the existing 60-min path.
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

// Mirrors caption_model_dir_if_present() in test_full_stack_captions.cpp.
fs::path caption_model_dir_if_present() {
    fs::path dir = caption_model_dir("");  // empty → default name
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    return is_caption_model_cached("") ? dir : fs::path{};
}

// Aggregated caption-event observations. Shared between the event
// callback (called on the calling thread inside IpcClient::call /
// read_events) and the main test thread. Access is single-threaded by
// construction: the callback runs only on the thread that drives
// read_events / call, which IS the main test thread (the worker only
// calls send_stream_audio + a one-shot process.stream.commit from a
// separate IpcClient connection per the design below).
//
// Per Phase 4 step 2 brief: bucket caption.degraded by `reason` field.
// The daemon emits "buffer_overrun" or "engine_error" today (see
// streaming_session.cpp:481-489, :501-502). Any other value is logged
// verbatim under the unknown-reason bucket so an undocumented reason
// added later still shows up in the report.
struct CaptionStats {
    std::map<std::string, std::size_t> degraded_by_reason;
    std::size_t caption_partials = 0;
    std::size_t caption_finals = 0;
    std::size_t other_caption_events = 0;  // any "caption.*" we don't
                                           // explicitly classify
};

} // anonymous namespace


// ===========================================================================
// 4.2 — real-time wire-pump captions discriminator
//
// The discriminator: feeds the debate fixture into the captions pipeline
// at real-time pacing (100ms chunks at 100ms cadence — matches the tray's
// `tray_stream_pump` cadence at src/tray.cpp:2046-2047) against a real
// SpawnedDaemon over the IPC wire path. Counts degraded events. Two
// outcomes, both informative:
//
//   * Zero/near-zero degraded events → the 60-min test's 248 count is a
//     test-shape artifact (the 1ms/chunk synthetic feed at 100x
//     wall-clock overflows the engine ring by design). The fix is to
//     rewrite the existing assertion shape.
//   * Non-trivial degraded events at real-time pacing → there IS a
//     producer-side bug. The 10-min real-time repro is then the bisect
//     substrate against commit 5cc6e59 (original C.10a producer).
//
// The test does NOT REQUIRE specific counts — it logs them via INFO so
// the operator can read the numbers from runner output and decide
// assertion shape after the experiment.
// ===========================================================================
TEST_CASE("Captions paced full-stack: real-time wire-pump degraded-events discriminator",
          "[full-stack][captions][paced]") {
    using clock = std::chrono::steady_clock;
    const auto test_t0 = clock::now();

    // --------------------------------------------------------------------
    // 0. SKIP gates — every missing precondition is a SKIP, not a FAIL.
    // --------------------------------------------------------------------
    fs::path root = test_helpers::find_project_root();
    if (root.empty()) SKIP("Project root with assets/ not found");

    fs::path audio_src = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_src)) SKIP("Debate audio asset not found");

    // Whisper isn't strictly used by the streaming path itself, but the
    // process.stream.commit handoff enqueues a postprocess job whose
    // worker tries to load whatever model session_init pinned. We pick
    // the first cached candidate (tiny → base → small → medium) so the
    // gate works on any host the operator has primed for the other
    // [full-stack] tests. The discriminator's signal — degraded events
    // during the live stream — does NOT depend on which whisper model
    // is selected; that only affects the post-commit transcription.
    std::string whisper_model;
    for (const char* m : {"tiny", "base", "small", "medium"}) {
        if (is_whisper_model_cached(m)) { whisper_model = m; break; }
    }
    if (whisper_model.empty())
        SKIP("No whisper model cached (tried tiny/base/small/medium)");

    fs::path caption_model = caption_model_dir_if_present();
    if (caption_model.empty()) {
        SKIP("Streaming caption model (sherpa-onnx-streaming-zipformer-"
             "en-2023-06-26) not cached at "
             "~/.local/share/recmeet/models/sherpa/online/en-2023-06-26 — "
             "the captions discriminator needs the live engine.");
    }

    // --------------------------------------------------------------------
    // 1. Per-test workdir + daemon config.
    // --------------------------------------------------------------------
    fs::path workdir = recmeet::test::tmp_path(
        "recmeet_full_stack_captions_paced");
    fs::remove_all(workdir);
    fs::create_directories(workdir);

    fs::path xdg_config = workdir / "config";
    fs::path meetings   = workdir / "meetings";
    fs::path sock_dir   = workdir / "sock";
    fs::create_directories(meetings);
    fs::create_directories(sock_dir);

    write_daemon_yaml(xdg_config, meetings);

    // --------------------------------------------------------------------
    // 2. SpawnedDaemon over Unix socket.
    // --------------------------------------------------------------------
    fs::path sock_path = sock_dir / "daemon.sock";
    fs::path daemon_bin = full_stack::find_daemon_binary();

    full_stack::SpawnedDaemon daemon(
        daemon_bin,
        full_stack::SpawnedDaemon::Transport::Unix,
        /*tcp_addr=*/std::string{},
        /*psk=*/std::string{},
        xdg_config,
        sock_path);
    REQUIRE(daemon.pid() > 0);

    // --------------------------------------------------------------------
    // 3. IpcClient connect + session.init. The main thread owns this
    //    connection; it drives process.stream, the event poll loop, and
    //    process.stream.commit. The worker thread uses send_stream_audio
    //    on the SAME IpcClient instance — send_stream_audio is a
    //    fire-and-forget write to fd_ (no per-frame response, no
    //    pending-id state to coordinate with the main thread's
    //    call()/read_events()). The fd write is the only contention
    //    point; the kernel serialises concurrent write(2)s on a single
    //    fd at frame boundaries since each frame is one write() call
    //    (the write loop runs to completion under blocking semantics).
    //    With 100ms cadence (10 Hz) and a 5-byte header + 3.2 KB
    //    payload, partial-write interleaving is effectively impossible.
    // --------------------------------------------------------------------
    IpcClient client(sock_path.string());
    REQUIRE(client.connect());
    CHECK_FALSE(client.is_remote());
    REQUIRE(client.protocol_version() == IPC_PROTOCOL_VERSION);

    {
        JsonMap creds;
        JsonMap prefs;
        prefs["output_dir"]        = meetings.string();
        prefs["note_dir"]          = meetings.string();
        prefs["whisper_model"]     = whisper_model;
        prefs["language"]          = std::string("en");
        prefs["captions_enabled"]  = true;
        prefs["caption_latency_ms"] = static_cast<int64_t>(500);

        IpcResponse resp;
        IpcError err;
        REQUIRE(client.session_init(creds, prefs, resp, err));
        REQUIRE(json_val_as_bool(resp.result["ok"]) == true);
    }

    // --------------------------------------------------------------------
    // 4. Wire the event callback BEFORE process.stream — caption events
    //    can race the response in principle (the daemon emits them from
    //    the engine worker via server.post(), so they're queued behind
    //    the response, but the callback must be in place by the time
    //    read_events drains). Stats are mutated only on the main
    //    thread (the callback runs synchronously inside call() /
    //    read_events()), so no mutex is needed.
    // --------------------------------------------------------------------
    CaptionStats stats;
    int64_t       stream_job_id_seen = 0;
    client.set_event_callback([&stats, &stream_job_id_seen](const IpcEvent& ev) {
        // Captions can attach a `job_id` field — capture the first
        // non-zero one we see so the operator can correlate with the
        // streaming Job id printed by daemon log lines.
        if (ev.event == "caption") {
            bool is_partial = false;
            auto pit = ev.data.find("is_partial");
            if (pit != ev.data.end()) is_partial = json_val_as_bool(pit->second);
            if (is_partial) stats.caption_partials++;
            else            stats.caption_finals++;
            if (stream_job_id_seen == 0) {
                auto jit = ev.data.find("job_id");
                if (jit != ev.data.end())
                    stream_job_id_seen = json_val_as_int(jit->second);
            }
        } else if (ev.event == "caption.degraded") {
            std::string reason;
            auto rit = ev.data.find("reason");
            if (rit != ev.data.end())
                reason = json_val_as_string(rit->second);
            if (reason.empty()) reason = "<missing>";
            stats.degraded_by_reason[reason]++;
        } else if (ev.event.rfind("caption", 0) == 0) {
            // any other caption.* event (caption.partial separate frame,
            // future shapes, etc.)
            stats.other_caption_events++;
        }
    });

    // --------------------------------------------------------------------
    // 5. Open the streaming session via process.stream. The
    //    `meeting_id` field is required to be a canonical UUID v4 by
    //    the daemon's wire-boundary guard (daemon_handlers.cpp:391-396);
    //    legacy v1 clients omit it but we set it to be explicit.
    // --------------------------------------------------------------------
    recmeet::test::PhaseEcho echo;
    echo("captions start");
    const std::string meeting_id = make_uuid_v4();
    std::string stream_token;
    int64_t     stream_job_id = 0;
    {
        JsonMap p;
        p["format"]            = std::string("s16le");
        p["sample_rate"]       = static_cast<int64_t>(16000);
        p["channels"]          = static_cast<int64_t>(1);
        p["language"]          = std::string("en");
        p["captions_enabled"]  = true;
        p["latency_budget_ms"] = static_cast<int64_t>(500);
        p["meeting_id"]        = meeting_id;
        p["context"]           = std::string("");  // optional

        IpcResponse resp;
        IpcError err;
        if (!client.call("process.stream", p, resp, err, /*timeout_ms=*/10000)) {
            FAIL("process.stream call failed: code=" << err.code
                 << " message=" << err.message);
        }
        stream_token = json_val_as_string(resp.result["stream_token"]);
        stream_job_id = json_val_as_int(resp.result["job_id"]);
        REQUIRE_FALSE(stream_token.empty());
        REQUIRE(stream_job_id > 0);
    }
    INFO("stream_token=" << stream_token << " stream_job_id=" << stream_job_id);

    // --------------------------------------------------------------------
    // 6. Load the debate fixture and convert to int16 mono.
    //
    // The fixture is RIFF/PCM 16-bit mono 16 kHz with a `LIST` chunk
    // between fmt and data — we use the project's libsndfile-backed
    // read_wav_float helper rather than a hand-coded 44-byte-header
    // skip so the chunk layout is handled correctly.
    // --------------------------------------------------------------------
    INFO("Loading debate fixture: " << audio_src.string());
    auto floats = read_wav_float(audio_src);
    REQUIRE_FALSE(floats.empty());
    auto fixture_samples = floats_to_int16(floats);
    const std::size_t fixture_count = fixture_samples.size();
    const double fixture_seconds =
        static_cast<double>(fixture_count) / 16000.0;
    INFO("fixture samples=" << fixture_count
         << " duration=" << fixture_seconds << " s");

    // --------------------------------------------------------------------
    // 7. Spawn the paced wire-pump worker.
    //
    //   target wall-clock: max(10 min, 1 fixture pass)
    //   chunk:             1600 samples (100ms @ 16 kHz) = 3200 bytes
    //   cadence:           100ms via sleep_until (drift-resistant)
    //   total ticks:       600 (10 min) — looping the fixture as needed
    //
    // The worker maintains a steady_clock-relative schedule (NOT a
    // sleep_for stack-up); drift between expected and actual elapsed
    // is reported via the pump's own atomic counter so the operator
    // can validate the real-time-ness of the run.
    // --------------------------------------------------------------------
    constexpr std::size_t kChunkSamples = 1600;
    constexpr int kTargetSeconds = 600;  // 10 minutes
    constexpr int kTotalTicks    = kTargetSeconds * 10;  // 10 Hz cadence
    constexpr auto kTickPeriod   = std::chrono::milliseconds(100);

    std::atomic<int>    ticks_sent{0};
    std::atomic<int>    write_failures{0};
    std::atomic<double> pump_wall_secs{0.0};
    std::atomic<double> pump_drift_secs{0.0};

    std::atomic<bool>   worker_done{false};
    std::atomic<bool>   commit_requested{false};

    std::thread pump([&]() {
        std::size_t offset = 0;
        auto pump_t0 = clock::now();
        auto next_tick = pump_t0;

        for (int tick = 0; tick < kTotalTicks; ++tick) {
            // Slice a 1600-sample window out of the fixture; wrap to the
            // start when we've consumed everything. The fixture is ~15
            // minutes, so for a 10-minute target we never actually
            // wrap — but the wrap path is correct regardless and lets
            // the test scale up cleanly if the operator extends the
            // target later.
            if (offset + kChunkSamples > fixture_count) {
                offset = 0;
            }
            const int16_t* src = fixture_samples.data() + offset;
            const std::size_t n = kChunkSamples;
            offset += n;

            // send_stream_audio runs entirely on the worker thread. It
            // writes a single 0x03 frame (5-byte header + 3200-byte
            // payload) to the fd in one blocking write loop. The
            // main thread may concurrently call read(fd) via
            // read_events — POSIX guarantees concurrent read/write on
            // a socket fd are independent (different directions of
            // the duplex pipe).
            if (!client.send_stream_audio(src, n)) {
                write_failures.fetch_add(1);
                // A write failure usually means the daemon dropped the
                // connection (e.g. detected a malformed frame). There's
                // no point continuing to feed; break and let the main
                // thread observe via the failure counter.
                break;
            }
            ticks_sent.fetch_add(1);

            // Drift-resistant pacing: schedule from a fixed start
            // anchor, not from the previous wake-up. sleep_until lets
            // the kernel absorb wakeup jitter against the absolute
            // deadline.
            next_tick += kTickPeriod;
            std::this_thread::sleep_until(next_tick);
        }

        // Worker is done pumping. Report wall + drift counters.
        auto pump_t1 = clock::now();
        double wall = std::chrono::duration<double>(pump_t1 - pump_t0).count();
        double expected =
            static_cast<double>(ticks_sent.load()) * 0.1;
        pump_wall_secs.store(wall);
        pump_drift_secs.store(wall - expected);

        // Signal main thread that we're ready for it to issue
        // process.stream.commit. The main thread can't call commit
        // from inside the event-poll loop without coordination, so we
        // flip the flag and let the main thread observe it.
        commit_requested.store(true);
        worker_done.store(true);
    });

    // --------------------------------------------------------------------
    // 8. Main-thread event-poll loop. read_events with a short timeout
    //    so we wake up and check the worker_done flag periodically.
    //    Hard wall-clock cap 15 minutes (10 min audio + 5 min margin
    //    for engine drain + commit).
    // --------------------------------------------------------------------
    const auto hard_deadline = test_t0 + std::chrono::minutes(15);
    while (!worker_done.load() &&
           clock::now() < hard_deadline) {
        // 200ms tick — plenty of time to drain queued events between
        // wakeups but short enough to notice the worker_done flag.
        client.read_events(/*until_event=*/"", /*timeout_ms=*/200);
    }

    // --------------------------------------------------------------------
    // 9. Send process.stream.commit. The daemon stops the engine,
    //    closes the WAV, and enqueues a postprocess job — we don't
    //    care about the postprocess job for this experiment; we only
    //    want the commit to land cleanly. Then drain residual caption
    //    events that race the commit response.
    // --------------------------------------------------------------------
    pump.join();

    bool commit_ok = false;
    int64_t postprocess_job_id = 0;
    {
        JsonMap p;
        p["stream_token"] = stream_token;
        IpcResponse resp;
        IpcError err;
        if (client.call("process.stream.commit", p, resp, err,
                        /*timeout_ms=*/30000)) {
            commit_ok = json_val_as_bool(resp.result["ok"]);
            postprocess_job_id = json_val_as_int(resp.result["job_id"]);
        } else {
            // Commit failed — surface but don't FAIL; this is the
            // discriminator, not a behavior test.
            commit_ok = false;
            fprintf(stderr,
                    "\n[full-stack][captions][paced] commit failed: "
                    "code=%d message=%s\n",
                    err.code, err.message.c_str());
        }
    }

    // Drain a short tail of events (the engine's flush + final partial
    // can fire just after commit). 2 seconds is plenty of margin.
    {
        auto tail_deadline = clock::now() + 2s;
        while (clock::now() < tail_deadline && clock::now() < hard_deadline) {
            client.read_events(/*until_event=*/"", /*timeout_ms=*/200);
        }
    }
    echo("captions complete");

    // --------------------------------------------------------------------
    // 10. Report.
    //
    // We print the numbers via fprintf(stderr, ...) so they appear in
    // the test runner output regardless of the Catch2 reporter, AND
    // via INFO so they're attached to the test for the JUnit/console
    // reporters. The operator reads these numbers to decide whether
    // the captions regression is real.
    // --------------------------------------------------------------------
    const double test_wall_secs =
        std::chrono::duration<double>(clock::now() - test_t0).count();

    std::size_t total_degraded = 0;
    std::ostringstream degraded_bucket_str;
    for (auto& kv : stats.degraded_by_reason) {
        total_degraded += kv.second;
        if (!degraded_bucket_str.str().empty()) degraded_bucket_str << ", ";
        degraded_bucket_str << kv.first << "=" << kv.second;
    }
    if (degraded_bucket_str.str().empty())
        degraded_bucket_str << "(none)";

    fprintf(stderr,
            "\n[full-stack][captions][paced] DISCRIMINATOR RESULTS:\n"
            "  fixture_seconds       = %.3f\n"
            "  ticks_sent            = %d / %d\n"
            "  samples_sent          = %lld (%lld bytes)\n"
            "  write_failures        = %d\n"
            "  pump_wall_secs        = %.3f\n"
            "  pump_drift_secs       = %.6f  (positive == slower than real-time)\n"
            "  test_wall_secs        = %.3f\n"
            "  caption.degraded total= %zu  buckets: %s\n"
            "  caption.final         = %zu\n"
            "  caption.partial       = %zu\n"
            "  other caption.*       = %zu\n"
            "  commit_ok             = %d\n"
            "  postprocess_job_id    = %lld\n"
            "  stream_job_id         = %lld (seen via event=%lld)\n",
            fixture_seconds,
            ticks_sent.load(), kTotalTicks,
            static_cast<long long>(static_cast<int64_t>(ticks_sent.load())
                                   * static_cast<int64_t>(kChunkSamples)),
            static_cast<long long>(static_cast<int64_t>(ticks_sent.load())
                                   * static_cast<int64_t>(kChunkSamples)
                                   * 2),
            write_failures.load(),
            pump_wall_secs.load(),
            pump_drift_secs.load(),
            test_wall_secs,
            total_degraded, degraded_bucket_str.str().c_str(),
            stats.caption_finals,
            stats.caption_partials,
            stats.other_caption_events,
            commit_ok ? 1 : 0,
            static_cast<long long>(postprocess_job_id),
            static_cast<long long>(stream_job_id),
            static_cast<long long>(stream_job_id_seen));

    INFO("fixture_seconds=" << fixture_seconds
         << " ticks_sent=" << ticks_sent.load() << "/" << kTotalTicks
         << " write_failures=" << write_failures.load()
         << " pump_wall=" << pump_wall_secs.load() << "s"
         << " drift=" << pump_drift_secs.load() << "s"
         << " test_wall=" << test_wall_secs << "s"
         << " degraded_total=" << total_degraded
         << " degraded_buckets=[" << degraded_bucket_str.str() << "]"
         << " finals=" << stats.caption_finals
         << " partials=" << stats.caption_partials
         << " other_caption=" << stats.other_caption_events
         << " commit_ok=" << commit_ok
         << " pp_job_id=" << postprocess_job_id);

    // --- Wiring assertions (REQUIRE/CHECK) ---
    //
    // Discriminator experiment contract: assert the test ran to
    // completion (pump completed all ticks, stream committed
    // cleanly, wall under the 15-min budget). Do NOT assert specific
    // event counts — those are the experimental output.
    REQUIRE(test_wall_secs < 15.0 * 60.0);
    CHECK(write_failures.load() == 0);
    CHECK(ticks_sent.load() == kTotalTicks);
    CHECK(commit_ok);

    // --------------------------------------------------------------------
    // 11. Cleanup. Best-effort — leave on failure for debug.
    // --------------------------------------------------------------------
    client.close_connection();
    if (write_failures.load() == 0 && commit_ok) {
        std::error_code ec;
        fs::remove_all(workdir, ec);
    }
}

} // namespace recmeet

#endif // RECMEET_USE_SHERPA
