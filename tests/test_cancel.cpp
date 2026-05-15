// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.5 — `process.cancel` tests.
//
// `process.cancel` is the unified cancellation surface added in C.5. It takes
// a single `job_id` and routes by kind + state to the right teardown path:
// JobQueue::cancel for Queued / WaitingOnDownload / ModelDownload paths;
// UploadSessionManager::cancel_by_job_id for an in-flight upload reservation;
// StreamingSessionManager::cancel_by_job_id for an active streaming session;
// JobQueue::cancel + SIGTERM-equivalent stop-token for a running postprocess.
//
// These tests exercise the manager-level cancel paths directly. The IPC
// dispatch state-machine (the `process.cancel` handler body in daemon.cpp) is
// re-implemented as a small `dispatch_process_cancel()` helper in this file
// so we can validate both the routing (which path got hit for which state)
// and the manager-side teardown (slot released, file unlinked, verdict
// preserved) without standing up the daemon binary — that path has no model
// files, no subprocess fork/exec, and no socket I/O dependency.
//
// Thread hygiene (orchestrator rule 5): every std::thread is owned by a RAII
// guard that joins on destruction. ~Guard runs even when a REQUIRE throws.

#include <catch2/catch_test_macros.hpp>

#include "config.h"
#include "ipc_protocol.h"
#include "job_queue.h"
#include "streaming_session.h"
#include "upload_session.h"
#include "util.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <unistd.h>

using namespace recmeet;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// SigpipeIgnorer — defensive (no socket I/O here, but matches sibling tests).
// ---------------------------------------------------------------------------
struct SigpipeIgnorer {
    SigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} s_ignore_sigpipe;

// ---------------------------------------------------------------------------
// CancelDispatchOutcome — what `dispatch_process_cancel()` returns. Mirrors
// the handler's IpcError + ok shape. The state-machine table the handler
// implements is asserted by the tests inspecting this struct.
// ---------------------------------------------------------------------------
struct CancelDispatchOutcome {
    bool ok = false;
    int  code = 0;            ///< IpcErrorCode value when !ok.
    std::string message;
};

// ---------------------------------------------------------------------------
// StopProbe — observable surrogate for the `g_pp_stop` + SIGTERM kick that
// the production handler uses for a Running Postprocess job. We capture the
// fact-of-call and any child_pid that was signalled; the tests then assert
// the right state-machine branch fired.
// ---------------------------------------------------------------------------
struct StopProbe {
    std::atomic<int>   stop_requested{0};
    std::atomic<int>   sigterm_calls{0};
    std::atomic<pid_t> last_signalled_pid{0};

    void request_stop() { stop_requested.fetch_add(1); }
    void kick_pid(pid_t pid) {
        sigterm_calls.fetch_add(1);
        last_signalled_pid.store(pid);
    }
};

