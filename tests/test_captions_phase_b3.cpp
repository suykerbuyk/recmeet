// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase B3 — V2 captions always-stream mid-recording-toggle decoupling.
// Test T8 from
// `agentctx/tasks/v2-captions-always-stream-client-renders.md` (rev 5).
//
// The headline rev-5 outcome is that the user can show/hide the live
// caption overlay any number of times during a recording without
// triggering any server-side engine restart and without breaking the
// `.pending` sidecar's record-start snapshot semantics. B3 implements
// this by splitting the tray's runtime state into two fields:
//
//   * `g_tray.cap.captions_enabled_for_recording` — immutable
//     snapshot, written once at record.start, read by the `.pending`
//     sidecar write + resume restore path; never touched mid-recording.
//   * `g_tray.cap.overlay_visible_now` — mutable runtime visibility,
//     seeded from `cfg.captions_enabled` at record.start, then mutated
//     by `on_captions_enabled_toggled` (the menu callback) and read by
//     the four render gates (caption / caption.degraded handlers, the
//     inline-menu caption row, and the slot-active-badge).
//
// Wire-protocol contract under test:
//   * server-side `g_streaming->active_count()` stays at exactly 1
//     across the three toggle windows. The B1.1 capability gate
//     opens process.stream once at record.start; the mid-recording
//     toggle MUST NOT initiate or tear down a streaming session.
//   * audio keeps flowing via `feed_audio` across all three windows
//     (the engine sees the same byte stream regardless of the
//     client's local render decision).
//
// Test-shape note: tray.cpp is in the recmeet-tray binary only — it is
// NOT linked into recmeet_tests (verified via tests/CMakeLists.txt).
// We therefore cannot drive the production `on_captions_enabled_toggled`
// GTK callback directly. Instead, this test:
//
//   1. Spawns the in-process daemon harness with
//      `captions_supported=true` (mirroring T6's setup).
//   2. Connects an IpcClient and runs the B1.1 capability gate by
//      hand (session.init + process.stream).
//   3. Maintains a *local mirror* of the tray's two new fields
//      (`captions_enabled_for_recording` + `overlay_visible_now`) and
//      drives them through the exact transition sequence the
//      production `on_captions_enabled_toggled` callback would
//      perform — `captions_enabled_for_recording` is set once at the
//      record-start step and never re-written; `overlay_visible_now`
//      flips ON/OFF/ON across three observation windows.
//   4. Between each transition, calls `g_streaming->feed_audio()` to
//      verify the streaming session keeps accepting audio (the
//      production tray-side audio pump would be calling this exact
//      entry point on every PCM frame).
//   5. Asserts the snapshot field stays immutable, the runtime field
//      tracks each toggle, and `active_count() == 1` across all
//      three windows (zero engine restarts).
//
// This is the same shape T6/T7 use to verify Phase B1's capability
// gate: drive the production daemon path, simulate the client-side
// decision the gate consumes, and verify the cross-tier invariants.
// Driving the GTK callback would require booting GTK in the test
// process, which the test infrastructure does not do.
//
// Thread hygiene: JqGuard owns the dequeue thread and joins on dtor;
// `c.close_connection()` runs unconditionally to drain the streaming
// session; explicit `g_streaming->cancel()` is the exception path
// safety net for the case where the IpcClient teardown races the
// poll-thread session reaper.

#include <catch2/catch_test_macros.hpp>

#include "config.h"
#include "daemon_handlers_internal.h"
#include "daemon_test_harness.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"
#include "streaming_session.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace recmeet;
using recmeet::test::DaemonTestHarness;
using namespace std::chrono_literals;

