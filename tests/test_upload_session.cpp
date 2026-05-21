// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.2 — upload-session core tests.
//
// Two layers (mirrors test_streaming_session.cpp):
//   * UploadSessionManager unit tests — exercise create / feed_chunk /
//     cancel / on_client_disconnect against a real JobQueue, deterministic
//     and socket-free.
//   * Wire round-trip tests — drive a real IpcServer + IpcClient over a
//     Unix socket and assert `0x01` upload frames + `process.submit` /
//     `process.submit.cancel` verbs behave end-to-end, including NDJSON
//     interleaved mid-`0x01`-upload (the C.1 FrameReader contract).
//
// Thread hygiene (orchestrator rule 5): every std::thread spawned here is
// owned by a RAII guard that joins on destruction, so a REQUIRE throwing
// between thread construction and join cannot call std::terminate(). The
// JobQueue postprocess-worker drain is wrapped in PpDrainGuard; the
// IpcServer poll thread via ServerGuard.

#include <catch2/catch_test_macros.hpp>

#include "config.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"
#include "meeting_index.h"
#include "pipeline.h"          // load_meeting_id
#include "upload_session.h"
#include "util.h"

#include "daemon_test_harness.h"   // Phase 2b ext — wire tests drive the
                                   // real process.submit handler.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sndfile.h>
#include <signal.h>
#include <unistd.h>

using namespace recmeet;
namespace fs = std::filesystem;

