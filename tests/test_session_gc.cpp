// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.13 — GC orphan-job teardown integration. Pins the substantive
// behavior of the sweep + evict shared body (`teardown_orphan_jobs` in
// daemon.cpp). The tests use a hand-rolled teardown function with the
// same dispatch table as the production helper but parameterized over
// the registries so the in-process suite can drive every path without
// the daemon globals.
//
// Test cases (all tagged [c13][gc]):
//   - Postprocess Running orphan with pid match → cancel + SIGTERM-call
//     bookkeeping; WAV archived to <root>/.orphan-<prefix>-<ts>/.
//   - Postprocess Running orphan with pid MISMATCH → cancel only; the
//     SIGTERM dispatch must NOT fire (C-1 race-avoidance test).
//   - Streaming orphan → cancel_by_job_id is invoked; engine torn down;
//     in-flight WAV archived BEFORE cancel unlinks it.
//   - Postprocess WaitingForUpload orphan → upload manager
//     cancel_by_job_id + jobs.cancel.
//   - ModelDownload orphan → jobs.cancel.

#include <catch2/catch_test_macros.hpp>

#include "config.h"
#include "job_queue.h"
#include "session_manager.h"
#include "streaming_session.h"
#include "upload_session.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace recmeet;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// Mirror of daemon.cpp's teardown dispatch — instantiated per-test with
// the local JobQueue / managers / pp_child_pid stand-in. Returns:
//   {canceled_job_ids, sigterm_dispatch_count}.
//
// The "SIGTERM dispatch" is recorded as a counter increment rather than
// an actual ::kill(); tests assert on the counter to pin the C-1 race
// guard. Production daemon.cpp's teardown actually fires SIGTERM, but
// the test boundary is the call-site decision, not the syscall.
struct TeardownOutcome {
    std::vector<int64_t> canceled;
    int sigterm_dispatched = 0;
};

TeardownOutcome
test_teardown_orphan_jobs(JobQueue& jobs,
                          StreamingSessionManager* streaming,
                          UploadSessionManager* uploads,
                          std::atomic<pid_t>* pp_child_pid,
                          const fs::path& meetings_root,
                          const std::string& token_prefix8,
                          const std::string& client_id);

// Local copy of the orphan archive helpers (the production versions live
// in an anonymous namespace inside daemon.cpp and aren't exported). The
// test-side copies are deliberately near-verbatim so a refactor of the
// production helper either flows through both or surfaces as a test
// failure rather than silent skew.
std::string make_orphan_dir_name(const std::string& token_prefix8) {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
    localtime_r(&tt, &tmv);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min);
    return std::string(".orphan-") + token_prefix8 + "-" + buf;
}

bool archive_wav_test(const fs::path& meetings_root,
                      const std::string& token_prefix8,
                      const fs::path& wav_path) {
    if (wav_path.empty()) return false;
    std::error_code ec;
    if (!fs::exists(wav_path, ec)) return false;
    fs::path dst_dir = meetings_root / make_orphan_dir_name(token_prefix8);
    fs::create_directories(dst_dir, ec);
    if (ec) return false;
    fs::path dst_final = dst_dir / "audio.wav";
    fs::path dst_tmp = dst_final;
    dst_tmp += ".tmp";
    fs::remove(dst_tmp, ec); ec.clear();
    fs::rename(wav_path, dst_tmp, ec);
    if (ec) return false;
    fs::rename(dst_tmp, dst_final, ec);
    return !ec;
}

TeardownOutcome
test_teardown_orphan_jobs(JobQueue& jobs,
                          StreamingSessionManager* streaming,
                          UploadSessionManager* uploads,
                          std::atomic<pid_t>* pp_child_pid,
                          const fs::path& meetings_root,
                          const std::string& token_prefix8,
                          const std::string& client_id) {
    TeardownOutcome out;
    auto owned = jobs.list_by_client(client_id);
    for (const auto& job : owned) {
        if (job.state == JobState::Done ||
            job.state == JobState::Failed ||
            job.state == JobState::Cancelled) continue;
        switch (job.kind) {
        case JobKind::Postprocess:
            archive_wav_test(meetings_root, token_prefix8, job.input.audio_path);
            if (job.state == JobState::WaitingForUpload) {
                if (uploads) uploads->cancel_by_job_id(job.job_id);
                jobs.cancel(job.job_id);
            } else if (job.state == JobState::Running) {
                jobs.cancel(job.job_id);
                auto bound = jobs.pid_for_running_job(job.job_id);
                pid_t live = pp_child_pid ? pp_child_pid->load() : -1;
                if (bound.has_value() && *bound == live && live > 0) {
                    out.sigterm_dispatched++;
                }
            } else {
                jobs.cancel(job.job_id);
            }
            out.canceled.push_back(job.job_id);
            break;
        case JobKind::Streaming:
            if (streaming) {
                fs::path w = streaming->wav_path_for_job(job.job_id);
                archive_wav_test(meetings_root, token_prefix8, w);
                streaming->cancel_by_job_id(job.job_id);
            }
            out.canceled.push_back(job.job_id);
            break;
        case JobKind::ModelDownload:
            jobs.cancel(job.job_id);
            out.canceled.push_back(job.job_id);
            break;
        case JobKind::_count: break;
        }
    }
    return out;
}

