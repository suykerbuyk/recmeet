// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.6 — `job.status` / `job.list` (read-only registry queries) tests.
//
// `job.status` returns a single-job snapshot for re-sync (D.3), `job.list`
// returns every job owned by the caller (D.5). Both go straight through the
// existing `JobQueue::status` / `JobQueue::list_by_client` /
// `JobQueue::client_for_job` API; this file pins the WIRE-LEVEL shape (field
// set, ordering, secret-field absence, terminal-job inclusion, ownership
// reject) so a regression in the daemon's response builder is caught by a
// failing assertion rather than discovered on a live reconnect.
//
// Style mirrors test_fetch.cpp:
//   * The IPC handler body in src/daemon.cpp depends on the global `g_jobs`.
//     We do NOT exercise that handler directly — we re-implement the same
//     body against a test-local JobQueue and register it on a test-local
//     IpcServer. The handler shape MUST stay in sync with daemon.cpp's
//     job.status / job.list. Any drift is a Phase-D re-sync bug.
//   * Thread hygiene (orchestrator rule 5): every std::thread spawned here
//     is owned by a RAII guard (`ServerGuard`, `JqShutdownGuard`) that
//     joins on destruction so a REQUIRE between thread construction and
//     `.join()` cannot trigger std::terminate(). These tests do NOT spawn
//     a per-client reader thread — every IPC operation is a synchronous
//     `client.call()` request/response. `IpcClient::call` does its own
//     read pump inline; adding a background reader on the same fd races
//     with it and was observed to hang test 2 (a second consumer reads
//     the response frame before `call()` sees it).

#include <catch2/catch_test_macros.hpp>

#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"
#include "json_util.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <unistd.h>

using namespace recmeet;
namespace fs = std::filesystem;

namespace {

// Ignore SIGPIPE so a connection-drop mid-handler doesn't kill the test.
struct SigpipeIgnorer {
    SigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} s_ignore_sigpipe;

const char* QUERY_SOCK = "/tmp/recmeet_test_job_query.sock";

// ---------------------------------------------------------------------------
// RAII thread guards — identical idiom to test_fetch.cpp / test_routed_events.
// ---------------------------------------------------------------------------

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

// JqShutdownGuard — these tests stage jobs synchronously (enqueue → dequeue
// → finish in-test) rather than rely on a background drain worker; we still
// shutdown the queue on scope exit so any future dequeue (none today) wakes
// with std::nullopt rather than hanging the suite.
struct JqShutdownGuard {
    JobQueue& q;
    explicit JqShutdownGuard(JobQueue& q_) : q(q_) {}
    ~JqShutdownGuard() { q.shutdown(); }
};

// ---------------------------------------------------------------------------
// register_job_query_handlers — mirrors src/daemon.cpp's `job.status` and
// `job.list` registration against a test-local JobQueue+IpcServer (since the
// production handlers consume the global `g_jobs`). Keep in lockstep with
// daemon.cpp; the test asserts both shapes and field sets — a daemon-side
// divergence will surface as a failing test rather than a silent re-sync
// regression downstream (D.3 / D.5).
//
// The shared per-job serializer is a stand-alone lambda inside this helper
// (same code shape as the daemon's `serialize_job_object`) so the array-
// element path (`job.list`) and the flat-result path (`job.status`) emit
// the exact same field set in the exact same order.
// ---------------------------------------------------------------------------
void register_job_query_handlers(IpcServer& server, JobQueue& q) {
    server.on("job.status",
              [&q](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        auto jit = req.params.find("job_id");
        if (jit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "job.status: missing 'job_id'";
            return false;
        }
        const int64_t job_id = json_val_as_int(jit->second, 0);
        if (job_id <= 0) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "job.status: 'job_id' must be a positive integer";
            return false;
        }
        auto snap = q.status(job_id);
        if (!snap.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "job.status: unknown job_id "
                        + std::to_string(job_id);
            return false;
        }
        auto owner = q.client_for_job(job_id);
        if (!owner.has_value() || *owner != req.client_id) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "job.status: job_id "
                        + std::to_string(job_id)
                        + " is not owned by this client";
            return false;
        }
        resp.result["job_id"]     = static_cast<int64_t>(snap->job_id);
        resp.result["kind"]       = std::string(job_kind_name(snap->kind));
        resp.result["state"]      = std::string(job_state_name(snap->state));
        resp.result["client_id"]  = snap->client_id;
        resp.result["model_id"]   = snap->model_id;
        resp.result["error"]      = snap->error;
        resp.result["meeting_id"] = snap->meeting_id;   // C.11
        // C.14 — mirror daemon.cpp's job.status handler. Empty cached phase
        // derives from state; progress emitted unconditionally as int.
        resp.result["phase"] = snap->phase.empty()
            ? std::string(default_phase_for_state(snap->state))
            : snap->phase;
        resp.result["progress"] = static_cast<int64_t>(snap->progress);
        return true;
    });

    server.on("job.list",
              [&q](const IpcRequest& req, IpcResponse& resp, IpcError& /*err*/) {
        std::vector<Job> jobs = q.list_by_client(req.client_id);
        std::string arr = "[";
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (i > 0) arr += ",";
            arr += "{\"job_id\":";
            arr += std::to_string(jobs[i].job_id);
            arr += ",\"kind\":\"";
            arr += job_kind_name(jobs[i].kind);
            arr += "\",\"state\":\"";
            arr += job_state_name(jobs[i].state);
            arr += "\",\"client_id\":\"";
            arr += json_escape(jobs[i].client_id);
            arr += "\",\"model_id\":\"";
            arr += json_escape(jobs[i].model_id);
            arr += "\",\"error\":\"";
            arr += json_escape(jobs[i].error);
            arr += "\",\"meeting_id\":\"";              // C.11
            arr += json_escape(jobs[i].meeting_id);
            // C.14 — mirror daemon.cpp's serialize_job_object. Empty cached
            // phase derives from state; progress always serialized as int.
            arr += "\",\"phase\":\"";
            arr += json_escape(jobs[i].phase.empty()
                                   ? std::string(default_phase_for_state(jobs[i].state))
                                   : jobs[i].phase);
            arr += "\",\"progress\":";
            arr += std::to_string(jobs[i].progress);
            arr += "}";
        }
        arr += "]";
        resp.result["jobs"]  = arr;
        resp.result["count"] = static_cast<int64_t>(jobs.size());
        return true;
    });
}

