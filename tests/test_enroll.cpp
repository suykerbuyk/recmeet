// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.8 — Enrollment via process.submit + enroll.finalize tests.
//
// Five layers:
//
//   * DiarizationCache unit tests — put/get/erase, TTL eviction with the
//     clock-injection seam, sweep_expired, persistence-free semantics.
//   * diarization.json artifact round-trip — pipeline.cpp writer + the
//     cache's load_diarization_artifact reader agree on the schema.
//   * UploadSessionManager mode='enroll' tests — create() rejects missing
//     enroll_name; finalize stamps cfg.enroll_mode + cfg.enroll_name on
//     the outgoing Job.
//   * `enroll.finalize` handler validation suite (mirrors test_fetch.cpp's
//     `register_process_fetch_handler` style — a handler registered on a
//     test-local IpcServer + JobQueue, mirroring the production daemon
//     handler line-for-line).
//   * End-to-end flow (a) + (b): drive process.submit (enroll) through to
//     a Done job whose cache entry is pre-populated, then enroll.finalize
//     and read back the speakers DB on disk.
//
// Thread hygiene (orchestrator rule 5): every std::thread is owned by a
// RAII guard (`ServerGuard`, `JqShutdownGuard`) that joins on
// destruction.

#include <catch2/catch_test_macros.hpp>

#include "config.h"
#include "daemon_handlers_internal.h"  // g_jobs / g_diar_cache externs
#include "daemon_test_harness.h"
#include "diarization_cache.h"
#include "fetch_artifacts.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"
#include "speaker_id.h"
#include "test_tmpdir.h"
#include "upload_session.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <unistd.h>

using namespace recmeet;
using recmeet::test::DaemonTestHarness;
namespace fs = std::filesystem;

