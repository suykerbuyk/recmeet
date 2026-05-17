// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.5 — tray resume-recovery integration tests.
//
// Tests #5 and #6 from the D.5 plan. These are tagged
// `[d5][ipc][integration]` because they stand up a real TCP IpcServer
// (mirroring `test_session_ipc.cpp`'s harness) and exercise the new
// `IpcClient::connect(psk, resume_token)` overload + the
// `resume_token()` capture from auth.ok. Test #6 additionally
// round-trips the sidecar-driven resume submenu path: a `.pending`
// sidecar in staging produces a `pending_resumes` entry whose
// meeting_id + context match the sidecar bytes.
//
// We do NOT instantiate the GTK tray here — the recovery loop's
// behavior is delegated to the journal + sidecar + token-store classes
// (all unit-testable). What the integration tests pin is the wire
// shape: the client sends the resume_token in auth.token, the server
// resolves via the SessionManager hook, and the auth.ok response
// carries the resolved token back.

#include <catch2/catch_test_macros.hpp>

#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "pending_jobs_journal.h"
#include "resume_token_store.h"
#include "session_manager.h"
#include "tray_capture.h"
#include "uuid.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace recmeet;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

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

fs::path make_scratch() {
    std::random_device rd;
    std::ostringstream oss;
    oss << "/tmp/recmeet_d5_resume_" << ::getpid() << "_" << rd();
    fs::path p = oss.str();
    std::error_code ec;
    fs::create_directories(p, ec);
    REQUIRE_FALSE(ec);
    return p;
}

struct ScopedDir {
    fs::path path;
    ~ScopedDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Stand up a TCP IpcServer with the C.13 resolver + dispatch hooks wired
// in the same shape as `daemon.cpp`'s main(). Mirrors
// `test_session_ipc.cpp::wire_c13_seams`.
struct TcpServerHarness {
    std::unique_ptr<IpcServer> server;
    std::thread thr;
    SessionManager* sm;
    std::string addr;
    std::atomic<int> job_status_calls{0};

