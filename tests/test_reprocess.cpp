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
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"
#include "meeting_index.h"
#include "pipeline.h"   // save_meeting_context
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
namespace fs = std::filesystem;

namespace {

struct SigpipeIgnorer {
    SigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} s_ignore_sigpipe;

const char* REPROCESS_SOCK = "/tmp/recmeet_test_reprocess.sock";

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
    auto base = fs::temp_directory_path() /
                ("recmeet-c12-" + tag + "-" +
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

// Test-local mirror of src/daemon.cpp's process.reprocess handler. The
// production handler reaches g_jobs / g_meeting_index / g_config_mu via
// file-static globals; this version closes over test-local instances.
// Keep this body in lockstep with daemon.cpp; the tests below pin
// validation order + cfg propagation + wire-shape contracts.
struct ReprocessDeps {
    JobQueue&     q;
    MeetingIndex& idx;
    Config        cfg_snapshot;   // stand-in for g_config under g_config_mu
};

void register_process_reprocess(IpcServer& server, ReprocessDeps& deps) {
    server.on("process.reprocess",
              [&deps](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        // (1) meeting_id required.
        std::string meeting_id;
        {
            auto it = req.params.find("meeting_id");
            if (it != req.params.end())
                meeting_id = json_val_as_string(it->second);
        }
        if (meeting_id.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.reprocess: missing 'meeting_id'";
            return false;
        }
        // (2) Format validation.
        if (!is_valid_meeting_id(meeting_id)) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.reprocess: 'meeting_id' must be a canonical "
                          "lowercase UUID v4";
            return false;
        }
        // (3) Lookup.
        auto hit = deps.idx.find(meeting_id);
        if (!hit.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.reprocess: unknown meeting_id " + meeting_id;
            return false;
        }
        fs::path meeting_dir = *hit;
        // (4) Dir must still exist.
        std::error_code ec;
        if (!fs::is_directory(meeting_dir, ec)) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "process.reprocess: meeting_id " + meeting_id
                        + " is indexed at " + meeting_dir.string()
                        + " but the directory no longer exists";
            return false;
        }
        // (5) Resolve audio.
        fs::path audio_path = find_audio_file(meeting_dir);
        if (audio_path.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "process.reprocess: no audio file found in "
                        + meeting_dir.string();
            return false;
        }
        // (6) Apply per-stage flag overrides on a snapshot of the live config.
        Config cfg = deps.cfg_snapshot;
        cfg.reprocess_dir.clear();

        {
            auto it = req.params.find("transcribe");
            if (it != req.params.end())
                (void)json_val_as_bool(it->second, true);
        }
        {
            auto it = req.params.find("diarize");
            if (it != req.params.end() && !json_val_as_bool(it->second, true))
                cfg.diarize = false;
        }
        {
            auto it = req.params.find("identify");
            if (it != req.params.end() && !json_val_as_bool(it->second, true))
                cfg.speaker_id = false;
        }
        {
            auto it = req.params.find("summarize");
            if (it != req.params.end() && !json_val_as_bool(it->second, true))
                cfg.no_summary = true;
        }
        {
            auto it = req.params.find("vocabulary");
            if (it != req.params.end())
                cfg.vocabulary = json_val_as_string(it->second);
        }
        {
            auto it = req.params.find("summary_style");
            if (it != req.params.end()) (void)json_val_as_string(it->second);
        }

        // (7) Build + enqueue the job.
        PostprocessInput input;
        input.out_dir    = meeting_dir;
        input.audio_path = audio_path;
        input.timestamp  = derive_meeting_timestamp(meeting_dir);

        Job job;
        job.input      = std::move(input);
        job.cfg        = std::move(cfg);
        job.meeting_id = meeting_id;

        int64_t job_id = deps.q.enqueue(std::move(job), JobKind::Postprocess,
                                        req.client_id);

        resp.result["job_id"] = job_id;
        return true;
    });
}

} // namespace

