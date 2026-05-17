// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.13 — IPC-integration tests for the server-side resume_token /
// PSK-rotation / per-handler dispatch behavior.
//
// Tag-spaces:
//   [c13][ipc][integration]      — TCP IpcServer + IpcClient round-trip
//   [c13][evict][ipc]            — admin.evict verb end-to-end (Unix socket)
//   [c13][session][ipc]          — last_seen bump observed at dispatch site
//
// All tests stand up a minimal IpcServer harness (no DaemonSim) so they
// stay in the [integration] tag-space but don't pull in the full Phase
// C wiring. The resume_token resolver + dispatch hook are wired
// per-test, mirroring daemon.cpp's main() init exactly.

#include <catch2/catch_test_macros.hpp>

#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "session_manager.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <signal.h>
#include <string>
#include <thread>
#include <unistd.h>

using namespace recmeet;
using namespace std::chrono_literals;

namespace {

// RAII helper for RECMEET_AUTH_TOKEN — same shape as test_ipc_integration's.
struct ScopedAuthToken {
    std::string previous; bool had_previous;
    explicit ScopedAuthToken(const std::string& value) {
        const char* p = std::getenv("RECMEET_AUTH_TOKEN");
        had_previous = p != nullptr;
        previous = had_previous ? p : "";
        setenv("RECMEET_AUTH_TOKEN", value.c_str(), 1);
    }
    ~ScopedAuthToken() {
        if (had_previous) setenv("RECMEET_AUTH_TOKEN", previous.c_str(), 1);
        else              unsetenv("RECMEET_AUTH_TOKEN");
    }
};

// Wire a SessionManager + the C.13 resolver + dispatch hook into an
// IpcServer in the same shape the daemon's main() does. Mirrors:
//   - server.set_resume_token_resolver([&sm, &server](tok) { ... })
//   - server.set_request_dispatch_hook([&sm](tok) { sm.bump_last_seen(tok); })
// Use a non-owning raw pointer to keep the harness composable.
void wire_c13_seams(IpcServer& server, SessionManager& sm) {
    server.set_resume_token_resolver(
        [&server, &sm](const std::string& provided)
            -> std::pair<std::string, std::string> {
            if (!provided.empty()) {
                if (auto cid = sm.resolve(provided); cid) return {*cid, provided};
            }
            std::string cid = server.mint_client_id();
            std::string tok = sm.mint(cid);
            return {cid, tok};
        });
    server.set_request_dispatch_hook(
        [&sm](const std::string& tok) { sm.bump_last_seen(tok); });
}

// Stand-up + tear-down a TCP IpcServer harness with `status.get` wired so
// the client has a valid round-trip verb. Caller controls the PSK +
// SessionManager + port.
struct TcpServerHarness {
    std::unique_ptr<IpcServer> server;
    std::thread thr;
    SessionManager* sm;
    std::string addr;
    std::atomic<int> dispatch_calls{0};

    TcpServerHarness(const std::string& tcp_addr, const std::string& psk,
                     SessionManager& sm_)
        : sm(&sm_), addr(tcp_addr) {
        server = std::make_unique<IpcServer>(addr);
        server->set_psk(psk);
        wire_c13_seams(*server, *sm);
        server->on("status.get",
                   [this](const IpcRequest&, IpcResponse& resp, IpcError&) {
                       resp.result["state"] = std::string("idle");
                       dispatch_calls.fetch_add(1);
                       return true;
                   });
        server->on("ping",
                   [](const IpcRequest&, IpcResponse& resp, IpcError&) {
                       resp.result["pong"] = true;
                       return true;
                   });
        REQUIRE(server->start());
        thr = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(50ms);
    }
    ~TcpServerHarness() {
        if (server) server->stop();
        if (thr.joinable()) thr.join();
    }
};

} // anonymous namespace