namespace {

// Ignore SIGPIPE so a test that drops a connection mid-handler does not
// kill the test process.
struct SigpipeIgnorer {
    SigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} s_ignore_sigpipe;

const std::string ENROLL_SOCK =
    recmeet::test::tmp_path("recmeet_test_enroll.sock").string();

// ---------------------------------------------------------------------------
// RAII guards (same pattern as test_fetch.cpp / test_cancel.cpp)
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

struct JqShutdownGuard {
    JobQueue& q;
    explicit JqShutdownGuard(JobQueue& q_) : q(q_) {}
    ~JqShutdownGuard() { q.shutdown(); }
};

// ---------------------------------------------------------------------------
// Test-local injectable clock for TTL tests
// ---------------------------------------------------------------------------

struct FakeClock {
    std::atomic<int64_t> now{0};
    DiarizationCache::Clock fn() {
        return [this]() { return now.load(); };
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fs::path test_temp_dir(const std::string& tag) {
    fs::path d = recmeet::test::tmp_path("recmeet_enroll_test_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

// Build a synthetic DiarizationCluster vector for fixture use.
std::vector<DiarizationCluster> make_synthetic_clusters(int n,
                                                        int emb_dim = 8) {
    std::vector<DiarizationCluster> out;
    for (int i = 0; i < n; ++i) {
        DiarizationCluster c;
        c.idx = i;
        c.duration_ms = static_cast<int64_t>(1000 * (i + 1));
        // Distinct sentinel pattern per cluster so we can assert which
        // embedding landed where on the disk.
        c.embedding.resize(emb_dim);
        for (int j = 0; j < emb_dim; ++j) {
            c.embedding[j] = static_cast<float>(i + 1) * 0.1f
                             + static_cast<float>(j) * 0.001f;
        }
        out.push_back(std::move(c));
    }
    return out;
}

// Stage a Postprocess job through Queued -> Running -> Done. Returns the
// job_id. Same idiom as test_fetch.cpp / test_cancel.cpp.
int64_t stage_done_job(JobQueue& q, const std::string& client_id,
                       const fs::path& out_dir,
                       bool enroll_mode = false) {
    Job j;
    j.input.out_dir = out_dir;
    j.cfg.enroll_mode = enroll_mode;
    int64_t id = q.enqueue(std::move(j), JobKind::Postprocess, client_id);
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    REQUIRE(dq->job_id == id);
    q.finish(id, /*ok=*/true);
    return id;
}

int64_t stage_running_job(JobQueue& q, const std::string& client_id,
                          const fs::path& out_dir) {
    Job j;
    j.input.out_dir = out_dir;
    int64_t id = q.enqueue(std::move(j), JobKind::Postprocess, client_id);
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    return id;
}

// Phase 2b: the prior `register_enroll_finalize_handler` stub was retired
// in favor of `register_daemon_handlers()` via DaemonTestHarness. The
// production handler at src/daemon_handlers.cpp:1465 reads `g_jobs`,
// `g_diar_cache`, `g_server_config.speaker_db` — the harness wires all
// three.

// Phase 2b: helper that boots a DaemonTestHarness with the speaker-db
// pointed at a deterministic path and (optionally) installs a clock-
// injectable DiarizationCache as g_diar_cache so TTL-expiry tests can
// drive the clock forward. The harness wires g_jobs already.
//
// DaemonTestHarness is non-copyable + non-movable, so EnrollHarness must
// be constructed in-place. The TTL-test variant takes the (ttl, clock)
// pair via the constructor.
struct EnrollHarness {
    DaemonTestHarness harness;

    EnrollHarness() {
        harness.start();
    }
    // Variant for the TTL test — swap in a clock-injectable cache BEFORE
    // start() so the handler sees it. The default-constructed harness
    // installs a default `g_diar_cache`; we replace it here before
    // start() registers handlers + starts the poll thread, so the
    // handler captures the swapped pointer on first dereference.
    EnrollHarness(int64_t ttl_secs, DiarizationCache::Clock clock) {
        g_diar_cache = std::make_unique<DiarizationCache>(ttl_secs,
                                                          std::move(clock));
        harness.start();
    }

    EnrollHarness(const EnrollHarness&)            = delete;
    EnrollHarness& operator=(const EnrollHarness&) = delete;

    fs::path speaker_db_dir() const { return harness.speaker_db_dir(); }
    std::unique_ptr<IpcClient> make_client() { return harness.make_client(); }
    JobQueue& jobs() { return *g_jobs; }
    DiarizationCache& cache() { return *g_diar_cache; }
};

} // anonymous namespace

// ===========================================================================
// 1. DiarizationCache — put/get round-trip.
// ===========================================================================

TEST_CASE("DiarizationCache: put/get round-trip preserves clusters",
          "[c8][enroll]") {
    DiarizationCache cache(/*ttl=*/0);  // disable TTL
    auto clusters = make_synthetic_clusters(3);
    auto cluster_copy = clusters;  // preserve for assertions
    cache.put(42, std::move(clusters));

    CHECK(cache.size() == 1);
    auto got = cache.get(42);
    REQUIRE(got.has_value());
    CHECK(got->job_id == 42);
    REQUIRE(got->clusters.size() == 3);
    for (int i = 0; i < 3; ++i) {
        CHECK(got->clusters[i].idx == cluster_copy[i].idx);
        CHECK(got->clusters[i].duration_ms == cluster_copy[i].duration_ms);
        CHECK(got->clusters[i].embedding.size()
              == cluster_copy[i].embedding.size());
        for (size_t j = 0; j < cluster_copy[i].embedding.size(); ++j) {
            CHECK(got->clusters[i].embedding[j]
                  == cluster_copy[i].embedding[j]);
        }
    }
}

// ===========================================================================
// 2. DiarizationCache — TTL eviction with the clock-injection seam.
// ===========================================================================

TEST_CASE("DiarizationCache: TTL eviction is lazy on lookup",
          "[c8][enroll]") {
    FakeClock clk;
    clk.now.store(1000);
    DiarizationCache cache(/*ttl=*/60, clk.fn());

    cache.put(7, make_synthetic_clusters(2));
    CHECK(cache.size() == 1);

    // Within TTL — still present.
    clk.now.store(1059);
    CHECK(cache.get(7).has_value());

    // Past TTL — lazy eviction erases on lookup, miss returned.
    clk.now.store(1100);
    CHECK_FALSE(cache.get(7).has_value());
    CHECK(cache.size() == 0);  // confirms eviction happened
}

// ===========================================================================
// 3. DiarizationCache — sweep_expired drops stale entries en masse.
// ===========================================================================

TEST_CASE("DiarizationCache: sweep_expired drops stale entries",
          "[c8][enroll]") {
    FakeClock clk;
    clk.now.store(1000);
    DiarizationCache cache(/*ttl=*/60, clk.fn());

    cache.put(1, make_synthetic_clusters(1));
    cache.put(2, make_synthetic_clusters(1));
    clk.now.store(1100);  // both stale
    cache.put(3, make_synthetic_clusters(1));  // fresh

    CHECK(cache.sweep_expired() == 2);
    CHECK(cache.size() == 1);
    CHECK(cache.get(3).has_value());
}

// ===========================================================================
// 4. DiarizationCache — TTL=0 means never expire.
// ===========================================================================

TEST_CASE("DiarizationCache: ttl=0 disables expiry", "[c8][enroll]") {
    FakeClock clk;
    clk.now.store(0);
    DiarizationCache cache(/*ttl=*/0, clk.fn());
    cache.put(5, make_synthetic_clusters(1));
    clk.now.store(static_cast<int64_t>(1e9));  // huge jump
    CHECK(cache.get(5).has_value());
    CHECK(cache.size() == 1);
}

// ===========================================================================
// 5. DiarizationCache — load_diarization_artifact parses the
//    pipeline.cpp `diarization.json` writer's output.
// ===========================================================================

TEST_CASE("DiarizationCache: load_diarization_artifact parses writer output",
          "[c8][enroll]") {
    // Hand-rolled fixture matching write_diarization_artifact's schema.
    fs::path d = test_temp_dir("load_artifact");
    fs::path p = d / "diarization.json";
    std::ofstream out(p);
    out <<
        "{\n"
        "  \"num_speakers\": 2,\n"
        "  \"clusters\": [\n"
        "    { \"idx\": 0, \"duration_ms\": 5000, "
              "\"embedding\": [0.1, 0.2, 0.3] },\n"
        "    { \"idx\": 1, \"duration_ms\": 2500, "
              "\"embedding\": [0.4, 0.5, 0.6] }\n"
        "  ]\n"
        "}\n";
    out.close();

    auto clusters = DiarizationCache::load_diarization_artifact(p);
    REQUIRE(clusters.size() == 2);
    CHECK(clusters[0].idx == 0);
    CHECK(clusters[0].duration_ms == 5000);
    REQUIRE(clusters[0].embedding.size() == 3);
    CHECK(clusters[0].embedding[0] == 0.1f);
    CHECK(clusters[1].duration_ms == 2500);
    CHECK(clusters[1].embedding[2] == 0.6f);
}

// ===========================================================================
// 6. UploadSessionManager — process.submit mode='enroll' requires
//    enroll_name (InvalidParams when missing).
// ===========================================================================

TEST_CASE("process.submit mode=enroll: missing enroll_name → InvalidParams",
          "[c8][enroll]") {
    JobQueue q;
    JqShutdownGuard jqg(q);
    UploadSessionManager mgr(q, test_temp_dir("enroll_missing_name"));
    JobConfig cfg;

    SubmitRequest r;
    r.audio_size = 1024;
    r.format = "s16le";
    r.sample_rate = 16000;
    r.channels = 1;
    r.mode = "enroll";
    // enroll_name deliberately empty
    auto res = mgr.create("client-A", r, cfg, /*max=*/1 << 20);
    CHECK_FALSE(res.ok);
    CHECK(res.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(res.error.find("enroll_name") != std::string::npos);
}

// ===========================================================================
// 7. UploadSessionManager — happy path: mode='enroll' with enroll_name
//    finalizes a Postprocess job whose cfg.enroll_mode=true,
//    cfg.enroll_name='Alice'.
// ===========================================================================

TEST_CASE("process.submit mode=enroll: finalize stamps cfg.enroll_mode",
          "[c8][enroll]") {
    JobQueue q;

    // Drain the slot so finalize's enqueue_reserved doesn't block forever.
    std::atomic<bool> stop{false};
    Job captured;
    std::atomic<bool> captured_ready{false};
    std::thread drain([&]() {
        for (;;) {
            auto dq = q.dequeue(JobKind::Postprocess);
            if (!dq.has_value()) return;
            captured = *dq;
            captured_ready.store(true);
            q.finish(dq->job_id, true);
            if (stop.load()) return;
        }
    });

    UploadSessionManager mgr(q, test_temp_dir("enroll_finalize"));
    JobConfig cfg;
    SubmitRequest r;
    r.audio_size = 6;  // tiny — 3 s16 samples
    r.format = "s16le";
    r.sample_rate = 16000;
    r.channels = 1;
    r.mode = "enroll";
    r.enroll_name = "Alice";
    auto res = mgr.create("client-A", r, cfg, 1 << 20);
    REQUIRE(res.ok);

    // Feed the entire payload in one chunk — finalize fires.
    std::string payload(6, '\0');
    bool ok = mgr.feed_chunk("client-A", payload);
    CHECK(ok);

    // Wait for the drain worker to pick the job up.
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(2000);
    while (!captured_ready.load()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop.store(true);
    q.shutdown();
    if (drain.joinable()) drain.join();

    REQUIRE(captured_ready.load());
    CHECK(captured.cfg.enroll_mode == true);
    CHECK(captured.cfg.enroll_name == "Alice");
}

// ===========================================================================
// 8. process.submit unknown mode is still rejected.
// ===========================================================================

TEST_CASE("process.submit: unknown mode → InvalidParams",
          "[c8][enroll]") {
    JobQueue q;
    JqShutdownGuard jqg(q);
    UploadSessionManager mgr(q, test_temp_dir("unknown_mode"));
    JobConfig cfg;
    SubmitRequest r;
    r.audio_size = 1024;
    r.format = "s16le";
    r.sample_rate = 16000;
    r.channels = 1;
    r.mode = "foo";
    auto res = mgr.create("client-A", r, cfg, 1 << 20);
    CHECK_FALSE(res.ok);
    CHECK(res.code == static_cast<int>(IpcErrorCode::InvalidParams));
}

// ===========================================================================
// 9. enroll.finalize — happy path: appends a new embedding to a profile
//    and the on-disk speakers DB reflects it.
//    Covers Flow (a) at the handler+cache+DB layer (subprocess is
//    represented by a synthetic cache entry).
// ===========================================================================

TEST_CASE("enroll.finalize: appends embedding and persists to disk",
          "[c8][enroll]") {
    EnrollHarness eh;
    auto client = eh.make_client();

    // Stage a Done enroll-mode job and pre-populate the cache.
    fs::path out_dir = test_temp_dir("happy_outdir");
    int64_t job_id = stage_done_job(eh.jobs(), client->client_id(), out_dir,
                                     /*enroll_mode=*/true);
    eh.cache().put(job_id, make_synthetic_clusters(2));

    // Call enroll.finalize with target_speaker=0, name="Bob".
    JsonMap params;
    params["job_id"] = static_cast<int64_t>(job_id);
    params["target_speaker"] = static_cast<int64_t>(0);
    params["enroll_name"] = std::string("Bob");

    IpcResponse resp;
    IpcError err;
    bool ok = client->call("enroll.finalize", params, resp, err,
                           /*timeout_ms=*/2000);
    INFO("err code=" << err.code << " message=" << err.message);
    REQUIRE(ok);
    CHECK(err.message.empty());
    CHECK(json_val_as_bool(resp.result["ok"]) == true);
    CHECK(json_val_as_string(resp.result["enroll_name"]) == "Bob");
    CHECK(json_val_as_int(resp.result["embedding_count"]) == 1);

    // Read back the on-disk speakers DB. The handler wrote
    // <speaker_db>/Bob.json with one embedding matching cluster 0's centroid.
    auto profiles = load_speaker_db(eh.speaker_db_dir());
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].name == "Bob");
    REQUIRE(profiles[0].embeddings.size() == 1);
    auto fixture = make_synthetic_clusters(2);
    CHECK(profiles[0].embeddings[0].size()
          == fixture[0].embedding.size());
}

// ===========================================================================
// 10. enroll.finalize — Flow (b): a regular postprocess job (enroll_mode
//     false) with a cache entry pre-populated yields the same DB write
//     path.
// ===========================================================================

TEST_CASE("enroll.finalize: flow-b reuses a regular postprocess job's cache",
          "[c8][enroll]") {
    EnrollHarness eh;
    auto client = eh.make_client();

    // enroll_mode=false on the job, but the cache entry is there.
    fs::path out_dir = test_temp_dir("flowb_outdir");
    int64_t job_id = stage_done_job(eh.jobs(), client->client_id(), out_dir,
                                     /*enroll_mode=*/false);
    eh.cache().put(job_id, make_synthetic_clusters(3));

    JsonMap params;
    params["job_id"] = static_cast<int64_t>(job_id);
    params["target_speaker"] = static_cast<int64_t>(2);
    params["enroll_name"] = std::string("Charlie");

    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("enroll.finalize", params, resp, err, 2000));
    CHECK(err.message.empty());
    CHECK(json_val_as_int(resp.result["embedding_count"]) == 1);

    auto profiles = load_speaker_db(eh.speaker_db_dir());
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].name == "Charlie");
}

// ===========================================================================
// 11. enroll.finalize — multi-sample profile: two finalizes for the
//     same name produce a 2-embedding profile (append-one semantic).
// ===========================================================================

TEST_CASE("enroll.finalize: two calls for same name yield 2-embedding profile",
          "[c8][enroll]") {
    EnrollHarness eh;
    auto client = eh.make_client();

    int64_t jid1 = stage_done_job(eh.jobs(), client->client_id(),
                                  test_temp_dir("ms_a"), true);
    eh.cache().put(jid1, make_synthetic_clusters(1));
    int64_t jid2 = stage_done_job(eh.jobs(), client->client_id(),
                                  test_temp_dir("ms_b"), true);
    eh.cache().put(jid2, make_synthetic_clusters(1));

    for (int64_t jid : {jid1, jid2}) {
        JsonMap params;
        params["job_id"] = jid;
        params["target_speaker"] = static_cast<int64_t>(0);
        params["enroll_name"] = std::string("Dana");
        IpcResponse resp;
        IpcError err;
        REQUIRE(client->call("enroll.finalize", params, resp, err, 2000));
        CHECK(err.message.empty());
    }

    auto profiles = load_speaker_db(eh.speaker_db_dir());
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].name == "Dana");
    CHECK(profiles[0].embeddings.size() == 2);
}

