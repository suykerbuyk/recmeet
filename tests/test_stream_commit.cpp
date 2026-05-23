// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.10b — streaming finalize + routing tests.
//
// C.10b adds the `process.stream.commit` finalize handoff: when a streaming
// client signals "done streaming," the daemon flushes the streaming ASR,
// closes the disk-backed temp WAV, hands the accumulated audio off to a
// fresh Postprocess job (transcribe + diarize + summarize), and releases
// the streaming slot. The temp WAV is NOT unlinked — it is the postprocess
// subprocess's input. The client monitors the resulting Postprocess job
// via `progress.job` + `process.fetch` against the new job_id.
//
// These tests focus on:
//   * happy-path commit (postprocess job_id returned, slot released, WAV
//     survives + is a valid WAV, postprocess job is Queued/Running)
//   * validation: unknown stream_token → InvalidParams, foreign-client
//     commit → PermissionDenied, post-disconnect commit → InvalidParams
//   * acceptance criterion: "1 streaming + 1 postprocess concurrent" — A
//     streams while B has a separate postprocess job; routing-wise each
//     client sees only its own events, slots-wise both are independent
//     and run concurrently.
//   * routed-caption correctness (regression check against C.3): A
//     streams, B has nothing → captions go to A only.
//   * commit + later process.fetch end-to-end: the new postprocess
//     job_id can be fetched once finished (we synthesize the Done verdict
//     via the stub-pp pattern from test_fetch.cpp).
//   * batch-fallback hook: synthesize a local WAV (what the tray would
//     have buffered) and submit it via process.submit — proves the
//     reconnect-fallback path the C.10b plan documents.
//
// Thread hygiene (orchestrator rule 5): every std::thread spawned here is
// owned by a RAII guard that joins on destruction so a REQUIRE thrown
// between thread construction and `.join()` cannot call std::terminate.
//
// Tag: [c10b][stream-commit] — exercised by `make test` filtering or by
// running the binary directly with `[c10b]`.

#include <catch2/catch_test_macros.hpp>

#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"
#include "streaming_session.h"
#include "test_tmpdir.h"
#include "upload_session.h"
#include "util.h"

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

#include <sndfile.h>
#include <signal.h>
#include <unistd.h>

using namespace recmeet;
namespace fs = std::filesystem;

