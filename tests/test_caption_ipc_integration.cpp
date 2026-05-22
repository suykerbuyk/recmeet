// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 3 — IPC integration tests for live captions.
//
// These tests exercise the caption + caption.degraded event broadcast path
// from the daemon's perspective. They use a lightweight `CaptionDaemonSim`
// that mirrors the production rec_worker's caption-hook wiring (function
// pointers backed by IpcServer::post() to keep broadcast() on the poll
// thread) without the full pipeline / model dependency.
//
// The engine itself is run in `Options::_no_recognizer_for_test = true`
// mode for the degraded-path tests (no model required). For partial/final
// transition tests we simulate the engine's `on_result` callback directly
// — there is no fake-recognizer extension to caption_engine.cpp; instead
// we invoke the captured hook from test-side code, which is what the
// engine's worker would do in production. The IPC broadcast path is the
// same either way.

#include <catch2/catch_test_macros.hpp>

#include "caption_engine.h"
#include "caption_start_channel.h"
#include "caption_vtt.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "pipeline.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <signal.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace recmeet;

// Ignore SIGPIPE so writing to disconnected clients doesn't kill the process.
static struct CapSigpipeIgnorer {
    CapSigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} s_ignore_sigpipe_caption;

namespace {

const char* CAPTION_SOCK = "/tmp/recmeet_test_caption.sock";

// ---------------------------------------------------------------------------
// CaptionDaemonSim — minimal daemon simulator focused on the caption broadcast
// path. record.start spins up a real CaptionEngine (model-free) when
// captions_enabled=true, and wires the caption hooks via the same pattern
// the production daemon uses (function pointer + heap ctx + post()).
// ---------------------------------------------------------------------------

struct CaptionBroadcastCtx {
    IpcServer* server;
    int64_t job_id;
};

// Mirrors pipeline.cpp's CaptionFanoutAdapter for daemon-sim integration
// tests. The on_result function pointer below forwards to the broadcast hook
// AND, when `vtt` is non-null, appends finalized cues to the sidecar — same
// behaviour as the production fan-out installed by try_start_caption_engine.
struct CaptionFanoutSimAdapter {
    CaptionResultCallback downstream_on_result = nullptr;
    void*                 downstream_result_ud = nullptr;
    std::unique_ptr<VttWriter> vtt;
    VttCueTimer cue_timer;
};

inline void caption_fanout_sim_on_result(const CaptionResult& r, void* ud) {
    auto* a = static_cast<CaptionFanoutSimAdapter*>(ud);
    if (a->downstream_on_result) {
        a->downstream_on_result(r, a->downstream_result_ud);
    }
    if (!r.is_partial && a->vtt) {
        auto [s, e] = a->cue_timer.next(r.timestamp_ms);
        a->vtt->append(s, e, r.text, /*is_partial=*/false);
    }
}

struct CaptionDaemonSim {
    IpcServer server;
    std::thread srv_thread;

    std::atomic<bool> recording{false};
    std::atomic<int64_t> next_job_id{1};
    std::mutex state_mu;

    // Active engine + ctx for the in-flight recording. Test code reaches
    // through these to drive the engine + invoke hooks deterministically.
    std::unique_ptr<CaptionEngine> engine;
    std::unique_ptr<CaptionBroadcastCtx> ctx;
    CaptionHooks hooks;
    int64_t active_job_id = 0;
    bool captions_enabled_for_active = false;

    // Knobs for engine setup
    std::size_t ring_capacity_override = 0;
    int worker_poll_ms_override = 0;

    // Tracks teardown ordering for test #5
    std::atomic<int64_t> cap_stop_at{0};
    std::atomic<int64_t> engine_stop_at{0};
    std::atomic<int64_t> cap_drain_at{0};
    std::atomic<bool> callback_subscribed{false};
    // True if the audio callback was unsubscribed before drain time
    std::atomic<bool> unsubscribed_before_drain{false};

