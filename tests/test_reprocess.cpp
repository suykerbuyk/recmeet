// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.12 — `process.reprocess` tests.
//
// process.reprocess re-runs the pipeline against a server-resident meeting,
// keyed by `meeting_id` (C.11). Per-stage flags (transcribe / diarize /
// identify / summarize) let the operator skip individual stages; vocabulary
// can be overridden per-job.
//
// Style mirrors test_job_query.cpp / test_fetch.cpp: the production handler
// in src/daemon.cpp depends on globals (g_jobs, g_meeting_index, g_config),
// so we re-implement the same body against a test-local IpcServer + JobQueue
// + MeetingIndex and pin the wire-shape contract here. Drift between this
// helper and daemon.cpp's handler will surface as a failing assertion.
//
// Thread hygiene (orchestrator rule 5): every std::thread is owned by a
// RAII guard so a REQUIRE between construction and join cannot trigger
// std::terminate(). Every IPC call is synchronous via IpcClient::call —
// no background reader thread is spawned on the test side.

#include <catch2/catch_test_macros.hpp>

#include "config.h"
#include "daemon_handlers_internal.h"  // g_jobs / g_meeting_index externs
#include "daemon_test_harness.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"
#include "meeting_index.h"
#include "pipeline.h"   // save_meeting_context
#include "test_tmpdir.h"
#include "util.h"       // find_audio_file, is_valid_meeting_id, derive_meeting_timestamp

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include <signal.h>
#include <unistd.h>

using namespace recmeet;
using recmeet::test::DaemonTestHarness;
namespace fs = std::filesystem;

namespace {

struct SigpipeIgnorer {
    SigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} s_ignore_sigpipe;

const std::string REPROCESS_SOCK =
    recmeet::test::tmp_path("recmeet_test_reprocess.sock").string();

struct ServerGuard {
    IpcServer& server;
    std::thread thr;
    explicit ServerGuard(IpcServer& s) : server(s) {
        thr = std::thread([this]() { server.run(); });
    }
    ~ServerGuard() {
        server.stop();
        if (thr.joinable()) thr.join();
    }
};

struct JqShutdownGuard {
    JobQueue& q;
    explicit JqShutdownGuard(JobQueue& q_) : q(q_) {}
    ~JqShutdownGuard() { q.shutdown(); }
};

fs::path tmp_root(const std::string& tag) {
    auto base = recmeet::test::tmp_path(
        "recmeet-c12-" + tag + "-" +
        std::to_string(::getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch().count()));
    fs::create_directories(base);
    return base;
}

// Materialize a meeting directory with the given id and a tiny placeholder
// audio file. The pipeline doesn't run in these unit tests — the handler
// only needs find_audio_file() to succeed (it returns the path) and the
// MeetingIndex lookup to hit.
fs::path make_meeting(const fs::path& root,
                      const std::string& dirname,
                      const std::string& meeting_id,
                      const std::string& timestamp = "2026-05-17_12-00") {
    fs::path dir = root / dirname;
    fs::create_directories(dir);
    save_meeting_context(dir, "ctx", {}, timestamp, meeting_id);
    // Timestamped audio filename matches the C.11.4 / v2 producer path.
    fs::path audio = dir / ("audio_" + timestamp + ".wav");
    std::ofstream out(audio, std::ios::binary);
    // 44-byte RIFF placeholder — find_audio_file only checks the filename;
    // no decoder runs in this unit-test scope.
    out.write("RIFF\0\0\0\0WAVEfmt \20\0\0\0\1\0\1\0", 28);
    out.write("\x80\x3e\0\0\0\x7d\0\0\2\0\20\0data\0\0\0\0", 16);
    return dir;
}

// Phase 2b: the test-local process.reprocess stub was retired in favor of
// `register_daemon_handlers()` via DaemonTestHarness. The production
// handler reads `g_jobs`, `g_meeting_index`, and `g_server_config` — the
// harness wires all three. Per-test setup uses harness.mutate_config
// when a test needs a non-default starting cfg.

// Phase 2b: ReprocessHarness wraps DaemonTestHarness so existing test
// bodies keep the q/idx/client locals.
struct ReprocessHarness {
    DaemonTestHarness harness;
    std::unique_ptr<IpcClient> client_owned;