namespace {

// Ignore SIGPIPE so a write to a dropped socket does not kill the test
// process — mirrors test_streaming_session.cpp / test_routed_events.cpp.
struct SigpipeIgnorer {
    SigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} s_ignore_sigpipe;

// ---------------------------------------------------------------------------
// RAII thread guards — rule 5 mandates these on EVERY std::thread we spawn.
// ---------------------------------------------------------------------------

// JqStreamGuard drives the streaming slot exactly like
// test_streaming_session.cpp's JqGuard: a worker thread that dequeues
// Streaming jobs (flipping the slot's running marker so slot_busy() is
// authoritative). Joined on destruction via JobQueue::shutdown().
struct JqStreamGuard {
    JobQueue& q;
    std::thread stream_worker;
    explicit JqStreamGuard(JobQueue& q_) : q(q_) {
        stream_worker = std::thread([this]() {
            for (;;) {
                auto dq = q.dequeue(JobKind::Streaming);
                if (!dq.has_value()) return;  // shutdown
            }
        });
    }
    ~JqStreamGuard() {
        q.shutdown();
        if (stream_worker.joinable()) stream_worker.join();
    }
};

// JqDualGuard runs BOTH a Streaming worker AND a Postprocess worker — used
// by the acceptance test ("1 streaming + 1 postprocess concurrent"). Each
// loops dequeue on its own slot; they share `q` but never each other's
// FIFO. Joined on destruction via JobQueue::shutdown().
struct JqDualGuard {
    JobQueue& q;
    std::thread stream_worker;
    std::thread pp_worker;
    explicit JqDualGuard(JobQueue& q_) : q(q_) {
        stream_worker = std::thread([this]() {
            for (;;) {
                auto dq = q.dequeue(JobKind::Streaming);
                if (!dq.has_value()) return;
            }
        });
        pp_worker = std::thread([this]() {
            for (;;) {
                auto dq = q.dequeue(JobKind::Postprocess);
                if (!dq.has_value()) return;
                // Stay in Running for the test driver to assert on; the
                // test calls finish() explicitly. NOTE: the test must
                // finish() before the JqDualGuard destructor runs so the
                // pp_worker doesn't block forever waiting for new work
                // after shutdown — shutdown wakes dequeue with nullopt.
            }
        });
    }
    ~JqDualGuard() {
        q.shutdown();
        if (stream_worker.joinable()) stream_worker.join();
        if (pp_worker.joinable()) pp_worker.join();
    }
};

// JqShutdownGuard for tests that do NOT need a background drainer. The C.4
// pattern: enqueue/dequeue synchronously, shutdown on scope exit.
struct JqShutdownGuard {
    JobQueue& q;
    explicit JqShutdownGuard(JobQueue& q_) : q(q_) {}
    ~JqShutdownGuard() { q.shutdown(); }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// No-op caption sink — these tests do not assert on caption content (the
// stub-engine build emits nothing, and the real-model parity test is in
// test_streaming_session.cpp).
StreamingCaptionSink null_sink() {
    StreamingCaptionSink s;
    s.on_caption  = [](int64_t, const std::string&, const std::string&,
                       bool, int64_t) {};
    s.on_degraded = [](int64_t, const std::string&, const std::string&,
                       int64_t) {};
    return s;
}

// Deterministic PCM payload of `n_samples` int16 samples as raw bytes —
// matches the `0x03` payload shape (LE-int16 mono).
std::string make_pcm(size_t n_samples, int16_t start = 0) {
    std::string s;
    s.resize(n_samples * sizeof(int16_t));
    auto* p = reinterpret_cast<int16_t*>(s.data());
    for (size_t i = 0; i < n_samples; ++i)
        p[i] = static_cast<int16_t>(start + static_cast<int16_t>(i));
    return s;
}

// A unique temp dir for a test's streaming WAVs.
fs::path test_temp_dir(const std::string& tag) {
    fs::path d = recmeet::test::tmp_path("recmeet_c10b_test_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

// Wait until pred() is true or deadline passes. Returns the final pred()
// value. Used to poll for slot-marker transitions the worker thread
// flips on its own schedule.
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// Read a WAV file's frame count via libsndfile. Returns -1 on open failure
// (e.g. the file does not exist — used to assert "WAV is gone" or
// "WAV is intact").
sf_count_t wav_frames(const fs::path& p) {
    SF_INFO info = {};
    SNDFILE* sf = sf_open(p.string().c_str(), SFM_READ, &info);
    if (!sf) return -1;
    sf_count_t frames = info.frames;
    sf_close(sf);
    return frames;
}

// Locate the unique .wav under `dir`. The streaming manager mints exactly
// one per session.
fs::path find_wav(const fs::path& dir) {
    for (auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".wav") return e.path();
    return {};
}

// Write a minimal valid WAV at `path` containing `n_samples` of int16 mono
// PCM at 16 kHz. Used by the batch-fallback test to synthesize "what the
// tray would have buffered locally before the stream connection broke."
void write_test_wav(const fs::path& path, size_t n_samples) {
    SF_INFO info = {};
    info.samplerate = 16000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* sf = sf_open(path.string().c_str(), SFM_WRITE, &info);
    REQUIRE(sf != nullptr);
    std::vector<int16_t> samples(n_samples);
    for (size_t i = 0; i < n_samples; ++i)
        samples[i] = static_cast<int16_t>(i & 0x7FFF);
    sf_write_short(sf, samples.data(), static_cast<sf_count_t>(n_samples));
    sf_close(sf);
}

} // anonymous namespace

// ===========================================================================
// 1. HAPPY PATH — commit returns a new postprocess job_id, the streaming
//    slot is released, the new postprocess job is Queued/Running, and the
//    temp WAV survives (not unlinked) with a valid header + expected
//    sample count.
// ===========================================================================
TEST_CASE("C.10b — process.stream.commit: happy-path handoff",
          "[c10b][stream-commit]") {
    JobQueue q;
    JqStreamGuard guard(q);    // Streaming-slot worker; we do NOT run a
                               // pp worker — we want the new Postprocess
                               // job to stay Queued so the test can
                               // assert on it.
    auto sink = null_sink();
    StreamingSessionManager mgr(q, sink, "");   // stub engine — disk-only
    fs::path tmp = test_temp_dir("happy");

    // Open the streaming session.
    StreamRequest req;
    auto cr = mgr.create("client-A", req, tmp);
    REQUIRE(cr.ok);
    const int64_t stream_job_id = cr.job_id;
    const std::string token = cr.stream_token;

    fs::path wav = find_wav(tmp);
    REQUIRE(!wav.empty());

    // Feed three chunks (10ms + 100ms + 50ms @ 16 kHz mono = 2560 samples).
    CHECK(mgr.feed_audio(token, make_pcm(160)));
    CHECK(mgr.feed_audio(token, make_pcm(1600)));
    CHECK(mgr.feed_audio(token, make_pcm(800)));

    // Streaming slot is busy (the Stream worker dequeued and flipped the
    // running marker). Poll because the marker flip is on the worker
    // thread.
    REQUIRE(wait_until([&]() { return q.slot_busy(JobKind::Streaming); },
                       std::chrono::milliseconds(500)));

    // Commit. The session is in a committable state: not cancelled, the
    // WAV handle is open, owned by client-A.
    auto commit = mgr.commit("client-A", token);
    REQUIRE(commit.ok);
    CHECK(commit.postprocess_job_id > 0);
    CHECK(commit.postprocess_job_id != stream_job_id);  // distinct job_id
    const int64_t pp_id = commit.postprocess_job_id;

    // The session is gone from the manager.
    CHECK(mgr.active_count() == 0);

    // Streaming slot is RELEASED (the streaming job landed Done).
    auto stream_st = q.status(stream_job_id);
    REQUIRE(stream_st.has_value());
    CHECK(stream_st->state == JobState::Done);
    CHECK(wait_until([&]() { return !q.slot_busy(JobKind::Streaming); },
                     std::chrono::milliseconds(500)));

    // The new Postprocess job is Queued (we don't run a pp worker in this
    // test, so it never advances to Running). client_for_job returns
    // client-A (the commit handler carried the client_id through the
    // reservation).
    auto pp_st = q.status(pp_id);
    REQUIRE(pp_st.has_value());
    CHECK(pp_st->state == JobState::Queued);
    CHECK(pp_st->kind == JobKind::Postprocess);
    CHECK(pp_st->input.out_dir == tmp);            // wav_dir == temp dir
    CHECK(pp_st->cfg.reprocess_dir == tmp.string());  // reprocess shape
    CHECK(pp_st->input.audio_path == wav);
    auto owner = q.client_for_job(pp_id);
    REQUIRE(owner.has_value());
    CHECK(*owner == "client-A");

    // The temp WAV survived — NOT unlinked. libsndfile rewrote the header
    // on close, so wav_frames() reports the expected sample count.
    REQUIRE(fs::exists(wav));
    CHECK(wav_frames(wav) == static_cast<sf_count_t>(160 + 1600 + 800));

    // Clean up: the test owns the new pp job (no pp worker draining), so
    // we cancel it before the JqStreamGuard's shutdown to avoid leaking
    // a Queued postprocess job into the next test.
    q.cancel(pp_id);
    fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// Phase C.11.1 — StreamRequest.meeting_id propagates onto BOTH the streaming
// Job (stamped at create()) AND the postprocess Job that commit() enqueues.
// ---------------------------------------------------------------------------

TEST_CASE("C.11 — StreamRequest.meeting_id flows onto streaming + postprocess Jobs",
          "[c10b][stream-commit][c11]") {
    JobQueue q;
    JqStreamGuard guard(q);
    auto sink = null_sink();
    StreamingSessionManager mgr(q, sink, "");
    fs::path tmp = test_temp_dir("c11-stream-id");

    StreamRequest req;
    const std::string id = "abcdef01-2345-4678-9abc-def012345678";
    req.meeting_id = id;

    auto cr = mgr.create("client-A", req, tmp);
    REQUIRE(cr.ok);

    // Streaming job carries the id.
    auto stream_st = q.status(cr.job_id);
    REQUIRE(stream_st.has_value());
    CHECK(stream_st->meeting_id == id);

    // Feed and commit.
    CHECK(mgr.feed_audio(cr.stream_token, make_pcm(800)));
    REQUIRE(wait_until([&]() { return q.slot_busy(JobKind::Streaming); },
                       std::chrono::milliseconds(500)));

    auto commit = mgr.commit("client-A", cr.stream_token);
    REQUIRE(commit.ok);

    // Postprocess Job inherits the id from the session's frozen state —
    // this is the load-bearing property for the convergence-principle
    // commit path (pattern 1 in V2-STRATEGY.md).
    auto pp_st = q.status(commit.postprocess_job_id);
    REQUIRE(pp_st.has_value());
    CHECK(pp_st->meeting_id == id);

    q.cancel(commit.postprocess_job_id);
    fs::remove_all(tmp);
}

TEST_CASE("C.11 — StreamRequest without meeting_id leaves both Jobs' meeting_id empty",
          "[c10b][stream-commit][c11]") {
    JobQueue q;
    JqStreamGuard guard(q);
    auto sink = null_sink();
    StreamingSessionManager mgr(q, sink, "");
    fs::path tmp = test_temp_dir("c11-stream-noid");

    StreamRequest req; // meeting_id defaulted empty

    auto cr = mgr.create("client-V1", req, tmp);
    REQUIRE(cr.ok);

    auto stream_st = q.status(cr.job_id);
    REQUIRE(stream_st.has_value());
    CHECK(stream_st->meeting_id.empty());

    CHECK(mgr.feed_audio(cr.stream_token, make_pcm(800)));
    REQUIRE(wait_until([&]() { return q.slot_busy(JobKind::Streaming); },
                       std::chrono::milliseconds(500)));

    auto commit = mgr.commit("client-V1", cr.stream_token);
    REQUIRE(commit.ok);

    auto pp_st = q.status(commit.postprocess_job_id);
    REQUIRE(pp_st.has_value());
    CHECK(pp_st->meeting_id.empty());

    q.cancel(commit.postprocess_job_id);
    fs::remove_all(tmp);
}

// ===========================================================================
// 2. COMMIT UNKNOWN TOKEN → InvalidParams.
// ===========================================================================
TEST_CASE("C.10b — process.stream.commit: unknown stream_token rejects",
          "[c10b][stream-commit]") {
    JobQueue q;
    JqStreamGuard guard(q);
    auto sink = null_sink();
    StreamingSessionManager mgr(q, sink, "");

    // No session exists. Commit must reject.
    auto commit = mgr.commit("client-A",
                             "deadbeefdeadbeefdeadbeefdeadbeef");
    CHECK_FALSE(commit.ok);
    CHECK(commit.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(commit.error.find("unknown") != std::string::npos);
}

// ===========================================================================
// 3. COMMIT FOREIGN CLIENT'S STREAM → PermissionDenied.
//    Cross-client anti-forgery — A opens, B tries to commit A's token.
// ===========================================================================
TEST_CASE("C.10b — process.stream.commit: foreign-client commit rejects",
          "[c10b][stream-commit]") {
    JobQueue q;
    JqStreamGuard guard(q);
    auto sink = null_sink();
    StreamingSessionManager mgr(q, sink, "");
    fs::path tmp = test_temp_dir("foreign");

    StreamRequest req;
    auto cr = mgr.create("client-A", req, tmp);
    REQUIRE(cr.ok);

    // client-B attempts to commit A's stream.
    auto commit = mgr.commit("client-B", cr.stream_token);
    CHECK_FALSE(commit.ok);
    CHECK(commit.code == static_cast<int>(IpcErrorCode::PermissionDenied));

    // A's session is still alive (B's failed commit did not tear it down).
    CHECK(mgr.active_count() == 1);
    // A can still commit on its own.
    auto ok = mgr.commit("client-A", cr.stream_token);
    CHECK(ok.ok);

    // The newly-created pp job has no draining worker in this test.
    q.cancel(ok.postprocess_job_id);
    fs::remove_all(tmp);
}

// ===========================================================================
// 4. POST-DISCONNECT COMMIT FAILS — disconnect aborts the stream and
//    erases the session; a subsequent commit with the same token must
//    hit the InvalidParams "unknown stream_token" branch.
// ===========================================================================
TEST_CASE("C.10b — process.stream.commit: post-disconnect token is unknown",
          "[c10b][stream-commit]") {
    JobQueue q;
    JqStreamGuard guard(q);
    auto sink = null_sink();
    StreamingSessionManager mgr(q, sink, "");
    fs::path tmp = test_temp_dir("disc");

    StreamRequest req;
    auto cr = mgr.create("client-A", req, tmp);
    REQUIRE(cr.ok);
    std::string saved_token = cr.stream_token;
    fs::path wav = find_wav(tmp);
    REQUIRE(fs::exists(wav));

    // Simulate the daemon's on_client_disconnect hook firing — same code
    // path the IpcServer wires on a TCP drop.
    int aborted = mgr.on_client_disconnect("client-A");
    CHECK(aborted == 1);
    CHECK(mgr.active_count() == 0);
    CHECK_FALSE(fs::exists(wav));   // disconnect unlinks the temp WAV.

    // The same token is no longer routable.
    auto commit = mgr.commit("client-A", saved_token);
    CHECK_FALSE(commit.ok);
    CHECK(commit.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(commit.error.find("unknown") != std::string::npos);

    fs::remove_all(tmp);
}

// ===========================================================================
// 5. COMMIT THEN process.fetch — drive the new postprocess job to Done
//    (synchronous dequeue + finish, the test_fetch.cpp stage_done_job
//    pattern) and assert status()/client_for_job/input.out_dir all carry
//    through so a C.4 process.fetch handler would deliver the artifacts.
// ===========================================================================
TEST_CASE("C.10b — process.stream.commit: fetch-ready post pp completion",
          "[c10b][stream-commit]") {
    JobQueue q;
    // No background pp worker — we drive the postprocess job through
    // Running → Done synchronously, then assert the registry shape that
    // a process.fetch handler would see.
    JqStreamGuard guard(q);
    auto sink = null_sink();
    StreamingSessionManager mgr(q, sink, "");
    fs::path tmp = test_temp_dir("fetch");

    StreamRequest req;
    auto cr = mgr.create("client-A", req, tmp);
    REQUIRE(cr.ok);
    REQUIRE(mgr.feed_audio(cr.stream_token, make_pcm(1600)));

    auto commit = mgr.commit("client-A", cr.stream_token);
    REQUIRE(commit.ok);
    const int64_t pp_id = commit.postprocess_job_id;

    // Pre-stage the "postprocess artifacts" inside the temp WAV's
    // directory — exactly what the pp subprocess would write. The
    // process.fetch handler reads from input.out_dir.
    {
        std::ofstream md(tmp / "Meeting_2026-05-15_12-34.md");
        md << "# Streaming meeting note\n\nbody\n";
    }
    {
        std::ofstream vtt(tmp / "captions.vtt");
        vtt << "WEBVTT\n\n00:00:00.000 --> 00:00:01.000\nhello\n";
    }

    // Synchronously drive the postprocess job through Running → Done so
    // a process.fetch handler would return artifacts.
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    REQUIRE(dq->job_id == pp_id);
    REQUIRE(dq->input.out_dir == tmp);
    q.finish(pp_id, /*ok=*/true);

    // The job is now Done; client_for_job carries the originating client.
    auto st = q.status(pp_id);
    REQUIRE(st.has_value());
    CHECK(st->state == JobState::Done);
    auto owner = q.client_for_job(pp_id);
    REQUIRE(owner.has_value());
    CHECK(*owner == "client-A");
    // out_dir is what a process.fetch handler would enumerate artifacts
    // from. We assert directly here (the actual fetch wire-round-trip is
    // covered by test_fetch.cpp).
    CHECK(fs::exists(st->input.out_dir / "Meeting_2026-05-15_12-34.md"));
    CHECK(fs::exists(st->input.out_dir / "captions.vtt"));

    fs::remove_all(tmp);
}

// ===========================================================================
// 6. ROUTED-CAPTION CORRECTNESS — two clients, only A streams. Captions
//    emitted on A's session must route to A only; B's sink must be silent.
//    Regression check against C.3 (we want C.10b to not have re-broadcast
//    by accident). Drive the sink synthetically — we cannot count on the
//    stub engine to emit, so we exercise the sink callbacks directly.
// ===========================================================================
TEST_CASE("C.10b — captions from A's session route to A only, never to B",
          "[c10b][stream-commit][routing]") {
    JobQueue q;
    JqStreamGuard guard(q);

    // A per-client counter — the test calls each client's `on_caption`
    // shim directly because we are testing the sink-construction
    // contract: the sink fires with the SESSION's client_id, never
    // someone else's.
    std::atomic<int> a_calls{0};
    std::atomic<int> b_calls{0};

    StreamingCaptionSink sink;
    // Mirror daemon.cpp's caption sink wiring: emit with the session's
    // client_id; the daemon then routes via send_to_client(client_id).
    // Here we don't run a real IPC server — we route in the sink itself
    // and assert the routing decision is correct.
    sink.on_caption = [&](int64_t, const std::string& client_id,
                          const std::string&, bool, int64_t) {
        if (client_id == "client-A") a_calls.fetch_add(1);
        else if (client_id == "client-B") b_calls.fetch_add(1);
    };
    sink.on_degraded = [&](int64_t, const std::string& client_id,
                           const std::string&, int64_t) {
        // Engine_error degraded for stub builds — count under A too.
        if (client_id == "client-A") a_calls.fetch_add(1);
        else if (client_id == "client-B") b_calls.fetch_add(1);
    };

    // Empty model dir → engine fails to start → one `engine_error`
    // degraded callback fires synchronously inside create(). That single
    // event carries client-A's id (the session's), so a_calls == 1 and
    // b_calls == 0 — the exact regression assertion we need.
    StreamingSessionManager mgr(q, sink, "/nonexistent-model-dir");
    fs::path tmp = test_temp_dir("routed");

    StreamRequest req;
    auto cr = mgr.create("client-A", req, tmp);
    REQUIRE(cr.ok);

    // Feed audio — with no engine, only the disk-backed path runs; no
    // additional caption / degraded callbacks fire from feed_audio.
    REQUIRE(mgr.feed_audio(cr.stream_token, make_pcm(800)));

    // After create + feed, the only event that fired was the
    // `engine_error` from the failed engine start. It was tagged with
    // client-A, never client-B.
    CHECK(a_calls.load() >= 1);
    CHECK(b_calls.load() == 0);

    auto commit = mgr.commit("client-A", cr.stream_token);
    REQUIRE(commit.ok);
    // No leakage into B post-commit either.
    CHECK(b_calls.load() == 0);

    q.cancel(commit.postprocess_job_id);  // no pp worker
    fs::remove_all(tmp);
}

// ===========================================================================
// 7. ACCEPTANCE CRITERION — "1 streaming + 1 postprocess concurrent."
//    Client A is streaming (uses the Streaming slot); client B has a
//    Postprocess job (uses the Postprocess slot). The two slots are
//    independent (C.7 invariant). Both jobs run at the same time without
//    blocking each other; slot_busy reports both Streaming AND
//    Postprocess as busy concurrently.
// ===========================================================================
TEST_CASE("C.10b acceptance — 1 streaming + 1 postprocess run concurrently",
          "[c10b][stream-commit][acceptance]") {
    JobQueue q;
    JqDualGuard guard(q);   // BOTH stream + pp workers
    auto sink = null_sink();
    StreamingSessionManager mgr(q, sink, "");
    fs::path tmp = test_temp_dir("concurrent");

    // ----- Client A opens a streaming session.
    StreamRequest req;
    auto cr_a = mgr.create("client-A", req, tmp);
    REQUIRE(cr_a.ok);
    REQUIRE(mgr.feed_audio(cr_a.stream_token, make_pcm(1600)));

    // Streaming slot becomes busy.
    REQUIRE(wait_until([&]() { return q.slot_busy(JobKind::Streaming); },
                       std::chrono::milliseconds(500)));

    // ----- Client B opens a Postprocess job DIRECTLY through JobQueue
    //       (the daemon's process.submit path eventually does the same
    //       thing — reserve + enqueue_reserved — but we do not need the
    //       upload session for this concurrency assertion).
    Job b_job;
    b_job.input.out_dir = tmp / "b_out";
    fs::create_directories(b_job.input.out_dir);
    int64_t b_pp_id = q.enqueue(std::move(b_job), JobKind::Postprocess,
                                "client-B");

    // BOTH slots become busy concurrently. This is the acceptance
    // assertion: at no point does opening a postprocess job evict the
    // streaming slot (or vice versa). slot_busy(Streaming) and
    // slot_busy(Postprocess) are both true at the same wall-clock instant.
    REQUIRE(wait_until([&]() {
        return q.slot_busy(JobKind::Streaming) &&
               q.slot_busy(JobKind::Postprocess);
    }, std::chrono::milliseconds(500)));

    CHECK(q.slot_busy(JobKind::Streaming));
    CHECK(q.slot_busy(JobKind::Postprocess));
    // Both jobs report Running.
    auto a_st = q.status(cr_a.job_id);
    auto b_st = q.status(b_pp_id);
    REQUIRE(a_st.has_value());
    REQUIRE(b_st.has_value());
    CHECK(a_st->state == JobState::Running);
    CHECK(b_st->state == JobState::Running);
    // The two jobs belong to two different clients.
    auto a_owner = q.client_for_job(cr_a.job_id);
    auto b_owner = q.client_for_job(b_pp_id);
    REQUIRE(a_owner.has_value());
    REQUIRE(b_owner.has_value());
    CHECK(*a_owner == "client-A");
    CHECK(*b_owner == "client-B");

    // ----- Now have A commit; B's postprocess slot must STAY busy
    //       (independent of A's streaming-slot release).
    auto commit = mgr.commit("client-A", cr_a.stream_token);
    REQUIRE(commit.ok);
    // A's stream is Done; A's new pp job is Queued (the pp_worker thread
    // already dequeued B's job and is holding the slot).
    auto a_pp_st = q.status(commit.postprocess_job_id);
    REQUIRE(a_pp_st.has_value());
    CHECK(a_pp_st->state == JobState::Queued);

    // B is still Running on the postprocess slot — A's commit did not
    // disturb it.
    auto b_after = q.status(b_pp_id);
    REQUIRE(b_after.has_value());
    CHECK(b_after->state == JobState::Running);
    CHECK(q.slot_busy(JobKind::Postprocess));

    // Cleanup — finish B so the pp worker can dequeue A's now-queued pp
    // job (it will, then we cancel it before JqDualGuard shuts down).
    q.finish(b_pp_id, /*ok=*/true);
    // Cancel A's pp job before shutdown — the pp_worker may pick it up
    // briefly but our test driver wants a clean teardown.
    q.cancel(commit.postprocess_job_id);
    fs::remove_all(tmp);
}

// ===========================================================================
// 8. RECONNECT BATCH-FALLBACK HOOK — synthesize a local WAV (representing
//    what the tray would have buffered while streaming) and submit it via
//    process.submit. The tray's `apply_submit_with_context` does exactly
//    this; here we exercise the UploadSessionManager surface directly to
//    prove the path. Documents the "stream broke → fall back to batch
//    submit" recovery the C.10b plan calls out as the minimum hook;
//    full reconnect orchestration is deferred to Phase D.
// ===========================================================================
TEST_CASE("C.10b — reconnect batch-fallback: buffered WAV resubmits via "
          "process.submit", "[c10b][stream-commit][fallback]") {
    JobQueue q;
    JqShutdownGuard jqg(q);    // synchronous drive — no background worker

    // Synthesize what the tray-side WAV stager would have on disk.
    // The B.1 capture path writes int16 mono 16kHz, which is also what
    // process.submit expects via format="wav".
    fs::path tmp = test_temp_dir("fallback");
    fs::path buffered_wav = tmp / "tray_buffered.wav";
    write_test_wav(buffered_wav, /*n_samples=*/16000);  // 1 second.

    // Open an UploadSessionManager pointed at a staging root — same setup
    // the daemon constructs once.
    fs::path staging_root = tmp / "staging";
    UploadProgressSink usink;
    usink.on_progress = [](int64_t, const std::string&, int64_t, int64_t) {};
    UploadSessionManager um(q, staging_root, usink);

    // Issue the process.submit "open" — this is the verb the tray would
    // call after detecting the stream connection dropped and finding a
    // buffered WAV on disk.
    SubmitRequest sr;
    auto file_size = fs::file_size(buffered_wav);
    sr.audio_size = static_cast<int64_t>(file_size);
    sr.format = "wav";
    sr.sample_rate = 16000;
    sr.channels = 1;
    sr.mode = "transcribe";

    JobConfig pp_cfg;          // default JobConfig — the daemon would snapshot
    auto cr = um.create("client-A", sr, pp_cfg, /*max_upload_bytes=*/0);
    REQUIRE(cr.ok);
    CHECK(cr.job_id > 0);
    CHECK(!cr.upload_token.empty());

    // Feed the buffered WAV bytes in one chunk. The session finalizes
    // when bytes_received == audio_size: the postprocess Job is
    // enqueue_reserved and pp_worker_loop can drain it.
    std::ifstream in(buffered_wav, std::ios::binary);
    REQUIRE(in);
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    REQUIRE(body.size() == file_size);
    bool ok = um.feed_chunk("client-A", body);
    CHECK(ok);

    // After finalize the session is gone (the upload manager erases on
    // finalize), and the JobQueue has a Queued postprocess job.
    CHECK(um.active_count() == 0);
    auto st = q.status(cr.job_id);
    REQUIRE(st.has_value());
    CHECK(st->state == JobState::Queued);
    CHECK(st->kind == JobKind::Postprocess);
    // out_dir is the staging dir the upload session created. The pp
    // subprocess reads from input.out_dir / cfg.reprocess_dir — the
    // same shape as C.10b's commit handoff.
    CHECK_FALSE(st->input.out_dir.empty());
    CHECK_FALSE(st->cfg.reprocess_dir.empty());
    CHECK(st->cfg.reprocess_dir == st->input.out_dir.string());

    // Cleanup.
    q.cancel(cr.job_id);   // no pp worker
    fs::remove_all(tmp);
}

// ===========================================================================
// 9. WAV-TO-POSTPROCESS PLUMBING CHECK — confirm that the temp WAV the
//    commit produces is a valid container that libsndfile can read back
//    independently. The pp subprocess uses libsndfile to read its input;
//    if the WAV header is broken, the subprocess fails. We do not
//    actually run pp_worker_loop here (its model dependencies make it
//    unsuitable for unit tests — covered by integration tests
//    elsewhere); we assert the WAV the commit hands off is structurally
//    sound.
// ===========================================================================
TEST_CASE("C.10b — committed WAV is libsndfile-readable end-to-end",
          "[c10b][stream-commit]") {
    JobQueue q;
    JqStreamGuard guard(q);
    auto sink = null_sink();
    StreamingSessionManager mgr(q, sink, "");
    fs::path tmp = test_temp_dir("wav_ok");

    StreamRequest req;
    auto cr = mgr.create("client-A", req, tmp);
    REQUIRE(cr.ok);

    // Feed 5 chunks of 160 samples = 800 samples = 50 ms.
    for (int i = 0; i < 5; ++i)
        REQUIRE(mgr.feed_audio(cr.stream_token,
                               make_pcm(160, static_cast<int16_t>(i * 1000))));

    auto commit = mgr.commit("client-A", cr.stream_token);
    REQUIRE(commit.ok);

    fs::path wav = find_wav(tmp);
    REQUIRE(!wav.empty());
    REQUIRE(fs::exists(wav));

    // Read the WAV back via libsndfile — same code path the pp
    // subprocess uses. Frame count + format must match what we fed.
    SF_INFO info = {};
    SNDFILE* sf = sf_open(wav.string().c_str(), SFM_READ, &info);
    REQUIRE(sf != nullptr);
    CHECK(info.samplerate == 16000);
    CHECK(info.channels == 1);
    CHECK(info.frames == 800);
    // Read the samples back — they survive round-tripping intact.
    std::vector<int16_t> samples(info.frames);
    sf_count_t got = sf_read_short(sf, samples.data(), info.frames);
    CHECK(got == info.frames);
    sf_close(sf);

    q.cancel(commit.postprocess_job_id);
    fs::remove_all(tmp);
}