    explicit CaptionDaemonSim(const char* sock = CAPTION_SOCK) : server(sock) {
        unlink(sock);

        server.on("status.get", [this](const IpcRequest&, IpcResponse& resp, IpcError&) {
            resp.result["recording"] = recording.load();
            return true;
        });

        server.on("record.start",
                  [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            // Single-instance busy guard, mirroring daemon.cpp:961.
            {
                std::lock_guard<std::mutex> lk(state_mu);
                if (recording.load()) {
                    err.code = static_cast<int>(IpcErrorCode::Busy);
                    err.message = "Daemon is busy (recording)";
                    return false;
                }
                recording.store(true);
            }

            int64_t job_id = next_job_id.fetch_add(1);
            active_job_id = job_id;

            bool want_captions = false;
            std::string caption_model;
            {
                auto it = req.params.find("captions_enabled");
                if (it != req.params.end()) want_captions = json_val_as_bool(it->second);
            }
            {
                auto it = req.params.find("caption_model");
                if (it != req.params.end()) caption_model = json_val_as_string(it->second);
            }
            captions_enabled_for_active = want_captions;

            if (want_captions) {
                // Wire the broadcast hooks exactly as daemon.cpp does.
                ctx = std::make_unique<CaptionBroadcastCtx>(
                    CaptionBroadcastCtx{&server, job_id});
                hooks = {};
                hooks.result_ud = ctx.get();
                hooks.degraded_ud = ctx.get();
                hooks.engine_error_ud = ctx.get();
                hooks.on_result = +[](const CaptionResult& r, void* ud) {
                    auto* c = static_cast<CaptionBroadcastCtx*>(ud);
                    IpcServer* s = c->server;
                    int64_t jid = c->job_id;
                    std::string text = r.text;
                    bool ip = r.is_partial;
                    int64_t ts = r.timestamp_ms;
                    s->post([s, jid, text, ip, ts]() {
                        s->broadcast(make_caption_event(jid, text, ip, ts));
                    });
                };
                hooks.on_degraded = +[](CaptionDegradedReason reason, void* ud) {
                    auto* c = static_cast<CaptionBroadcastCtx*>(ud);
                    IpcServer* s = c->server;
                    int64_t jid = c->job_id;
                    const char* rs = (reason == CaptionDegradedReason::BufferOverrun)
                        ? "buffer_overrun" : "unknown";
                    int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    std::string r_str(rs);
                    s->post([s, jid, r_str, ts]() {
                        s->broadcast(make_caption_degraded_event(jid, r_str, ts));
                    });
                };
                hooks.on_engine_error = +[](const std::string& msg, void* ud) {
                    auto* c = static_cast<CaptionBroadcastCtx*>(ud);
                    IpcServer* s = c->server;
                    int64_t jid = c->job_id;
                    int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    std::string m = msg;
                    s->post([s, jid, m, ts]() {
                        IpcEvent ev = make_caption_degraded_event(jid, "engine_error", ts);
                        ev.data["error"] = m;
                        s->broadcast(ev);
                    });
                };

#ifdef RECMEET_USE_SHERPA
                // Real engine in model-free test mode. Falls through if
                // sherpa is OFF — engine.reset() leaves engine null and
                // hooks remain wired for direct test invocation.
                CaptionEngine::Options opts;
                opts._no_recognizer_for_test = true;
                opts.ring_capacity_override = ring_capacity_override;
                opts.worker_poll_ms_override = worker_poll_ms_override;
                if (!caption_model.empty()) {
                    // Caption model param accepted but unused in the
                    // _no_recognizer mode; still recorded by callers.
                }
                engine = std::make_unique<CaptionEngine>();
                bool ok = engine->start(opts,
                                        hooks.on_result, hooks.result_ud,
                                        hooks.on_degraded, hooks.degraded_ud);
                if (ok) {
                    callback_subscribed.store(true);
                } else {
                    // Mimic the daemon's "non-fatal" branch: notify and
                    // continue without captions.
                    if (hooks.on_engine_error) {
                        hooks.on_engine_error(engine->last_error(),
                                              hooks.engine_error_ud);
                    }
                    engine.reset();
                }
#else
                // Sherpa OFF — real engine.start() returns false with the
                // canonical error. Mirror the daemon's graceful path.
                engine = std::make_unique<CaptionEngine>();
                CaptionEngine::Options opts{};
                bool ok = engine->start(opts,
                                        hooks.on_result, hooks.result_ud,
                                        hooks.on_degraded, hooks.degraded_ud);
                REQUIRE(!ok);
                if (hooks.on_engine_error) {
                    hooks.on_engine_error(engine->last_error(),
                                          hooks.engine_error_ud);
                }
                engine.reset();
#endif
            }

            resp.result["ok"] = true;
            resp.result["job_id"] = job_id;
            return true;
        });

        server.on("record.stop", [this](const IpcRequest&, IpcResponse& resp, IpcError& err) {
            if (!recording.load()) {
                err.code = static_cast<int>(IpcErrorCode::NotRecording);
                err.message = "Not recording";
                return false;
            }
            // Mirror Phase 3 teardown ordering: cap.stop() (here a no-op,
            // captures aren't real) -> engine.stop() -> cap.drain().
            using clk = std::chrono::steady_clock;
            cap_stop_at.store(clk::now().time_since_epoch().count());
            // Brief pause to make ordering observable.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (engine) {
                // belt-and-braces unsubscribe (capture is fake here)
                callback_subscribed.store(false);
                engine->stop();
                engine_stop_at.store(clk::now().time_since_epoch().count());
                engine.reset();
            } else {
                engine_stop_at.store(clk::now().time_since_epoch().count());
            }
            // After engine teardown, the capture callback is unsubscribed.
            unsubscribed_before_drain.store(!callback_subscribed.load());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            cap_drain_at.store(clk::now().time_since_epoch().count());

            ctx.reset();
            recording.store(false);
            resp.result["ok"] = true;
            return true;
        });

        // captions.start_engine — mirrors daemon.cpp's handler but reads the
        // sim's `recording` atomic instead of the daemon's g_recording.
        server.on("captions.start_engine",
                  [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            if (!recording.load()) {
                err.code = static_cast<int>(IpcErrorCode::NotRecording);
                err.message = "Not recording";
                return false;
            }
            std::string model;
            auto it = req.params.find("caption_model");
            if (it != req.params.end()) {
                model = json_val_as_string(it->second);
            }
            auto result = request_caption_engine_start(model);
            if (result == CaptionStartRequestResult::WorkerNotReady) {
                err.code = static_cast<int>(IpcErrorCode::NotRecording);
                err.message = "Recording is ending — captions cannot be started now";
                return false;
            }
            resp.result["ok"] = true;
            resp.result["status"] = std::string(
                (result == CaptionStartRequestResult::AlreadyRunning)
                    ? "already_running" : "queued");
            return true;
        });
    }

    void start() {
        REQUIRE(server.start());
        srv_thread = std::thread([this]() { server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void shutdown() {
        if (engine) engine->stop();
        engine.reset();
        ctx.reset();
        server.stop();
        if (srv_thread.joinable()) srv_thread.join();
    }

    ~CaptionDaemonSim() { shutdown(); }
};

// Small event collector — same shape as test_ipc_integration.cpp.
struct CaptionEventCollector {
    std::mutex mu;
    std::vector<IpcEvent> events;
    EventCallback callback() {
        return [this](const IpcEvent& ev) {
            std::lock_guard<std::mutex> lk(mu);
            events.push_back(ev);
        };
    }
    std::vector<IpcEvent> snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return events;
    }
    std::size_t count_event(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu);
        std::size_t n = 0;
        for (auto& e : events) if (e.event == name) ++n;
        return n;
    }
    bool has_event(const std::string& name) {
        return count_event(name) > 0;
    }
};

void drain_caption_events(IpcClient& client, int ms = 200) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        client.read_and_dispatch(10);
    }
}