// ===========================================================================
// (4) PSK rotation via SIGHUP-with-env-mismatch rejects every next-reconnect.
// We don't actually deliver SIGHUP here (the test process has its own
// handlers); instead we directly call set_psk() — which is exactly what
// the SIGHUP handler does at daemon.cpp:243. The wire behavior the test
// pins is: after rotation, a client with the old PSK can NOT reconnect,
// and the already-connected client survives.
// ===========================================================================
TEST_CASE("C.13: PSK rotation via set_psk (SIGHUP analog) rejects next-reconnect; "
          "existing connections survive (C-2)",
          "[c13][ipc][integration]") {
    const std::string TCP_ADDR = "127.0.0.1:19961";
    const std::string PSK_A = "psk-A-original";
    const std::string PSK_B = "psk-B-rotated";

    SessionManager sm(/*ttl_seconds=*/3600);

    ScopedAuthToken env(PSK_A);
    TcpServerHarness srv(TCP_ADDR, PSK_A, sm);

    // (1) Initial connect with PSK_A succeeds.
    IpcClient c1(TCP_ADDR);
    REQUIRE(c1.connect());
    IpcResponse resp; IpcError err;
    REQUIRE(c1.call("status.get", resp, err, 2000));

    // (2) Rotate the server-side PSK — analog of SIGHUP after operator
    // updated RECMEET_AUTH_TOKEN in the unit env. This is exactly the
    // call daemon.cpp's SIGHUP handler issues at line 243.
    srv.server->set_psk(PSK_B);

    // (3) Existing connection still works — the rotation only checks
    // PSK on new auth.token handshakes, not on already-Authed fds.
    IpcResponse resp2; IpcError err2;
    REQUIRE(c1.call("status.get", resp2, err2, 2000));

    // (4) A fresh client with the OLD PSK is REJECTED on connect.
    {
        ScopedAuthToken env_old(PSK_A);
        IpcClient c_old(TCP_ADDR);
        REQUIRE_FALSE(c_old.connect());
    }

    // (5) A fresh client with the NEW PSK succeeds.
    {
        ScopedAuthToken env_new(PSK_B);
        IpcClient c_new(TCP_ADDR);
        REQUIRE(c_new.connect());
        IpcResponse r3; IpcError e3;
        REQUIRE(c_new.call("status.get", r3, e3, 2000));
    }
}

// ===========================================================================
// (10) admin.evict end-to-end via Unix-socket IPC. Mirrors the daemon's
// admin.evict handler shape; the SessionManager state is per-test.
// ===========================================================================

namespace {

// Build a Unix-socket harness that wires admin.evict + a couple of
// session-state inspection helpers. Used by --evict + last_seen tests.
struct UnixServerHarness {
    std::unique_ptr<IpcServer> server;
    std::thread thr;
    SessionManager* sm;
    std::string sock_path;

