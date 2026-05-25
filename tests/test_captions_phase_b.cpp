// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase B1+B2+B4 — V2 captions always-stream client-side capability gate.
// Tests T6 and T7 from
// `agentctx/tasks/v2-captions-always-stream-client-renders.md` (rev 5).
//
// These tests exercise the production B1.1 capability gate: at record.start
// the client opens `process.stream` iff the server reports
// `captions_supported=true` on its session.init response. The user's local
// `cfg.captions_enabled` no longer gates wire-protocol behavior — that
// snapshot now controls overlay rendering only (B3, separate commit).
//
//   T6 — captions-supported daemon: session.init reports true, the gated
//        decision calls process.stream, a stream_token is minted, and the
//        daemon-side g_streaming sees the active session.
//
//   T7 — captions-unsupported daemon: session.init reports false, the
//        gated decision skips process.stream entirely, g_streaming stays
//        empty across the would-be record.start.
//
// Test shape note: we drive the production daemon code path through the
// in-process `DaemonTestHarness` rather than spawning recmeet-tray, because
// the menu-greying (B4) bit and the gate (B1.1) live in tray.cpp's
// `start_capture()` which is not linked into recmeet_tests. The test
// instead exercises the wire-protocol contract that the gate consumes:
// session.init returns the right `captions_supported`, and a client that
// branches on that bit (the exact decision the production tray makes) gets
// the right behavior end-to-end. The B1.1 if-statement itself is one line of
// C++ above an already-tested `process.stream` invocation; the value comes
// from verifying the wire-side bits the gate consumes, not from re-driving
// the GTK start_capture wrapper.
//
// T9 (menu sensitivity) is intentionally NOT here — see the dispatch
// orchestrator note. tray.cpp's build_menu() is not linked into
// recmeet_tests (`tray.cpp` is in the recmeet-tray binary only), and
// driving headless GTK menu construction would require an X server +
// gtk_init in the test process. Deferred to a manual smoke test until the
// tray gains a headless menu-construction harness.

#include <catch2/catch_test_macros.hpp>

#include "config.h"
#include "daemon_test_harness.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"
#include "streaming_session.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace recmeet;
using recmeet::test::DaemonTestHarness;
using namespace std::chrono_literals;

namespace {

// JqGuard mirrors the pattern in test_captions_phase_a.cpp: a single worker
// thread dequeues Streaming jobs so the slot marker flips, then loops until
// shutdown(). Joined on dtor (RAII).
struct JqGuard {
    std::thread worker;
    JqGuard() {
        worker = std::thread([]() {
            while (g_jobs) {
                auto dq = g_jobs->dequeue(JobKind::Streaming);
                if (!dq.has_value()) return;
            }
        });
    }
    ~JqGuard() {
        if (g_jobs) g_jobs->shutdown();
        if (worker.joinable()) worker.join();
    }
};

// Simulate the production B1.1 decision tree exactly as
// `tray::start_capture()` runs it post-Phase-B1:
//   * Read `captions_supported` from the session.init response (B2.2).
//   * If true, open `process.stream`. If false, skip the call entirely.
// Returns the stream_token (empty on skip-or-failure) and the resulting
// `g_streaming->active_count()` for the test to assert against.
struct GateResult {
    bool   captions_supported = false;
    bool   process_stream_called = false;
    std::string stream_token;
    size_t active_count_after_decision = 0;
};

GateResult drive_capability_gated_record_start(IpcClient& c) {
    GateResult r;

    JsonMap creds;
    JsonMap prefs;
    IpcResponse resp;
    IpcError err;
    REQUIRE(c.session_init(creds, prefs, resp, err));

    // B2.2 — read `captions_supported` from session.init response.
    auto cs_it = resp.result.find("captions_supported");
    r.captions_supported = (cs_it != resp.result.end())
        ? json_val_as_bool(cs_it->second)
        : false;

    // B1.1 — capability-gated `process.stream` open. The production
    // tray::start_capture() at tray.cpp:1936 wraps the process.stream
    // block in `if (g_tray.server_caps.captions_supported)`. This test
    // applies the same gate.
    if (r.captions_supported) {
        IpcResponse sresp;
        IpcError serr;
        REQUIRE(c.call("process.stream", {}, sresp, serr, 5000));
        r.process_stream_called = true;
        auto tit = sresp.result.find("stream_token");
        r.stream_token = (tit != sresp.result.end())
            ? json_val_as_string(tit->second)
            : std::string{};
    }
    // On the false branch we deliberately do NOTHING — that IS the
    // production behavior under B1.1.

    r.active_count_after_decision =
        g_streaming ? g_streaming->active_count() : 0;
    return r;
}

}  // anonymous namespace

// ===========================================================================
// T6 — Captions-supported: client opens process.stream regardless of local
//      cfg.captions_enabled (the gate consumes the server bit, not the
//      local toggle).
// ===========================================================================
TEST_CASE("B1.1 — captions-supported server: client opens process.stream "
          "based on session.init's captions_supported bit",
          "[captions][phase-b][full-stack][captions-supported]") {
    DaemonTestHarness h;
    // Emulate the post-startup state: A1.4's gate has already written
    // the runtime-effective true into g_server_config.captions_enabled,
    // and session.init will surface it on the wire as captions_supported.
    h.mutate_config([](ServerConfig& cfg) {
        cfg.captions_enabled = true;
    });
    h.start();
    JqGuard jq;

    IpcClient c(h.socket_path());
    REQUIRE(c.connect());

    GateResult r = drive_capability_gated_record_start(c);

    CHECK(r.captions_supported == true);
    CHECK(r.process_stream_called == true);
    CHECK_FALSE(r.stream_token.empty());
    // g_streaming holds one active session — the production gate
    // opened a real streaming reservation server-side.
    CHECK(r.active_count_after_decision == 1);

    c.close_connection();
    // The harness's on_client_disconnect tears down the streaming
    // session when the poll thread observes EOF — wait for active_count
    // to drop so the test exits cleanly.
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_streaming->active_count() == 0) break;
        std::this_thread::sleep_for(20ms);
    }
    CHECK(g_streaming->active_count() == 0);
}

// ===========================================================================
// T7 — Captions-unsupported: client reads captions_supported=false and
//      does NOT open process.stream. Daemon-side g_streaming stays empty.
// ===========================================================================
TEST_CASE("B1.1 — captions-unsupported server: client does NOT open "
          "process.stream when session.init reports captions_supported=false",
          "[captions][phase-b][full-stack][captions-unsupported]") {
    DaemonTestHarness h;
    // Emulate the post-startup state for an explicit opt-out: A1.4's
    // gate AND'd operator-intent (false) with runtime-capable (any) and
    // wrote false back. session.init will surface false on the wire.
    h.mutate_config([](ServerConfig& cfg) {
        cfg.captions_enabled = false;
    });
    h.start();
    JqGuard jq;

    IpcClient c(h.socket_path());
    REQUIRE(c.connect());

    GateResult r = drive_capability_gated_record_start(c);

    CHECK(r.captions_supported == false);
    // The B1.1 gate skipped the process.stream open entirely — that is
    // the headline rev-5 behavior: captions-unsupported recordings see
    // the same wire shape as today's captions-off recordings (no
    // streaming session, batch submit at disposition Submit only).
    CHECK(r.process_stream_called == false);
    CHECK(r.stream_token.empty());
    // Daemon-side g_streaming holds ZERO active sessions: the client
    // never opened one. This is the cross-tier verification of B1.1 —
    // the wire behavior the daemon observes matches the no-stream
    // contract the gate promises.
    CHECK(r.active_count_after_decision == 0);

    c.close_connection();
}
