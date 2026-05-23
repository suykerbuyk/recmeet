// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.1 — per-slot-kind submission queue.
// Phase D.2 — drain worker (per slot kind) + journal write at submit /
// stream commit return.
//
// All tests run against pure data structures (`SlotQueues`,
// `PendingJobsJournal`); no IPC / GTK / tray globals are touched.
// The journal write is exercised against a tmp-dir journal so the
// test cannot collide with the operator's
// `~/.local/share/recmeet/pending_jobs.json`.
//
// Test budget (from the iter-166 brief):
//   - D.1 ~3: admission per slot routes correctly; capacity-1
//     per slot; three slots independent.
//   - D.2 ~3: journal write on `process.submit` return shape;
//     journal write on `process.stream.commit` return shape (we
//     exercise the analogous `process.stream` open return because
//     the tray does not call the commit verb today — see comment in
//     tray.cpp:apply_submit_with_context for the spec deviation
//     rationale); backlog drain promotes next entry on terminal
//     status and dispatches it through the seam.

#include <catch2/catch_test_macros.hpp>

#include "pending_jobs_journal.h"
#include "slot_queue.h"
#include "test_tmpdir.h"

#include <unistd.h>

#include <chrono>
#include <random>
#include <sstream>

using namespace recmeet;
namespace fs = std::filesystem;