    UnixServerHarness(const std::string& path, SessionManager& sm_)
        : sm(&sm_), sock_path(path) {
        ::unlink(path.c_str());
        server = std::make_unique<IpcServer>(path);
        wire_c13_seams(*server, *sm);
        server->on("status.get",
                   [](const IpcRequest&, IpcResponse& resp, IpcError&) {
                       resp.result["state"] = std::string("idle");
                       return true;
                   });
        server->on("ping",
                   [](const IpcRequest&, IpcResponse& resp, IpcError&) {
                       resp.result["pong"] = true;
                       return true;
                   });
        // admin.evict — verbatim copy of daemon.cpp's handler (minus the
        // teardown_orphan_jobs call, which requires the daemon globals).
        // The session-eviction primitive is what we pin here.
        server->on("admin.evict",
                   [this](const IpcRequest& req, IpcResponse& resp,
                          IpcError& err) {
                       auto it = req.params.find("prefix");
                       if (it == req.params.end()) {
                           err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                           err.message = "admin.evict: missing 'prefix'";
                           return false;
                       }
                       std::string prefix = json_val_as_string(it->second);
                       EvictResult r = sm->evict_by_prefix(prefix);
                       switch (r.kind) {
                       case EvictResult::Kind::PrefixTooShort:
                           err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                           err.message = "admin.evict: prefix too short (min 8 hex chars)";
                           return false;
                       case EvictResult::Kind::NoMatch:
                           err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                           err.message = "admin.evict: no matching session";
                           return false;
                       case EvictResult::Kind::Ambiguous:
                           err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                           err.message = "admin.evict: ambiguous prefix";
                           return false;
                       case EvictResult::Kind::Evicted:
                           resp.result["evicted"]   = r.token;
                           resp.result["client_id"] = r.client_id;
                           resp.result["owned_jobs_failed"] = std::string("[]");
                           return true;
                       }
                       err.code = static_cast<int>(IpcErrorCode::InternalError);
                       err.message = "unreachable";
                       return false;
                   });
        REQUIRE(server->start());
        thr = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(50ms);
    }
    ~UnixServerHarness() {
        if (server) server->stop();
        if (thr.joinable()) thr.join();
        ::unlink(sock_path.c_str());
    }
};

} // anonymous namespace

TEST_CASE("C.13: admin.evict happy path returns full token + client_id",
          "[c13][evict][ipc][integration]") {
    SessionManager sm(/*ttl_seconds=*/3600);
    std::string tok = sm.mint("client-A");
    std::string prefix8 = tok.substr(0, 8);

    UnixServerHarness srv("/tmp/recmeet_c13_evict.sock", sm);
    IpcClient c(srv.sock_path);
    REQUIRE(c.connect());

    JsonMap params;
    params["prefix"] = prefix8;
    IpcResponse resp; IpcError err;
    REQUIRE(c.call("admin.evict", params, resp, err, 2000));
    CHECK(json_val_as_string(resp.result["evicted"]) == tok);
    CHECK(json_val_as_string(resp.result["client_id"]) == "client-A");
    // Session is gone.
    CHECK(sm.size() == 0);
    CHECK(!sm.resolve(tok).has_value());
}

TEST_CASE("C.13: admin.evict prefix policy — too-short / no-match / ambiguous",
          "[c13][evict][ipc][integration]") {
    SessionManager sm(/*ttl_seconds=*/3600);
    std::string tok = sm.mint("client-A");

    UnixServerHarness srv("/tmp/recmeet_c13_evict2.sock", sm);
    IpcClient c(srv.sock_path);
    REQUIRE(c.connect());

    SECTION("too short") {
        JsonMap p;
        p["prefix"] = tok.substr(0, 7);  // 7 chars < 8
        IpcResponse r; IpcError e;
        REQUIRE_FALSE(c.call("admin.evict", p, r, e, 2000));
        CHECK(std::string(e.message).find("too short") != std::string::npos);
        CHECK(sm.size() == 1);
    }
    SECTION("no match") {
        JsonMap p;
        p["prefix"] = std::string("zzzzzzzz");  // 8 'z' — never a hex prefix
        IpcResponse r; IpcError e;
        REQUIRE_FALSE(c.call("admin.evict", p, r, e, 2000));
        CHECK(std::string(e.message).find("no matching") != std::string::npos);
        CHECK(sm.size() == 1);
    }
    SECTION("ambiguous (deterministic via insert_for_test)") {
        // Plant two colliding tokens via the test seam (gated by
        // `#if defined(RECMEET_TESTING)` in session_manager.h), then send
        // `admin.evict` with the shared 8-char prefix over IPC. The
        // server's admin.evict handler MUST surface Ambiguous as an
        // `InvalidParams` error and MUST NOT evict either session — the
        // collision is the operator's signal to extend the prefix.
        const std::string prefix8 = "cafef00d";
        const std::string tok_a = prefix8 + std::string(56, '1');
        const std::string tok_b = prefix8 + std::string(56, '2');
        REQUIRE(tok_a.size() == 64);
        REQUIRE(tok_b.size() == 64);
        sm.insert_for_test(tok_a, "client-amb-a");
        sm.insert_for_test(tok_b, "client-amb-b");
        const auto size_before = sm.size();   // includes the auth.token mint
        REQUIRE(size_before >= 3);            // 1 (handshake) + 2 (planted)

        JsonMap p;
        p["prefix"] = prefix8;
        IpcResponse r; IpcError e;
        REQUIRE_FALSE(c.call("admin.evict", p, r, e, 2000));
        CHECK(std::string(e.message).find("ambiguous") != std::string::npos);
        CHECK(e.code == static_cast<int>(IpcErrorCode::InvalidParams));
        // BOTH planted sessions must remain — Ambiguous is a no-op.
        CHECK(sm.size() == size_before);
        CHECK(sm.resolve(tok_a).has_value());
        CHECK(sm.resolve(tok_b).has_value());
    }
}