    ReprocessHarness() {
        harness.start();
        client_owned = harness.make_client();
    }
    JobQueue&     q()      const { return *g_jobs; }
    MeetingIndex& idx()    const { return *g_meeting_index; }
    IpcClient&    client() const { return *client_owned; }
};


} // namespace

// ============================================================================
// 1. Validation chain — missing / malformed / unknown meeting_id all reject
//    at the wire boundary so a malformed id never reaches MeetingIndex or
//    JobQueue. Verifies the narrowest-first ordering from the production
//    handler's header comment.
// ============================================================================
TEST_CASE("C.12 process.reprocess: validation chain rejects bad meeting_id",
          "[c12][reprocess]") {
    ReprocessHarness h;
    auto& q = h.q();
    auto& client = h.client();

    // Missing meeting_id.
    {
        IpcResponse resp; IpcError err;
        JsonMap params;
        CHECK_FALSE(client.call("process.reprocess", params, resp, err, 2000));
        CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
        CHECK(err.message.find("missing 'meeting_id'") != std::string::npos);
    }

    // Malformed: wrong length.
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["meeting_id"] = std::string("not-a-uuid");
        CHECK_FALSE(client.call("process.reprocess", params, resp, err, 2000));
        CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
        CHECK(err.message.find("UUID v4") != std::string::npos);
    }

    // Malformed: uppercase hex (is_valid_meeting_id requires lowercase).
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["meeting_id"] =
            std::string("ABCDEF01-2345-4678-9ABC-DEF012345678");
        CHECK_FALSE(client.call("process.reprocess", params, resp, err, 2000));
        CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
        CHECK(err.message.find("UUID v4") != std::string::npos);
    }

    // Well-formed but unknown — not in the (empty) MeetingIndex.
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["meeting_id"] =
            std::string("12345678-1234-4567-89ab-1234567890ab");
        CHECK_FALSE(client.call("process.reprocess", params, resp, err, 2000));
        CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
        CHECK(err.message.find("unknown meeting_id") != std::string::npos);
    }

    // No job should have landed in the queue across any of those rejects.
    CHECK(q.queued_count(JobKind::Postprocess) == 0);
    CHECK_FALSE(q.slot_busy(JobKind::Postprocess));

}

// ============================================================================
// 2. Happy path — a known meeting_id resolves through the MeetingIndex, the
//    resulting Postprocess job carries the right out_dir, audio_path,
//    meeting_id binding, and client ownership.
// ============================================================================
TEST_CASE("C.12 process.reprocess: known meeting_id enqueues job pointing at "
          "the indexed dir",
          "[c12][reprocess]") {
    auto root = tmp_root("happy");
    const std::string id = "abcdef01-2345-4678-9abc-def012345678";
    fs::path mdir = make_meeting(root, "2026-05-17_12-00", id);
    fs::path expected_audio = mdir / "audio_2026-05-17_12-00.wav";
    REQUIRE(fs::exists(expected_audio));

    ReprocessHarness h;
    auto& q = h.q();
    auto& idx = h.idx();
    auto& client = h.client();
    idx.bind(id, mdir);
    REQUIRE(!client.client_id().empty());

    IpcResponse resp; IpcError err;
    JsonMap params; params["meeting_id"] = id;
    REQUIRE(client.call("process.reprocess", params, resp, err, 2000));

    int64_t job_id = json_val_as_int(resp.result["job_id"]);
    CHECK(job_id > 0);

    // Dequeue and inspect — pp_worker_loop would do this in production.
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    CHECK(dq->job_id == job_id);
    CHECK(dq->kind   == JobKind::Postprocess);
    CHECK(dq->state  == JobState::Running);
    CHECK(dq->client_id  == client.client_id());
    CHECK(dq->meeting_id == id);
    CHECK(dq->input.out_dir    == mdir);
    CHECK(dq->input.audio_path == expected_audio);
    CHECK(dq->input.timestamp  == "2026-05-17_12-00");

    // job.list / job.status (C.6 + C.11) can resolve this job's meeting_id.
    auto snap = q.status(job_id);
    REQUIRE(snap.has_value());
    CHECK(snap->meeting_id == id);

    fs::remove_all(root);
}