namespace {

// Per-test scratch dir so a parallel `recmeet_tests` run from `ctest -j`
// cannot clobber another worker's journal. PID + a random suffix is
// sufficient — Catch2 itself runs single-threaded inside a process.
fs::path make_scratch(const char* prefix) {
    std::random_device rd;
    std::ostringstream oss;
    oss << "recmeet_" << prefix << "_" << ::getpid() << "_" << rd();
    fs::path p = recmeet::test::tmp_path(oss.str());
    std::error_code ec;
    fs::create_directories(p, ec);
    REQUIRE_FALSE(ec);
    return p;
}

struct ScopedDir {
    fs::path path;
    ~ScopedDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

JobEntry make_entry(const std::string& meeting_id,
                    const std::string& kind,
                    int64_t job_id = 0) {
    JobEntry e;
    e.meeting_id       = meeting_id;
    e.staging_wav_path = "/tmp/staging/" + meeting_id + ".wav";
    e.kind             = kind;
    e.job_id           = job_id;
    return e;
}

int64_t now_unix() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

// ---------------------------------------------------------------------------
// D.1 — per-slot-kind submission queue
// ---------------------------------------------------------------------------

TEST_CASE("D.1: admission routes to the correct typed slot",
          "[d1][queue]") {
    SlotQueues q;

    // (a) Postprocess admission lands in the postprocess slot's
    //     in-flight position; streaming + model_download stay idle.
    auto pp_entry = make_entry("mtg-pp-001", "submit");
    REQUIRE(q.postprocess.admit(pp_entry));
    CHECK(q.postprocess.is_in_flight());
    CHECK_FALSE(q.streaming.is_in_flight());
    CHECK_FALSE(q.model_download.is_in_flight());
    REQUIRE(q.postprocess.in_flight() != nullptr);
    CHECK(q.postprocess.in_flight()->meeting_id == "mtg-pp-001");
    CHECK(q.postprocess.in_flight()->kind == "submit");

    // (b) Streaming admission lands in the streaming slot — note
    //     `pp_entry` is still in flight; the two slots track
    //     independently.
    auto st_entry = make_entry("mtg-st-002", "stream");
    REQUIRE(q.streaming.admit(st_entry));
    CHECK(q.streaming.is_in_flight());
    REQUIRE(q.streaming.in_flight() != nullptr);
    CHECK(q.streaming.in_flight()->meeting_id == "mtg-st-002");
    CHECK(q.streaming.in_flight()->kind == "stream");

    // (c) Model-download admission lands in its own slot.
    auto md_entry = make_entry("model-large-v3", "model_download");
    REQUIRE(q.model_download.admit(md_entry));
    CHECK(q.model_download.is_in_flight());
    REQUIRE(q.model_download.in_flight() != nullptr);
    CHECK(q.model_download.in_flight()->meeting_id == "model-large-v3");

    // (d) The `select` helper resolves a SlotKind to the right slot
    //     so callers do not have to switch on the enum themselves.
    CHECK(&q.select(SlotKind::Postprocess)   == &q.postprocess);
    CHECK(&q.select(SlotKind::Streaming)     == &q.streaming);
    CHECK(&q.select(SlotKind::ModelDownload) == &q.model_download);

    // (e) On-wire slot_kind strings round-trip via the to_string
    //     helper — the journal `slot_kind` field is keyed on these.
    CHECK(std::string(to_string(SlotKind::Postprocess))   == "postprocess");
    CHECK(std::string(to_string(SlotKind::Streaming))     == "streaming");
    CHECK(std::string(to_string(SlotKind::ModelDownload)) == "model_download");
}

TEST_CASE("D.1: capacity-1-in-flight per slot — second admission goes "
          "to backlog, not in-flight",
          "[d1][queue]") {
    SlotQueues q;

    // First admission promotes to in-flight.
    auto e1 = make_entry("mtg-pp-A", "submit", /*job_id=*/100);
    REQUIRE(q.postprocess.admit(e1));
    REQUIRE(q.postprocess.is_in_flight());
    CHECK(q.postprocess.in_flight()->job_id == 100);
    CHECK(q.postprocess.backlog_size() == 0);

    // Second admission joins the backlog.
    auto e2 = make_entry("mtg-pp-B", "submit");
    bool admitted_in_flight = q.postprocess.admit(e2);
    CHECK_FALSE(admitted_in_flight);
    // In-flight still pointing at the original entry.
    CHECK(q.postprocess.in_flight()->meeting_id == "mtg-pp-A");
    CHECK(q.postprocess.backlog_size() == 1);

    // Third admission deepens the backlog (FIFO order preserved).
    auto e3 = make_entry("mtg-pp-C", "submit");
    CHECK_FALSE(q.postprocess.admit(e3));
    CHECK(q.postprocess.backlog_size() == 2);
    CHECK(q.postprocess.in_flight()->meeting_id == "mtg-pp-A");

    // Terminal status on the in-flight entry pops the next backlog
    // entry into in-flight position — FIFO drain.
    auto next = q.postprocess.complete_in_flight();
    REQUIRE(next.has_value());
    CHECK(next->meeting_id == "mtg-pp-B");
    CHECK(q.postprocess.in_flight()->meeting_id == "mtg-pp-B");
    CHECK(q.postprocess.backlog_size() == 1);

    // Drain again: B → C.
    next = q.postprocess.complete_in_flight();
    REQUIRE(next.has_value());
    CHECK(next->meeting_id == "mtg-pp-C");
    CHECK(q.postprocess.backlog_size() == 0);

    // Drain C with empty backlog → no next entry; slot fully idle.
    next = q.postprocess.complete_in_flight();
    CHECK_FALSE(next.has_value());
    CHECK_FALSE(q.postprocess.is_in_flight());
}

TEST_CASE("D.1: the three slots are independent — admitting to one "
          "does not affect the others",
          "[d1][queue]") {
    SlotQueues q;

    // Postprocess holds an in-flight + backlog entry.
    REQUIRE(q.postprocess.admit(make_entry("mtg-pp", "submit", 1)));
    CHECK_FALSE(q.postprocess.admit(make_entry("mtg-pp-2", "submit")));
    REQUIRE(q.postprocess.is_in_flight());
    CHECK(q.postprocess.backlog_size() == 1);

    // Streaming admission lands as in-flight (its own slot is idle);
    // postprocess state is unchanged.
    REQUIRE(q.streaming.admit(make_entry("mtg-st", "stream", 2)));
    CHECK(q.streaming.is_in_flight());
    CHECK(q.streaming.backlog_size() == 0);
    CHECK(q.postprocess.is_in_flight());
    CHECK(q.postprocess.backlog_size() == 1);

    // Model-download admission is the same shape — independent slot,
    // independent in-flight bookkeeping.
    REQUIRE(q.model_download.admit(make_entry("model-x", "model_download", 3)));
    CHECK(q.model_download.is_in_flight());
    CHECK(q.streaming.is_in_flight());
    CHECK(q.postprocess.is_in_flight());

    // Terminal status on the streaming slot drains ONLY streaming.
    auto next = q.streaming.complete_in_flight();
    CHECK_FALSE(next.has_value());
    CHECK_FALSE(q.streaming.is_in_flight());
    CHECK(q.postprocess.is_in_flight());   // unaffected
    CHECK(q.model_download.is_in_flight()); // unaffected
    CHECK(q.postprocess.backlog_size() == 1);  // unaffected
}

// ---------------------------------------------------------------------------
// D.2 — drain worker + journal write at submit / stream-commit return
// ---------------------------------------------------------------------------

TEST_CASE("D.2: journal entry written on `process.submit` return carries "
          "the seven-field schema with correct slot_kind and kind",
          "[d2][drain][journal]") {
    auto scratch = make_scratch("d2_submit");
    ScopedDir guard{scratch};
    PendingJobsJournal j(scratch / "pending_jobs.json");

    // Simulate the production tray's apply_submit_with_context journal
    // write site (tray.cpp): on a successful process.submit return,
    // the tray appends an Entry to the journal with kind="submit",
    // slot_kind="postprocess", and the meeting_id+wav_path+job_id
    // captured from the per-recording state.
    PendingJobsJournal::Entry e;
    e.endpoint         = "/tmp/test-daemon.sock";
    e.meeting_id       = "12345678-aaaa-4000-8000-000000000001";
    e.job_id           = "42";
    e.staging_wav_path = "/home/op/.local/share/recmeet/staging/audio_x.wav";
    e.kind             = "submit";
    e.slot_kind        = to_string(SlotKind::Postprocess);
    e.submitted_at_unix = now_unix();
    j.append(e);

    auto loaded = j.load();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].endpoint          == e.endpoint);
    CHECK(loaded[0].meeting_id        == e.meeting_id);
    CHECK(loaded[0].job_id            == "42");
    CHECK(loaded[0].staging_wav_path  == e.staging_wav_path);
    CHECK(loaded[0].kind              == "submit");
    CHECK(loaded[0].slot_kind         == "postprocess");
    CHECK(loaded[0].submitted_at_unix > 0);