namespace {

// Ignore SIGPIPE so a write to a dropped socket does not kill the process.
struct SigpipeIgnorer {
    SigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} s_ignore_sigpipe;

// ---------------------------------------------------------------------------
// PpDrainGuard — drains the JobQueue postprocess slot during a test so
// finalize() can place a job into the FIFO and (for the test's purposes) a
// dequeue is observable. We DO NOT run the actual reprocess subprocess; the
// worker just dequeues, immediately calls finish(true), and loops. That is
// enough to prove the upload manager handed control to JobQueue cleanly.
// Joined on destruction via JobQueue::shutdown().
// ---------------------------------------------------------------------------
struct PpDrainGuard {
    JobQueue& q;
    std::thread worker;
    std::atomic<int> dequeued{0};
    explicit PpDrainGuard(JobQueue& q_) : q(q_) {
        worker = std::thread([this]() {
            for (;;) {
                auto dq = q.dequeue(JobKind::Postprocess);
                if (!dq.has_value()) return;  // shutdown
                int64_t id = dq->job_id;
                dequeued.fetch_add(1);
                // Mark immediately Done so the slot frees and the next job
                // (if any) can be picked up. Real pp_worker_loop runs a
                // subprocess; tests don't.
                q.finish(id, /*ok=*/true);
            }
        });
    }
    ~PpDrainGuard() {
        q.shutdown();
        if (worker.joinable()) worker.join();
    }
};

// No-op progress sink — most unit tests don't assert on progress.
UploadProgressSink null_progress_sink() {
    UploadProgressSink s;
    s.on_progress = [](int64_t, const std::string&, int64_t, int64_t) {};
    return s;
}

// Counting progress sink for tests that DO assert on progress emission.
struct CountingProgressSink {
    std::mutex mtx;
    int calls = 0;
    int64_t last_bytes_received = 0;
    int64_t last_audio_size = 0;
    int64_t last_job_id = 0;
    std::string last_client_id;
    UploadProgressSink make() {
        UploadProgressSink s;
        s.on_progress = [this](int64_t job_id, const std::string& client_id,
                               int64_t bytes, int64_t total) {
            std::lock_guard<std::mutex> lk(mtx);
            ++calls;
            last_bytes_received = bytes;
            last_audio_size = total;
            last_job_id = job_id;
            last_client_id = client_id;
        };
        return s;
    }
};

// Wait until `pred()` is true or the deadline passes.
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// Build a deterministic S16LE PCM payload of `n_samples` int16 samples.
std::string make_pcm(size_t n_samples, int16_t start = 0) {
    std::string s;
    s.resize(n_samples * sizeof(int16_t));
    auto* p = reinterpret_cast<int16_t*>(s.data());
    for (size_t i = 0; i < n_samples; ++i)
        p[i] = static_cast<int16_t>(start + static_cast<int16_t>(i));
    return s;
}

// A unique temp dir for a test's staging files.
fs::path test_temp_dir(const std::string& tag) {
    fs::path d = fs::temp_directory_path() / ("recmeet_upload_test_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);   // start clean
    fs::create_directories(d);
    return d;
}

// Build a SubmitRequest with sane defaults for raw s16le.
SubmitRequest default_req(int64_t audio_size) {
    SubmitRequest r;
    r.audio_size = audio_size;
    r.format = "s16le";
    r.sample_rate = 16000;
    r.channels = 1;
    r.mode = "transcribe";
    return r;
}

} // anonymous namespace

// ===========================================================================
// 1. create() success: reserves a job_id, mints a crypto-random upload_token,
//    opens the staging dir + audio file, reports max_size.
// ===========================================================================
TEST_CASE("UploadSession: create reserves job and returns token",
          "[upload-session]") {
    JobQueue q;
    PpDrainGuard guard(q);
    UploadSessionManager mgr(q, test_temp_dir("create"), null_progress_sink());

    JobConfig cfg;
    auto req = default_req(/*audio_size=*/3200);  // 100ms of 16kHz mono s16
    auto res = mgr.create("client-A", req, cfg, /*max_upload_bytes=*/1<<20);

    REQUIRE(res.ok);
    CHECK(res.job_id > 0);
    CHECK(res.upload_token.size() == 32);           // 128-bit hex token
    CHECK(res.max_size == (1 << 20));
    CHECK(mgr.active_count() == 1);
    CHECK(mgr.job_id_for_token(res.upload_token) == res.job_id);
    CHECK(mgr.token_for_client("client-A") == res.upload_token);
    CHECK(mgr.bytes_received_for_token(res.upload_token) == 0);

    // The reservation exists in JobQueue's registry, in the sentinel state.
    auto st = q.status(res.job_id);
    REQUIRE(st.has_value());
    CHECK(st->state == JobState::WaitingForUpload);
    CHECK(st->kind == JobKind::Postprocess);
    CHECK(st->client_id == "client-A");

    // The reservation does NOT occupy the postprocess slot — no FIFO entry,
    // no "running" marker. The slot is free.
    CHECK_FALSE(q.slot_busy(JobKind::Postprocess));
    CHECK(q.queued_count(JobKind::Postprocess) == 0);

    // Clean up.
    CHECK(mgr.cancel("client-A", res.upload_token));
}

// ===========================================================================
// 2. Format validation: enroll mode without enroll_name is rejected;
//    unknown mode is rejected; unsupported format is rejected.
//
// Pre-C.8 this case rejected enroll mode entirely with a "C.8 not landed"
// message. C.8 wires enroll mode, but still rejects when `enroll_name` is
// missing (the eventual enroll.finalize requires it).
// ===========================================================================
TEST_CASE("UploadSession: validation rejects enroll mode and unknown formats",
          "[upload-session]") {
    JobQueue q;
    PpDrainGuard guard(q);
    UploadSessionManager mgr(q, test_temp_dir("validate"), null_progress_sink());
    JobConfig cfg;

    // mode=="enroll" with no enroll_name → InvalidParams.
    {
        auto r = default_req(1024);
        r.mode = "enroll";
        // r.enroll_name deliberately empty
        auto res = mgr.create("c", r, cfg, 1<<20);
        CHECK_FALSE(res.ok);
        CHECK(res.code == static_cast<int>(IpcErrorCode::InvalidParams));
        CHECK(res.error.find("enroll_name") != std::string::npos);
    }
    // Unknown mode → InvalidParams.
    {
        auto r = default_req(1024);
        r.mode = "summarize-only";
        auto res = mgr.create("c", r, cfg, 1<<20);
        CHECK_FALSE(res.ok);
        CHECK(res.code == static_cast<int>(IpcErrorCode::InvalidParams));
    }
    // Unknown format → InvalidParams.
    {
        auto r = default_req(1024);
        r.format = "opus-but-not-really";
        auto res = mgr.create("c", r, cfg, 1<<20);
        CHECK_FALSE(res.ok);
        CHECK(res.code == static_cast<int>(IpcErrorCode::InvalidParams));
    }
    // audio_size <= 0 → InvalidParams.
    {
        auto r = default_req(0);
        auto res = mgr.create("c", r, cfg, 1<<20);
        CHECK_FALSE(res.ok);
        CHECK(res.code == static_cast<int>(IpcErrorCode::InvalidParams));
    }
    // audio_size > max → InvalidParams.
    {
        auto r = default_req(1024);
        auto res = mgr.create("c", r, cfg, /*max=*/512);
        CHECK_FALSE(res.ok);
        CHECK(res.code == static_cast<int>(IpcErrorCode::InvalidParams));
        CHECK(res.error.find("exceeds") != std::string::npos);
    }

    CHECK(mgr.active_count() == 0);
}

// ===========================================================================
// 3. feed_chunk multi-chunk upload: 3 chunks totaling audio_size sum
//    correctly, then finalize enqueues into the postprocess slot.
// ===========================================================================
TEST_CASE("UploadSession: multi-chunk upload finalizes on bytes_received==audio_size",
          "[upload-session]") {
    JobQueue q;
    PpDrainGuard drain(q);
    fs::path tmp = test_temp_dir("multichunk");
    CountingProgressSink prog;
    UploadSessionManager mgr(q, tmp, prog.make());
    JobConfig cfg;

    // 3 chunks of 1600 samples each = 4800 samples = 9600 bytes total.
    constexpr int64_t kTotal = 9600;
    auto req = default_req(kTotal);
    auto res = mgr.create("client-A", req, cfg, /*max=*/1<<20);
    REQUIRE(res.ok);

    std::string chunk = make_pcm(1600);  // 3200 bytes each
    REQUIRE(mgr.feed_chunk("client-A", chunk));
    CHECK(mgr.bytes_received_for_token(res.upload_token) == 3200);
    REQUIRE(mgr.feed_chunk("client-A", chunk));
    CHECK(mgr.bytes_received_for_token(res.upload_token) == 6400);

    // Final chunk crosses audio_size — the session finalizes, the registry
    // entry remains (now Queued/Running/Done — PpDrainGuard drains it
    // immediately), and the manager's map is cleared.
    REQUIRE(mgr.feed_chunk("client-A", chunk));
    CHECK(mgr.active_count() == 0);
    CHECK(mgr.bytes_received_for_token(res.upload_token) == -1);   // gone

    // PpDrainGuard's worker observes the Queued job and finish()es it Done.
    CHECK(wait_until([&]() { return drain.dequeued.load() >= 1; },
                     std::chrono::milliseconds(1000)));
    auto st = q.status(res.job_id);
    REQUIRE(st.has_value());
    CHECK(st->state == JobState::Done);

    // Progress sink fired on every feed_chunk call (3x).
    {
        std::lock_guard<std::mutex> lk(prog.mtx);
        CHECK(prog.calls == 3);
        CHECK(prog.last_bytes_received == kTotal);
        CHECK(prog.last_audio_size == kTotal);
        CHECK(prog.last_job_id == res.job_id);
        CHECK(prog.last_client_id == "client-A");
    }
}

// --- Phase C.11.1 — SubmitRequest.meeting_id propagates onto Job ---------

TEST_CASE("UploadSession: SubmitRequest.meeting_id is stamped onto the Job at finalize",
          "[upload-session][c11]") {
    JobQueue q;
    // Capture the dequeued Job without immediately finishing it, so we can
    // assert meeting_id on the snapshot the worker sees.
    std::atomic<bool> seen{false};
    std::string seen_meeting_id;
    std::thread worker([&]() {
        auto dq = q.dequeue(JobKind::Postprocess);
        if (dq.has_value()) {
            seen_meeting_id = dq->meeting_id;
            seen.store(true);
            q.finish(dq->job_id, /*ok=*/true, "");
        }
    });

    fs::path tmp = test_temp_dir("c11-pp-id");
    UploadSessionManager mgr(q, tmp, null_progress_sink());
    JobConfig cfg;

    constexpr int64_t kTotal = 3200;
    auto req = default_req(kTotal);
    const std::string id = "12345678-1234-4567-89ab-1234567890ab";
    req.meeting_id = id;

    auto res = mgr.create("client-MID", req, cfg, /*max=*/1 << 20);
    REQUIRE(res.ok);

    REQUIRE(mgr.feed_chunk("client-MID", make_pcm(1600)));
    CHECK(wait_until([&]() { return seen.load(); },
                     std::chrono::milliseconds(1000)));
    worker.join();

    CHECK(seen_meeting_id == id);

    // Status snapshot also carries the id (terminal-job registry retention).
    auto st = q.status(res.job_id);
    REQUIRE(st.has_value());
    CHECK(st->meeting_id == id);
}

TEST_CASE("UploadSession: SubmitRequest without meeting_id leaves Job.meeting_id empty "
          "(v1-shaped client back-compat)",
          "[upload-session][c11]") {
    JobQueue q;
    PpDrainGuard guard(q);
    fs::path tmp = test_temp_dir("c11-pp-noid");
    UploadSessionManager mgr(q, tmp, null_progress_sink());
    JobConfig cfg;

    auto req = default_req(3200);
    // req.meeting_id deliberately unset → empty string.
    auto res = mgr.create("client-V1", req, cfg, /*max=*/1 << 20);
    REQUIRE(res.ok);
    REQUIRE(mgr.feed_chunk("client-V1", make_pcm(1600)));

    CHECK(wait_until([&]() {
        auto st = q.status(res.job_id);
        return st.has_value() && st->state == JobState::Done;
    }, std::chrono::milliseconds(1000)));

    auto st = q.status(res.job_id);
    REQUIRE(st.has_value());
    CHECK(st->meeting_id.empty());
}

// ===========================================================================
// 4. feed_chunk rejects bytes_received > audio_size.
// ===========================================================================
TEST_CASE("UploadSession: feed_chunk rejects overflow past audio_size",
          "[upload-session]") {
    JobQueue q;
    PpDrainGuard drain(q);
    UploadSessionManager mgr(q, test_temp_dir("overflow"), null_progress_sink());
    JobConfig cfg;

    auto res = mgr.create("c", default_req(/*audio_size=*/3200), cfg, 1<<20);
    REQUIRE(res.ok);
    // 3200 bytes is exactly the declared size; another chunk overflows.
    REQUIRE(mgr.feed_chunk("c", make_pcm(1600)));   // ok — exactly enough
    // Session finalized — a second chunk now has no live session.
    CHECK_FALSE(mgr.feed_chunk("c", make_pcm(1)));
}

// ===========================================================================
// 5. feed_chunk on a fresh non-finalized session: a single chunk that exceeds
//    audio_size is rejected as a protocol violation (does NOT corrupt the
//    staging file). This is distinct from test 4 above (which covers
//    finalize-then-extra).
// ===========================================================================
TEST_CASE("UploadSession: feed_chunk rejects single chunk that exceeds audio_size",
          "[upload-session]") {
    JobQueue q;
    PpDrainGuard drain(q);
    UploadSessionManager mgr(q, test_temp_dir("oversize_chunk"),
                             null_progress_sink());
    JobConfig cfg;

    auto res = mgr.create("c", default_req(/*audio_size=*/1024), cfg, 1<<20);
    REQUIRE(res.ok);
    // 4096 byte chunk vs declared 1024 → protocol violation, false return.
    CHECK_FALSE(mgr.feed_chunk("c", std::string(4096, 'A')));
    // The session is still alive (cancel must clean up).
    CHECK(mgr.active_count() == 1);
    CHECK(mgr.cancel("c", res.upload_token));
}

// ===========================================================================
// 6. Concurrent second process.submit from same client is rejected Busy.
// ===========================================================================
TEST_CASE("UploadSession: second submit from same client is rejected Busy",
          "[upload-session]") {
    JobQueue q;
    PpDrainGuard drain(q);
    UploadSessionManager mgr(q, test_temp_dir("doublesubmit"),
                             null_progress_sink());
    JobConfig cfg;

    auto a = mgr.create("client-A", default_req(3200), cfg, 1<<20);
    REQUIRE(a.ok);
    auto b = mgr.create("client-A", default_req(6400), cfg, 1<<20);
    CHECK_FALSE(b.ok);
    CHECK(b.code == static_cast<int>(IpcErrorCode::Busy));
    // A different client can submit in parallel (different upload manager
    // slot — the capacity-1 is per-client, not global).
    auto c = mgr.create("client-B", default_req(3200), cfg, 1<<20);
    CHECK(c.ok);
    CHECK(mgr.active_count() == 2);

    mgr.cancel("client-A", a.upload_token);
    mgr.cancel("client-B", c.upload_token);
}

// ===========================================================================
// 7. process.submit.cancel cleanup: cancel marks the reservation Cancelled,
//    unlinks the staging dir, frees the session. Cross-client cancel is
//    refused.
// ===========================================================================
TEST_CASE("UploadSession: cancel tears down staging and marks reservation Cancelled",
          "[upload-session]") {
    JobQueue q;
    PpDrainGuard drain(q);
    fs::path root = test_temp_dir("cancel");
    UploadSessionManager mgr(q, root, null_progress_sink());
    JobConfig cfg;

    auto r = mgr.create("client-A", default_req(3200), cfg, 1<<20);
    REQUIRE(r.ok);
    fs::path dir;
    for (auto& e : fs::directory_iterator(root))
        if (e.is_directory()) dir = e.path();
    REQUIRE(!dir.empty());
    REQUIRE(fs::exists(dir));

    // Cross-client cancel is refused.
    CHECK_FALSE(mgr.cancel("client-B", r.upload_token));
    CHECK(mgr.active_count() == 1);

    // Owning client cancels — staging dir vanishes, JobQueue says Cancelled.
    CHECK(mgr.cancel("client-A", r.upload_token));
    CHECK(mgr.active_count() == 0);
    CHECK_FALSE(fs::exists(dir));
    auto st = q.status(r.job_id);
    REQUIRE(st.has_value());
    CHECK(st->state == JobState::Cancelled);

    // Cancelling an unknown token → false.
    CHECK_FALSE(mgr.cancel("client-A", r.upload_token));
}

// ===========================================================================
// 8. Client disconnect mid-upload unlinks the staging dir and releases the
//    reservation.
// ===========================================================================
TEST_CASE("UploadSession: client disconnect mid-upload aborts the session",
          "[upload-session]") {
    JobQueue q;
    PpDrainGuard drain(q);
    fs::path root = test_temp_dir("disconnect");
    UploadSessionManager mgr(q, root, null_progress_sink());
    JobConfig cfg;

    auto r = mgr.create("client-A", default_req(6400), cfg, 1<<20);
    REQUIRE(r.ok);
    REQUIRE(mgr.feed_chunk("client-A", make_pcm(800)));   // partial upload

    fs::path dir;
    for (auto& e : fs::directory_iterator(root))
        if (e.is_directory()) dir = e.path();
    REQUIRE(!dir.empty());

    // Unrelated disconnect — no-op.
    CHECK(mgr.on_client_disconnect("client-Z") == 0);
    CHECK(mgr.active_count() == 1);

    // Owning client disconnects.
    CHECK(mgr.on_client_disconnect("client-A") == 1);
    CHECK(mgr.active_count() == 0);
    CHECK_FALSE(fs::exists(dir));

    // The reservation is marked Failed (C.10a parity — the client did not
    // ask to stop, so we don't transition to Cancelled). The registry entry
    // survives — JobQueue retains terminal jobs for the binding-survival
    // contract.
    auto st = q.status(r.job_id);
    REQUIRE(st.has_value());
    CHECK(st->state == JobState::Failed);
}

// ===========================================================================
// 9. Token ownership enforcement: cancel with mismatched client is refused.
//    (Belt-and-suspenders to test 7 — explicit assertion on the security
//    contract.)
// ===========================================================================
TEST_CASE("UploadSession: cancel enforces client ownership of upload_token",
          "[upload-session]") {
    JobQueue q;
    PpDrainGuard drain(q);
    UploadSessionManager mgr(q, test_temp_dir("ownership"),
                             null_progress_sink());
    JobConfig cfg;

    auto a = mgr.create("client-A", default_req(1600), cfg, 1<<20);
    auto b = mgr.create("client-B", default_req(1600), cfg, 1<<20);
    REQUIRE(a.ok);
    REQUIRE(b.ok);

    // client-B cannot cancel client-A's upload (even with the right token).
    CHECK_FALSE(mgr.cancel("client-B", a.upload_token));
    CHECK_FALSE(mgr.cancel("client-A", b.upload_token));
    CHECK(mgr.active_count() == 2);

    // Each client cancels its own — both succeed.
    CHECK(mgr.cancel("client-A", a.upload_token));
    CHECK(mgr.cancel("client-B", b.upload_token));
}

// ===========================================================================
// 10. Raw S16LE round-trips: the staging WAV is a valid 16kHz mono S16
//     file after finalize, frame count matches the PCM samples fed.
// ===========================================================================
TEST_CASE("UploadSession: raw s16le PCM round-trips to a valid WAV",
          "[upload-session]") {
    JobQueue q;
    PpDrainGuard drain(q);
    fs::path root = test_temp_dir("roundtrip");
    UploadSessionManager mgr(q, root, null_progress_sink());
    JobConfig cfg;

    // 800 samples = 50ms of 16kHz mono → 1600 bytes total.
    constexpr int64_t kBytes = 1600;
    auto req = default_req(kBytes);
    auto res = mgr.create("c", req, cfg, 1<<20);
    REQUIRE(res.ok);

    // Find the staging dir + audio file BEFORE finalize (finalize hands the
    // dir to pp_worker_loop — it stays on disk, the manager just stops
    // owning it).
    fs::path dir;
    for (auto& e : fs::directory_iterator(root))
        if (e.is_directory()) dir = e.path();
    REQUIRE(!dir.empty());
    fs::path wav = dir / "audio.wav";

    // Feed all in one chunk — finalize fires.
    REQUIRE(mgr.feed_chunk("c", make_pcm(800)));
    CHECK(mgr.active_count() == 0);

    // Drain to terminal state so the postprocess slot is consumed.
    CHECK(wait_until([&]() { return drain.dequeued.load() >= 1; },
                     std::chrono::milliseconds(1000)));
    auto st = q.status(res.job_id);
    REQUIRE(st.has_value());
    CHECK(st->state == JobState::Done);

    // The staging WAV is still on disk (pp_worker_loop owns the dir now).
    REQUIRE(fs::exists(wav));
    SF_INFO info{};
    SNDFILE* sf = sf_open(wav.string().c_str(), SFM_READ, &info);
    REQUIRE(sf != nullptr);
    CHECK(info.samplerate == 16000);
    CHECK(info.channels == 1);
    CHECK(info.frames == 800);
    sf_close(sf);

    // Test cleanup — we own the staging dir now (PpDrainGuard didn't
    // actually fork a subprocess).
    fs::remove_all(dir);
}

// ===========================================================================
// 11. Container format (wav): bytes are written verbatim (no libsndfile
//     wrapping), the file extension matches, the file is valid as-is.
// ===========================================================================
TEST_CASE("UploadSession: container WAV upload is byte-exact on disk",
          "[upload-session]") {
    JobQueue q;
    PpDrainGuard drain(q);
    fs::path root = test_temp_dir("container_wav");
    UploadSessionManager mgr(q, root, null_progress_sink());
    JobConfig cfg;

    // Build a minimal but valid 16kHz mono S16 WAV in memory using
    // libsndfile (so the bytes that arrive on the wire really are a WAV).
    fs::path scratch = root / "scratch.wav";
    {
        SF_INFO info{};
        info.samplerate = 16000;
        info.channels = 1;
        info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        SNDFILE* sf = sf_open(scratch.string().c_str(), SFM_WRITE, &info);
        REQUIRE(sf != nullptr);
        std::vector<int16_t> pcm(400, 0);  // 400 frames of silence
        sf_write_short(sf, pcm.data(), pcm.size());
        sf_close(sf);
    }
    std::ifstream in(scratch, std::ios::binary);
    std::string wav_bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    REQUIRE(wav_bytes.size() > 44);   // bigger than a header

    SubmitRequest req;
    req.audio_size = static_cast<int64_t>(wav_bytes.size());
    req.format = "wav";
    req.sample_rate = 16000;
    req.channels = 1;
    req.mode = "transcribe";
    auto res = mgr.create("c", req, cfg, 1<<20);
    REQUIRE(res.ok);

    fs::path dir;
    for (auto& e : fs::directory_iterator(root)) {
        if (e.is_directory() && e.path().filename().string()
                .find("recmeet-upload-") == 0)
            dir = e.path();
    }
    REQUIRE(!dir.empty());
    fs::path staged = dir / "audio.wav";

    // Send in two pieces to exercise the multi-chunk path on the container
    // writer (std::ofstream append).
    REQUIRE(mgr.feed_chunk("c", wav_bytes.substr(0, 100)));
    REQUIRE(mgr.feed_chunk("c", wav_bytes.substr(100)));
    CHECK(mgr.active_count() == 0);

    // Byte-exact on disk.
    REQUIRE(fs::exists(staged));
    std::ifstream check(staged, std::ios::binary);
    std::string disk_bytes((std::istreambuf_iterator<char>(check)),
                           std::istreambuf_iterator<char>());
    CHECK(disk_bytes == wav_bytes);

    // And libsndfile re-opens it cleanly.
    SF_INFO info{};
    SNDFILE* sf = sf_open(staged.string().c_str(), SFM_READ, &info);
    REQUIRE(sf != nullptr);
    CHECK(info.frames == 400);
    sf_close(sf);

    fs::remove_all(dir);
}

// ===========================================================================
// 12. Wire round-trip: process.submit + `0x01` upload frames + interleaved
//     NDJSON + process.submit.cancel round-trip through a real IpcServer +
//     IpcClient. Confirms the C.1 FrameReader's interleave contract holds
//     for `0x01` exactly as for `0x03`.
//
// Phase 2b extension — the wire tests now drive the PRODUCTION
// `process.submit` / `process.submit.cancel` / `status.get` handlers via
// `DaemonTestHarness::start()` + `register_daemon_handlers(server)`. The
// upload-progress sink is injected via the harness so the test's progress
// counter sees the same callbacks the production handler triggers.
// ===========================================================================
namespace {

// HarnessPpDrainGuard — drives the harness's g_jobs postprocess slot the
// same way PpDrainGuard does for a local JobQueue. Declared AFTER the
// DaemonTestHarness in the test scope so destruction order is:
//   1. HarnessPpDrainGuard::~ — calls g_jobs->shutdown() + joins worker
//   2. ~DaemonTestHarness     — tears down g_uploads, g_jobs, server
// That order is critical: the worker thread MUST exit before g_jobs is
// reset() or it would touch a destroyed JobQueue.
struct HarnessPpDrainGuard {
    std::thread worker;
    std::atomic<int> dequeued{0};
    HarnessPpDrainGuard() {
        worker = std::thread([this]() {
            while (g_jobs) {
                auto dq = g_jobs->dequeue(JobKind::Postprocess);
                if (!dq.has_value()) return;   // shutdown signal
                int64_t id = dq->job_id;
                dequeued.fetch_add(1);
                g_jobs->finish(id, /*ok=*/true);
            }
        });
    }
    ~HarnessPpDrainGuard() {
        if (g_jobs) g_jobs->shutdown();
        if (worker.joinable()) worker.join();
    }
};

} // anonymous namespace

TEST_CASE("UploadSession: 0x01 frames round-trip through IpcServer/IpcClient",
          "[upload-session][upload-wire]") {
    using namespace recmeet::test;
    DaemonTestHarness harness;

    // Inject a counting upload-progress sink BEFORE start() so the
    // production handler's `on_progress` callbacks are observable.
    std::atomic<int> progress_calls{0};
    UploadProgressSink ps;
    ps.on_progress = [&](int64_t, const std::string&, int64_t, int64_t) {
        progress_calls.fetch_add(1);
    };
    harness.set_upload_progress_sink(std::move(ps));

    // The harness's default upload staging_root is its tmp_dir() — fine
    // for these tests. We just need to know where staging dirs land so
    // we can later observe them; use the harness's tmp_dir directly.
    harness.start();
    HarnessPpDrainGuard pp;   // destroyed BEFORE the harness — see comment.

    auto client = harness.make_client();

    // Open the upload session: declare 3*1600 samples = 9600 bytes.
    JsonMap p;
    p["audio_size"] = static_cast<int64_t>(9600);
    p["format"] = std::string("s16le");
    p["sample_rate"] = static_cast<int64_t>(16000);
    p["channels"] = static_cast<int64_t>(1);
    p["mode"] = std::string("transcribe");
    IpcResponse resp; IpcError err;
    REQUIRE(client->call("process.submit", p, resp, err, 5000));
    auto tit = resp.result.find("upload_token");
    REQUIRE(tit != resp.result.end());
    std::string token = json_val_as_string(tit->second);
    REQUIRE(!token.empty());
    auto mit = resp.result.find("max_size");
    REQUIRE(mit != resp.result.end());
    // The harness's ServerConfig leaves max_upload_bytes at default (0);
    // the production handler treats 0 as "no cap" and reports INT64_MAX
    // on the wire. Either way the size sent on the wire is positive.
    CHECK(json_val_as_int(mit->second) > 0);

    // First chunk.
    REQUIRE(client->send_upload_chunk(make_pcm(1600)));

    // NDJSON interleave mid-upload — the C.1 FrameReader assembles this
    // status.get between two `0x01` frames. The production `status.get`
    // handler is registered via register_daemon_handlers; we just need
    // to confirm the call succeeds (a successful response proves the
    // FrameReader interleaved correctly).
    IpcResponse sresp; IpcError serr;
    REQUIRE(client->call("status.get", {}, sresp, serr, 5000));

    // Remaining chunks — the final one crosses audio_size and finalizes.
    REQUIRE(client->send_upload_chunk(make_pcm(1600)));
    REQUIRE(client->send_upload_chunk(make_pcm(1600)));

    // Wait for finalize + the pp drain worker to flip the job Done.
    CHECK(wait_until([&]() { return pp.dequeued.load() >= 1; },
                     std::chrono::milliseconds(2000)));
    CHECK(g_uploads->active_count() == 0);

    // Progress sink fired at least 3x (once per chunk).
    CHECK(progress_calls.load() >= 3);

    client->close_connection();
}

// ===========================================================================
// 13. Wire round-trip: process.submit.cancel over the wire + the disconnect
//     handler. Mirrors the C.10a sibling test.
// ===========================================================================
TEST_CASE("UploadSession: process.submit.cancel + disconnect handler over the wire",
          "[upload-session][upload-wire]") {
    using namespace recmeet::test;
    DaemonTestHarness harness;
    harness.start();
    HarnessPpDrainGuard pp;
    // The harness's daemon-side on_client_disconnect already invokes
    // g_uploads->on_client_disconnect (production behavior). The test
    // observes the cleanup via g_uploads->active_count() and the staging
    // dir disappearing — no separate counter is required.

    // --- Sub-case A: explicit process.submit.cancel after a partial upload.
    {
        auto client = harness.make_client();
        JsonMap p;
        p["audio_size"] = static_cast<int64_t>(6400);
        IpcResponse resp; IpcError err;
        REQUIRE(client->call("process.submit", p, resp, err, 5000));
        std::string token = json_val_as_string(resp.result["upload_token"]);
        REQUIRE(!token.empty());

        REQUIRE(client->send_upload_chunk(make_pcm(800)));  // partial

        // Locate the staging dir under the harness's tmp_dir. The
        // production UploadSessionManager places each upload's staging
        // under `staging_root/recmeet-upload-<job_id>-<token8>/`.
        fs::path dir;
        for (auto& e : fs::directory_iterator(harness.tmp_dir())) {
            if (e.is_directory()
                && e.path().filename().string().find("recmeet-upload-") == 0)
                dir = e.path();
        }
        REQUIRE(!dir.empty());
        REQUIRE(fs::exists(dir));

        // Cancel over the wire.
        JsonMap cp; cp["upload_token"] = token;
        IpcResponse cresp; IpcError cerr;
        REQUIRE(client->call("process.submit.cancel", cp, cresp, cerr, 5000));
        CHECK(wait_until([&]() { return g_uploads->active_count() == 0; },
                         std::chrono::milliseconds(500)));
        CHECK_FALSE(fs::exists(dir));

        client->close_connection();
    }

    // --- Sub-case B: mid-upload TCP drop fires the disconnect handler.
    {
        auto client = harness.make_client();
        JsonMap p;
        p["audio_size"] = static_cast<int64_t>(6400);
        IpcResponse resp; IpcError err;
        REQUIRE(client->call("process.submit", p, resp, err, 5000));
        std::string token = json_val_as_string(resp.result["upload_token"]);
        REQUIRE(!token.empty());

        REQUIRE(client->send_upload_chunk(make_pcm(800)));   // partial

        fs::path dir;
        for (auto& e : fs::directory_iterator(harness.tmp_dir())) {
            if (e.is_directory()
                && e.path().filename().string().find("recmeet-upload-") == 0)
                dir = e.path();
        }
        REQUIRE(!dir.empty());
        REQUIRE(fs::exists(dir));
        CHECK(g_uploads->active_count() == 1);

        client->close_connection();   // mid-upload drop

        // The production daemon-side on_client_disconnect runs
        // g_uploads->on_client_disconnect, which unlinks the staging
        // dir and marks the JobQueue reservation Failed.
        CHECK(wait_until([&]() { return g_uploads->active_count() == 0; },
                         std::chrono::milliseconds(2000)));
        CHECK_FALSE(fs::exists(dir));
    }
}

// ===========================================================================
// Phase C.11.5 — convergence-principle dedup contract tests
// ===========================================================================
//
// These exercise the upload-finalize migration path landed in C.11.4: the
// staging WAV is atomically relocated into a real `~/meetings/{ts}/`
// directory under a dedicated `meetings_root`, the MeetingIndex is bound,
// and a follow-up submit carrying the same meeting_id overwrites
// atomically rather than allocating a new dir.
//
// Each test wires its own MeetingIndex + meetings_root pair (tmpfs-rooted
// per-test to keep them isolated). The unwired path (no index, no root) is
// covered by the existing tests above — we don't repeat that ground.

namespace {

fs::path test_meetings_root(const std::string& tag) {
    fs::path d = fs::temp_directory_path() / ("recmeet_meetings_root_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

// A capturing variant of PpDrainGuard: each dequeued Job is recorded
// before finish() so tests can assert on the finalized payload's shape
// (input.out_dir, meeting_id, etc.).
struct PpCapturingGuard {
    JobQueue& q;
    std::thread worker;
    std::mutex mtx;
    std::vector<Job> jobs;
    explicit PpCapturingGuard(JobQueue& q_) : q(q_) {
        worker = std::thread([this]() {
            for (;;) {
                auto dq = q.dequeue(JobKind::Postprocess);
                if (!dq.has_value()) return;
                int64_t id = dq->job_id;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    jobs.push_back(std::move(*dq));
                }
                q.finish(id, /*ok=*/true);
            }
        });
    }
    ~PpCapturingGuard() {
        q.shutdown();
        if (worker.joinable()) worker.join();
    }
    std::vector<Job> snapshot() {
        std::lock_guard<std::mutex> lk(mtx);
        return jobs;
    }
};

bool drive_full_upload(UploadSessionManager& mgr, const std::string& client,
                       int64_t n_bytes) {
    if (n_bytes % 2 != 0) return false;
    constexpr size_t kChunkSamples = 800;
    constexpr size_t kChunkBytes = kChunkSamples * sizeof(int16_t);
    int64_t sent = 0;
    int16_t cursor = 0;
    while (sent < n_bytes) {
        int64_t remaining = n_bytes - sent;
        size_t take_bytes = remaining < (int64_t)kChunkBytes
            ? static_cast<size_t>(remaining) : kChunkBytes;
        size_t take_samples = take_bytes / sizeof(int16_t);
        std::string chunk = make_pcm(take_samples, cursor);
        cursor += static_cast<int16_t>(take_samples);
        if (!mgr.feed_chunk(client, chunk)) return false;
        sent += static_cast<int64_t>(take_bytes);
    }
    return true;
}

bool wait_for_jobs(PpCapturingGuard& g, size_t n,
                   std::chrono::milliseconds timeout =
                       std::chrono::milliseconds(2000)) {
    return wait_until([&]() { return g.snapshot().size() >= n; }, timeout);
}

// Deterministic UUID v4 for reproducible test failures.
std::string make_uuid_v4(uint32_t seed_a = 0xDEADBEEF,
                         uint32_t seed_b = 0xCAFEBABE) {
    std::mt19937 rng(seed_a ^ seed_b);
    std::array<uint8_t, 16> bytes{};
    for (auto& b : bytes) b = static_cast<uint8_t>(rng() & 0xFF);
    bytes[6] = (bytes[6] & 0x0F) | 0x40; // version 4
    bytes[8] = (bytes[8] & 0x3F) | 0x80; // variant 1
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    for (size_t i = 0; i < 16; ++i) {
        s.push_back(hex[bytes[i] >> 4]);
        s.push_back(hex[bytes[i] & 0xF]);
        if (i == 3 || i == 5 || i == 7 || i == 9) s.push_back('-');
    }
    return s;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// C.11.5.1 — Allocate-on-unknown-id.
// ---------------------------------------------------------------------------
TEST_CASE("dedup: upload with unknown meeting_id allocates fresh meeting dir "
          "and binds the index",
          "[c11][dedup][upload]") {
    JobQueue q;
    PpCapturingGuard drain(q);
    fs::path staging = test_temp_dir("dedup_alloc_staging");
    fs::path meetings = test_meetings_root("dedup_alloc");
    MeetingIndex idx;
    UploadSessionManager mgr(q, staging, null_progress_sink(), &idx, meetings);

    const std::string id = make_uuid_v4(1, 1);
    REQUIRE(idx.find(id) == std::nullopt);

    SubmitRequest req = default_req(/*audio_size=*/3200);
    req.meeting_id = id;
    req.context = "Daily standup";
    auto cr = mgr.create("client-A", req, JobConfig{}, 1 << 20);
    REQUIRE(cr.ok);

    REQUIRE(drive_full_upload(mgr, "client-A", 3200));
    REQUIRE(wait_for_jobs(drain, 1));

    auto bound = idx.find(id);
    REQUIRE(bound.has_value());
    CHECK(bound->parent_path() == meetings);
    CHECK(fs::is_directory(*bound));

    fs::path audio = find_audio_file(*bound);
    REQUIRE(!audio.empty());
    CHECK(audio.parent_path() == *bound);
    CHECK(fs::file_size(audio) > 0);

    CHECK(fs::is_empty(staging));

    CHECK(load_meeting_id(*bound) == id);

    auto jobs = drain.snapshot();
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].input.out_dir == *bound);
    CHECK(jobs[0].meeting_id == id);
}

// ---------------------------------------------------------------------------
// C.11.5.2 — Overwrite-on-known-id (atomic replace, full-context-replace).
// ---------------------------------------------------------------------------
TEST_CASE("dedup: upload with known meeting_id overwrites the existing dir's "
          "audio atomically",
          "[c11][dedup][upload]") {
    JobQueue q;
    PpCapturingGuard drain(q);
    fs::path staging = test_temp_dir("dedup_overwrite_staging");
    fs::path meetings = test_meetings_root("dedup_overwrite");
    MeetingIndex idx;
    UploadSessionManager mgr(q, staging, null_progress_sink(), &idx, meetings);

    const std::string id = make_uuid_v4(2, 2);

    SubmitRequest req = default_req(3200);
    req.meeting_id = id;
    req.context = "first submit";
    {
        auto cr = mgr.create("client-A", req, JobConfig{}, 1 << 20);
        REQUIRE(cr.ok);
        REQUIRE(drive_full_upload(mgr, "client-A", 3200));
        REQUIRE(wait_for_jobs(drain, 1));
    }
    auto bound1 = idx.find(id);
    REQUIRE(bound1.has_value());
    fs::path audio1 = find_audio_file(*bound1);
    REQUIRE(!audio1.empty());
    auto sz1 = fs::file_size(audio1);
    std::ifstream a1(audio1, std::ios::binary);
    std::vector<char> a1_bytes(std::istreambuf_iterator<char>(a1), {});

    SubmitRequest req2 = default_req(6400);   // double size
    req2.meeting_id = id;
    req2.context = "second submit overwrites";
    {
        auto cr = mgr.create("client-A", req2, JobConfig{}, 1 << 20);
        REQUIRE(cr.ok);
        REQUIRE(drive_full_upload(mgr, "client-A", 6400));
        REQUIRE(wait_for_jobs(drain, 2));
    }
    auto bound2 = idx.find(id);
    REQUIRE(bound2.has_value());
    CHECK(*bound2 == *bound1);              // converged on same dir

    fs::path audio2 = find_audio_file(*bound2);
    REQUIRE(!audio2.empty());
    CHECK(audio2 == audio1);                // same canonical filename
    CHECK(fs::file_size(audio2) > sz1);     // larger payload overwrote

    std::ifstream a2(audio2, std::ios::binary);
    std::vector<char> a2_bytes(std::istreambuf_iterator<char>(a2), {});
    CHECK(a2_bytes != a1_bytes);            // bytes differ
    CHECK(load_meeting_id(*bound2) == id);

    int subdirs = 0;
    for (auto& e : fs::directory_iterator(meetings))
        if (e.is_directory()) ++subdirs;
    CHECK(subdirs == 1);
}

// ---------------------------------------------------------------------------
// C.11.5.3 — v1-shaped upload still migrates into real meeting dir,
// without polluting the index.
// ---------------------------------------------------------------------------
TEST_CASE("dedup: v1-shaped upload (empty meeting_id) lands in a real meeting "
          "dir without an index binding",
          "[c11][dedup][upload]") {
    JobQueue q;
    PpCapturingGuard drain(q);
    fs::path staging = test_temp_dir("dedup_v1_staging");
    fs::path meetings = test_meetings_root("dedup_v1");
    MeetingIndex idx;
    UploadSessionManager mgr(q, staging, null_progress_sink(), &idx, meetings);

    SubmitRequest req = default_req(3200);
    // req.meeting_id deliberately left empty
    auto cr = mgr.create("client-A", req, JobConfig{}, 1 << 20);
    REQUIRE(cr.ok);
    REQUIRE(drive_full_upload(mgr, "client-A", 3200));
    REQUIRE(wait_for_jobs(drain, 1));

    CHECK(idx.size() == 0);

    int subdirs = 0;
    fs::path the_dir;
    for (auto& e : fs::directory_iterator(meetings)) {
        if (e.is_directory()) { ++subdirs; the_dir = e.path(); }
    }
    REQUIRE(subdirs == 1);
    CHECK(fs::is_directory(the_dir));
    CHECK(!find_audio_file(the_dir).empty());

    CHECK(load_meeting_id(the_dir) == "");   // no meeting_id field

    auto jobs = drain.snapshot();
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].input.out_dir == the_dir);
    CHECK(jobs[0].meeting_id == "");
}

