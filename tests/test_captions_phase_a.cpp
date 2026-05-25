// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase A — V2 captions always-stream + server capability reporting.
// Tests T1-T5 from
// `agentctx/tasks/v2-captions-always-stream-client-renders.md` (rev 5).
//
// Layers:
//
//   T1-T2 (`[captions][phase-a]`) — in-process harness: drive a real
//     IpcServer + IpcClient pair against `register_daemon_handlers()`,
//     pre-set `g_server_config.captions_enabled` to the runtime-effective
//     value under `mutate_config`, then assert session.init returns
//     `captions_supported` correctly. The startup AND-in itself
//     (daemon.cpp) is not driven here — that's T3's job — but the
//     wire-protocol reader (A2.1) is exercised in production form.
//
//   T3 (`[captions][phase-a][full-stack]`) — spawns a real
//     `recmeet-daemon` child with a deliberately-bogus caption_model
//     path. Asserts session.init reports `captions_supported=false`
//     AND that the startup INFO log line "[startup] captions disabled
//     at runtime: model directory not found" was emitted. Exercises the
//     daemon startup gate's failure mode end-to-end.
//
//   T4 (`[captions][phase-a][no-sherpa]`) — only runs in builds where
//     RECMEET_USE_SHERPA is OFF. Asserts session.init reports
//     `captions_supported=false`. Tagged so the standard CI matrix
//     (sherpa ON) skips it; the no-sherpa CI configuration runs it.
//
//   T5 (`[captions][phase-a]`) — directly construct
//     `StreamingSessionManager` with `captions_enabled_at_startup=false`,
//     call `create()`, and verify the resulting session has
//     `has_engine() == false`, zero `caption.*` events of any kind
//     emitted, and audio still writes to the streaming WAV cleanly.
//
// Thread hygiene: every spawned thread (JqGuard's worker, the test's
// own audio-feed thread if any) is owned by a RAII guard whose
// destructor joins on every exit path.

#include <catch2/catch_test_macros.hpp>

#include "config.h"
#include "daemon_test_harness.h"
#include "full_stack_helpers.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"
#include "model_manager.h"
#include "pipeline.h"     // resolve_caption_model_dir
#include "streaming_session.h"
#include "test_tmpdir.h"

#include <sndfile.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace recmeet;
using recmeet::test::DaemonTestHarness;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// JqGuard mirrors the pattern in tests/test_streaming_session.cpp: a
// single worker thread dequeues Streaming jobs so the slot marker
// flips, then loops until shutdown(). Joined on dtor (RAII).
struct JqGuard {
    JobQueue& q;
    std::thread worker;
    explicit JqGuard(JobQueue& q_) : q(q_) {
        worker = std::thread([this]() {
            for (;;) {
                auto dq = q.dequeue(JobKind::Streaming);
                if (!dq.has_value()) return;
            }
        });
    }
    ~JqGuard() {
        q.shutdown();
        if (worker.joinable()) worker.join();
    }
};

// Counting sink — records every `caption.*` event the manager fires so
// T5 can assert zero emissions. Mirrors tests/test_streaming_session.cpp's
// CountingSink shape but stripped to T5's needs.
struct AssertNoEmissionsSink {
    std::mutex mtx;
    std::atomic<int> caption_calls{0};
    std::atomic<int> degraded_calls{0};
    std::vector<std::string> degraded_reasons;
    StreamingCaptionSink make() {
        StreamingCaptionSink s;
        s.on_caption = [this](int64_t, const std::string&,
                              const std::string&, bool, int64_t) {
            caption_calls.fetch_add(1, std::memory_order_acq_rel);
        };
        s.on_degraded = [this](int64_t, const std::string&,
                               const std::string& reason, int64_t) {
            degraded_calls.fetch_add(1, std::memory_order_acq_rel);
            std::lock_guard<std::mutex> lk(mtx);
            degraded_reasons.push_back(reason);
        };
        return s;
    }
};