// ============================================================================
// 3. Per-stage flag overrides land on the dequeued job's JobConfig. Asserts the
//    inversion semantics for `summarize` (positive on the wire ↔ negative
//    cfg.no_summary) and the replace-not-merge semantics for vocabulary.
// ============================================================================
TEST_CASE("C.12 process.reprocess: per-stage flag overrides propagate to cfg",
          "[c12][reprocess]") {
    ::unlink(REPROCESS_SOCK.c_str());

    auto root = tmp_root("flags");
    const std::string id = "11111111-2222-4333-8444-555555555555";
    fs::path mdir = make_meeting(root, "2026-05-17_13-00", id,
                                 "2026-05-17_13-00");

    ReprocessHarness h;
    auto& q = h.q();
    auto& idx = h.idx();
    auto& client = h.client();
    idx.bind(id, mdir);

    // The production handler builds JobConfig from g_server_config + session
    // creds/prefs. Default ServerConfig has diarize=true; the JobConfig built
    // from it has speaker_id=true / no_summary=false / vocabulary empty. The
    // request below sends `diarize=false`, `identify=false`, `summarize=false`,
    // `vocabulary="contoso..."` — those overrides MUST land on the dequeued
    // job's cfg regardless of the snapshot starting state.

    IpcResponse resp; IpcError err;
    JsonMap params;
    params["meeting_id"]    = id;
    params["diarize"]       = false;
    params["identify"]      = false;
    params["summarize"]     = false;
    params["vocabulary"]    = std::string("contoso, MTLS, ARM64");
    params["transcribe"]    = true;          // accepted; ignored in v1
    params["summary_style"] = std::string("brief");  // reserved; ignored
    REQUIRE(client.call("process.reprocess", params, resp, err, 2000));

    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    CHECK(dq->cfg.diarize    == false);
    CHECK(dq->cfg.speaker_id == false);
    CHECK(dq->cfg.no_summary == true);
    CHECK(dq->cfg.vocabulary == "contoso, MTLS, ARM64");
    // reprocess_dir is cleared on the snapshot path — pp_worker_loop fills
    // it from input.out_dir at dequeue time, so the daemon-side value here
    // MUST be empty before the worker runs.
    CHECK(dq->cfg.reprocess_dir.empty());
    // meeting_id stamped from the request.
    CHECK(dq->meeting_id == id);

    fs::remove_all(root);
}

// ============================================================================
// 4. Indexed-but-vanished — the MeetingIndex is in-memory only; an operator
//    `rm -rf` of a meeting dir between startup rebuild and a reprocess
//    request would leave a stale binding. Reject with InternalError rather
//    than enqueue a job that will fail in the subprocess (and rather than
//    unbind() under-foot, which would race a concurrent process.submit
//    re-creating the same meeting_id).
// ============================================================================
TEST_CASE("C.12 process.reprocess: indexed dir vanished from disk → "
          "InternalError",
          "[c12][reprocess]") {
    ::unlink(REPROCESS_SOCK.c_str());

    auto root = tmp_root("vanished");
    const std::string id = "22222222-3333-4444-8555-666666666666";
    fs::path mdir = make_meeting(root, "2026-05-17_14-00", id,
                                 "2026-05-17_14-00");

    ReprocessHarness h;
    auto& q = h.q();
    auto& idx = h.idx();
    auto& client = h.client();
    idx.bind(id, mdir);

    // Operator (or test) unlinks the directory tree but leaves the binding
    // in place — the classic "stale index" case.
    fs::remove_all(mdir);
    REQUIRE_FALSE(fs::is_directory(mdir));

    IpcResponse resp; IpcError err;
    JsonMap params; params["meeting_id"] = id;
    CHECK_FALSE(client.call("process.reprocess", params, resp, err, 2000));
    CHECK(err.code == static_cast<int>(IpcErrorCode::InternalError));
    CHECK(err.message.find("no longer exists") != std::string::npos);

    // Binding survives (we did NOT unbind under-foot).
    auto hit = idx.find(id);
    REQUIRE(hit.has_value());
    CHECK(*hit == mdir);

    // No job enqueued.
    CHECK(q.queued_count(JobKind::Postprocess) == 0);

    fs::remove_all(root);
}