// ---------------------------------------------------------------------------
// C.11.5.4 — Stale-index recovery + unbind.
// ---------------------------------------------------------------------------
TEST_CASE("dedup: bound meeting_id whose dir vanished re-allocates and "
          "unbinds the stale entry",
          "[c11][dedup][upload]") {
    JobQueue q;
    PpCapturingGuard drain(q);
    fs::path staging = test_temp_dir("dedup_stale_staging");
    fs::path meetings = test_meetings_root("dedup_stale");
    MeetingIndex idx;
    UploadSessionManager mgr(q, staging, null_progress_sink(), &idx, meetings);

    const std::string id = make_uuid_v4(3, 3);

    fs::path ghost = meetings / "2026-01-01_00-00";
    idx.bind(id, ghost);
    REQUIRE(idx.find(id) == ghost);
    REQUIRE_FALSE(fs::exists(ghost));

    SubmitRequest req = default_req(3200);
    req.meeting_id = id;
    auto cr = mgr.create("client-A", req, JobConfig{}, 1 << 20);
    REQUIRE(cr.ok);
    REQUIRE(drive_full_upload(mgr, "client-A", 3200));
    REQUIRE(wait_for_jobs(drain, 1));

    auto bound = idx.find(id);
    REQUIRE(bound.has_value());
    CHECK(*bound != ghost);
    CHECK(fs::is_directory(*bound));
    CHECK(bound->parent_path() == meetings);

    CHECK_FALSE(fs::exists(ghost));
}