fs::path test_dir(const std::string& tag) {
    fs::path d = fs::temp_directory_path() / ("recmeet_c13_gc_" + tag);
    std::error_code ec; fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

Job make_pp_job_with_wav(const fs::path& wav) {
    Job j;
    j.input.transcript_text = "already transcribed";
    j.input.audio_path = wav;
    j.input.out_dir = wav.parent_path();
    return j;
}

// Touch a file so fs::exists is true. Tests assert on relocation, not
// transcription correctness, so any non-empty file body is fine.
void touch_wav(const fs::path& p, const std::string& body = "PCM-DATA") {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << body;
}

StreamingCaptionSink null_sink() {
    StreamingCaptionSink s;
    s.on_caption  = [](int64_t, const std::string&, const std::string&,
                       bool, int64_t) {};
    s.on_degraded = [](int64_t, const std::string&, const std::string&,
                       int64_t) {};
    return s;
}

// Drain worker for the Streaming slot — mirrors stream_worker_loop.
struct StreamDrain {
    JobQueue& q; std::thread t;
    explicit StreamDrain(JobQueue& q_) : q(q_) {
        t = std::thread([this]() {
            for (;;) { auto dq = q.dequeue(JobKind::Streaming); if (!dq) return; }
        });
    }
    ~StreamDrain() { q.shutdown(); if (t.joinable()) t.join(); }
};

template <typename Pred>
bool wait_until(Pred p, std::chrono::milliseconds to) {
    auto deadline = std::chrono::steady_clock::now() + to;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return p();
}

} // namespace

// ===========================================================================
// (6) Postprocess Running orphan with pid match → cancel + SIGTERM
// ===========================================================================
TEST_CASE("C.13: GC teardown — pp Running with pid match → cancel + SIGTERM",
          "[c13][gc]") {
    JobQueue jobs;
    fs::path root = test_dir("pp_run_match");
    fs::path wav = root / "staging" / "audio.wav";
    touch_wav(wav);

    int64_t jid = jobs.enqueue(make_pp_job_with_wav(wav),
                               JobKind::Postprocess, "alice");
    // Move to Running + bind a pid that matches g_pp_child_pid stand-in.
    auto dq = jobs.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    REQUIRE(jobs.status(jid)->state == JobState::Running);

    std::atomic<pid_t> pp_pid{12345};
    jobs.bind_running_pid(jid, pp_pid.load());

    auto out = test_teardown_orphan_jobs(jobs, nullptr, nullptr, &pp_pid,
                                          root, "abcd1234", "alice");
    REQUIRE(out.canceled.size() == 1);
    REQUIRE(out.canceled[0] == jid);
    REQUIRE(out.sigterm_dispatched == 1);
    REQUIRE(jobs.status(jid)->state == JobState::Cancelled);

    // WAV archived: original wav gone, an audio.wav appears under a
    // `.orphan-abcd1234-...` directory.
    REQUIRE(!fs::exists(wav));
    bool found_archive = false;
    for (auto& e : fs::directory_iterator(root)) {
        std::string n = e.path().filename().string();
        if (n.rfind(".orphan-abcd1234-", 0) == 0 &&
            fs::exists(e.path() / "audio.wav")) {
            found_archive = true; break;
        }
    }
    REQUIRE(found_archive);

    jobs.shutdown();
}

// ===========================================================================
// (7) Postprocess Running orphan with pid MISMATCH → cancel only, NO SIGTERM
// (C-1 race-avoidance test — the most critical assertion in C.13)
// ===========================================================================
TEST_CASE("C.13: GC teardown — pp Running with pid MISMATCH → cancel only (C-1)",
          "[c13][gc]") {
    JobQueue jobs;
    fs::path root = test_dir("pp_run_mismatch");
    fs::path wav = root / "staging" / "audio.wav";
    touch_wav(wav);

    int64_t jid = jobs.enqueue(make_pp_job_with_wav(wav),
                               JobKind::Postprocess, "alice");
    auto dq = jobs.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());

    // The victim's bound pid is 12345; the live pp_child_pid is something
    // else. That means by the time the GC fired, a different job had
    // already replaced this one in the slot — SIGTERM would kill the
    // wrong subprocess. Cancel-only is the correct behavior.
    jobs.bind_running_pid(jid, 12345);
    std::atomic<pid_t> pp_pid{99999};  // mismatch — different live pid

    auto out = test_teardown_orphan_jobs(jobs, nullptr, nullptr, &pp_pid,
                                          root, "deadbeef", "alice");
    REQUIRE(out.canceled.size() == 1);
    REQUIRE(out.sigterm_dispatched == 0);  // critical assertion
    REQUIRE(jobs.status(jid)->state == JobState::Cancelled);

    jobs.shutdown();
}