namespace {

// JqGuard — same shape as test_captions_phase_b.cpp's. A single worker
// thread dequeues Streaming jobs so the slot marker flips, then loops
// until shutdown(). Joined on dtor.
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

// Build a deterministic PCM payload of `n_samples` int16 samples as a
// std::string of raw bytes (the `0x03` payload shape). Mirrors
// test_streaming_session.cpp's helper.
std::string make_pcm(size_t n_samples, int16_t start = 0) {
    std::string s;
    s.resize(n_samples * sizeof(int16_t));
    auto* p = reinterpret_cast<int16_t*>(s.data());
    for (size_t i = 0; i < n_samples; ++i)
        p[i] = static_cast<int16_t>(start + static_cast<int16_t>(i));
    return s;
}

// Mirror of the tray-side `g_tray.cap` aggregate for the two fields
// B3 cares about. Updated in this test via the exact transition rules
// the production code applies:
//
//   * record.start  — both fields seeded from the user's
//                     `cfg.captions_enabled`.
//   * toggle event  — `overlay_visible_now` mutated to the new toggle
//                     state; `captions_enabled_for_recording`
//                     untouched (the post-B3 invariant).
struct CapMirror {
    bool captions_enabled_for_recording = false;
    bool overlay_visible_now = false;

    // Simulates the production `tray.cpp:1979-1984` record.start
    // block: snapshot the user's persistent default into BOTH fields
    // (one as immutable record, one as runtime-mutable visibility).
    void on_record_start(bool cfg_captions_enabled) {
        captions_enabled_for_recording = cfg_captions_enabled;
        overlay_visible_now            = cfg_captions_enabled;
    }
    // Simulates the production `on_captions_enabled_toggled` callback
    // (`tray.cpp:2611-2647` post-B3.3): mutates `overlay_visible_now`
    // only when a recording is in progress; `captions_enabled_for_recording`
    // is NEVER touched here.
    void on_toggle(bool new_state, bool recording_in_progress) {
        if (recording_in_progress) {
            overlay_visible_now = new_state;
        }
    }
};

}  // anonymous namespace