// ===========================================================================
// 12. enroll.finalize — TTL expiry: cache entry past the TTL is gone;
//     handler reports a clear error. Uses the injected clock seam.
// ===========================================================================

TEST_CASE("enroll.finalize: expired cache entry → InvalidParams w/ TTL hint",
          "[c8][enroll]") {
    auto clk = std::make_shared<FakeClock>();
    clk->now.store(1000);
    EnrollHarness eh(/*ttl=*/60, [clk]() { return clk->now.load(); });
    auto client = eh.make_client();

    int64_t jid = stage_done_job(eh.jobs(), client->client_id(),
                                 test_temp_dir("ttl_outdir"), true);
    eh.cache().put(jid, make_synthetic_clusters(1));

    // Advance the clock past TTL.
    clk->now.store(1200);

    JsonMap params;
    params["job_id"] = jid;
    params["target_speaker"] = static_cast<int64_t>(0);
    params["enroll_name"] = std::string("Eve");
    IpcResponse resp;
    IpcError err;
    // The handler returns an error → call() returns false; err is populated.
    CHECK_FALSE(client->call("enroll.finalize", params, resp, err, 2000));
    CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(err.message.find("TTL") != std::string::npos);
}

// ===========================================================================
// 13. enroll.finalize — target_speaker out of range → InvalidParams.
// ===========================================================================