// Build a unique tmp dir for a given test's streaming WAVs.
fs::path captions_test_tmp_dir(const std::string& tag) {
    fs::path d = recmeet::test::tmp_path("recmeet_captions_phase_a_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

// Slurp a file into a string. Returns empty when the file is missing.
std::string slurp_file(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return {};
    std::string out, line;
    while (std::getline(in, line)) {
        out += line;
        out += '\n';
    }
    return out;
}

// Build a deterministic PCM payload of `n_samples` int16 samples as a
// std::string of raw bytes (the `0x03` payload shape).
std::string make_pcm(size_t n_samples, int16_t start = 1) {
    std::string s;
    s.resize(n_samples * sizeof(int16_t));
    auto* p = reinterpret_cast<int16_t*>(s.data());
    for (size_t i = 0; i < n_samples; ++i)
        p[i] = static_cast<int16_t>(start + static_cast<int16_t>(i));
    return s;
}

// Read the frame count of a WAV file via libsndfile. Returns -1 if the
// file cannot be opened.
sf_count_t wav_frames(const fs::path& p) {
    SF_INFO info = {};
    SNDFILE* sf = sf_open(p.string().c_str(), SFM_READ, &info);
    if (!sf) return -1;
    sf_count_t frames = info.frames;
    sf_close(sf);
    return frames;
}

}  // anonymous namespace

// ===========================================================================
// T1 (A2) — session.init returns captions_supported=true under default config
//           (with caption-capable runtime).
// ===========================================================================
TEST_CASE("session.init reports captions_supported=true when daemon's "
          "runtime-effective captions_enabled is true",
          "[captions][phase-a]") {
    DaemonTestHarness h;
    // T1 emulates the post-startup state: A1.4's gate has already written
    // the runtime-effective true into g_server_config.captions_enabled.
    h.mutate_config([](ServerConfig& cfg) {
        cfg.captions_enabled = true;
    });
    h.start();

    IpcClient c(h.socket_path());
    REQUIRE(c.connect());

    JsonMap creds;
    JsonMap prefs;
    IpcResponse resp;
    IpcError err;
    REQUIRE(c.session_init(creds, prefs, resp, err));

    CHECK(json_val_as_bool(resp.result["ok"]) == true);
    CHECK(json_val_as_bool(resp.result["session_active"]) == true);
    CHECK(json_val_as_bool(resp.result["captions_supported"]) == true);

    c.close_connection();
}

// ===========================================================================
// T2 (A2) — session.init returns captions_supported=false when the loaded
//           config has explicit `captions: { enabled: false }`.
// ===========================================================================
TEST_CASE("session.init reports captions_supported=false when the operator "
          "has opted out via daemon.yaml",
          "[captions][phase-a]") {
    DaemonTestHarness h;
    // Emulate the post-startup state for an explicit opt-out: A1.4's
    // gate AND'd operator-intent (false) with runtime-capable (any) and
    // wrote false back.
    h.mutate_config([](ServerConfig& cfg) {
        cfg.captions_enabled = false;
    });
    h.start();

    IpcClient c(h.socket_path());
    REQUIRE(c.connect());

    JsonMap creds;
    JsonMap prefs;
    IpcResponse resp;
    IpcError err;
    REQUIRE(c.session_init(creds, prefs, resp, err));

    CHECK(json_val_as_bool(resp.result["ok"]) == true);
    CHECK(json_val_as_bool(resp.result["captions_supported"]) == false);

    c.close_connection();
}

// ===========================================================================
// T3 (A2) — full-stack: spawn a real daemon with a bogus caption_model so
//           that the runtime-capability check fails (model directory does
//           not resolve to an existing directory). Assert (a) session.init
//           reports captions_supported=false AND (b) the daemon emitted
//           the "[startup] captions disabled at runtime: model directory
//           not found" INFO line.
//
// Verification-gate-failure-mode discipline: a prior verification gate in
// this project passed the happy path but missed the tampered case. T3
// exercises the failure mode of the captions-runtime gate explicitly.
// ===========================================================================
TEST_CASE("daemon startup gate logs and disables captions when the model "
          "directory does not exist",
          "[captions][phase-a][full-stack]") {
    fs::path daemon_bin = full_stack::find_daemon_binary();

    auto workdir = captions_test_tmp_dir("t3_bogus_model");
    fs::path xdg_config = workdir / "xdg";
    fs::create_directories(xdg_config / "recmeet");
    fs::path stderr_log = workdir / "daemon.stderr.log";
    fs::path socket_path = workdir / "ipc.sock";

    // Write a daemon.yaml with captions enabled (operator intent ON) but
    // pointing at a deliberately-bogus caption_model name that resolves
    // to ~/.local/share/recmeet/models/sherpa/online/<bogus> — which
    // does not exist. The startup AND-in collapses that to runtime false
    // and logs the reason.
    {
        std::ofstream daemon_yaml(xdg_config / "recmeet" / "daemon.yaml");
        REQUIRE(daemon_yaml.is_open());
        daemon_yaml << "# T3 test fixture — captions on, but model path bogus\n"
                    << "captions:\n"
                    << "  enabled: true\n"
                    << "  model: bogus-nonexistent-model-name-for-test\n";
    }

    full_stack::SpawnedDaemon daemon(
        daemon_bin,
        full_stack::SpawnedDaemon::Transport::Unix,
        /*tcp_addr=*/std::string{},
        /*psk=*/std::string{},
        xdg_config,
        socket_path,
        stderr_log);

    {
        IpcClient c(socket_path.string());
        REQUIRE(c.connect());

        JsonMap creds;
        JsonMap prefs;
        IpcResponse resp;
        IpcError err;
        REQUIRE(c.session_init(creds, prefs, resp, err));
        CHECK(json_val_as_bool(resp.result["ok"]) == true);
        CHECK(json_val_as_bool(resp.result["captions_supported"]) == false);

        c.close_connection();
    }

    // The startup INFO line is emitted before the listener accepts, so
    // by the time session.init succeeded the line is already in the log
    // file. We slurp once and grep.
    std::string log = slurp_file(stderr_log);
    INFO("Daemon stderr capture:\n" << log);
    CHECK(log.find("[startup] captions disabled at runtime: "
                   "model directory not found") != std::string::npos);
}

// ===========================================================================
// T4 (A2) — session.init returns captions_supported=false in
//           RECMEET_USE_SHERPA=OFF builds. Skipped in the standard CI matrix
//           (sherpa ON). The runtime-capability check zeroes runtime_capable
//           unconditionally when the build is no-sherpa, so even with the
//           model dir present and config opt-in, the announced capability
//           is false.
// ===========================================================================
#ifndef RECMEET_USE_SHERPA
TEST_CASE("session.init reports captions_supported=false in no-sherpa builds",
          "[captions][phase-a][no-sherpa]") {
    fs::path daemon_bin = full_stack::find_daemon_binary();

    auto workdir = captions_test_tmp_dir("t4_no_sherpa");
    fs::path xdg_config = workdir / "xdg";
    fs::create_directories(xdg_config / "recmeet");
    fs::path stderr_log = workdir / "daemon.stderr.log";
    fs::path socket_path = workdir / "ipc.sock";

    // Default daemon.yaml — captions ON (the A1 default). The no-sherpa
    // build flips runtime_capable to false regardless, so the gate logs
    // "RECMEET_USE_SHERPA=OFF build" and writes false back.
    {
        std::ofstream daemon_yaml(xdg_config / "recmeet" / "daemon.yaml");
        REQUIRE(daemon_yaml.is_open());
        daemon_yaml << "# T4 fixture — no-sherpa build\n";
    }

    full_stack::SpawnedDaemon daemon(
        daemon_bin,
        full_stack::SpawnedDaemon::Transport::Unix,
        /*tcp_addr=*/std::string{},
        /*psk=*/std::string{},
        xdg_config,
        socket_path,
        stderr_log);

    {
        IpcClient c(socket_path.string());
        REQUIRE(c.connect());

        JsonMap creds;
        JsonMap prefs;
        IpcResponse resp;
        IpcError err;
        REQUIRE(c.session_init(creds, prefs, resp, err));
        CHECK(json_val_as_bool(resp.result["captions_supported"]) == false);

        c.close_connection();
    }

    std::string log = slurp_file(stderr_log);
    INFO("Daemon stderr capture:\n" << log);
    CHECK(log.find("[startup] captions disabled at runtime: "
                   "RECMEET_USE_SHERPA=OFF build") != std::string::npos);
}
#endif  // !RECMEET_USE_SHERPA

// ===========================================================================
// T5 (A3) — direct StreamingSessionManager test: construct with
//           captions_enabled_at_startup=false, then create() a session and
//           verify:
//             (1) session has engine_ == nullptr (via has_engine()),
//             (2) zero caption.* events of any kind (including engine_error)
//                 are emitted,
//             (3) audio still writes to the streaming WAV cleanly via
//                 feed_audio.
// Unit-level — no IPC, no daemon spawn.
// ===========================================================================
TEST_CASE("StreamingSessionManager with captions_enabled_at_startup=false "
          "creates sessions with no engine and emits no caption events",
          "[captions][phase-a]") {
    JobQueue q;
    JqGuard guard(q);

    AssertNoEmissionsSink ne_sink;
    auto sink = ne_sink.make();

    // Pass a non-empty caption_model_dir that DOES exist as a directory,
    // so the test isolates the runtime-disabled-bit branch from the
    // engine-failed-to-start branch. The bit must be the sole reason the
    // engine doesn't start.
    fs::path tmp = captions_test_tmp_dir("t5_runtime_disabled");
    fs::path fake_model_dir = tmp / "fake_model_dir";
    fs::create_directories(fake_model_dir);

    StreamingSessionManager mgr(q, sink, fake_model_dir.string(),
                                /*meeting_index=*/nullptr,
                                /*meetings_root=*/fs::path{},
                                /*captions_enabled_at_startup=*/false);

    StreamRequest req;  // defaults: s16le / 16000 / mono / en / 500 ms
    auto res = mgr.create("client-T5", req, tmp);
    REQUIRE(res.ok);
    CHECK(res.job_id > 0);
    CHECK_FALSE(res.stream_token.empty());

    // (1) No engine on the session.
    StreamingSession* sess = mgr.session_for_token(res.stream_token);
    REQUIRE(sess != nullptr);
    CHECK_FALSE(sess->has_engine());

    // (3) Audio still writes to the streaming WAV cleanly via feed_audio.
    //     Feed a few chunks — feed_audio returns true on every accepted
    //     chunk and the WAV file appears in the temp dir.
    CHECK(mgr.feed_audio(res.stream_token, make_pcm(160)));    // 10ms
    CHECK(mgr.feed_audio(res.stream_token, make_pcm(1600)));   // 100ms
    CHECK(mgr.feed_audio(res.stream_token, make_pcm(800)));    // 50ms

    // Find the live WAV and snapshot it (cancel() unlinks the original).
    fs::path wav;
    for (auto& e : fs::directory_iterator(tmp))
        if (e.path().extension() == ".wav") wav = e.path();
    REQUIRE(!wav.empty());

    // Snapshot the WAV file before cancel() unlinks it so we can verify
    // the libsndfile-managed header is sane (sf_open succeeds and the
    // frame count is non-negative). The exact frame count depends on
    // whether libsndfile has flushed its header yet (it does on
    // sf_close), so we don't tightly bound the value — the
    // affirmative-not-negative-one return is the signal.
    fs::path snapshot = tmp / "snapshot.wav";
    fs::copy_file(wav, snapshot, fs::copy_options::overwrite_existing);

    // Tear down.
    CHECK(mgr.cancel("client-T5", res.stream_token));
    CHECK(mgr.active_count() == 0);

    // (2) Zero caption.* events of any kind across the entire lifecycle.
    //     The on_degraded would have fired for engine_error if create()
    //     had tried to construct + start an engine that failed. Since
    //     A3.1 short-circuits the whole block, neither on_caption nor
    //     on_degraded should have been called even once.
    CHECK(ne_sink.caption_calls.load() == 0);
    CHECK(ne_sink.degraded_calls.load() == 0);
    {
        std::lock_guard<std::mutex> lk(ne_sink.mtx);
        INFO("Unexpected degraded reasons (should be empty): "
             << [&]() {
                    std::string j;
                    for (auto& r : ne_sink.degraded_reasons) {
                        if (!j.empty()) j += ", ";
                        j += r;
                    }
                    return j;
                }());
        CHECK(ne_sink.degraded_reasons.empty());
    }

    // The snapshot we copied should be a valid WAV.
    CHECK(wav_frames(snapshot) >= 0);

    std::error_code ec;
    fs::remove_all(tmp, ec);
}