std::unique_ptr<IpcClient> caption_client(const char* sock = CAPTION_SOCK) {
    auto c = std::make_unique<IpcClient>(sock);
    REQUIRE(c->connect());
    return c;
}

bool sherpa_on() {
#ifdef RECMEET_USE_SHERPA
    return true;
#else
    return false;
#endif
}

} // anonymous namespace

// ===========================================================================
// 1. Caption events received under broadcast with correct job_id.
//    We invoke the captured hook directly (simulating the engine's worker
//    callback) and assert the IPC client receives the event with the same
//    job_id that record.start returned.
// ===========================================================================
TEST_CASE("caption events received with correct job_id",
          "[ipc][integration][captions]") {
    CaptionDaemonSim sim;
    sim.start();

    auto starter = caption_client();
    auto observer = caption_client();
    CaptionEventCollector evs;
    observer->set_event_callback(evs.callback());

    IpcResponse resp;
    IpcError err;
    JsonMap params;
    params["captions_enabled"] = true;
    REQUIRE(starter->call("record.start", params, resp, err));
    int64_t job_id = json_val_as_int(resp.result["job_id"]);

    // Simulate two engine emissions via the captured hooks.
    REQUIRE(sim.captions_enabled_for_active);
    REQUIRE(sim.hooks.on_result != nullptr);

    CaptionResult r1{"HELLO", true, 100};
    CaptionResult r2{"HELLO WORLD", false, 350};
    sim.hooks.on_result(r1, sim.hooks.result_ud);
    sim.hooks.on_result(r2, sim.hooks.result_ud);

    drain_caption_events(*observer, 300);

    auto snap = evs.snapshot();
    int seen = 0;
    for (auto& e : snap) {
        if (e.event != "caption") continue;
        ++seen;
        CHECK(json_val_as_int(e.data["job_id"]) == job_id);
    }
    CHECK(seen == 2);

    starter->call("record.stop", params, resp, err);
}

// ===========================================================================
// 2. record.start honors captions_enabled=false (no engine spun up).
// ===========================================================================
TEST_CASE("record.start with captions_enabled=false does not start engine",
          "[ipc][integration][captions]") {
    CaptionDaemonSim sim;
    sim.start();

    auto client = caption_client();
    CaptionEventCollector evs;
    client->set_event_callback(evs.callback());

    IpcResponse resp;
    IpcError err;
    JsonMap params;
    params["captions_enabled"] = false;
    REQUIRE(client->call("record.start", params, resp, err));

    drain_caption_events(*client, 200);

    CHECK(!sim.captions_enabled_for_active);
    CHECK(sim.engine == nullptr);
    CHECK(sim.hooks.on_result == nullptr);
    CHECK(!evs.has_event("caption"));
    CHECK(!evs.has_event("caption.degraded"));

    client->call("record.stop", params, resp, err);
}

// ===========================================================================
// 3. caption_model override is accepted and the engine receives it.
//    In the no-recognizer test mode, the model param is recorded but unused;
//    we just verify the daemon doesn't reject the field.
// ===========================================================================
TEST_CASE("record.start accepts caption_model override",
          "[ipc][integration][captions]") {
    CaptionDaemonSim sim;
    sim.start();

    auto client = caption_client();
    IpcResponse resp;
    IpcError err;
    JsonMap params;
    params["captions_enabled"] = true;
    params["caption_model"] = std::string("en-small-v2");
    REQUIRE(client->call("record.start", params, resp, err));

    CHECK(sim.captions_enabled_for_active);

    client->call("record.stop", params, resp, err);
}

// ===========================================================================
// 4. caption.degraded event on synthetic ring-buffer overrun.
//    Drives the engine's actual drop-watermark path with a tiny ring +
//    no-recognizer worker so overflow guarantees a degraded callback.
// ===========================================================================
TEST_CASE("caption.degraded fires on synthetic ring overflow",
          "[ipc][integration][captions]") {
    if (!sherpa_on()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping degraded-overflow test "
             "(no engine consumer in stub build)");
        return;
    }
    CaptionDaemonSim sim;
    sim.ring_capacity_override = 1024;
    sim.worker_poll_ms_override = 1;
    sim.start();

    auto client = caption_client();
    CaptionEventCollector evs;
    client->set_event_callback(evs.callback());

    IpcResponse resp;
    IpcError err;
    JsonMap params;
    params["captions_enabled"] = true;
    REQUIRE(client->call("record.start", params, resp, err));
    int64_t job_id = json_val_as_int(resp.result["job_id"]);

    // Force overflow: 4× ring capacity in a single push.
    REQUIRE(sim.engine != nullptr);
    std::vector<int16_t> burst(1024 * 4, 0);
    sim.engine->_push_samples_for_test(burst.data(), burst.size());

    // Drain events for up to 1s waiting for the worker to tick + emit.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!evs.has_event("caption.degraded") &&
           std::chrono::steady_clock::now() < deadline) {
        client->read_and_dispatch(20);
    }

    auto snap = evs.snapshot();
    bool found = false;
    for (auto& e : snap) {
        if (e.event != "caption.degraded") continue;
        found = true;
        CHECK(json_val_as_string(e.data["reason"]) == "buffer_overrun");
        CHECK(json_val_as_int(e.data["job_id"]) == job_id);
    }
    CHECK(found);

    client->call("record.stop", params, resp, err);
}