// ---------------------------------------------------------------------------
// Helpers — small wrappers that drive JobQueue through the synchronous
// staging transitions Queued / WaitingForUpload / Running / Done / Failed /
// Cancelled, matching the daemon's lifecycle. Tests invoke these directly so
// the registry state is deterministic at the moment we issue the IPC call.
// ---------------------------------------------------------------------------

int64_t enqueue_postprocess_queued(JobQueue& q, const std::string& cid) {
    Job j;
    return q.enqueue(std::move(j), JobKind::Postprocess, cid);
}

int64_t enqueue_streaming_queued(JobQueue& q, const std::string& cid) {
    Job j;
    return q.enqueue(std::move(j), JobKind::Streaming, cid);
}

int64_t enqueue_model_download_queued(JobQueue& q, const std::string& cid,
                                      const std::string& model_id) {
    Job j;
    j.model_id = model_id;
    return q.enqueue(std::move(j), JobKind::ModelDownload, cid);
}

int64_t stage_running_pp(JobQueue& q, const std::string& cid) {
    int64_t id = enqueue_postprocess_queued(q, cid);
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    REQUIRE(dq->job_id == id);
    return id;
}

int64_t stage_done_pp(JobQueue& q, const std::string& cid) {
    int64_t id = stage_running_pp(q, cid);
    q.finish(id, /*ok=*/true);
    return id;
}

int64_t stage_failed_pp(JobQueue& q, const std::string& cid,
                        const std::string& err_msg) {
    int64_t id = stage_running_pp(q, cid);
    q.finish(id, /*ok=*/false, err_msg);
    return id;
}

int64_t stage_cancelled_queued_pp(JobQueue& q, const std::string& cid) {
    int64_t id = enqueue_postprocess_queued(q, cid);
    bool cancelled = q.cancel(id);
    REQUIRE(cancelled);
    return id;
}