TEST_CASE("enroll.finalize: target_speaker out of range → InvalidParams",
          "[c8][enroll]") {
    EnrollHarness eh;
    auto client = eh.make_client();

    int64_t jid = stage_done_job(eh.jobs(), client->client_id(),
                                 test_temp_dir("oor_outdir"), true);
    eh.cache().put(jid, make_synthetic_clusters(2));

    JsonMap params;
    params["job_id"] = jid;
    params["target_speaker"] = static_cast<int64_t>(99);
    params["enroll_name"] = std::string("OOR");
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client->call("enroll.finalize", params, resp, err, 2000));
    CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(err.message.find("out of range") != std::string::npos);
}

// ===========================================================================
// 14. enroll.finalize — JobNotReady on a Running job.
// ===========================================================================

TEST_CASE("enroll.finalize: Running job → JobNotReady",
          "[c8][enroll]") {
    EnrollHarness eh;
    auto client = eh.make_client();

    int64_t jid = stage_running_job(eh.jobs(), client->client_id(),
                                    test_temp_dir("notready_outdir"));
    // No cache entry needed — state check fires first.

    JsonMap params;
    params["job_id"] = jid;
    params["target_speaker"] = static_cast<int64_t>(0);
    params["enroll_name"] = std::string("Frank");
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client->call("enroll.finalize", params, resp, err, 2000));
    CHECK(err.code == static_cast<int>(IpcErrorCode::JobNotReady));

    // Drain the running job so the harness's job-queue teardown is clean.
    eh.jobs().finish(jid, true);
}