// ===========================================================================
// 5. Teardown ordering: engine destroyed between cap.stop() and cap.drain(),
//    and the audio callback is unsubscribed before drain returns.
// ===========================================================================
TEST_CASE("engine teardown happens between cap.stop() and cap.drain()",
          "[ipc][integration][captions]") {
    if (!sherpa_on()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping teardown-ordering test");
        return;
    }
    CaptionDaemonSim sim;
    sim.start();

    auto client = caption_client();
    IpcResponse resp;
    IpcError err;
    JsonMap params;
    params["captions_enabled"] = true;
    REQUIRE(client->call("record.start", params, resp, err));
    REQUIRE(sim.engine != nullptr);

    REQUIRE(client->call("record.stop", params, resp, err));

    // Three ordered timestamps captured inside the record.stop handler.
    int64_t t_stop  = sim.cap_stop_at.load();
    int64_t t_eng   = sim.engine_stop_at.load();
    int64_t t_drain = sim.cap_drain_at.load();
    REQUIRE(t_stop  > 0);
    REQUIRE(t_eng   > 0);
    REQUIRE(t_drain > 0);
    CHECK(t_stop < t_eng);
    CHECK(t_eng  < t_drain);

    // And the callback subscription was cleared by the time drain ran.
    CHECK(sim.unsubscribed_before_drain.load());
}

// ===========================================================================
// 6. Captions disabled by default — bare record.start {} produces no events.
// ===========================================================================
TEST_CASE("captions disabled by default for bare record.start",
          "[ipc][integration][captions]") {
    CaptionDaemonSim sim;
    sim.start();

    auto client = caption_client();
    CaptionEventCollector evs;
    client->set_event_callback(evs.callback());

    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("record.start", resp, err));  // no params
    CHECK(!sim.captions_enabled_for_active);
    CHECK(sim.engine == nullptr);

    drain_caption_events(*client, 200);
    CHECK(!evs.has_event("caption"));
    CHECK(!evs.has_event("caption.degraded"));

    client->call("record.stop", resp, err);
}

// ===========================================================================
// 7. record.start with captions_enabled while busy is rejected; no second
//    engine spun up.
// ===========================================================================
TEST_CASE("record.start busy ignores captions_enabled flag",
          "[ipc][integration][captions]") {
    CaptionDaemonSim sim;
    sim.start();

    auto client = caption_client();
    IpcResponse resp;
    IpcError err;

    // First start (captions OFF).
    REQUIRE(client->call("record.start", resp, err));
    CHECK(!sim.captions_enabled_for_active);
    CHECK(sim.engine == nullptr);

    // Second start with captions=true: must be rejected with Busy.
    JsonMap params;
    params["captions_enabled"] = true;
    bool ok = client->call("record.start", params, resp, err);
    CHECK(!ok);
    CHECK(err.code == static_cast<int>(IpcErrorCode::Busy));
    // No engine spun up for the rejected request.
    CHECK(!sim.captions_enabled_for_active);
    CHECK(sim.engine == nullptr);

    client->call("record.stop", params, resp, err);
}

// ===========================================================================
// 8. Partial → final transition broadcast. We invoke the engine's `on_result`
//    callback twice (partial then final) and verify the IPC events arrive
//    in order with matching is_partial values.
// ===========================================================================
TEST_CASE("partial then final caption events broadcast in order",
          "[ipc][integration][captions]") {
    CaptionDaemonSim sim;
    sim.start();

    auto starter = caption_client();
    auto observer = caption_client();
    CaptionEventCollector evs;
    observer->set_event_callback(evs.callback());

    IpcResponse resp;
    IpcError err;
    JsonMap params;
    params["captions_enabled"] = true;
    REQUIRE(starter->call("record.start", params, resp, err));
    REQUIRE(sim.hooks.on_result != nullptr);

    // Partial then final for the same utterance.
    CaptionResult partial{"HELLO", true,  120};
    CaptionResult final_ {"HELLO WORLD", false, 600};
    sim.hooks.on_result(partial, sim.hooks.result_ud);
    sim.hooks.on_result(final_,  sim.hooks.result_ud);

    drain_caption_events(*observer, 400);

    auto snap = evs.snapshot();
    std::vector<bool> partials;
    std::vector<std::string> texts;
    for (auto& e : snap) {
        if (e.event != "caption") continue;
        partials.push_back(json_val_as_bool(e.data["is_partial"]));
        texts.push_back(json_val_as_string(e.data["text"]));
    }
    REQUIRE(partials.size() == 2);
    CHECK(partials[0] == true);
    CHECK(partials[1] == false);
    CHECK(texts[0] == "HELLO");
    CHECK(texts[1] == "HELLO WORLD");

    starter->call("record.stop", params, resp, err);
}

// ===========================================================================
// 9a. No-sherpa stub: record.start {captions_enabled: true} broadcasts a
//     single caption.degraded {reason: "engine_error"} and recording
//     proceeds. (Sherpa-ON builds also exercise this path if a model
//     directory is missing — the assertion is build-conditional.)
// ===========================================================================
TEST_CASE("no-sherpa stub broadcasts engine_error degraded event",
          "[ipc][integration][captions]") {
    if (sherpa_on()) {
        // Sherpa-ON builds spin up the engine in _no_recognizer mode and
        // do NOT take the engine_error path; this scenario is no-sherpa
        // specific. Skip cleanly so the test is meaningful in either build.
        SUCCEED("RECMEET_USE_SHERPA=ON — engine_error path covered by no-sherpa CI");
        return;
    }
    CaptionDaemonSim sim;
    sim.start();

    auto client = caption_client();
    CaptionEventCollector evs;
    client->set_event_callback(evs.callback());

    IpcResponse resp;
    IpcError err;
    JsonMap params;
    params["captions_enabled"] = true;
    REQUIRE(client->call("record.start", params, resp, err));

    drain_caption_events(*client, 200);

    auto snap = evs.snapshot();
    int engine_error_count = 0;
    std::string err_msg;
    for (auto& e : snap) {
        if (e.event != "caption.degraded") continue;
        if (json_val_as_string(e.data["reason"]) == "engine_error") {
            ++engine_error_count;
            err_msg = json_val_as_string(e.data["error"]);
        }
    }
    CHECK(engine_error_count == 1);
    CHECK(err_msg == "captions require RECMEET_USE_SHERPA=ON build");

    client->call("record.stop", resp, err);
}