// Find the literal `"key":"value"` (or `"key":VALUE`) substring boundary so
// the test can pull a single field out of the raw `jobs[]` substring without
// pulling in a JSON dependency. Returns the value as a string (the bytes
// between the value's open and close marker). The parsing is intentionally
// dumb — we own both encoder (handler above) and decoder (this helper) and
// don't need a real JSON parser to verify wire-shape contracts.
std::string extract_field(const std::string& obj, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    if (p >= obj.size()) return "";
    if (obj[p] == '"') {
        size_t e = obj.find('"', p + 1);
        if (e == std::string::npos) return "";
        return obj.substr(p + 1, e - (p + 1));
    }
    // Numeric / bool / null — read until `,` or `}` or `]`.
    size_t e = p;
    while (e < obj.size() && obj[e] != ',' && obj[e] != '}' && obj[e] != ']')
        ++e;
    return obj.substr(p, e - p);
}

// Split a top-level `[ {...}, {...}, ... ]` raw substring into per-element
// substrings. Same depth-aware idea as parse_json_object's nested branch.
std::vector<std::string> split_top_array(const std::string& arr) {
    std::vector<std::string> out;
    if (arr.size() < 2 || arr.front() != '[' || arr.back() != ']') return out;
    size_t p = 1;
    while (p < arr.size() - 1) {
        // Skip whitespace / separators.
        while (p < arr.size() - 1 &&
               (arr[p] == ' ' || arr[p] == ',' || arr[p] == '\n' ||
                arr[p] == '\t')) ++p;
        if (p >= arr.size() - 1) break;
        if (arr[p] != '{') break;  // malformed
        size_t start = p;
        int depth = 1;
        bool in_str = false;
        ++p;
        while (p < arr.size() && depth > 0) {
            if (in_str) {
                if (arr[p] == '\\') ++p;
                else if (arr[p] == '"') in_str = false;
            } else {
                if (arr[p] == '"') in_str = true;
                else if (arr[p] == '{') ++depth;
                else if (arr[p] == '}') --depth;
            }
            ++p;
        }
        out.push_back(arr.substr(start, p - start));
    }
    return out;
}

} // anonymous namespace

// ===========================================================================
// 1. job.status happy path — each JobKind in turn. The kind field is the
//    primary differentiator the tray uses to decide how to render a job in
//    the restart UI (D.5); we pin the on-wire kind string per JobKind.
// ===========================================================================
TEST_CASE("C.6 job.status: happy path for each JobKind",
          "[c6][query]") {
    ::unlink(QUERY_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(QUERY_SOCK);
    register_job_query_handlers(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(QUERY_SOCK);
    REQUIRE(client.connect());
    REQUIRE(!client.client_id().empty());

    int64_t pp  = enqueue_postprocess_queued(q, client.client_id());
    int64_t st  = enqueue_streaming_queued(q, client.client_id());
    int64_t md  = enqueue_model_download_queued(q, client.client_id(),
                                                "whisper/base.en");

    // Postprocess.
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = pp;
        REQUIRE(client.call("job.status", params, resp, err, 2000));
        CHECK(json_val_as_int(resp.result["job_id"]) == pp);
        CHECK(json_val_as_string(resp.result["kind"]) == "postprocess");
        CHECK(json_val_as_string(resp.result["state"]) == "queued");
        CHECK(json_val_as_string(resp.result["client_id"]) == client.client_id());
        CHECK(json_val_as_string(resp.result["model_id"]).empty());
        CHECK(json_val_as_string(resp.result["error"]).empty());
    }

    // Streaming.
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = st;
        REQUIRE(client.call("job.status", params, resp, err, 2000));
        CHECK(json_val_as_string(resp.result["kind"]) == "streaming");
        CHECK(json_val_as_string(resp.result["state"]) == "queued");
    }

    // ModelDownload — model_id field MUST round-trip.
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = md;
        REQUIRE(client.call("job.status", params, resp, err, 2000));
        CHECK(json_val_as_string(resp.result["kind"]) == "model_download");
        CHECK(json_val_as_string(resp.result["state"]) == "queued");
        CHECK(json_val_as_string(resp.result["model_id"]) == "whisper/base.en");
    }

    client.close_connection();
    ::unlink(QUERY_SOCK);
}

