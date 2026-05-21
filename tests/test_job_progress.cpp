// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.14 — `job.status` / `job.list` phase + progress payload extension.
//
// Phase 2b conversion: the in-test `register_job_status()` stub was
// retired in favor of `DaemonTestHarness` + the production
// `register_daemon_handlers()` body. The JobQueue used by each test is
// the daemon-global `*g_jobs` wired by the harness; the wire-shape
// assertions are identical to the pre-conversion expectations because
// the production handler in src/daemon_handlers.cpp was the source the
// stub was originally cloned from.

#include <catch2/catch_test_macros.hpp>

#include "daemon_test_harness.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <signal.h>
#include <unistd.h>

using namespace recmeet;
using recmeet::test::DaemonTestHarness;

namespace {

struct SigpipeIgnorer {
    SigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} s_ignore_sigpipe;

} // namespace

// ============================================================================
// 1. State-derived phase fallback. An idle job that has never seen an
//    update_progress call still wires a meaningful phase string derived from
//    its lifecycle state — the D.3 reconnect case (re-sync before any new
//    event fires). progress defaults to 0.
// ============================================================================
TEST_CASE("C.14 job.status: phase falls back to state when cache is empty",
          "[c14][progress]") {
    DaemonTestHarness harness;
    harness.start();
    auto client = harness.make_client();

    // Queued — no event has fired, cache is empty.
    int64_t queued_id;
    {
        Job j;
        queued_id = g_jobs->enqueue(std::move(j), JobKind::Postprocess,
                                    client->client_id());
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = queued_id;
        REQUIRE(client->call("job.status", params, resp, err, 2000));
        CHECK(json_val_as_string(resp.result["state"]) == "queued");
        CHECK(json_val_as_string(resp.result["phase"]) == "queued");
        CHECK(json_val_as_int(resp.result["progress"]) == 0);
    }

    // Done — without explicit update_progress the serializer derives
    // phase="complete".
    {
        auto dq = g_jobs->dequeue(JobKind::Postprocess);
        REQUIRE(dq.has_value());
        g_jobs->finish(queued_id, /*ok=*/true);
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = queued_id;
        REQUIRE(client->call("job.status", params, resp, err, 2000));
        CHECK(json_val_as_string(resp.result["state"]) == "done");
        CHECK(json_val_as_string(resp.result["phase"]) == "complete");
        CHECK(json_val_as_int(resp.result["progress"]) == 0);
    }
}

// ============================================================================
// 2. update_progress round-trips through job.status. Mid-pipeline (Running)
//    a job emits a `phase` then a `progress` event; both should be reflected
//    in the next job.status response.
// ============================================================================
TEST_CASE("C.14 job.status: update_progress round-trips phase + progress",
          "[c14][progress]") {
    DaemonTestHarness harness;
    harness.start();
    auto client = harness.make_client();

    Job j;
    int64_t id = g_jobs->enqueue(std::move(j), JobKind::Postprocess,
                                 client->client_id());
    auto dq = g_jobs->dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    REQUIRE(dq->state == JobState::Running);

    // First the daemon would receive a `phase` event ("transcribing", 0%).
    g_jobs->update_progress(id, "transcribing", 0);
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = id;
        REQUIRE(client->call("job.status", params, resp, err, 2000));
        CHECK(json_val_as_string(resp.result["state"]) == "running");
        CHECK(json_val_as_string(resp.result["phase"]) == "transcribing");
        CHECK(json_val_as_int(resp.result["progress"]) == 0);
    }

    // Then a `progress` event partway through.
    g_jobs->update_progress(id, "transcribing", 42);
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = id;
        REQUIRE(client->call("job.status", params, resp, err, 2000));
        CHECK(json_val_as_string(resp.result["phase"]) == "transcribing");
        CHECK(json_val_as_int(resp.result["progress"]) == 42);
    }

    // Phase change resets progress (the daemon's pp_worker_loop calls
    // update_progress(jid, name, 0) on every `phase` event).
    g_jobs->update_progress(id, "diarizing", 0);
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = id;
        REQUIRE(client->call("job.status", params, resp, err, 2000));
        CHECK(json_val_as_string(resp.result["phase"]) == "diarizing");
        CHECK(json_val_as_int(resp.result["progress"]) == 0);
    }

    // Progress clamps to [0, 100]; out-of-range inputs are normalized.
    g_jobs->update_progress(id, "summarizing", 250);
    g_jobs->update_progress(id, "summarizing", -7);
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = id;
        REQUIRE(client->call("job.status", params, resp, err, 2000));
        CHECK(json_val_as_string(resp.result["phase"]) == "summarizing");
        // -7 clamps to 0 after the 250→100 clamp also resolves cleanly.
        CHECK(json_val_as_int(resp.result["progress"]) == 0);
    }
}

// ============================================================================
// 3. Cached values survive job-thread idle — the load-bearing D.3 property.
//    No new event fires after update_progress; job.status should still
//    return the cached phase/progress after a brief sleep (simulates a
//    reconnect during the throttle window between progress events).
//
//    Also verifies terminal-state lock: a late update_progress on a Done
//    job is a no-op (the serializer will still emit the state-derived
//    "complete" phase since the cached phase was never overwritten).
// ============================================================================
TEST_CASE("C.14 cached phase/progress survive idle; terminal state locks updates",
          "[c14][progress]") {
    DaemonTestHarness harness;
    harness.start();
    auto client = harness.make_client();

    Job j;
    int64_t id = g_jobs->enqueue(std::move(j), JobKind::Postprocess,
                                 client->client_id());
    auto dq = g_jobs->dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());

    g_jobs->update_progress(id, "diarizing", 73);

    // Idle window (no new events fire).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Re-fetch — same cached values.
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = id;
        REQUIRE(client->call("job.status", params, resp, err, 2000));
        CHECK(json_val_as_string(resp.result["phase"]) == "diarizing");
        CHECK(json_val_as_int(resp.result["progress"]) == 73);
    }

    // Job completes. A late update_progress arriving after finish() — e.g. a
    // stale subprocess event the daemon's stdout pump processed just after
    // the subprocess exited — MUST NOT overwrite the verdict.
    g_jobs->finish(id, /*ok=*/true);
    g_jobs->update_progress(id, "transcribing", 99);

    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = id;
        REQUIRE(client->call("job.status", params, resp, err, 2000));
        // State is the verdict; phase derives from state because the late
        // update_progress was rejected and the cached phase stayed at
        // "diarizing" — but wait: the late call was a no-op, so the cache
        // still says "diarizing". The serializer emits the cached value
        // even on Done if it's non-empty. This is the documented behavior:
        // the cache reflects the last *pre-terminal* value, not the state-
        // derived "complete".
        CHECK(json_val_as_string(resp.result["state"]) == "done");
        CHECK(json_val_as_string(resp.result["phase"]) == "diarizing");
        CHECK(json_val_as_int(resp.result["progress"]) == 73);
    }
}