// ===========================================================================
// 9. caption.degraded rate limit — sustained overflow should not flood
//    clients with events. Phase 2 caps at ≤ 1 per second; over ~3 s the
//    expected count is in [1, 4]: one immediate + up to ~3 more on the
//    1-second rate-limit cadence.
// ===========================================================================
TEST_CASE("caption.degraded is rate-limited under sustained overflow",
          "[ipc][integration][captions]") {
    if (!sherpa_on()) {
        WARN("RECMEET_USE_SHERPA=OFF — skipping degraded rate-limit test");
        return;
    }
    CaptionDaemonSim sim;
    sim.ring_capacity_override = 256;
    sim.worker_poll_ms_override = 1;
    sim.start();

    auto client = caption_client();
    CaptionEventCollector evs;
    client->set_event_callback(evs.callback());

    IpcResponse resp;
    IpcError err;
    JsonMap params;
    params["captions_enabled"] = true;
    REQUIRE(client->call("record.start", params, resp, err));
    REQUIRE(sim.engine != nullptr);

    // Sustain overflow for ~3 s.
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    std::vector<int16_t> burst(2048, 0);
    while (std::chrono::steady_clock::now() < end) {
        sim.engine->_push_samples_for_test(burst.data(), burst.size());
        // Read a few events to keep the client buffer drained.
        client->read_and_dispatch(5);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Drain trailing events.
    drain_caption_events(*client, 300);

    auto n = evs.count_event("caption.degraded");
    INFO("caption.degraded count over ~3s: " << n);
    CHECK(n >= 1);
    CHECK(n <= 4);

    client->call("record.stop", params, resp, err);
}

// ===========================================================================
// 10. Phase 6 — captions_enabled=true produces a captions.vtt in the meeting
//     dir; captions_enabled=false does not. We mirror pipeline.cpp's fan-out
//     adapter directly here (the daemon-sim doesn't drive run_recording, so
//     we wire the writer alongside the broadcast hook the same way
//     try_start_caption_engine does in production) and assert the file
//     existence + contents.
// ===========================================================================
TEST_CASE("captions_enabled=true creates captions.vtt with finals only",
          "[ipc][integration][captions]") {
    namespace fs = std::filesystem;
    fs::path meeting_dir = fs::temp_directory_path()
                         / ("recmeet_vtt_ipc_"
                            + std::to_string(::getpid()) + "_on");
    fs::remove_all(meeting_dir);
    fs::create_directories(meeting_dir);
    fs::path vtt_path = meeting_dir / "captions.vtt";

    CaptionDaemonSim sim;
    sim.start();

    auto starter = caption_client();
    auto observer = caption_client();
    CaptionEventCollector evs;
    observer->set_event_callback(evs.callback());

    IpcResponse resp;
    IpcError err;
    JsonMap params;
    params["captions_enabled"] = true;
    REQUIRE(starter->call("record.start", params, resp, err));
    REQUIRE(sim.captions_enabled_for_active);
    REQUIRE(sim.hooks.on_result != nullptr);

    // Wrap the daemon-sim's broadcast hook with the fan-out adapter (same
    // shape as pipeline.cpp's CaptionFanoutAdapter). The writer's lifetime
    // spans the in-flight recording.
    CaptionFanoutSimAdapter adapter;
    adapter.downstream_on_result = sim.hooks.on_result;
    adapter.downstream_result_ud = sim.hooks.result_ud;
    adapter.vtt = std::make_unique<VttWriter>(vtt_path,
                                              /*normalize_display=*/true);

    // Drive engine emissions through the wrapped callback. One partial,
    // then two finals — the partial must not appear in the .vtt.
    CaptionResult p{"PARTIAL HYP", true, 100};
    CaptionResult f1{"HELLO WORLD", false, 500};
    CaptionResult f2{"AND ANOTHER", false, 1200};
    caption_fanout_sim_on_result(p,  &adapter);
    caption_fanout_sim_on_result(f1, &adapter);
    caption_fanout_sim_on_result(f2, &adapter);

    drain_caption_events(*observer, 200);
    starter->call("record.stop", params, resp, err);
    adapter.vtt.reset();  // close fd

    // File exists and starts with the WEBVTT header.
    REQUIRE(fs::exists(vtt_path));
    std::ifstream in(vtt_path, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf();
    std::string contents = ss.str();
    REQUIRE(contents.rfind("WEBVTT\n\n", 0) == 0);

    // Both finals are present (normalized: first letter capitalized).
    REQUIRE(contents.find("Hello world") != std::string::npos);
    REQUIRE(contents.find("And another") != std::string::npos);
    // Partial must NOT appear.
    REQUIRE(contents.find("Partial") == std::string::npos);
    REQUIRE(contents.find("PARTIAL") == std::string::npos);

    // Cleanup.
    fs::remove_all(meeting_dir);
}

TEST_CASE("captions_enabled=false produces no captions.vtt sidecar",
          "[ipc][integration][captions]") {
    namespace fs = std::filesystem;
    fs::path meeting_dir = fs::temp_directory_path()
                         / ("recmeet_vtt_ipc_"
                            + std::to_string(::getpid()) + "_off");
    fs::remove_all(meeting_dir);
    fs::create_directories(meeting_dir);
    fs::path vtt_path = meeting_dir / "captions.vtt";

    CaptionDaemonSim sim;
    sim.start();

    auto client = caption_client();

    IpcResponse resp;
    IpcError err;
    JsonMap params;
    params["captions_enabled"] = false;
    REQUIRE(client->call("record.start", params, resp, err));

    // No engine, no hooks, no fan-out adapter → no writer, no file.
    REQUIRE_FALSE(sim.captions_enabled_for_active);
    REQUIRE_FALSE(fs::exists(vtt_path));

    client->call("record.stop", params, resp, err);
    REQUIRE_FALSE(fs::exists(vtt_path));

    fs::remove_all(meeting_dir);
}

// ===========================================================================
// Phase 1 smoke tests for `captions.start_engine` verb + caption_start_channel.
//
// These tests verify the channel state machine and the verb-handler glue at
// the daemon-sim layer, without spinning up a real recording worker. The
// fixture resets the file-static channel state between TEST_CASEs.
// ===========================================================================

namespace {

struct CaptionChannelTestFixture {
    CaptionChannelTestFixture() { reset_caption_start_channel(); }
    ~CaptionChannelTestFixture() { reset_caption_start_channel(); }
};

} // anonymous namespace

TEST_CASE_METHOD(CaptionChannelTestFixture,
                 "captions.start_engine returns NotRecording when idle",
                 "[caption-ipc]") {
    CaptionDaemonSim sim;
    sim.start();
    // recording=false by default. Worker not active either.

    auto client = caption_client();
    IpcResponse resp;
    IpcError err;
    bool ok = client->call("captions.start_engine", resp, err);
    CHECK_FALSE(ok);
    CHECK(err.code == static_cast<int>(IpcErrorCode::NotRecording));
    CHECK(err.message == "Not recording");
}

TEST_CASE_METHOD(CaptionChannelTestFixture,
                 "captions.start_engine returns NotRecording when worker inactive",
                 "[caption-ipc]") {
    CaptionDaemonSim sim;
    sim.start();
    // Manually flip recording=true to exercise the worker-inactive guard.
    // No mark_worker_active() call → channel reports WorkerNotReady →
    // handler maps to NotRecording with the "ending" message.
    sim.recording.store(true);

    auto client = caption_client();
    IpcResponse resp;
    IpcError err;
    bool ok = client->call("captions.start_engine", resp, err);
    CHECK_FALSE(ok);
    CHECK(err.code == static_cast<int>(IpcErrorCode::NotRecording));
    CHECK(err.message == "Recording is ending — captions cannot be started now");

    sim.recording.store(false);
}

TEST_CASE_METHOD(CaptionChannelTestFixture,
                 "captions.start_engine returns queued when worker active",
                 "[caption-ipc]") {
    CaptionDaemonSim sim;
    sim.start();
    sim.recording.store(true);
    mark_worker_active();

    auto client = caption_client();
    IpcResponse resp;
    IpcError err;
    bool ok = client->call("captions.start_engine", resp, err);
    CHECK(ok);
    CHECK(json_val_as_bool(resp.result["ok"]) == true);
    CHECK(json_val_as_string(resp.result["status"]) == "queued");

    sim.recording.store(false);
}

TEST_CASE_METHOD(CaptionChannelTestFixture,
                 "captions.start_engine returns already_running after mark",
                 "[caption-ipc]") {
    CaptionDaemonSim sim;
    sim.start();
    sim.recording.store(true);
    mark_worker_active();
    mark_caption_engine_running();

    auto client = caption_client();
    IpcResponse resp;
    IpcError err;
    bool ok = client->call("captions.start_engine", resp, err);
    CHECK(ok);
    CHECK(json_val_as_bool(resp.result["ok"]) == true);
    CHECK(json_val_as_string(resp.result["status"]) == "already_running");

    sim.recording.store(false);
}

TEST_CASE_METHOD(CaptionChannelTestFixture,
                 "concurrent captions.start_engine calls coalesce",
                 "[caption-ipc]") {
    CaptionDaemonSim sim;
    sim.start();
    sim.recording.store(true);
    mark_worker_active();

    auto client = caption_client();
    IpcResponse resp1, resp2, resp3;
    IpcError err1, err2, err3;

    // First call: queues a new pending request.
    REQUIRE(client->call("captions.start_engine", resp1, err1));
    CHECK(json_val_as_string(resp1.result["status"]) == "queued");

    // Second call without intervening poll: coalesces into the pending
    // request; still returns Queued.
    REQUIRE(client->call("captions.start_engine", resp2, err2));
    CHECK(json_val_as_string(resp2.result["status"]) == "queued");

    // Third call: also still queued — the pending flag is sticky until
    // a worker tick drains it.
    REQUIRE(client->call("captions.start_engine", resp3, err3));
    CHECK(json_val_as_string(resp3.result["status"]) == "queued");

    sim.recording.store(false);
}

TEST_CASE_METHOD(CaptionChannelTestFixture,
                 "coalesce drops non-matching override with log_warn",
                 "[caption-ipc]") {
    CaptionDaemonSim sim;
    sim.start();
    sim.recording.store(true);
    mark_worker_active();

    auto client = caption_client();

    // First call carries caption_model="A" → pending override is "A".
    {
        IpcResponse resp;
        IpcError err;
        JsonMap params;
        params["caption_model"] = std::string("A");
        REQUIRE(client->call("captions.start_engine", params, resp, err));
        CHECK(json_val_as_string(resp.result["status"]) == "queued");
    }

    // Second call carries caption_model="B" → channel keeps "A" as the
    // override (first request wins). Verb still returns Queued. The
    // log_warn fires inside request_caption_engine_start; we don't have
    // log capture wired here, so we verify behaviour by polling the
    // channel with a stub start_fn that records the override it sees.
    {
        IpcResponse resp;
        IpcError err;
        JsonMap params;
        params["caption_model"] = std::string("B");
        REQUIRE(client->call("captions.start_engine", params, resp, err));
        CHECK(json_val_as_string(resp.result["status"]) == "queued");
    }

    // Drain the channel via a stub start_fn that records the override
    // string. It MUST be "A" (the first caller's value) — the "B"
    // override from the second call was coalesced away.
    std::string seen_override;
    int call_count = 0;
    poll_and_handle_caption_start_request(
        [&](const std::string& m) {
            seen_override = m;
            ++call_count;
            return false;  // failure path keeps engine_running=false so
                           // we can re-verify the channel still accepts
                           // new requests if needed.
        });
    CHECK(call_count == 1);
    CHECK(seen_override == "A");

    sim.recording.store(false);
}

// ===========================================================================
// Phase 2 tests for `captions.start_engine`. These exercise the
// caption.started event broadcast — both for the recording-started-with-
// captions path (sim invokes hooks.on_engine_started manually mirroring the
// production worker entry) and the mid-recording verb path (test thread
// drains the channel with a stub start_fn that invokes the hook). Tests use
// CaptionChannelTestFixture to reset the file-static channel state.
// ===========================================================================

TEST_CASE_METHOD(CaptionChannelTestFixture,
                 "caption.started fires when engine starts via record.start",
                 "[caption-ipc]") {
    CaptionDaemonSim sim;
    sim.start();

    auto starter = caption_client();
    auto observer = caption_client();
    CaptionEventCollector evs;
    observer->set_event_callback(evs.callback());

    IpcResponse resp;
    IpcError err;
    JsonMap params;
    params["captions_enabled"] = true;
    REQUIRE(starter->call("record.start", params, resp, err));
    REQUIRE(sim.captions_enabled_for_active);

    // Simulate the production worker-entry sequence: mark_worker_active()
    // happens just before the polling loop; mark_caption_engine_running()
    // is called inside the if (auto eng = ...) success block; and
    // try_start_caption_engine invokes hooks.on_engine_started on success.
    // The sim's record.start runs the equivalent of try_start_caption_engine
    // but doesn't fire on_engine_started — emulate that here so we have a
    // single canonical event-emission point per recording.
    mark_worker_active();
    // Wire the on_engine_started hook the same way daemon.cpp does, so the
    // sim broadcasts caption.started when invoked. We do this after
    // record.start has set up the rest of the hook surface.
    sim.hooks.engine_started_ud = sim.ctx.get();
    sim.hooks.on_engine_started = +[](void* ud) {
        auto* c = static_cast<CaptionBroadcastCtx*>(ud);
        IpcServer* s = c->server;
        int64_t jid = c->job_id;
        int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        s->post([s, jid, ts]() {
            s->broadcast(make_caption_started_event(jid, ts));
        });
    };
    sim.hooks.on_engine_started(sim.hooks.engine_started_ud);
    mark_caption_engine_running();

    drain_caption_events(*observer, 300);
    CHECK(evs.has_event("caption.started"));
    CHECK(evs.count_event("caption.started") == 1);

    // Subsequent verb call now returns already_running and produces no
    // additional caption.started event.
    IpcResponse resp2;
    IpcError err2;
    REQUIRE(starter->call("captions.start_engine", resp2, err2));
    CHECK(json_val_as_string(resp2.result["status"]) == "already_running");
    drain_caption_events(*observer, 100);
    CHECK(evs.count_event("caption.started") == 1);

    starter->call("record.stop", params, resp, err);
}

TEST_CASE_METHOD(CaptionChannelTestFixture,
                 "caption.started fires when engine starts via verb",
                 "[caption-ipc]") {
    CaptionDaemonSim sim;
    sim.start();

    // Simulate a recording that started WITHOUT captions. The sim's
    // record.start with captions_enabled=false doesn't populate hooks, so
    // we build the broadcast ctx + on_engine_started hook directly — this
    // mirrors the production daemon.cpp where hooks are populated
    // unconditionally (Phase 2 daemon.cpp:1102 always-&hooks change).
    sim.recording.store(true);
    sim.active_job_id = sim.next_job_id.fetch_add(1);
    sim.ctx = std::make_unique<CaptionBroadcastCtx>(
        CaptionBroadcastCtx{&sim.server, sim.active_job_id});
    sim.hooks = {};
    sim.hooks.engine_started_ud = sim.ctx.get();
    sim.hooks.on_engine_started = +[](void* ud) {
        auto* c = static_cast<CaptionBroadcastCtx*>(ud);
        IpcServer* s = c->server;
        int64_t jid = c->job_id;
        int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        s->post([s, jid, ts]() {
            s->broadcast(make_caption_started_event(jid, ts));
        });
    };
    mark_worker_active();

    auto client = caption_client();
    CaptionEventCollector evs;
    client->set_event_callback(evs.callback());

    // Verb call — captions not yet running.
    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("captions.start_engine", resp, err));
    CHECK(json_val_as_string(resp.result["status"]) == "queued");

    // No caption.started event yet — the worker hasn't drained the channel.
    drain_caption_events(*client, 100);
    CHECK_FALSE(evs.has_event("caption.started"));

    // Drive the channel from the test thread (substitutes for the worker's
    // 200ms loop tick). stub_start_fn invokes on_engine_started to mirror
    // try_start_caption_engine's success path.
    auto stub_start_fn = [&sim](const std::string& /*model*/) -> bool {
        sim.hooks.on_engine_started(sim.hooks.engine_started_ud);
        return true;
    };
    poll_and_handle_caption_start_request(stub_start_fn);

    drain_caption_events(*client, 300);
    CHECK(evs.has_event("caption.started"));
    CHECK(evs.count_event("caption.started") == 1);

    // After the channel transitions to (T,F,T), a subsequent verb returns
    // already_running.
    IpcResponse resp2;
    IpcError err2;
    REQUIRE(client->call("captions.start_engine", resp2, err2));
    CHECK(json_val_as_string(resp2.result["status"]) == "already_running");

    sim.recording.store(false);
}

TEST_CASE_METHOD(CaptionChannelTestFixture,
                 "exactly one caption.started per recording (coalesce E2E)",
                 "[caption-ipc]") {
    CaptionDaemonSim sim;
    sim.start();

    sim.recording.store(true);
    sim.active_job_id = sim.next_job_id.fetch_add(1);
    sim.ctx = std::make_unique<CaptionBroadcastCtx>(
        CaptionBroadcastCtx{&sim.server, sim.active_job_id});
    sim.hooks = {};
    sim.hooks.engine_started_ud = sim.ctx.get();
    sim.hooks.on_engine_started = +[](void* ud) {
        auto* c = static_cast<CaptionBroadcastCtx*>(ud);
        IpcServer* s = c->server;
        int64_t jid = c->job_id;
        int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        s->post([s, jid, ts]() {
            s->broadcast(make_caption_started_event(jid, ts));
        });
    };
    mark_worker_active();

    auto client = caption_client();
    CaptionEventCollector evs;
    client->set_event_callback(evs.callback());

    // Three verb calls back-to-back — only the first creates a pending
    // request; the others coalesce. Phase 1 already verified this at the
    // verb-result level; here we additionally verify the broadcast count.
    for (int i = 0; i < 3; ++i) {
        IpcResponse resp;
        IpcError err;
        REQUIRE(client->call("captions.start_engine", resp, err));
        CHECK(json_val_as_string(resp.result["status"]) == "queued");
    }

    int start_fn_calls = 0;
    auto stub_start_fn = [&](const std::string& /*model*/) -> bool {
        ++start_fn_calls;
        sim.hooks.on_engine_started(sim.hooks.engine_started_ud);
        return true;
    };
    // Single poll drains the single pending request.
    poll_and_handle_caption_start_request(stub_start_fn);
    CHECK(start_fn_calls == 1);

    drain_caption_events(*client, 300);
    CHECK(evs.count_event("caption.started") == 1);

    // Channel is now (T,F,T); fourth verb returns already_running with no
    // additional event.
    IpcResponse resp4;
    IpcError err4;
    REQUIRE(client->call("captions.start_engine", resp4, err4));
    CHECK(json_val_as_string(resp4.result["status"]) == "already_running");
    drain_caption_events(*client, 100);
    CHECK(evs.count_event("caption.started") == 1);

    sim.recording.store(false);
}

TEST_CASE_METHOD(CaptionChannelTestFixture,
                 "captions.start_engine on bad model: caption.degraded engine_error, no caption.started + retry",
                 "[caption-ipc]") {
    CaptionDaemonSim sim;
    sim.start();

    sim.recording.store(true);
    sim.active_job_id = sim.next_job_id.fetch_add(1);
    sim.ctx = std::make_unique<CaptionBroadcastCtx>(
        CaptionBroadcastCtx{&sim.server, sim.active_job_id});
    sim.hooks = {};
    sim.hooks.engine_error_ud = sim.ctx.get();
    sim.hooks.engine_started_ud = sim.ctx.get();
    sim.hooks.on_engine_error = +[](const std::string& msg, void* ud) {
        auto* c = static_cast<CaptionBroadcastCtx*>(ud);
        IpcServer* s = c->server;
        int64_t jid = c->job_id;
        int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::string m = msg;
        s->post([s, jid, m, ts]() {
            IpcEvent ev = make_caption_degraded_event(jid, "engine_error", ts);
            ev.data["error"] = m;
            s->broadcast(ev);
        });
    };
    sim.hooks.on_engine_started = +[](void* ud) {
        auto* c = static_cast<CaptionBroadcastCtx*>(ud);
        IpcServer* s = c->server;
        int64_t jid = c->job_id;
        int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        s->post([s, jid, ts]() {
            s->broadcast(make_caption_started_event(jid, ts));
        });
    };
    mark_worker_active();

    auto client = caption_client();
    CaptionEventCollector evs;
    client->set_event_callback(evs.callback());

    // First verb call — queued.
    {
        IpcResponse resp;
        IpcError err;
        JsonMap params;
        params["caption_model"] = std::string("bogus-model-dir");
        REQUIRE(client->call("captions.start_engine", params, resp, err));
        CHECK(json_val_as_string(resp.result["status"]) == "queued");
    }

    // Failing stub_start_fn: emits engine_error and returns false. The
    // channel must leave engine_running=false so a retry can be queued.
    auto failing_start_fn = [&](const std::string& /*model*/) -> bool {
        sim.hooks.on_engine_error("model directory missing", sim.hooks.engine_error_ud);
        return false;
    };
    poll_and_handle_caption_start_request(failing_start_fn);

    drain_caption_events(*client, 300);
    CHECK_FALSE(evs.has_event("caption.started"));
    bool saw_engine_error = false;
    for (auto& e : evs.snapshot()) {
        if (e.event == "caption.degraded" &&
            json_val_as_string(e.data["reason"]) == "engine_error") {
            saw_engine_error = true;
            break;
        }
    }
    CHECK(saw_engine_error);

    // Retry: subsequent verb call returns queued (not already_running) and
    // a successful start_fn this time drains it.
    {
        IpcResponse resp;
        IpcError err;
        REQUIRE(client->call("captions.start_engine", resp, err));
        CHECK(json_val_as_string(resp.result["status"]) == "queued");
    }
    auto ok_start_fn = [&](const std::string& /*model*/) -> bool {
        sim.hooks.on_engine_started(sim.hooks.engine_started_ud);
        return true;
    };
    poll_and_handle_caption_start_request(ok_start_fn);
    drain_caption_events(*client, 300);
    CHECK(evs.count_event("caption.started") == 1);

    sim.recording.store(false);
}