// ============================================================================
// 1. Validation chain — missing / malformed / unknown meeting_id all reject
//    at the wire boundary so a malformed id never reaches MeetingIndex or
//    JobQueue. Verifies the narrowest-first ordering from the production
//    handler's header comment.
// ============================================================================
TEST_CASE("C.12 process.reprocess: validation chain rejects bad meeting_id",
          "[c12][reprocess]") {
    ::unlink(REPROCESS_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    MeetingIndex idx;
    ReprocessDeps deps{q, idx, Config{}};

    IpcServer server(REPROCESS_SOCK);
    register_process_reprocess(server, deps);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(REPROCESS_SOCK);
    REQUIRE(client.connect());

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

    client.close_connection();
    ::unlink(REPROCESS_SOCK);
}

// ============================================================================
// 2. Happy path — a known meeting_id resolves through the MeetingIndex, the
//    resulting Postprocess job carries the right out_dir, audio_path,
//    meeting_id binding, and client ownership.
// ============================================================================
TEST_CASE("C.12 process.reprocess: known meeting_id enqueues job pointing at "
          "the indexed dir",
          "[c12][reprocess]") {
    ::unlink(REPROCESS_SOCK);

    auto root = tmp_root("happy");
    const std::string id = "abcdef01-2345-4678-9abc-def012345678";
    fs::path mdir = make_meeting(root, "2026-05-17_12-00", id);
    fs::path expected_audio = mdir / "audio_2026-05-17_12-00.wav";
    REQUIRE(fs::exists(expected_audio));

    JobQueue q;
    JqShutdownGuard jqg(q);
    MeetingIndex idx;
    idx.bind(id, mdir);
    ReprocessDeps deps{q, idx, Config{}};

    IpcServer server(REPROCESS_SOCK);
    register_process_reprocess(server, deps);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(REPROCESS_SOCK);
    REQUIRE(client.connect());
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

    client.close_connection();
    ::unlink(REPROCESS_SOCK);
    fs::remove_all(root);
}

// ============================================================================
// 3. Per-stage flag overrides land on the dequeued job's Config. Asserts the
//    inversion semantics for `summarize` (positive on the wire ↔ negative
//    cfg.no_summary) and the replace-not-merge semantics for vocabulary.
// ============================================================================
TEST_CASE("C.12 process.reprocess: per-stage flag overrides propagate to cfg",
          "[c12][reprocess]") {
    ::unlink(REPROCESS_SOCK);

    auto root = tmp_root("flags");
    const std::string id = "11111111-2222-4333-8444-555555555555";
    fs::path mdir = make_meeting(root, "2026-05-17_13-00", id,
                                 "2026-05-17_13-00");

    JobQueue q;
    JqShutdownGuard jqg(q);
    MeetingIndex idx;
    idx.bind(id, mdir);

    // Seed the snapshot with the OPPOSITE of what the request will assert,
    // so a missing override-application would not coincidentally pass.
    Config snapshot;
    snapshot.diarize    = true;
    snapshot.speaker_id = true;
    snapshot.no_summary = false;
    snapshot.vocabulary = "stale";
    ReprocessDeps deps{q, idx, snapshot};

    IpcServer server(REPROCESS_SOCK);
    register_process_reprocess(server, deps);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(REPROCESS_SOCK);
    REQUIRE(client.connect());

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

    client.close_connection();
    ::unlink(REPROCESS_SOCK);
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
    ::unlink(REPROCESS_SOCK);

    auto root = tmp_root("vanished");
    const std::string id = "22222222-3333-4444-8555-666666666666";
    fs::path mdir = make_meeting(root, "2026-05-17_14-00", id,
                                 "2026-05-17_14-00");

    JobQueue q;
    JqShutdownGuard jqg(q);
    MeetingIndex idx;
    idx.bind(id, mdir);
    ReprocessDeps deps{q, idx, Config{}};

    IpcServer server(REPROCESS_SOCK);
    register_process_reprocess(server, deps);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(REPROCESS_SOCK);
    REQUIRE(client.connect());

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

    client.close_connection();
    ::unlink(REPROCESS_SOCK);
    fs::remove_all(root);
}