// ===========================================================================
// 15. enroll.finalize — PermissionDenied on another client's job.
// ===========================================================================

TEST_CASE("enroll.finalize: other client's job → PermissionDenied",
          "[c8][enroll]") {
    EnrollHarness eh;
    auto a = eh.make_client();
    auto b = eh.make_client();

    int64_t a_job = stage_done_job(eh.jobs(), a->client_id(),
                                   test_temp_dir("perm_outdir"), true);
    eh.cache().put(a_job, make_synthetic_clusters(1));

    JsonMap params;
    params["job_id"] = a_job;
    params["target_speaker"] = static_cast<int64_t>(0);
    params["enroll_name"] = std::string("Foreign");
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(b->call("enroll.finalize", params, resp, err, 2000));
    CHECK(err.code == static_cast<int>(IpcErrorCode::PermissionDenied));
}

// ===========================================================================
// 16. enroll.finalize — missing enroll_name → InvalidParams.
// ===========================================================================

TEST_CASE("enroll.finalize: missing enroll_name → InvalidParams",
          "[c8][enroll]") {
    EnrollHarness eh;
    auto client = eh.make_client();

    int64_t jid = stage_done_job(eh.jobs(), client->client_id(),
                                 test_temp_dir("missing_name_outdir"), true);
    eh.cache().put(jid, make_synthetic_clusters(1));

    JsonMap params;
    params["job_id"] = jid;
    params["target_speaker"] = static_cast<int64_t>(0);
    // enroll_name deliberately absent
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client->call("enroll.finalize", params, resp, err, 2000));
    CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(err.message.find("enroll_name") != std::string::npos);
}