// ===========================================================================
// 2. job.status state coverage — Queued / WaitingForUpload / Running / Done /
//    Failed / Cancelled all surface on the wire correctly. The `error` field
//    is populated only for Failed; everything else has it empty.
// ===========================================================================
TEST_CASE("C.6 job.status: per-state wire shape (incl. error only for failed)",
          "[c6][query]") {
    ::unlink(QUERY_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(QUERY_SOCK);
    register_job_query_handlers(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(QUERY_SOCK);
    REQUIRE(client.connect());

    // Stage each terminal state in turn, COMPLETING the postprocess slot
    // between transitions. The slot is capacity-1 and `dequeue` pulls
    // FIFO-front — interleaving enqueue/dequeue without releasing the
    // running marker would have the next dequeue block waiting for the
    // current running job to finish. So the cadence is:
    //   enqueue → dequeue (Running) → finish (Done/Failed) → next enqueue.
    //
    // Cancelled-while-queued does NOT touch the running marker — we
    // enqueue, then cancel before any dequeue (which removes the job from
    // the FIFO eagerly via the Cancelled-state guard in pick_runnable). So
    // a Cancelled-Queued job can sit alongside a separate Queued job, but
    // we still stage them in dequeue-safe order: Cancelled FIRST so the
    // next stage_running_pp doesn't accidentally dequeue the cancelled
    // one — actually, `cancel(id)` only marks Cancelled; the FIFO removal
    // is lazy at pick_runnable. To avoid that interleaving entirely we
    // stage Cancelled LAST, after every state that needs `dequeue`.

    // Done — enqueue → dequeue → finish(ok=true).
    int64_t done_id = stage_done_pp(q, client.client_id());

    // Failed — enqueue → dequeue → finish(ok=false, msg).
    int64_t failed_id = stage_failed_pp(q, client.client_id(),
                                        "transcription crashed");

    // Running — enqueue → dequeue (NO finish — leaves the slot busy).
    int64_t running_id = stage_running_pp(q, client.client_id());

    // Queued — enqueue only. The pp slot is busy (running_id), so this
    // sits in the FIFO as Queued. No dequeue, no finish.
    int64_t queued_id = enqueue_postprocess_queued(q, client.client_id());

    // WaitingForUpload — reserve_job_id mints the entry without touching
    // the FIFO. Independent of the pp slot.
    int64_t reserved_id = q.reserve_job_id(JobKind::Postprocess,
                                           client.client_id());

    // Cancelled — enqueue then cancel. The cancel() call marks Cancelled
    // immediately (FIFO removal is lazy at pick_runnable; the job state
    // is the wire-observable surface, not the FIFO membership). The
    // pp_slot stays busy with running_id, so no dequeue races.
    int64_t cancelled_id = stage_cancelled_queued_pp(q, client.client_id());

    auto query = [&](int64_t jid, const std::string& want_state,
                     const std::string& want_error) {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = jid;
        REQUIRE(client.call("job.status", params, resp, err, 2000));
        INFO("state for job_id=" << jid
             << " got=" << json_val_as_string(resp.result["state"])
             << " want=" << want_state);
        CHECK(json_val_as_string(resp.result["state"]) == want_state);
        CHECK(json_val_as_string(resp.result["error"]) == want_error);
    };

    query(done_id,      "done",               "");
    query(failed_id,    "failed",             "transcription crashed");
    query(running_id,   "running",            "");
    query(queued_id,    "queued",             "");
    query(reserved_id,  "waiting_for_upload", "");
    query(cancelled_id, "cancelled",          "");

    // Free the slot so JqShutdownGuard tears down cleanly.
    q.finish(running_id, /*ok=*/true);

    client.close_connection();
    ::unlink(QUERY_SOCK);
}

// ===========================================================================
// 3. job.status unknown job_id → InvalidParams. Missing/non-positive job_id
//    is also rejected with InvalidParams. Distinct from PermissionDenied so
//    the client surfaces "no such job" rather than "you don't own that job"
//    (the latter would leak existence of foreign job_ids).
// ===========================================================================
TEST_CASE("C.6 job.status: unknown / missing job_id → InvalidParams",
          "[c6][query]") {
    ::unlink(QUERY_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(QUERY_SOCK);
    register_job_query_handlers(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(QUERY_SOCK);
    REQUIRE(client.connect());

    // Unknown job_id.
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = static_cast<int64_t>(99999);
        CHECK_FALSE(client.call("job.status", params, resp, err, 2000));
        CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
        CHECK(err.message.find("unknown") != std::string::npos);
        CHECK(err.message.find("99999") != std::string::npos);
    }

    // Missing job_id.
    {
        IpcResponse resp; IpcError err;
        JsonMap params;
        CHECK_FALSE(client.call("job.status", params, resp, err, 2000));
        CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
        CHECK(err.message.find("missing") != std::string::npos);
    }

    // Zero job_id.
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = static_cast<int64_t>(0);
        CHECK_FALSE(client.call("job.status", params, resp, err, 2000));
        CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
        CHECK(err.message.find("positive") != std::string::npos);
    }

    // Negative job_id.
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = static_cast<int64_t>(-7);
        CHECK_FALSE(client.call("job.status", params, resp, err, 2000));
        CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
        CHECK(err.message.find("positive") != std::string::npos);
    }

    client.close_connection();
    ::unlink(QUERY_SOCK);
}

// ===========================================================================
// 4. job.status PermissionDenied — client B tries to query A's job. Mirrors
//    process.fetch / process.cancel ownership posture; D.3 reconnect re-sync
//    must NOT leak another tab's job state.
// ===========================================================================
TEST_CASE("C.6 job.status: foreign client's job → PermissionDenied",
          "[c6][query]") {
    ::unlink(QUERY_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(QUERY_SOCK);
    register_job_query_handlers(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient a(QUERY_SOCK), b(QUERY_SOCK);
    REQUIRE(a.connect());
    REQUIRE(b.connect());
    REQUIRE(a.client_id() != b.client_id());

    int64_t a_jid = enqueue_postprocess_queued(q, a.client_id());

    // B queries A's job — must be rejected.
    IpcResponse resp; IpcError err;
    JsonMap params; params["job_id"] = a_jid;
    CHECK_FALSE(b.call("job.status", params, resp, err, 2000));
    CHECK(err.code == static_cast<int>(IpcErrorCode::PermissionDenied));
    CHECK(err.message.find("not owned") != std::string::npos);

    // A can still see its own job.
    IpcResponse resp_a; IpcError err_a;
    CHECK(a.call("job.status", params, resp_a, err_a, 2000));
    CHECK(json_val_as_int(resp_a.result["job_id"]) == a_jid);

    a.close_connection();
    b.close_connection();
    ::unlink(QUERY_SOCK);
}

// ===========================================================================
// 5. job.status / job.list — `input` and `cfg` are NEVER on the wire. The
//    Job struct carries PostprocessInput (paths, transcript text) and
//    Config (api_keys, output_dir) — neither belongs to the wire surface.
//    A regression here would be a credentials-leak; the positive assertion
//    is that those keys are ABSENT (not "empty but present").
//
// This is the deliverable-level test the spec explicitly calls out:
// "Do NOT assert on input/cfg fields — confirm they are NOT in the
//  response (positive assertion: those keys are absent)."
// ===========================================================================
TEST_CASE("C.6 job.status / job.list: input/cfg keys are absent from response",
          "[c6][query]") {
    ::unlink(QUERY_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(QUERY_SOCK);
    register_job_query_handlers(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(QUERY_SOCK);
    REQUIRE(client.connect());

    // Enqueue a job WITH input + cfg fields filled in (the secrets we are
    // verifying do NOT leak). The handler must not emit any of these keys.
    Job j;
    j.input.out_dir = "/secret/output_dir";
    j.input.transcript_text = "secret-transcript-content";
    j.cfg.api_keys["openai"] = "sk-SECRET-API-KEY";
    int64_t jid = q.enqueue(std::move(j), JobKind::Postprocess,
                            client.client_id());

    // (a) job.status — assert by-key absence on the parsed result map.
    {
        IpcResponse resp; IpcError err;
        JsonMap params; params["job_id"] = jid;
        REQUIRE(client.call("job.status", params, resp, err, 2000));

        // Sanity — the expected keys ARE present.
        CHECK(resp.result.count("job_id") == 1);
        CHECK(resp.result.count("kind") == 1);
        CHECK(resp.result.count("state") == 1);
        CHECK(resp.result.count("client_id") == 1);
        CHECK(resp.result.count("model_id") == 1);
        CHECK(resp.result.count("error") == 1);

        // POSITIVE absence assertions — load-bearing for credentials safety.
        CHECK(resp.result.count("input") == 0);
        CHECK(resp.result.count("cfg") == 0);
        CHECK(resp.result.count("api_key") == 0);
        CHECK(resp.result.count("api_keys") == 0);
        CHECK(resp.result.count("out_dir") == 0);
        CHECK(resp.result.count("output_dir") == 0);
        CHECK(resp.result.count("transcript_text") == 0);
        CHECK(resp.result.count("audio_path") == 0);

        // Defense-in-depth: re-serialize the response and grep for the
        // literal secret values. Even if a key name slipped past, the
        // value bytes must not appear anywhere.
        const std::string wire = serialize(resp);
        CHECK(wire.find("sk-SECRET-API-KEY") == std::string::npos);
        CHECK(wire.find("secret-transcript-content") == std::string::npos);
        CHECK(wire.find("/secret/output_dir") == std::string::npos);
    }

    // (b) job.list — same absence assertions, this time on the array body.
    {
        IpcResponse resp; IpcError err;
        REQUIRE(client.call("job.list", JsonMap{}, resp, err, 2000));
        CHECK(resp.result.count("input") == 0);
        CHECK(resp.result.count("cfg") == 0);

        const std::string jobs_arr = json_val_as_string(resp.result["jobs"]);
        REQUIRE(!jobs_arr.empty());
        // The whole array body must not contain any leak markers.
        CHECK(jobs_arr.find("sk-SECRET-API-KEY") == std::string::npos);
        CHECK(jobs_arr.find("secret-transcript-content") == std::string::npos);
        CHECK(jobs_arr.find("/secret/output_dir") == std::string::npos);
        CHECK(jobs_arr.find("\"input\"") == std::string::npos);
        CHECK(jobs_arr.find("\"cfg\"") == std::string::npos);
        CHECK(jobs_arr.find("\"api_keys\"") == std::string::npos);
        CHECK(jobs_arr.find("\"transcript_text\"") == std::string::npos);
        CHECK(jobs_arr.find("\"out_dir\"") == std::string::npos);
        CHECK(jobs_arr.find("\"audio_path\"") == std::string::npos);
    }

    client.close_connection();
    ::unlink(QUERY_SOCK);
}

// ===========================================================================
// 6. job.list — empty for a client with no jobs. count==0 and jobs==[].
// ===========================================================================
TEST_CASE("C.6 job.list: empty for a client with no jobs",
          "[c6][query]") {
    ::unlink(QUERY_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(QUERY_SOCK);
    register_job_query_handlers(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(QUERY_SOCK);
    REQUIRE(client.connect());

    IpcResponse resp; IpcError err;
    REQUIRE(client.call("job.list", JsonMap{}, resp, err, 2000));
    CHECK(json_val_as_int(resp.result["count"]) == 0);
    CHECK(json_val_as_string(resp.result["jobs"]) == "[]");

    client.close_connection();
    ::unlink(QUERY_SOCK);
}

// ===========================================================================
// 7. job.list — mixed kinds + mixed states for one client, foreign client's
//    jobs excluded. The cross-client filter is the load-bearing assertion:
//    a leak here is a tray re-sync security regression (D.5).
//
// Also covers: terminal jobs (Done / Failed / Cancelled) ARE included —
// that's the C.7-retained registry contract that D.5 relies on.
// ===========================================================================
TEST_CASE("C.6 job.list: mixed kinds + states, foreign jobs excluded, "
          "terminal jobs included",
          "[c6][query]") {
    ::unlink(QUERY_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(QUERY_SOCK);
    register_job_query_handlers(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient a(QUERY_SOCK), b(QUERY_SOCK);
    REQUIRE(a.connect());
    REQUIRE(b.connect());
    REQUIRE(a.client_id() != b.client_id());

    // A: 1 done postprocess, 1 failed postprocess, 1 cancelled streaming
    //    (cancelled-while-queued), 1 model download still queued, 1 queued
    //    postprocess. The pp slot must be drained between Done/Failed so the
    //    next staging can take the slot.
    int64_t a_done    = stage_done_pp(q, a.client_id());
    int64_t a_failed  = stage_failed_pp(q, a.client_id(), "boom");
    int64_t a_strm_x  = enqueue_streaming_queued(q, a.client_id());
    REQUIRE(q.cancel(a_strm_x));
    int64_t a_md      = enqueue_model_download_queued(q, a.client_id(),
                                                      "whisper/base");
    int64_t a_q       = enqueue_postprocess_queued(q, a.client_id());

    // B: 1 queued postprocess — must NOT appear in A's list.
    int64_t b_q       = enqueue_postprocess_queued(q, b.client_id());

    // Query A's list.
    IpcResponse resp_a; IpcError err_a;
    REQUIRE(a.call("job.list", JsonMap{}, resp_a, err_a, 2000));
    CHECK(json_val_as_int(resp_a.result["count"]) == 5);
    const std::string a_arr = json_val_as_string(resp_a.result["jobs"]);
    auto a_elems = split_top_array(a_arr);
    REQUIRE(a_elems.size() == 5);

    // Collect the (id, kind, state) tuples for A. The exact bytes are
    // verified by extract_field on each element.
    std::set<int64_t> a_ids;
    std::set<std::string> a_kinds, a_states;
    for (const auto& e : a_elems) {
        std::string id_s = extract_field(e, "job_id");
        std::string kind = extract_field(e, "kind");
        std::string state = extract_field(e, "state");
        std::string cid = extract_field(e, "client_id");
        a_ids.insert(std::stoll(id_s));
        a_kinds.insert(kind);
        a_states.insert(state);
        // Every element belongs to A.
        CHECK(cid == a.client_id());
    }
    CHECK(a_ids.count(a_done) == 1);
    CHECK(a_ids.count(a_failed) == 1);
    CHECK(a_ids.count(a_strm_x) == 1);
    CHECK(a_ids.count(a_md) == 1);
    CHECK(a_ids.count(a_q) == 1);
    // B's job MUST NOT appear in A's list (foreign-jobs-excluded).
    CHECK(a_ids.count(b_q) == 0);

    // Terminal jobs included.
    CHECK(a_states.count("done") == 1);
    CHECK(a_states.count("failed") == 1);
    CHECK(a_states.count("cancelled") == 1);
    // Mixed kinds.
    CHECK(a_kinds.count("postprocess") == 1);
    CHECK(a_kinds.count("model_download") == 1);
    CHECK(a_kinds.count("streaming") == 1);

    // Query B's list — exactly 1 job, owned by B.
    IpcResponse resp_b; IpcError err_b;
    REQUIRE(b.call("job.list", JsonMap{}, resp_b, err_b, 2000));
    CHECK(json_val_as_int(resp_b.result["count"]) == 1);
    auto b_elems = split_top_array(
        json_val_as_string(resp_b.result["jobs"]));
    REQUIRE(b_elems.size() == 1);
    CHECK(extract_field(b_elems[0], "client_id") == b.client_id());
    CHECK(std::stoll(extract_field(b_elems[0], "job_id")) == b_q);

    a.close_connection();
    b.close_connection();
    ::unlink(QUERY_SOCK);
}

// ===========================================================================
// 8. job.list — ordering is ascending job_id. JobQueue::list_by_client
//    iterates `std::map<int64_t, Job>` keyed by job_id (ascending), and the
//    handler appends in iterator order, so the wire MUST preserve that.
//    D.5 reconnect re-sync renders the list newest-last, so a permutation
//    bug here would show the user the wrong "most recent job" entry.
// ===========================================================================
TEST_CASE("C.6 job.list: ascending job_id ordering",
          "[c6][query]") {
    ::unlink(QUERY_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(QUERY_SOCK);
    register_job_query_handlers(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(QUERY_SOCK);
    REQUIRE(client.connect());

    // Enqueue several jobs interleaving kinds — the registry keys by job_id
    // alone, so order on the wire MUST be ascending regardless of kind.
    int64_t a = enqueue_postprocess_queued(q,    client.client_id());
    int64_t b = enqueue_streaming_queued(q,      client.client_id());
    int64_t c = enqueue_model_download_queued(q, client.client_id(), "m1");
    int64_t d = enqueue_postprocess_queued(q,    client.client_id());
    int64_t e = enqueue_streaming_queued(q,      client.client_id());
    // Sanity — JobQueue mints ascending ids.
    REQUIRE(a < b);
    REQUIRE(b < c);
    REQUIRE(c < d);
    REQUIRE(d < e);

    IpcResponse resp; IpcError err;
    REQUIRE(client.call("job.list", JsonMap{}, resp, err, 2000));
    auto elems = split_top_array(json_val_as_string(resp.result["jobs"]));
    REQUIRE(elems.size() == 5);

    std::vector<int64_t> ids;
    for (const auto& el : elems) {
        ids.push_back(std::stoll(extract_field(el, "job_id")));
    }
    CHECK(ids[0] == a);
    CHECK(ids[1] == b);
    CHECK(ids[2] == c);
    CHECK(ids[3] == d);
    CHECK(ids[4] == e);

    // Strict-ascending — guards against a future change that swaps to an
    // unordered_map without preserving the documented order.
    for (size_t i = 1; i < ids.size(); ++i) {
        CHECK(ids[i] > ids[i - 1]);
    }

    client.close_connection();
    ::unlink(QUERY_SOCK);
}

// ---------------------------------------------------------------------------
// Phase C.11.1 — job.status / job.list responses carry meeting_id.
// Field is emitted unconditionally; empty string for v1-shaped jobs.
// ---------------------------------------------------------------------------

TEST_CASE("C.11 — job.status response carries meeting_id "
          "(populated and empty round-trips)",
          "[c6][query][c11]") {
    ::unlink(QUERY_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(QUERY_SOCK);
    register_job_query_handlers(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(QUERY_SOCK);
    REQUIRE(client.connect());

    // Job WITH meeting_id (mirrors what process.submit will do once C.11.4
    // wires the SubmitRequest field onto the Job at finalize).
    const std::string id = "12345678-1234-4567-89ab-1234567890ab";
    Job j;
    j.meeting_id = id;
    int64_t with_id = q.enqueue(std::move(j), JobKind::Postprocess,
                                client.client_id());

    // Job WITHOUT meeting_id (the v1-client fallback).
    int64_t no_id = enqueue_postprocess_queued(q, client.client_id());

    // job.status on the populated job emits the id.
    {
        JsonMap params; params["job_id"] = with_id;
        IpcResponse resp; IpcError err;
        REQUIRE(client.call("job.status", params, resp, err, 2000));
        CHECK(json_val_as_string(resp.result["meeting_id"]) == id);
    }
    // job.status on the v1-shaped job emits empty string (not absent).
    {
        JsonMap params; params["job_id"] = no_id;
        IpcResponse resp; IpcError err;
        REQUIRE(client.call("job.status", params, resp, err, 2000));
        auto it = resp.result.find("meeting_id");
        REQUIRE(it != resp.result.end());
        CHECK(json_val_as_string(it->second).empty());
    }
    // job.list emits meeting_id for each entry.
    {
        IpcResponse resp; IpcError err;
        REQUIRE(client.call("job.list", JsonMap{}, resp, err, 2000));
        auto elems = split_top_array(json_val_as_string(resp.result["jobs"]));
        REQUIRE(elems.size() == 2);
        size_t hits = 0;
        for (const auto& el : elems) {
            std::string jid = extract_field(el, "job_id");
            std::string mid = extract_field(el, "meeting_id");
            if (std::stoll(jid) == with_id) { CHECK(mid == id); ++hits; }
            else if (std::stoll(jid) == no_id) { CHECK(mid.empty()); ++hits; }
        }
        CHECK(hits == 2);
    }

    client.close_connection();
    ::unlink(QUERY_SOCK);
}