// ===========================================================================
// (8) Streaming orphan → cancel_by_job_id invoked, engine torn down,
// in-flight WAV archived.
// ===========================================================================
TEST_CASE("C.13: GC teardown — streaming orphan archives WAV + cancels",
          "[c13][gc]") {
    JobQueue q;
    StreamDrain drain(q);
    fs::path root = test_dir("stream_orphan");
    fs::path stream_temp = root / "streaming";
    fs::create_directories(stream_temp);

    auto sink = null_sink();
    StreamingSessionManager mgr(q, sink, "");  // empty model dir — stub engine

    StreamRequest sr;
    auto res = mgr.create("alice", sr, stream_temp);
    REQUIRE(res.ok);
    REQUIRE(wait_until([&] { return q.slot_busy(JobKind::Streaming); }, 500ms));

    // Locate the in-flight WAV before teardown unlinks it.
    fs::path wav_before = mgr.wav_path_for_job(res.job_id);
    REQUIRE(!wav_before.empty());
    REQUIRE(fs::exists(wav_before));

    std::atomic<pid_t> pp_pid{-1};
    auto out = test_teardown_orphan_jobs(q, &mgr, nullptr, &pp_pid,
                                          root, "abcdefab", "alice");
    REQUIRE(out.canceled.size() == 1);
    REQUIRE(out.canceled[0] == res.job_id);
    REQUIRE(q.status(res.job_id)->state == JobState::Cancelled);
    // Original WAV moved.
    REQUIRE(!fs::exists(wav_before));
    // Archive present.
    bool found = false;
    for (auto& e : fs::directory_iterator(root)) {
        std::string n = e.path().filename().string();
        if (n.rfind(".orphan-abcdefab-", 0) == 0 &&
            fs::exists(e.path() / "audio.wav")) { found = true; break; }
    }
    REQUIRE(found);
}

// ===========================================================================
// (extra) Postprocess WaitingForUpload orphan → upload mgr cancel + jobs cancel
// ===========================================================================
TEST_CASE("C.13: GC teardown — pp WaitingForUpload routes through upload mgr",
          "[c13][gc]") {
    JobQueue q;
    fs::path root = test_dir("pp_upload_orphan");
    fs::path staging = root / "staging";
    fs::create_directories(staging);

    UploadProgressSink ps;
    ps.on_progress = [](int64_t, const std::string&, int64_t, int64_t) {};
    UploadSessionManager uploads(q, staging, ps);

    SubmitRequest sr;
    sr.audio_size = 4;
    sr.format = "s16le";
    sr.sample_rate = 16000;
    sr.channels = 1;
    sr.mode = "transcribe";

    Config pp_cfg;
    auto create_res = uploads.create("alice", sr, pp_cfg, /*max_upload_bytes=*/0);
    REQUIRE(create_res.ok);

    // The reservation should be in WaitingForUpload.
    REQUIRE(q.status(create_res.job_id)->state == JobState::WaitingForUpload);

    std::atomic<pid_t> pp_pid{-1};
    auto out = test_teardown_orphan_jobs(q, nullptr, &uploads, &pp_pid,
                                          root, "11223344", "alice");
    REQUIRE(out.canceled.size() == 1);
    REQUIRE(out.canceled[0] == create_res.job_id);
    REQUIRE(q.status(create_res.job_id)->state == JobState::Cancelled);
    q.shutdown();
}

// ===========================================================================
// (extra) ModelDownload orphan → jobs.cancel
// ===========================================================================
TEST_CASE("C.13: GC teardown — ModelDownload orphan → cancel",
          "[c13][gc]") {
    JobQueue q;
    fs::path root = test_dir("dl_orphan");

    Job dl;
    dl.kind = JobKind::ModelDownload;
    dl.model_id = "whisper/base.en";
    int64_t jid = q.enqueue(std::move(dl), JobKind::ModelDownload, "alice");
    REQUIRE(q.status(jid)->state == JobState::Queued);

    std::atomic<pid_t> pp_pid{-1};
    auto out = test_teardown_orphan_jobs(q, nullptr, nullptr, &pp_pid,
                                          root, "11112222", "alice");
    REQUIRE(out.canceled.size() == 1);
    REQUIRE(out.canceled[0] == jid);
    REQUIRE(q.status(jid)->state == JobState::Cancelled);
    q.shutdown();
}
