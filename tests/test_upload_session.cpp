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
#include "upload_session.h"
#include "util.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

    Config cfg;
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
    Config cfg;

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
    Config cfg;

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
    Config cfg;

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
    Config cfg;

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
    Config cfg;

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
    Config cfg;

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
    Config cfg;

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
    Config cfg;

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
    Config cfg;

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
    Config cfg;

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
    Config cfg;

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
    Config cfg;

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
// ===========================================================================
namespace {

const char* UPLOAD_SOCK = "/tmp/recmeet_test_upload.sock";

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

} // anonymous namespace

TEST_CASE("UploadSession: 0x01 frames round-trip through IpcServer/IpcClient",
          "[upload-session][upload-wire]") {
    ::unlink(UPLOAD_SOCK);

    JobQueue q;
    PpDrainGuard pp(q);
    fs::path root = test_temp_dir("wire");
    std::atomic<int> progress_calls{0};
    UploadProgressSink ps;
    ps.on_progress = [&](int64_t, const std::string&, int64_t, int64_t) {
        progress_calls.fetch_add(1);
    };
    UploadSessionManager mgr(q, root, ps);

    IpcServer server(UPLOAD_SOCK);

    std::atomic<bool> interleaved_cmd_seen{false};

    // Wire handlers — mirror the daemon registration but local to this test.
    server.on("process.submit",
              [&](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        SubmitRequest sr;
        auto gs = [&](const char* k, const std::string& def) {
            auto it = req.params.find(k);
            return it != req.params.end() ? json_val_as_string(it->second) : def;
        };
        auto gi = [&](const char* k, int64_t def) {
            auto it = req.params.find(k);
            return it != req.params.end() ? json_val_as_int(it->second) : def;
        };
        sr.audio_size  = gi("audio_size", 0);
        sr.format      = gs("format", sr.format);
        sr.sample_rate = static_cast<int32_t>(gi("sample_rate", sr.sample_rate));
        sr.channels    = static_cast<int32_t>(gi("channels", sr.channels));
        sr.mode        = gs("mode", sr.mode);
        Config cfg;
        auto cr = mgr.create(req.client_id, sr, cfg, 1<<20);
        if (!cr.ok) { err.code = cr.code; err.message = cr.error; return false; }
        resp.result["job_id"]       = cr.job_id;
        resp.result["upload_token"] = cr.upload_token;
        resp.result["max_size"]     = cr.max_size;
        return true;
    });
    server.on("process.submit.cancel",
              [&](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        auto it = req.params.find("upload_token");
        if (it == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "missing upload_token"; return false;
        }
        if (!mgr.cancel(req.client_id, json_val_as_string(it->second))) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "unknown upload_token"; return false;
        }
        resp.result["ok"] = true;
        return true;
    });
    server.on("status.get", [&](const IpcRequest&, IpcResponse& resp, IpcError&) {
        interleaved_cmd_seen.store(true);
        resp.result["ok"] = true; return true;
    });
    // Binary-frame handler — routes 0x01 to the upload manager by client_id.
    server.on_binary_frame([&](const std::string& client_id,
                               FrameType type,
                               const std::string& payload) -> bool {
        if (type == FrameType::BinaryUpload)
            return mgr.feed_chunk(client_id, payload);
        return true;
    });
    server.on_client_disconnect([&](const std::string& cid) {
        mgr.on_client_disconnect(cid);
    });

    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(UPLOAD_SOCK);
    REQUIRE(client.connect());

    // Open the upload session: declare 3*1600 samples = 9600 bytes.
    JsonMap p;
    p["audio_size"] = static_cast<int64_t>(9600);
    p["format"] = std::string("s16le");
    p["sample_rate"] = static_cast<int64_t>(16000);
    p["channels"] = static_cast<int64_t>(1);
    p["mode"] = std::string("transcribe");
    IpcResponse resp; IpcError err;
    REQUIRE(client.call("process.submit", p, resp, err, 5000));
    auto tit = resp.result.find("upload_token");
    REQUIRE(tit != resp.result.end());
    std::string token = json_val_as_string(tit->second);
    REQUIRE(!token.empty());
    auto mit = resp.result.find("max_size");
    REQUIRE(mit != resp.result.end());
    CHECK(json_val_as_int(mit->second) == (1 << 20));

    // First chunk.
    REQUIRE(client.send_upload_chunk(make_pcm(1600)));

    // NDJSON interleave mid-upload — the C.1 FrameReader assembles this
    // status.get between two `0x01` frames.
    IpcResponse sresp; IpcError serr;
    REQUIRE(client.call("status.get", {}, sresp, serr, 5000));

    // Remaining chunks — the final one crosses audio_size and finalizes.
    REQUIRE(client.send_upload_chunk(make_pcm(1600)));
    REQUIRE(client.send_upload_chunk(make_pcm(1600)));

    // Wait for finalize + the pp drain worker to flip the job Done.
    CHECK(wait_until([&]() { return pp.dequeued.load() >= 1; },
                     std::chrono::milliseconds(2000)));
    CHECK(mgr.active_count() == 0);
    CHECK(interleaved_cmd_seen.load());

    // Progress sink fired at least 3x (once per chunk).
    CHECK(progress_calls.load() >= 3);

    client.close_connection();
    ::unlink(UPLOAD_SOCK);
}