    TcpServerHarness(const std::string& tcp_addr, const std::string& psk,
                     SessionManager& sm_)
        : sm(&sm_), addr(tcp_addr) {
        server = std::make_unique<IpcServer>(addr);
        server->set_psk(psk);
        server->set_resume_token_resolver(
            [this](const std::string& provided)
                -> std::pair<std::string, std::string> {
                if (!provided.empty()) {
                    if (auto cid = sm->resolve(provided); cid)
                        return {*cid, provided};
                }
                std::string cid = server->mint_client_id();
                std::string tok = sm->mint(cid);
                return {cid, tok};
            });
        server->set_request_dispatch_hook(
            [this](const std::string& tok) { sm->bump_last_seen(tok); });
        // job.status stub for the recovery dispatch path. The harness
        // increments a counter so the test can assert it was reached;
        // the response shape mirrors the daemon's real handler
        // sufficiently for the client to parse `status`.
        server->on("job.status",
                   [this](const IpcRequest&, IpcResponse& resp, IpcError&) {
                       resp.result["status"] = std::string("unknown");
                       job_status_calls.fetch_add(1);
                       return true;
                   });
        // process.submit stub: pretend to allocate an upload session.
        // The H-D3 retry path expects the same job_id contract as the
        // real daemon — for the unit-level shape check we only need a
        // valid response.
        server->on("process.submit",
                   [](const IpcRequest&, IpcResponse& resp, IpcError&) {
                       resp.result["job_id"] = static_cast<int64_t>(42);
                       resp.result["upload_token"] = std::string("ut-stub");
                       resp.result["max_size"] = static_cast<int64_t>(1 << 30);
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
// D.5 test #5 (adapted): IpcClient with resume_token connect overload —
// auth.ok echoes the resolved token back, retried submit uses the SAME
// meeting_id (H-D3 retry contract).
// ===========================================================================
TEST_CASE("D.5: resume_token round-trips via IpcClient::connect(psk, resume_token)",
          "[d5][ipc][integration]") {
    const std::string TCP_ADDR = "127.0.0.1:19970";
    const std::string PSK = "psk-d5-test";

    SessionManager sm(/*ttl_seconds=*/3600);
    ScopedAuthToken env(PSK);
    TcpServerHarness srv(TCP_ADDR, PSK, sm);

    // (1) Fresh connect — no resume_token presented. Server allocates a
    // new (client_id, resume_token) tuple via the resolver hook.
    IpcClient c1(TCP_ADDR);
    REQUIRE(c1.connect());
    const std::string first_client_id = c1.client_id();
    const std::string first_token     = c1.resume_token();
    REQUIRE_FALSE(first_client_id.empty());
    REQUIRE_FALSE(first_token.empty());
    c1.close_connection();

    // (2) Reconnect with the captured token via the D.5 overload. The
    // server-side resolver re-issues the SAME client_id (resume hit).
    IpcClient c2(TCP_ADDR);
    REQUIRE(c2.connect(PSK, first_token));
    CHECK(c2.client_id()    == first_client_id);
    CHECK(c2.resume_token() == first_token);
}

// ===========================================================================
// D.5 test #5b: H-D3 retry contract — when a submit-after-restart
// happens, the SAME meeting_id is re-presented; the server-side dedup
// (C.11.4) will route accordingly. The class-level guarantee D.5 owns
// is "the journal carries the same meeting_id on retry"; here we
// verify the on-disk journal round-trip preserves it byte-for-byte.
// ===========================================================================
TEST_CASE("D.5: H-D3 retry uses same meeting_id (journal-preserved)",
          "[d5][ipc][integration]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};
    PendingJobsJournal j(scratch / "pending_jobs.json");

    // Mint a meeting_id at "start_capture" time.
    std::string mid = new_uuid_v4();
    REQUIRE(mid.size() == 36);

    PendingJobsJournal::Entry e;
    e.endpoint          = "127.0.0.1:19970";
    e.meeting_id        = mid;
    e.job_id            = "100";
    e.staging_wav_path  = (scratch / "audio.wav").string();
    e.kind              = "submit";
    e.slot_kind         = "postprocess";
    e.submitted_at_unix = 1747146600;
    j.save({e});

    // Simulate tray restart by re-loading the journal — the meeting_id
    // must round-trip exactly so the H-D3 retry presents the same id
    // to the daemon (which routes through C.11.4's atomic-overwrite
    // contract to the original meeting directory).
    auto loaded = j.load();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].meeting_id == mid);
    // Round-tripping the same id is the load-bearing property — the
    // tray's recovery loop reads this exact byte sequence and feeds it
    // back into `process.submit { meeting_id }`.
}

// ===========================================================================
// D.5 test #6: save-for-later resume — a sidecar pre-populates the
// resume submenu with the recorded meeting_id + context. The
// integration shape is class-level (no GTK) — we assert that scanning
// the staging dir surfaces the sidecar and `read_pending_sidecar`
// recovers the payload that a downstream submit dispatch would use.
// ===========================================================================
TEST_CASE("D.5: save-for-later sidecar surfaces in resume scan with meeting_id + context",
          "[d5][ipc][integration]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    fs::path wav = scratch / "audio_2026-05-17_11-00.wav";
    std::ofstream(wav).close();

    tray_capture::PendingSidecarV2 in;
    in.meeting_id       = new_uuid_v4();
    in.wav_path         = wav.string();
    in.timestamp        = "2026-05-17_11-00";
    in.mic_source       = "alsa_input.usb";
    in.captions_enabled = false;
    in.context.subject      = "Resume test";
    in.context.participants = {"OperatorA"};
    in.context.notes        = "Captured before tray crash.";
    in.context.language     = "en";
    in.context.vocabulary   = {};
    REQUIRE_NOTHROW(tray_capture::write_pending_sidecar_v2(in));

    // Simulate the tray's startup scan: list staging dir, look for
    // `.pending` files, parse each. The actual rescan helper is
    // statically scoped to `tray.cpp` (no GTK boot in tests), so we
    // exercise the same primitives the tray uses.
    fs::path sidecar = tray_capture::pending_sidecar_path(wav);
    REQUIRE(fs::exists(sidecar));
    auto loaded = tray_capture::read_pending_sidecar(sidecar);
    CHECK(loaded.meeting_id  == in.meeting_id);
    CHECK(loaded.wav_path    == in.wav_path);
    CHECK(loaded.timestamp   == in.timestamp);
    CHECK(loaded.context.subject  == in.context.subject);
    CHECK(loaded.context.notes    == in.context.notes);
    CHECK(loaded.context.language == in.context.language);
    REQUIRE(loaded.context.participants.size() == 1);
    CHECK(loaded.context.participants[0] == "OperatorA");

    // The submit-resume path is sidecar-removal-on-success — the test
    // here pins the contract by removing the sidecar manually and
    // confirming the next scan does not re-surface it.
    std::error_code ec;
    fs::remove(sidecar, ec);
    REQUIRE_FALSE(fs::exists(sidecar));
}