// ===========================================================================
// 17. process.fetch on an enroll-mode Done job returns empty artifacts.
//     (The fetch handler's filter excludes diarization.json + audio.wav
//     already; an enroll job's out_dir contains exactly those two and
//     nothing the allowlist permits.)
// ===========================================================================

TEST_CASE("process.fetch on enroll-mode Done job returns empty artifacts",
          "[c8][enroll]") {
    // We don't drive the wire here — we just exercise the filter via
    // enumerate_artifacts(), which is what the process.fetch handler
    // calls. Tighter and avoids a second IpcServer setup.
    fs::path d = test_temp_dir("fetch_enroll");
    std::ofstream(d / "diarization.json") << "{}\n";
    std::ofstream(d / "audio.wav") << "RIFFxxxx";

    std::string e;
    auto arts = enumerate_artifacts(d, &e);
    CHECK(arts.empty());
    CHECK(e.empty());
}

// ===========================================================================
// P3-4 — enroll.finalize with >2 embeddings for the same profile.
//
// The existing "two calls for same name yield 2-embedding profile" case
// (test #11 above) covers the N=2 path. The handler should handle N>2
// equally — calling enroll.finalize multiple times for the same
// enroll_name across multiple cached jobs (or multiple target_speakers
// of the same cached job) should append each cluster's embedding to the
// SAME on-disk profile. Surfaced by the iter-156 audit.
// ===========================================================================