// ===========================================================================
// 13. Wire round-trip: process.submit.cancel over the wire + the disconnect
//     handler. Mirrors the C.10a sibling test.
// ===========================================================================
TEST_CASE("UploadSession: process.submit.cancel + disconnect handler over the wire",
          "[upload-session][upload-wire]") {
    ::unlink(UPLOAD_SOCK);

    JobQueue q;
    PpDrainGuard pp(q);
    fs::path root = test_temp_dir("wirecancel");
    UploadSessionManager mgr(q, root, null_progress_sink());
    IpcServer server(UPLOAD_SOCK);
    std::atomic<int> disconnect_aborts{0};

    server.on("process.submit",
              [&](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        SubmitRequest sr;
        auto it = req.params.find("audio_size");
        if (it != req.params.end()) sr.audio_size = json_val_as_int(it->second);
        sr.format = "s16le";
        sr.sample_rate = 16000;
        sr.channels = 1;
        sr.mode = "transcribe";
        Config cfg;
        auto cr = mgr.create(req.client_id, sr, cfg, 1<<20);
        if (!cr.ok) { err.code = cr.code; err.message = cr.error; return false; }
        resp.result["job_id"] = cr.job_id;
        resp.result["upload_token"] = cr.upload_token;
        resp.result["max_size"] = cr.max_size;
        return true;
    });
    server.on("process.submit.cancel",
              [&](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        auto it = req.params.find("upload_token");
        if (it == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "missing upload_token"; return false;
        }
        if (!mgr.cancel(req.client_id, json_val_as_string(it->second))) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "unknown upload_token"; return false;
        }
        resp.result["ok"] = true; return true;
    });
    server.on_binary_frame([&](const std::string& cid, FrameType type,
                               const std::string& payload) -> bool {
        if (type == FrameType::BinaryUpload) return mgr.feed_chunk(cid, payload);
        return true;
    });
    server.on_client_disconnect([&](const std::string& cid) {
        disconnect_aborts.fetch_add(mgr.on_client_disconnect(cid));
    });

    REQUIRE(server.start());
    ServerGuard sg(server);

    // --- Sub-case A: explicit process.submit.cancel after a partial upload.
    {
        IpcClient client(UPLOAD_SOCK);
        REQUIRE(client.connect());
        JsonMap p;
        p["audio_size"] = static_cast<int64_t>(6400);
        IpcResponse resp; IpcError err;
        REQUIRE(client.call("process.submit", p, resp, err, 5000));
        std::string token = json_val_as_string(resp.result["upload_token"]);
        REQUIRE(!token.empty());

        REQUIRE(client.send_upload_chunk(make_pcm(800)));  // partial

        fs::path dir;
        for (auto& e : fs::directory_iterator(root))
            if (e.is_directory()) dir = e.path();
        REQUIRE(fs::exists(dir));

        // Cancel over the wire.
        JsonMap cp; cp["upload_token"] = token;
        IpcResponse cresp; IpcError cerr;
        REQUIRE(client.call("process.submit.cancel", cp, cresp, cerr, 5000));
        // Give the server a brief moment to land the cancel (the call's
        // response arrival is sufficient ordering, but the manager-mutex
        // release order is observable to the test thread only after the
        // poll loop progresses one more turn).
        CHECK(wait_until([&]() { return mgr.active_count() == 0; },
                         std::chrono::milliseconds(500)));
        CHECK_FALSE(fs::exists(dir));

        client.close_connection();
    }

    // --- Sub-case B: mid-upload TCP drop fires the disconnect handler.
    {
        IpcClient client(UPLOAD_SOCK);
        REQUIRE(client.connect());
        JsonMap p;
        p["audio_size"] = static_cast<int64_t>(6400);
        IpcResponse resp; IpcError err;
        REQUIRE(client.call("process.submit", p, resp, err, 5000));
        std::string token = json_val_as_string(resp.result["upload_token"]);
        REQUIRE(!token.empty());

        REQUIRE(client.send_upload_chunk(make_pcm(800)));   // partial

        fs::path dir;
        for (auto& e : fs::directory_iterator(root))
            if (e.is_directory()) dir = e.path();
        REQUIRE(fs::exists(dir));
        CHECK(mgr.active_count() == 1);

        client.close_connection();   // mid-upload drop

        CHECK(wait_until([&]() { return disconnect_aborts.load() >= 1; },
                         std::chrono::milliseconds(2000)));
        CHECK(mgr.active_count() == 0);
        CHECK_FALSE(fs::exists(dir));
    }

    ::unlink(UPLOAD_SOCK);
}