// ---------------------------------------------------------------------------
// dispatch_process_cancel — pure C++ port of the `process.cancel` IPC
// handler body in src/daemon.cpp. The handler uses globals (g_jobs /
// g_streaming / g_uploads / g_pp_stop / g_pp_child_pid); here we accept
// dependency-injected references so the test can wire each scenario.
//
// The handler's behavior table (kept in lockstep with daemon.cpp):
//   (1) job_id missing/non-positive   -> InvalidParams
//   (2) unknown job_id                -> InvalidParams
//   (3) owner != requester            -> PermissionDenied
//   (4) terminal state (Done/Failed/Cancelled) -> InvalidParams
//   (5) Postprocess + Queued/WaitingOnDownload -> jobs.cancel
//   (5) Postprocess + WaitingForUpload -> uploads.cancel_by_job_id +
//                                          jobs.cancel
//   (5) Postprocess + Running         -> jobs.cancel + stop.request +
//                                          SIGTERM(child)
//   (5) Streaming   + non-terminal    -> streaming.cancel_by_job_id
//   (5) ModelDownload + non-terminal  -> jobs.cancel
// ---------------------------------------------------------------------------
CancelDispatchOutcome dispatch_process_cancel(
        JobQueue& jobs,
        UploadSessionManager* uploads,
        StreamingSessionManager* streaming,
        StopProbe* pp_stop_probe,
        std::atomic<pid_t>* pp_child_pid,
        int64_t job_id,
        const std::string& requester_client_id) {
    CancelDispatchOutcome out;

    if (job_id <= 0) {
        out.code = static_cast<int>(IpcErrorCode::InvalidParams);
        out.message = "process.cancel: 'job_id' must be a positive integer";
        return out;
    }
    auto snap = jobs.status(job_id);
    if (!snap.has_value()) {
        out.code = static_cast<int>(IpcErrorCode::InvalidParams);
        out.message = "process.cancel: unknown job_id";
        return out;
    }
    auto owner = jobs.client_for_job(job_id);
    if (!owner.has_value() || *owner != requester_client_id) {
        out.code = static_cast<int>(IpcErrorCode::PermissionDenied);
        out.message = "process.cancel: job is not owned by this client";
        return out;
    }
    if (snap->state == JobState::Done ||
        snap->state == JobState::Failed ||
        snap->state == JobState::Cancelled) {
        out.code = static_cast<int>(IpcErrorCode::InvalidParams);
        out.message = std::string("process.cancel: job is already in "
                                  "terminal state ")
                    + job_state_name(snap->state);
        return out;
    }

    switch (snap->kind) {
    case JobKind::Postprocess:
        switch (snap->state) {
        case JobState::Queued:
        case JobState::WaitingOnDownload:
            jobs.cancel(job_id);
            break;
        case JobState::WaitingForUpload:
            if (uploads) uploads->cancel_by_job_id(job_id);
            jobs.cancel(job_id);
            break;
        case JobState::Running:
            jobs.cancel(job_id);
            if (pp_stop_probe) pp_stop_probe->request_stop();
            if (pp_child_pid) {
                pid_t c = pp_child_pid->load();
                if (c > 0 && pp_stop_probe) pp_stop_probe->kick_pid(c);
            }
            break;
        default: break;
        }
        break;
    case JobKind::Streaming:
        if (streaming) streaming->cancel_by_job_id(job_id);
        else           jobs.cancel(job_id);
        break;
    case JobKind::ModelDownload:
        jobs.cancel(job_id);
        break;
    case JobKind::_count:
        break;
    }
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// ThreadGuard — joins worker threads on destruction even when REQUIRE throws.
// Same shape as test_job_queue.cpp's ThreadGuard.
// ---------------------------------------------------------------------------
struct ThreadGuard {
    JobQueue& q;
    std::deque<std::thread> threads;
    explicit ThreadGuard(JobQueue& q_) : q(q_) {}
    template <typename F>
    std::thread& spawn(F&& f) {
        threads.emplace_back(std::forward<F>(f));
        return threads.back();
    }
    ~ThreadGuard() {
        q.shutdown();
        for (auto& t : threads) if (t.joinable()) t.join();
    }
};

// A streaming-slot drain worker. Mirrors the daemon's stream_worker_loop:
// dequeue -> idle loop -> exit on shutdown. Joined via ~StreamDrainGuard.
struct StreamDrainGuard {
    JobQueue& q;
    std::thread worker;
    explicit StreamDrainGuard(JobQueue& q_) : q(q_) {
        worker = std::thread([this]() {
            for (;;) {
                auto dq = q.dequeue(JobKind::Streaming);
                if (!dq.has_value()) return;
            }
        });
    }
    ~StreamDrainGuard() {
        q.shutdown();
        if (worker.joinable()) worker.join();
    }
};

// A no-op caption sink — none of these tests assert on caption text.
StreamingCaptionSink null_caption_sink() {
    StreamingCaptionSink s;
    s.on_caption  = [](int64_t, const std::string&, const std::string&,
                       bool, int64_t) {};
    s.on_degraded = [](int64_t, const std::string&, const std::string&,
                       int64_t) {};
    return s;
}

// A no-op progress sink for the upload manager.
UploadProgressSink null_progress_sink() {
    UploadProgressSink s;
    s.on_progress = [](int64_t, const std::string&, int64_t, int64_t) {};
    return s;
}

// Build a postprocess Job with no model dependency.
Job make_pp_job() {
    Job j;
    j.input.transcript_text = "already transcribed";
    return j;
}

// Build a SubmitRequest of `audio_size` bytes of s16le PCM.
SubmitRequest default_submit_req(int64_t audio_size) {
    SubmitRequest r;
    r.audio_size  = audio_size;
    r.format      = "s16le";
    r.sample_rate = 16000;
    r.channels    = 1;
    r.mode        = "transcribe";
    return r;
}

// A unique temp dir for the test's staging files / WAVs.
fs::path test_temp_dir(const std::string& tag) {
    fs::path d = fs::temp_directory_path() / ("recmeet_c5_test_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

// Build a deterministic S16LE PCM payload.
std::string make_pcm(size_t n_samples, int16_t start = 0) {
    std::string s;
    s.resize(n_samples * sizeof(int16_t));
    auto* p = reinterpret_cast<int16_t*>(s.data());
    for (size_t i = 0; i < n_samples; ++i)
        p[i] = static_cast<int16_t>(start + static_cast<int16_t>(i));
    return s;
}

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

} // anonymous namespace

// ===========================================================================
// 1. Cancel a Queued postprocess job — state Cancelled; the job never runs.
// ===========================================================================
TEST_CASE("process.cancel: queued postprocess job is cancelled and skipped "
          "by dequeue", "[c5][cancel]") {
    JobQueue q;
    ThreadGuard guard(q);

    // Two jobs from the same client; both Queued behind the capacity-1 slot.
    // We cancel the head BEFORE any worker dequeues so the cancel races no
    // worker — the head is Queued.
    int64_t j1 = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");
    int64_t j2 = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");
    REQUIRE(q.status(j1)->state == JobState::Queued);

    auto r = dispatch_process_cancel(q, /*uploads=*/nullptr,
                                     /*streaming=*/nullptr,
                                     /*stop_probe=*/nullptr,
                                     /*child_pid=*/nullptr,
                                     j1, "alice");
    REQUIRE(r.ok);
    REQUIRE(q.status(j1)->state == JobState::Cancelled);

    // dequeue's lazy-removal path drops the cancelled head and returns j2.
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    REQUIRE(dq->job_id == j2);
    q.finish(j2, true);
}

// ===========================================================================
// 2. Concurrent producer + cancel — enqueue followed immediately by cancel
//    before any worker dequeues. Lazy-removal must keep the verdict.
// ===========================================================================
TEST_CASE("process.cancel: enqueue immediately followed by cancel — verdict "
          "survives the lazy-removal path", "[c5][cancel]") {
    JobQueue q;
    ThreadGuard guard(q);

    // No worker yet — just produce + cancel back-to-back.
    int64_t j = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");
    auto r = dispatch_process_cancel(q, nullptr, nullptr, nullptr, nullptr,
                                     j, "alice");
    REQUIRE(r.ok);
    REQUIRE(q.status(j)->state == JobState::Cancelled);

    // Now bring a worker online: dequeue must NOT hand out the cancelled
    // job. We give the worker something else to find so we can prove it
    // didn't get our cancelled job.
    int64_t j2 = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");

    std::atomic<int64_t> got{-1};
    std::thread& worker = guard.spawn([&] {
        auto dq = q.dequeue(JobKind::Postprocess);
        if (dq.has_value()) got = dq->job_id;
    });
    REQUIRE(wait_until([&] { return got.load() == j2; }, 500ms));
    q.finish(j2, true);
    worker.join();
}

// ===========================================================================
// 3. Cancel a Running postprocess job — child receives SIGTERM (probe);
//    Cancelled verdict survives the worker's later finish(false, "cancelled").
// ===========================================================================
TEST_CASE("process.cancel: running postprocess job — SIGTERM kick + verdict "
          "preserved by finish()", "[c5][cancel]") {
    JobQueue q;
    ThreadGuard guard(q);

    int64_t j = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");
    auto dq = q.dequeue(JobKind::Postprocess);   // job is now Running.
    REQUIRE(dq.has_value());
    REQUIRE(q.status(j)->state == JobState::Running);

    StopProbe probe;
    std::atomic<pid_t> child{42};     // surrogate child pid.

    auto r = dispatch_process_cancel(q, nullptr, nullptr, &probe, &child,
                                     j, "alice");
    REQUIRE(r.ok);
    CHECK(probe.stop_requested.load() == 1);
    CHECK(probe.sigterm_calls.load() == 1);
    CHECK(probe.last_signalled_pid.load() == 42);
    REQUIRE(q.status(j)->state == JobState::Cancelled);

    // Worker observes the Cancelled verdict and calls finish(false,
    // "cancelled"). Because state is already Cancelled, finish() preserves
    // it (does NOT flip to Failed) — that's the pp_worker_loop exit-code-2
    // contract.
    q.finish(j, /*ok=*/false, "cancelled");
    REQUIRE(q.status(j)->state == JobState::Cancelled);

    // Slot is now free for the next job.
    CHECK_FALSE(q.slot_busy(JobKind::Postprocess));
}

// ===========================================================================
// 4. Cancel a Running postprocess job WITHOUT a live child pid — SIGTERM
//    must be guarded by a child>0 check (no spurious kill of pid 0/-1).
// ===========================================================================
TEST_CASE("process.cancel: running postprocess job — no SIGTERM when child "
          "pid is unset", "[c5][cancel]") {
    JobQueue q;
    ThreadGuard guard(q);

    int64_t j = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());

    StopProbe probe;
    std::atomic<pid_t> child{-1};      // no child yet (process exited or
                                       // not forked).

    auto r = dispatch_process_cancel(q, nullptr, nullptr, &probe, &child,
                                     j, "alice");
    REQUIRE(r.ok);
    CHECK(probe.stop_requested.load() == 1);
    CHECK(probe.sigterm_calls.load() == 0);   // child<=0 path: NO kick.
    REQUIRE(q.status(j)->state == JobState::Cancelled);

    q.finish(j, false, "cancelled");
}

// ===========================================================================
// 5. Cancel a WaitingForUpload reservation — upload session torn down,
//    staging file unlinked, state Cancelled.
// ===========================================================================
TEST_CASE("process.cancel: WaitingForUpload — upload session torn down",
          "[c5][cancel]") {
    JobQueue q;
    ThreadGuard guard(q);
    fs::path tmp = test_temp_dir("upload_cancel");

    UploadSessionManager uploads(q, tmp, null_progress_sink());

    Config cfg;
    auto cr = uploads.create("alice", default_submit_req(/*audio_size=*/1024),
                             cfg, /*max_upload_bytes=*/0);
    REQUIRE(cr.ok);
    REQUIRE(q.status(cr.job_id)->state == JobState::WaitingForUpload);
    REQUIRE(uploads.active_count() == 1);

    // Find the staging file path before cancel unlinks it.
    fs::path staging_audio;
    {
        // The session is in the manager — we can ask for its bytes_received
        // (>=0 means alive) and locate via directory scan.
        REQUIRE(uploads.bytes_received_for_token(cr.upload_token) == 0);
        for (auto& e : fs::recursive_directory_iterator(tmp))
            if (e.is_regular_file()) { staging_audio = e.path(); break; }
        // Some libsndfile + format combos defer the file's first byte until
        // a write — accept "exists or empty path" as the pre-state.
    }

    auto r = dispatch_process_cancel(q, &uploads, nullptr, nullptr, nullptr,
                                     cr.job_id, "alice");
    REQUIRE(r.ok);
    CHECK(uploads.active_count() == 0);
    REQUIRE(q.status(cr.job_id)->state == JobState::Cancelled);

    // The staging audio file should no longer exist (cancel teardown
    // closes + unlinks it).
    if (!staging_audio.empty())
        CHECK_FALSE(fs::exists(staging_audio));

    fs::remove_all(tmp);
}

// ===========================================================================
// 6. Cancel a streaming job — engine stopped, temp WAV unlinked, slot
//    released, state Cancelled. AND it must behave the same as a direct
//    process.stream.cancel.
// ===========================================================================
TEST_CASE("process.cancel: streaming job — equivalent to process.stream.cancel",
          "[c5][cancel]") {
    JobQueue q;
    StreamDrainGuard guard(q);
    fs::path tmp = test_temp_dir("streaming_cancel");

    auto sink = null_caption_sink();
    StreamingSessionManager mgr(q, sink, "");   // empty model dir — stub engine

    StreamRequest sr;
    auto res = mgr.create("alice", sr, tmp);
    REQUIRE(res.ok);
    REQUIRE(mgr.active_count() == 1);

    // Capture the temp WAV path before cancel unlinks it.
    fs::path wav;
    for (auto& e : fs::directory_iterator(tmp))
        if (e.path().extension() == ".wav") wav = e.path();
    REQUIRE(!wav.empty());

    // Wait for the slot to be busy (the JqGuard worker may not have flipped
    // it the instant create() returned). The C.7 manager-side dequeue is
    // explicit.
    REQUIRE(wait_until([&] { return q.slot_busy(JobKind::Streaming); }, 500ms));

    auto r = dispatch_process_cancel(q, nullptr, &mgr, nullptr, nullptr,
                                     res.job_id, "alice");
    REQUIRE(r.ok);
    CHECK(mgr.active_count() == 0);
    REQUIRE(q.status(res.job_id)->state == JobState::Cancelled);
    CHECK_FALSE(fs::exists(wav));   // unlinked by manager teardown.
    // Slot is released so the next process.stream may proceed.
    CHECK(wait_until([&] { return !q.slot_busy(JobKind::Streaming); }, 500ms));

    fs::remove_all(tmp);
}

// Independent run via the narrower verb — same end state, no daemon-handler
// dispatch. Asserts the equivalence claim in the spec.
TEST_CASE("process.cancel: streaming-cancel equivalence with the narrower "
          "process.stream.cancel verb", "[c5][cancel]") {
    JobQueue q;
    StreamDrainGuard guard(q);
    fs::path tmp = test_temp_dir("streaming_eq");

    auto sink = null_caption_sink();
    StreamingSessionManager mgr(q, sink, "");

    StreamRequest sr;
    auto res = mgr.create("alice", sr, tmp);
    REQUIRE(res.ok);
    fs::path wav;
    for (auto& e : fs::directory_iterator(tmp))
        if (e.path().extension() == ".wav") wav = e.path();
    REQUIRE(!wav.empty());
    REQUIRE(wait_until([&] { return q.slot_busy(JobKind::Streaming); }, 500ms));

    // The narrower path — token-keyed.
    REQUIRE(mgr.cancel("alice", res.stream_token));
    CHECK(mgr.active_count() == 0);
    REQUIRE(q.status(res.job_id)->state == JobState::Cancelled);
    CHECK_FALSE(fs::exists(wav));

    fs::remove_all(tmp);
}

// ===========================================================================
// 7. Cancel a ModelDownload job — dependents (parked postprocess) land
//    Failed via finish_download.
// ===========================================================================
TEST_CASE("process.cancel: ModelDownload — dependents fail through "
          "finish_download", "[c5][cancel]") {
    JobQueue q;
    ThreadGuard guard(q);

    q.set_model_resolver([](const Job&) -> std::vector<std::string> {
        return {"whisper/base.en"};
    });
    q.set_model_cache_checker([](const std::string&) -> bool { return false; });

    int64_t pp = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");

    // Park the postprocess job on a download: a worker dequeues Postprocess
    // (which auto-enqueues the download and blocks waiting on it).
    std::thread& pp_worker = guard.spawn([&] {
        auto j = q.dequeue(JobKind::Postprocess);
        (void)j;   // expected: shutdown returns nullopt OR a re-armed job.
    });

    auto dl = q.dequeue(JobKind::ModelDownload);
    REQUIRE(dl.has_value());
    REQUIRE(dl->kind == JobKind::ModelDownload);
    REQUIRE(wait_until(
        [&] { return q.status(pp)->state == JobState::WaitingOnDownload; },
        500ms));

    // The download job is owned by the same client as its parent (C.7).
    REQUIRE(*q.client_for_job(dl->job_id) == "alice");

    auto r = dispatch_process_cancel(q, nullptr, nullptr, nullptr, nullptr,
                                     dl->job_id, "alice");
    REQUIRE(r.ok);
    REQUIRE(q.status(dl->job_id)->state == JobState::Cancelled);

    // Now drive finish_download(false) the way the model_dl_worker would
    // when it observes the Cancelled verdict: per the JobQueue contract,
    // finish_download() with ok=false fails every dependent.
    q.finish_download(dl->job_id, /*ok=*/false, "cancelled");

    // Dependent postprocess job lands Failed (the finish_download path
    // does NOT re-flip a Cancelled download to Done; the dependent fails).
    auto pp_st = q.status(pp);
    REQUIRE(pp_st.has_value());
    CHECK(pp_st->state == JobState::Failed);

    q.shutdown();
    if (pp_worker.joinable()) pp_worker.join();
}

// ===========================================================================
// 8. Permission check — client B cancels A's job → PermissionDenied;
//    A's job is unaffected.
// ===========================================================================
TEST_CASE("process.cancel: PermissionDenied when requester is not the owner",
          "[c5][cancel]") {
    JobQueue q;
    ThreadGuard guard(q);

    int64_t j = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");
    REQUIRE(q.status(j)->state == JobState::Queued);

    // Bob (different client) tries to cancel alice's job.
    auto r = dispatch_process_cancel(q, nullptr, nullptr, nullptr, nullptr,
                                     j, "bob");
    REQUIRE_FALSE(r.ok);
    CHECK(r.code == static_cast<int>(IpcErrorCode::PermissionDenied));
    // Alice's job is unaffected.
    REQUIRE(q.status(j)->state == JobState::Queued);

    // Alice can cancel her own job.
    auto r_ok = dispatch_process_cancel(q, nullptr, nullptr, nullptr, nullptr,
                                        j, "alice");
    REQUIRE(r_ok.ok);
    REQUIRE(q.status(j)->state == JobState::Cancelled);
}

// ===========================================================================
// 9. Unknown job_id → InvalidParams.
// ===========================================================================
TEST_CASE("process.cancel: unknown job_id is InvalidParams", "[c5][cancel]") {
    JobQueue q;

    auto r = dispatch_process_cancel(q, nullptr, nullptr, nullptr, nullptr,
                                     /*job_id=*/424242, "alice");
    REQUIRE_FALSE(r.ok);
    CHECK(r.code == static_cast<int>(IpcErrorCode::InvalidParams));

    // job_id == 0 / negative are also rejected at the parse layer.
    auto r0 = dispatch_process_cancel(q, nullptr, nullptr, nullptr, nullptr,
                                      0, "alice");
    CHECK_FALSE(r0.ok);
    CHECK(r0.code == static_cast<int>(IpcErrorCode::InvalidParams));

    auto rneg = dispatch_process_cancel(q, nullptr, nullptr, nullptr, nullptr,
                                        -1, "alice");
    CHECK_FALSE(rneg.ok);
    CHECK(rneg.code == static_cast<int>(IpcErrorCode::InvalidParams));
}

// ===========================================================================
// 10. Already-terminal job → InvalidParams.
// ===========================================================================
TEST_CASE("process.cancel: already-terminal job is InvalidParams",
          "[c5][cancel]") {
    JobQueue q;
    ThreadGuard guard(q);

    int64_t j = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    q.finish(j, true);   // -> Done
    REQUIRE(q.status(j)->state == JobState::Done);

    auto r = dispatch_process_cancel(q, nullptr, nullptr, nullptr, nullptr,
                                     j, "alice");
    REQUIRE_FALSE(r.ok);
    CHECK(r.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(r.message.find("terminal") != std::string::npos);

    // Failed terminal is similarly rejected.
    int64_t k = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");
    auto dq2 = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq2.has_value());
    q.finish(k, false, "boom");

    auto r2 = dispatch_process_cancel(q, nullptr, nullptr, nullptr, nullptr,
                                      k, "alice");
    REQUIRE_FALSE(r2.ok);
    CHECK(r2.code == static_cast<int>(IpcErrorCode::InvalidParams));
}

// ===========================================================================
// 11. Multiple concurrent cancels on the same job — second cancel is a no-op
//     (InvalidParams: already cancelled).
// ===========================================================================
TEST_CASE("process.cancel: double-cancel — second call rejects as terminal",
          "[c5][cancel]") {
    JobQueue q;
    ThreadGuard guard(q);

    int64_t j = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");

    auto first = dispatch_process_cancel(q, nullptr, nullptr, nullptr, nullptr,
                                         j, "alice");
    REQUIRE(first.ok);
    REQUIRE(q.status(j)->state == JobState::Cancelled);

    auto second = dispatch_process_cancel(q, nullptr, nullptr, nullptr, nullptr,
                                          j, "alice");
    REQUIRE_FALSE(second.ok);
    CHECK(second.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(second.message.find("terminal") != std::string::npos);
    CHECK(second.message.find("cancelled") != std::string::npos);

    // Concurrent racers — two threads call cancel simultaneously. At most
    // one wins; the loser sees the terminal-state reject. (The state is
    // serialized by the JobQueue mutex, so this is well-defined.)
    int64_t k = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");
    std::atomic<int> wins{0};
    std::deque<std::thread> threads;
    auto runner = [&]() {
        auto r = dispatch_process_cancel(q, nullptr, nullptr, nullptr,
                                         nullptr, k, "alice");
        if (r.ok) wins.fetch_add(1);
    };
    threads.emplace_back(runner);
    threads.emplace_back(runner);
    threads.emplace_back(runner);
    for (auto& t : threads) if (t.joinable()) t.join();
    CHECK(wins.load() == 1);
    REQUIRE(q.status(k)->state == JobState::Cancelled);
}

// ===========================================================================
// 12. Manager API direct: cancel_by_job_id returns false for unknown id.
//     Documents the contract that the daemon handler relies on (no live
//     session bound to job_id is a benign signal, not an error).
// ===========================================================================
TEST_CASE("process.cancel: cancel_by_job_id returns false on unknown id "
          "(both managers)", "[c5][cancel]") {
    JobQueue q;
    StreamDrainGuard guard(q);
    fs::path tmp = test_temp_dir("cbji_unknown");

    auto cap_sink = null_caption_sink();
    StreamingSessionManager smgr(q, cap_sink, "");
    UploadSessionManager umgr(q, tmp, null_progress_sink());

    CHECK_FALSE(smgr.cancel_by_job_id(999));
    CHECK_FALSE(umgr.cancel_by_job_id(999));

    // Create then cancel; the second cancel_by_job_id returns false because
    // the session is gone.
    StreamRequest sr;
    auto res = smgr.create("alice", sr, tmp);
    REQUIRE(res.ok);
    REQUIRE(wait_until([&] { return q.slot_busy(JobKind::Streaming); }, 500ms));

    REQUIRE(smgr.cancel_by_job_id(res.job_id));
    CHECK_FALSE(smgr.cancel_by_job_id(res.job_id));  // gone.

    fs::remove_all(tmp);
}