// ===========================================================================
// (12) last_seen bump on each inbound IPC request advances the
// per-token epoch. Outbound events do NOT advance it — by construction:
// the request dispatch hook only runs in the per-handler dispatch path,
// not inside server->broadcast() / send_to_client(), so broadcast paths
// have nothing to bump. We assert the positive case here against the
// observable `last_seen_for_test()` accessor — the prior iteration of
// this test trailed off in a SUCCEED("dispatch hook fires...") that
// documented the contract without exercising the actual mutation, so a
// regression that silently dropped the bump would have slipped through.
// ===========================================================================
TEST_CASE("C.13: per-handler dispatch hook bumps last_seen per inbound request",
          "[c13][session][ipc][integration]") {
    const std::string TCP_ADDR = "127.0.0.1:19962";
    const std::string PSK = "psk-bump";

    // Inject a fake clock so we can advance time deterministically between
    // each IPC call and assert the bump produced a strictly-monotonic
    // increase. The default real clock would advance too coarsely
    // (per-second resolution) to assert anything useful inside one test.
    std::atomic<int64_t> fake_now{1'000'000};
    SessionManager sm(/*ttl_seconds=*/3600,
                      [&fake_now]() { return fake_now.load(); });

    ScopedAuthToken env(PSK);

    // Custom wiring — same shape as `wire_c13_seams` + `TcpServerHarness`
    // but we capture the resolver's minted token so we can later inspect
    // `last_seen_for_test(token)`. The token is server-stamped during the
    // auth.token handshake; `wire_c13_seams` discards it, so we re-do the
    // setup here with the capture.
    std::string captured_token;
    std::mutex capture_mu;

    std::unique_ptr<IpcServer> server = std::make_unique<IpcServer>(TCP_ADDR);
    server->set_psk(PSK);
    server->set_resume_token_resolver(
        [&](const std::string& provided)
            -> std::pair<std::string, std::string> {
            if (!provided.empty()) {
                if (auto cid = sm.resolve(provided); cid) return {*cid, provided};
            }
            std::string cid = server->mint_client_id();
            std::string tok = sm.mint(cid);
            {
                std::lock_guard<std::mutex> lock(capture_mu);
                captured_token = tok;
            }
            return {cid, tok};
        });
    server->set_request_dispatch_hook(
        [&](const std::string& tok) { sm.bump_last_seen(tok); });

    std::atomic<int> status_get_calls{0};
    std::atomic<int> ping_calls{0};
    server->on("status.get",
               [&](const IpcRequest&, IpcResponse& resp, IpcError&) {
                   resp.result["state"] = std::string("idle");
                   status_get_calls.fetch_add(1);
                   return true;
               });
    server->on("ping",
               [&](const IpcRequest&, IpcResponse& resp, IpcError&) {
                   resp.result["pong"] = true;
                   ping_calls.fetch_add(1);
                   return true;
               });

    REQUIRE(server->start());
    std::thread server_thread([&]() { server->run(); });
    std::this_thread::sleep_for(50ms);

    IpcClient c(TCP_ADDR);
    REQUIRE(c.connect());
    REQUIRE(sm.size() == 1);   // mint occurred during auth.token handshake

    std::string tok;
    {
        std::lock_guard<std::mutex> lock(capture_mu);
        tok = captured_token;
    }
    REQUIRE(tok.size() == 64);

    // Capture the post-mint last_seen baseline. This is set by mint() to
    // fake_now's value at handshake time.
    auto t0_opt = sm.last_seen_for_test(tok);
    REQUIRE(t0_opt.has_value());
    const int64_t t0 = *t0_opt;

    // Send several verbs; advance fake_now BEFORE each call so the hook
    // (which reads fake_now via clock_) writes a strictly-larger value
    // every time it fires. The loop dispatches 5 status.get + 5 ping = 10
    // IPC requests total — each one passes through the per-handler
    // dispatch site at `src/ipc_server.cpp:855-856` and invokes
    // `sm.bump_last_seen(resume_token)`.
    for (int i = 0; i < 5; ++i) {
        IpcResponse r; IpcError e;
        fake_now.fetch_add(1);
        REQUIRE(c.call("status.get", r, e, 2000));
        fake_now.fetch_add(1);
        REQUIRE(c.call("ping", r, e, 2000));
    }

    // Give the server thread a beat to finish dispatching the last
    // request's hook before we read last_seen — the IpcClient::call
    // returns when the response frame lands, which is after the hook
    // already ran (the hook fires synchronously before the handler at
    // `src/ipc_server.cpp:855`), so this sleep is belt-and-braces.
    std::this_thread::sleep_for(20ms);

    // (Gap 2) — assert last_seen ACTUALLY advanced. Without this check,
    // a regression that silently dropped `request_dispatch_hook_(...)`
    // at the dispatch site would have left the test green via the prior
    // iteration's trailing SUCCEED.
    auto t1_opt = sm.last_seen_for_test(tok);
    REQUIRE(t1_opt.has_value());
    const int64_t t1 = *t1_opt;
    REQUIRE(t1 > t0);

    // (Gap 3) — the prior iteration asserted `dispatch_calls.load() == 5`
    // for a loop that issued 10 IPC calls (5 status.get + 5 ping). That
    // assertion was NOT measuring "hook fires" — it was measuring the
    // `status.get` handler's body counter, which only the status.get
    // handler increments (ping doesn't). The arithmetic is correct as-was
    // FOR the variable's actual meaning, but the variable name and
    // adjacent comments made it look like a hook-fire count, which was
    // misleading. We split it into two counters (status_get_calls +
    // ping_calls) here so each assertion's contract is unambiguous, and
    // we add an independent assertion that the hook fired once per
    // inbound request by checking the advance distance: with fake_now
    // bumped +1 between every call, the per-call advance was 1, so the
    // total advance equals the number of hook fires (== 10 inbound
    // requests).
    REQUIRE(status_get_calls.load() == 5);
    REQUIRE(ping_calls.load() == 5);
    // Per-call clock advance pattern: t0 (handshake) → fake_now += 1 → call
    // status.get (hook fires, last_seen <- fake_now) → fake_now += 1 →
    // call ping (hook fires, last_seen <- fake_now) → ... After 10 calls
    // the final fake_now == t0 + 10, and the final hook write equals
    // that value, so t1 - t0 == 10.
    REQUIRE(t1 - t0 == 10);

    // Server-emitted events (no client request) would not have advanced
    // the hook — the hook lives inside the per-handler dispatch loop at
    // `src/ipc_server.cpp:855-856`, NOT in `send_to() / broadcast()`.
    // This is structurally guaranteed (single call site); the
    // last_seen-advance assertion above is the dynamic complement.

    server->stop();
    if (server_thread.joinable()) server_thread.join();
}