    // Confirm the on-disk file is the only artifact (atomic-write
    // contract: no stale .tmp; D.5 covered the contract — D.2's
    // repeat here is cheap insurance that the call-site rerun does
    // not leave behind a .tmp).
    fs::path tmp_path = scratch / "pending_jobs.json";
    tmp_path += ".tmp";
    CHECK_FALSE(fs::exists(tmp_path));
}

TEST_CASE("D.2: journal entry written for the streaming-slot path "
          "(process.stream open return) carries kind=stream and "
          "slot_kind=streaming",
          "[d2][drain][journal]") {
    auto scratch = make_scratch("d2_stream");
    ScopedDir guard{scratch};
    PendingJobsJournal j(scratch / "pending_jobs.json");

    // The plan-body cites `process.stream.commit` as the canonical
    // streaming journal-write point; the tray code does NOT call that
    // verb today (the daemon's StreamingSession auto-commits during
    // teardown), so the equivalent client-observable "daemon now
    // holds a reservation" boundary is `process.stream` open return.
    // This entry's `kind=stream` discriminator lets the recovery UI
    // tell the two flows apart on tray restart.
    PendingJobsJournal::Entry e;
    e.endpoint          = "/tmp/test-daemon.sock";
    e.meeting_id        = "abcdef01-bbbb-4000-8000-000000000002";
    e.job_id            = "99";
    e.staging_wav_path  = "/home/op/.local/share/recmeet/staging/audio_y.wav";
    e.kind              = "stream";
    e.slot_kind         = to_string(SlotKind::Streaming);
    e.submitted_at_unix = now_unix();
    j.append(e);

    auto loaded = j.load();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].kind      == "stream");
    CHECK(loaded[0].slot_kind == "streaming");
    CHECK(loaded[0].job_id    == "99");

    // Drain-side: when the terminal status arrives for this entry,
    // the journal removal keys on job_id.
    j.remove_by_job_id("99");
    CHECK(j.load().empty());
}

TEST_CASE("D.2: backlog drain — terminal status on in-flight pops next "
          "from the same slot's backlog and surfaces it for dispatch",
          "[d2][drain]") {
    SlotQueues q;

    // Stage: two postprocess entries (one in-flight, one in backlog)
    // + one streaming entry in-flight. Tests that drain only
    // touches the targeted slot.
    auto pp_a = make_entry("mtg-pp-A", "submit", 1001);
    auto pp_b = make_entry("mtg-pp-B", "submit");  // job_id assigned later
    auto st_x = make_entry("mtg-st-X", "stream",   2001);

    REQUIRE(q.postprocess.admit(pp_a));
    CHECK_FALSE(q.postprocess.admit(pp_b));  // backlog placement
    REQUIRE(q.streaming.admit(st_x));

    REQUIRE(q.postprocess.is_in_flight());
    CHECK(q.postprocess.backlog_size() == 1);
    CHECK(q.streaming.is_in_flight());

    // Drain the in-flight postprocess job (1001) — should promote
    // pp_b to in-flight and surface it via `next_to_dispatch`.
    auto drain = drain_on_terminal(q, 1001);
    REQUIRE(drain.matched);
    CHECK(drain.slot == SlotKind::Postprocess);
    CHECK(drain.meeting_id == "mtg-pp-A");
    REQUIRE(drain.next_to_dispatch.has_value());
    CHECK(drain.next_to_dispatch->meeting_id == "mtg-pp-B");

    // Post-drain: pp_b is the new in-flight; backlog is empty;
    // streaming is unaffected.
    REQUIRE(q.postprocess.in_flight() != nullptr);
    CHECK(q.postprocess.in_flight()->meeting_id == "mtg-pp-B");
    CHECK(q.postprocess.backlog_size() == 0);
    CHECK(q.streaming.in_flight() != nullptr);
    CHECK(q.streaming.in_flight()->meeting_id == "mtg-st-X");

    // Draining pp_b (now in-flight) clears the slot fully.
    // Backfill its job_id first (the production flow does this on
    // dispatch return; in the test we wire it directly).
    q.postprocess.set_in_flight_job_id(1002);
    auto drain2 = drain_on_terminal(q, 1002);
    REQUIRE(drain2.matched);
    CHECK_FALSE(drain2.next_to_dispatch.has_value());
    CHECK_FALSE(q.postprocess.is_in_flight());

    // A terminal status for a job_id not in any slot is a no-op (the
    // event might be for a different client's job, or a job we
    // already drained — the drain worker just logs and moves on).
    auto drain_unknown = drain_on_terminal(q, 99999);
    CHECK_FALSE(drain_unknown.matched);
    CHECK_FALSE(drain_unknown.next_to_dispatch.has_value());

    // Streaming slot continues to track its in-flight entry —
    // unaffected by postprocess drains.
    CHECK(q.streaming.is_in_flight());
    CHECK(q.streaming.in_flight()->job_id == 2001);
}