// ---------------------------------------------------------------------------
// C.11.5.5 — MeetingIndex::rebuild_from_disk reconstructs bindings.
// ---------------------------------------------------------------------------
TEST_CASE("dedup: MeetingIndex::rebuild_from_disk reconstructs bindings from "
          "context.json",
          "[c11][dedup][index]") {
    fs::path meetings = test_meetings_root("dedup_rebuild");

    const std::string id_a = make_uuid_v4(4, 1);
    const std::string id_b = make_uuid_v4(4, 2);

    fs::path dir_a = meetings / "2026-05-16_10-00";
    fs::path dir_b = meetings / "2026-05-16_11-30";
    fs::create_directories(dir_a);
    fs::create_directories(dir_b);
    save_meeting_context(dir_a, "ctx a", fs::path{}, "2026-05-16_10-00", id_a);
    save_meeting_context(dir_b, "ctx b", fs::path{}, "2026-05-16_11-30", id_b);

    // Legacy v1 dir (no meeting_id) — must be skipped silently.
    fs::path dir_legacy = meetings / "2026-05-16_12-15";
    fs::create_directories(dir_legacy);
    save_meeting_context(dir_legacy, "ctx legacy", fs::path{},
                         "2026-05-16_12-15", /*meeting_id=*/"");

    // Stray non-dir entry — must be skipped.
    std::ofstream(meetings / "stray.txt") << "noise";

    MeetingIndex idx;
    std::size_t n = idx.rebuild_from_disk(meetings);
    CHECK(n == 2);
    CHECK(idx.size() == 2);
    REQUIRE(idx.find(id_a) == dir_a);
    REQUIRE(idx.find(id_b) == dir_b);
}