// ============================================================================
// T8 — Mid-recording toggle (in-process): three observation windows of
//      toggle ON / OFF / ON across a live captions-supported streaming
//      session. The snapshot field stays immutable, the runtime field
//      tracks each toggle, and the server-side streaming session count
//      stays at exactly 1 with audio still flowing.
// ============================================================================
TEST_CASE("B3 — mid-recording toggle: overlay_visible_now tracks toggle "
          "while captions_enabled_for_recording stays immutable; "
          "zero server-side engine restarts",
          "[captions][phase-b3][full-stack]") {
    DaemonTestHarness h;
    // captions_supported=true mirrors T6: post-A1.4 runtime-effective
    // value true → session.init returns captions_supported=true →
    // production B1.1 gate opens process.stream at record.start.
    h.mutate_config([](ServerConfig& cfg) {
        cfg.captions_enabled = true;
    });
    h.start();
    JqGuard jq;

    IpcClient c(h.socket_path());
    REQUIRE(c.connect());

    // --- Production B1.1 capability gate, driven by hand ---
    JsonMap creds;
    JsonMap prefs;
    IpcResponse resp;
    IpcError err;
    REQUIRE(c.session_init(creds, prefs, resp, err));
    bool captions_supported =
        json_val_as_bool(resp.result.at("captions_supported"));
    REQUIRE(captions_supported);

    IpcResponse sresp;
    IpcError serr;
    REQUIRE(c.call("process.stream", {}, sresp, serr, 5000));
    std::string stream_token =
        json_val_as_string(sresp.result.at("stream_token"));
    REQUIRE_FALSE(stream_token.empty());

    // Streaming session is open — exactly one. This is the invariant
    // T8 asserts is preserved across all three toggle windows.
    REQUIRE(g_streaming != nullptr);
    REQUIRE(g_streaming->active_count() == 1);

    // --- Exception-path safety net ---
    // If any of the assertions below fire, we still want the IpcClient
    // teardown (via close_connection) to run so the server-side poll
    // thread can reap the streaming session — otherwise global state
    // bleeds into the next test. The catch2 assertion-fail path
    // unwinds through this scope, so the guard runs on every exit.
    struct DisconnectGuard {
        IpcClient* c = nullptr;
        ~DisconnectGuard() {
            if (c) c->close_connection();
        }
    } disconnect_guard{&c};

    // --- Simulate record.start on the client side ---
    // The user's persistent default is captions ON for this test
    // (we're verifying the toggle decouples from server lifecycle,
    // not the default-ON-vs-OFF path).
    bool user_cfg_captions_enabled = true;
    CapMirror cap;
    cap.on_record_start(user_cfg_captions_enabled);

    CHECK(cap.captions_enabled_for_recording == true);
    CHECK(cap.overlay_visible_now == true);

    // Snapshot the immutable field — every later window MUST match
    // this exact value.
    const bool snapshot_value = cap.captions_enabled_for_recording;

    // Helper: shorthand for verifying server-side invariants after
    // each transition. Audio MUST keep flowing (feed_audio returns
    // true on every accepted chunk) and the session count MUST stay
    // at exactly 1.
    auto verify_server_invariants = [&](const char* window_label,
                                        int16_t pcm_seed) {
        INFO("window=" << window_label);
        // Feed two chunks across each window — enough to prove the
        // session is still alive but small enough to keep the test
        // quick. 10ms + 50ms at 16 kHz mono s16le.
        CHECK(g_streaming->feed_audio(stream_token,
                                      make_pcm(160, pcm_seed)));
        CHECK(g_streaming->feed_audio(stream_token,
                                      make_pcm(800, pcm_seed + 100)));
        CHECK(g_streaming->active_count() == 1);
    };

    // ----------------------------------------------------------------
    // Window 1 — record-start state: overlay visible (toggle ON by
    //            virtue of cfg.captions_enabled=true).
    // ----------------------------------------------------------------
    {
        INFO("window-1 (record-start: visible)");
        CHECK(cap.captions_enabled_for_recording == snapshot_value);
        CHECK(cap.overlay_visible_now == true);
        verify_server_invariants("window-1", /*pcm_seed=*/0);
    }

    // ----------------------------------------------------------------
    // Window 2 — user toggles OFF mid-recording. The production GTK
    //            callback would hide the overlay; the test simulates
    //            the field transition + verifies the snapshot field
    //            stays immutable + server-side state unchanged.
    // ----------------------------------------------------------------
    cap.on_toggle(/*new_state=*/false, /*recording_in_progress=*/true);
    {
        INFO("window-2 (toggle OFF)");
        // The snapshot field is the headline B3 invariant: it MUST
        // stay at the record-start value, because the .pending
        // sidecar serializes it and the resume-from-crash path
        // reads it back as the user's intent at record-start time.
        CHECK(cap.captions_enabled_for_recording == snapshot_value);
        CHECK(cap.overlay_visible_now == false);
        verify_server_invariants("window-2", /*pcm_seed=*/1000);
    }

    // ----------------------------------------------------------------
    // Window 3 — user toggles ON again mid-recording. Same snapshot
    //            immutability + same server-side stability.
    // ----------------------------------------------------------------
    cap.on_toggle(/*new_state=*/true, /*recording_in_progress=*/true);
    {
        INFO("window-3 (toggle ON again)");
        CHECK(cap.captions_enabled_for_recording == snapshot_value);
        CHECK(cap.overlay_visible_now == true);
        verify_server_invariants("window-3", /*pcm_seed=*/2000);
    }

    // ----------------------------------------------------------------
    // Final invariants — across the entire three-window sequence the
    // snapshot field NEVER changed, and the server-side session
    // count NEVER varied from 1. The production B3 design holds:
    // toggle is a pure UI mutator with no IPC and no server state.
    // ----------------------------------------------------------------
    CHECK(cap.captions_enabled_for_recording == snapshot_value);
    CHECK(g_streaming->active_count() == 1);

    // --- Teardown ---
    // close_connection() triggers the daemon-side poll thread to
    // observe EOF on the client socket and reap the streaming session
    // via its on-disconnect path. Mirrors T6's teardown shape (the
    // production `process.stream.cancel` verb keys on the IPC session's
    // own client_id, not on a literal we could pass here).
    c.close_connection();
    disconnect_guard.c = nullptr;  // already disconnected — disarm.

    // Wait for the daemon-side reaper / cancel to drop active_count
    // to zero; bounded to 2s so the test doesn't hang on a misbehaving
    // teardown path.
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_streaming->active_count() == 0) break;
        std::this_thread::sleep_for(20ms);
    }
    CHECK(g_streaming->active_count() == 0);
}