TEST_CASE("enroll.finalize: N>2 embeddings for one name produce an N-embedding profile",
          "[c8][enroll][multi-embedding]") {
    EnrollHarness eh;
    auto client = eh.make_client();

    // Mix the two valid paths for "more embeddings": three SEPARATE
    // cached jobs (covers the typical "user re-enrolled three times in
    // separate recordings" flow) plus one same-job different-target
    // (covers the diarization-cluster-fan-out flow).
    //
    // Total expected: 4 embeddings on the profile.
    constexpr int kSeparateJobs = 3;
    std::vector<int64_t> job_ids;
    for (int i = 0; i < kSeparateJobs; ++i) {
        int64_t jid = stage_done_job(eh.jobs(), client->client_id(),
                                     test_temp_dir("multi_emb_job"
                                                   + std::to_string(i)),
                                     /*enroll_mode=*/true);
        eh.cache().put(jid, make_synthetic_clusters(1));
        job_ids.push_back(jid);
    }

    // Append three times via separate cached jobs, target_speaker=0.
    for (int64_t jid : job_ids) {
        JsonMap params;
        params["job_id"] = jid;
        params["target_speaker"] = static_cast<int64_t>(0);
        params["enroll_name"] = std::string("Alice");
        IpcResponse resp;
        IpcError err;
        INFO("job_id=" << jid);
        REQUIRE(client->call("enroll.finalize", params, resp, err, 2000));
        CHECK(err.message.empty());
    }
    // After 3 separate-job appends, the profile has exactly 3 embeddings.
    {
        auto profiles = load_speaker_db(eh.speaker_db_dir());
        REQUIRE(profiles.size() == 1);
        CHECK(profiles[0].name == "Alice");
        CHECK(profiles[0].embeddings.size() == 3);
    }

    // Now append a 4th from a NEW cached job that has multiple clusters,
    // picking target_speaker=1 (a different cluster than 0). This proves
    // the handler honors per-call target_speaker for an enroll job whose
    // diarization produced >1 speaker.
    int64_t multi_jid = stage_done_job(eh.jobs(), client->client_id(),
                                       test_temp_dir("multi_emb_multitgt"),
                                       /*enroll_mode=*/true);
    eh.cache().put(multi_jid, make_synthetic_clusters(3));
    {
        JsonMap params;
        params["job_id"] = multi_jid;
        params["target_speaker"] = static_cast<int64_t>(1);
        params["enroll_name"] = std::string("Alice");
        IpcResponse resp;
        IpcError err;
        REQUIRE(client->call("enroll.finalize", params, resp, err, 2000));
        CHECK(err.message.empty());
        CHECK(json_val_as_int(resp.result["embedding_count"]) == 4);
    }
    // Final profile has all 4 embeddings under a single "Alice" record.
    {
        auto profiles = load_speaker_db(eh.speaker_db_dir());
        REQUIRE(profiles.size() == 1);
        CHECK(profiles[0].name == "Alice");
        CHECK(profiles[0].embeddings.size() == 4);

        // Belt + braces: every embedding has the right dimension. Each
        // synthetic cluster is emb_dim=8 (make_synthetic_clusters default).
        for (const auto& emb : profiles[0].embeddings) {
            CHECK(emb.size() == 8);
        }
    }
}