// ---------------------------------------------------------------------------
// C.11.5.6 — Concurrent cross-client submits with same meeting_id converge
// on one dir.
// ---------------------------------------------------------------------------
TEST_CASE("dedup: concurrent cross-client submits with same meeting_id "
          "converge on one dir",
          "[c11][dedup][upload][concurrency]") {
    JobQueue q;
    PpCapturingGuard drain(q);
    fs::path staging = test_temp_dir("dedup_concurrent_staging");
    fs::path meetings = test_meetings_root("dedup_concurrent");
    MeetingIndex idx;
    UploadSessionManager mgr(q, staging, null_progress_sink(), &idx, meetings);

    const std::string id = make_uuid_v4(5, 5);

    SubmitRequest req = default_req(3200);
    req.meeting_id = id;
    auto cr_a = mgr.create("client-A", req, JobConfig{}, 1 << 20);
    auto cr_b = mgr.create("client-B", req, JobConfig{}, 1 << 20);
    REQUIRE(cr_a.ok);
    REQUIRE(cr_b.ok);
    CHECK(cr_a.job_id != cr_b.job_id);

    REQUIRE(drive_full_upload(mgr, "client-A", 3200));
    REQUIRE(drive_full_upload(mgr, "client-B", 3200));
    REQUIRE(wait_for_jobs(drain, 2));

    int subdirs = 0;
    fs::path the_dir;
    for (auto& e : fs::directory_iterator(meetings))
        if (e.is_directory()) { ++subdirs; the_dir = e.path(); }
    CHECK(subdirs == 1);

    auto bound = idx.find(id);
    REQUIRE(bound.has_value());
    CHECK(*bound == the_dir);

    auto jobs = drain.snapshot();
    REQUIRE(jobs.size() == 2);
    CHECK(jobs[0].input.out_dir == the_dir);
    CHECK(jobs[1].input.out_dir == the_dir);
    CHECK(jobs[0].meeting_id == id);
    CHECK(jobs[1].meeting_id == id);
}

