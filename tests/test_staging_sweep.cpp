// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.6 — disk-budget retention sweep tests.
//
// Six tests, all tagged `[d6]`:
//   1. [d6][dbus-notify]         — D.3's NotifyFailed arm re-verification:
//                                  classify_resynced_job over the failed /
//                                  unknown / cancelled state inputs returns
//                                  NotifyFailed (the dbus-notify trigger).
//                                  Streaming-failed sets streaming_aborted;
//                                  postprocess-failed does NOT — the "no
//                                  auto-resubmit" clause is the absence of
//                                  the streaming-fallback flag for non-
//                                  streaming kinds.
//   2. [d6][budget-enforce]      — pre-fill staging with N WAVs over the
//                                  budget; assert sweep brings the total
//                                  ≤ budget.
//   3. [d6][oldest-first]        — pre-fill with varying-mtime WAVs;
//                                  assert oldest M evicted in mtime-asc
//                                  order, newest survive.
//   4. [d6][sidecar-protected]   — pre-fill with mixed (sidecar / plain)
//                                  all over budget; sidecar entries survive,
//                                  plain entries evicted (even if oldest).
//   5. [d6][journal-protected]   — pre-fill with mixed (journal-referenced /
//                                  plain) all over budget; journal entries
//                                  survive.
//   6. [d6][periodic-sweep]      — exercise sweep_staging directly with a
//                                  scratch dir + scratch journal-set (the
//                                  pure-helper carve-out lets the harness
//                                  bypass the GTK timer).
//
// All tests construct a scratch staging dir under /tmp so the operator's
// real staging dir is never touched. mtime is set via `utimes(2)` so
// "older" / "newer" is reproducible across runs without sleep_for.

#include <catch2/catch_test_macros.hpp>

#include "reconnect_backoff.h"
#include "staging_sweep.h"
#include "test_tmpdir.h"
#include "util.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace recmeet;
namespace fs = std::filesystem;

namespace {

fs::path make_scratch() {
    std::random_device rd;
    std::ostringstream oss;
    oss << "recmeet_d6_sweep_" << ::getpid() << "_" << rd();
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

// Write a WAV-like blob of `size_bytes` bytes at `path`. The content is
// not parsed by the sweep — the eviction policy only looks at file size,
// mtime, and the .pending sibling. A pile of zero bytes is fine.
void write_wav_blob(const fs::path& path, std::uintmax_t size_bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(f.good());
    // Write in chunks to avoid allocating a multi-MB string on the stack.
    constexpr size_t kChunk = 4096;
    std::vector<char> chunk(kChunk, 0);
    std::uintmax_t remaining = size_bytes;
    while (remaining > 0) {
        size_t n = static_cast<size_t>(std::min<std::uintmax_t>(remaining, kChunk));
        f.write(chunk.data(), static_cast<std::streamsize>(n));
        REQUIRE(f.good());
        remaining -= n;
    }
}

// Set the mtime of `path` to `unix_secs`. Used to pin "oldest first"
// orderings across test runs without sleep_for.
void set_mtime(const fs::path& path, std::time_t unix_secs) {
    struct timeval tv[2];
    tv[0].tv_sec = unix_secs; tv[0].tv_usec = 0;  // atime
    tv[1].tv_sec = unix_secs; tv[1].tv_usec = 0;  // mtime
    int rc = ::utimes(path.c_str(), tv);
    REQUIRE(rc == 0);
}

// Write a `.pending` sidecar next to `wav_path`. The content is not
// parsed by the sweep — only the file's presence matters per D.5
// (`is_sidecar_protected`). Tiny stub mirrors the v2 schema fields.
void write_sidecar(const fs::path& wav_path) {
    fs::path sidecar = wav_path;
    sidecar.replace_extension(".pending");
    std::ofstream f(sidecar);
    REQUIRE(f.good());
    f << "{\"meeting_id\":\"00000000-0000-4000-8000-000000000000\","
      << "\"wav_path\":\"" << wav_path.string() << "\","
      << "\"timestamp\":\"2026-05-18_00-00\","
      << "\"mic_source\":\"test\","
      << "\"captions_enabled\":false,"
      << "\"context\":{\"subject\":\"\",\"participants\":[],\"notes\":\"\","
      << "\"language\":\"\",\"vocabulary\":[]}}";
}

std::uintmax_t total_wav_bytes(const fs::path& dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (de.path().extension() != ".wav") continue;
        std::error_code se;
        auto sz = fs::file_size(de.path(), se);
        if (!se) total += sz;
    }
    return total;
}

bool wav_exists(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    return fs::exists(dir / name, ec) && !ec;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test 1 — server-restart half: D.3 NotifyFailed arm re-verification.
//
// This is a D.3 re-verification more than a D.6 unit test — the dbus
// notification infrastructure was wired in D.3 (commit 58d660d) and D.6
// only adds the disk-budget retention half. We pin the contract here so
// any future regression in the classifier shows up under `[d6]`:
//   * "failed" / "cancelled" / "unknown" → NotifyFailed (the dbus hook).
//   * "running" / "queued" / "done"      → NOT NotifyFailed.
//   * "failed" + kind=="streaming"        → streaming_aborted = true
//                                            (convergence-pattern-2 fallback).
//   * "failed" + kind=="postprocess"      → streaming_aborted = false
//                                            (NO auto-resubmit; D.6 spec).
// ---------------------------------------------------------------------------

TEST_CASE("D.6: failed/unknown jobs classify as NotifyFailed (dbus-notify hook)",
          "[d6][dbus-notify]") {
    SECTION("failed postprocess → NotifyFailed, no streaming_aborted") {
        auto c = classify_resynced_job("failed", "postprocess");
        REQUIRE(c.action == JobResyncAction::NotifyFailed);
        REQUIRE_FALSE(c.streaming_aborted);
    }
    SECTION("failed streaming → NotifyFailed, streaming_aborted set") {
        auto c = classify_resynced_job("failed", "streaming");
        REQUIRE(c.action == JobResyncAction::NotifyFailed);
        REQUIRE(c.streaming_aborted);
    }
    SECTION("unknown postprocess → NotifyFailed, no streaming_aborted") {
        auto c = classify_resynced_job("unknown", "postprocess");
        REQUIRE(c.action == JobResyncAction::NotifyFailed);
        REQUIRE_FALSE(c.streaming_aborted);
    }
    SECTION("cancelled → NotifyFailed") {
        auto c = classify_resynced_job("cancelled", "postprocess");
        REQUIRE(c.action == JobResyncAction::NotifyFailed);
        REQUIRE_FALSE(c.streaming_aborted);
    }
    SECTION("running → Monitor (no notify, no fallback)") {
        auto c = classify_resynced_job("running", "postprocess");
        REQUIRE(c.action != JobResyncAction::NotifyFailed);
        REQUIRE_FALSE(c.streaming_aborted);
    }
    SECTION("done → Fetch (no notify, no fallback)") {
        auto c = classify_resynced_job("done", "postprocess");
        REQUIRE(c.action == JobResyncAction::Fetch);
        REQUIRE_FALSE(c.streaming_aborted);
    }
}

// ---------------------------------------------------------------------------
// Test 2 — disk-budget enforcement.
//
// Pre-fill staging with N WAVs whose cumulative size exceeds the budget,
// run the sweep, assert the resulting total is at or below the budget.
// Uses 1 MB WAVs and a 4 MB budget so we can fit 8 in the dir and require
// at least 4 to be evicted — small enough to be quick on tmpfs.
// ---------------------------------------------------------------------------

TEST_CASE("D.6: sweep enforces staging_max_bytes — total ≤ budget",
          "[d6][budget-enforce]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    constexpr std::uintmax_t kWavBytes = 1024 * 1024;     // 1 MB each
    constexpr std::uintmax_t kBudget   = 4 * 1024 * 1024; // 4 MB
    constexpr int kCount = 8;                              // 8 MB total

    std::time_t base = 1700000000;
    for (int i = 0; i < kCount; ++i) {
        fs::path p = scratch / ("audio_" + std::to_string(i) + ".wav");
        write_wav_blob(p, kWavBytes);
        set_mtime(p, base + i);  // older → newer with i
    }
    REQUIRE(total_wav_bytes(scratch) == kCount * kWavBytes);

    auto done = sweep_staging(scratch, /*protected=*/{}, kBudget);

    REQUIRE(total_wav_bytes(scratch) <= kBudget);
    REQUIRE(done.size() >= 4);  // 8 MB - 4 MB budget = 4 MB worth must go.
}

// ---------------------------------------------------------------------------
// Test 3 — oldest-first eviction order.
//
// Pre-fill with WAVs of monotonically increasing mtime; assert the sweep
// evicts the OLDEST WAVs (mtime asc) and leaves the newest untouched.
// ---------------------------------------------------------------------------

TEST_CASE("D.6: sweep evicts oldest WAVs first by mtime",
          "[d6][oldest-first]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    constexpr std::uintmax_t kWavBytes = 1024 * 1024;
    constexpr std::uintmax_t kBudget   = 3 * 1024 * 1024;
    constexpr int kCount = 6;  // 6 MB total → evict 3 oldest.

    std::time_t base = 1700000000;
    std::vector<std::string> names;
    for (int i = 0; i < kCount; ++i) {
        std::string n = "audio_" + std::to_string(i) + ".wav";
        fs::path p = scratch / n;
        write_wav_blob(p, kWavBytes);
        set_mtime(p, base + i);  // i=0 oldest, i=5 newest
        names.push_back(n);
    }

    auto done = sweep_staging(scratch, /*protected=*/{}, kBudget);
    REQUIRE(done.size() == 3);

    // The 3 oldest (i=0,1,2) must be gone; the 3 newest (i=3,4,5) survive.
    for (int i = 0; i < 3; ++i) {
        REQUIRE_FALSE(wav_exists(scratch, names[i]));
    }
    for (int i = 3; i < kCount; ++i) {
        REQUIRE(wav_exists(scratch, names[i]));
    }
}

// ---------------------------------------------------------------------------
// Test 4 — sidecar-protected eviction.
//
// Mix sidecar-protected and plain WAVs at varying mtimes; total exceeds
// budget. Even if a sidecar-protected WAV is the OLDEST it must survive
// (sidecar presence = save-for-later = PROTECTED per D.5).
// ---------------------------------------------------------------------------

TEST_CASE("D.6: sidecar-protected WAVs survive sweep even when oldest",
          "[d6][sidecar-protected]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    constexpr std::uintmax_t kWavBytes = 1024 * 1024;
    constexpr std::uintmax_t kBudget   = 2 * 1024 * 1024;

    std::time_t base = 1700000000;

    // The OLDEST WAV (mtime 0) has a sidecar — must survive.
    fs::path old_sidecar = scratch / "audio_old_sidecar.wav";
    write_wav_blob(old_sidecar, kWavBytes);
    set_mtime(old_sidecar, base + 0);
    write_sidecar(old_sidecar);

    // Three plain WAVs at later mtimes. Total = 4 MB; budget 2 MB; protected
    // 1 MB counts against the total → must evict 2 of the 3 plain ones,
    // oldest-of-the-plain-set first.
    fs::path plain1 = scratch / "audio_plain1.wav";
    fs::path plain2 = scratch / "audio_plain2.wav";
    fs::path plain3 = scratch / "audio_plain3.wav";
    write_wav_blob(plain1, kWavBytes); set_mtime(plain1, base + 1);
    write_wav_blob(plain2, kWavBytes); set_mtime(plain2, base + 2);
    write_wav_blob(plain3, kWavBytes); set_mtime(plain3, base + 3);

    auto done = sweep_staging(scratch, /*protected=*/{}, kBudget);

    REQUIRE(fs::exists(old_sidecar));  // protected
    // plain1 + plain2 evicted (oldest of the plain set); plain3 survives.
    REQUIRE_FALSE(fs::exists(plain1));
    REQUIRE_FALSE(fs::exists(plain2));
    REQUIRE(fs::exists(plain3));
    REQUIRE(done.size() == 2);
}

// ---------------------------------------------------------------------------
// Test 5 — journal-protected eviction.
//
// Mix journal-referenced and plain WAVs; total exceeds budget. Even the
// oldest journal-referenced WAV must survive (journal = in-flight upload
// per D.5; the drain worker may still be reading bytes from it).
// ---------------------------------------------------------------------------

TEST_CASE("D.6: journal-referenced WAVs survive sweep even when oldest",
          "[d6][journal-protected]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    constexpr std::uintmax_t kWavBytes = 1024 * 1024;
    constexpr std::uintmax_t kBudget   = 2 * 1024 * 1024;

    std::time_t base = 1700000000;

    // Two journal-referenced WAVs — the oldest pair. Both must survive
    // even though they are the oldest entries (in-flight upload contract).
    fs::path j1 = scratch / "audio_journal1.wav";
    fs::path j2 = scratch / "audio_journal2.wav";
    write_wav_blob(j1, kWavBytes); set_mtime(j1, base + 0);
    write_wav_blob(j2, kWavBytes); set_mtime(j2, base + 1);

    // Three plain WAVs at later mtimes. Total = 5 MB; budget 2 MB; protected
    // 2 MB → must evict all 3 plain (5 MB - 3 MB = 2 MB still > budget but
    // no safe entries left; sweep returns all 3 evicted; total = 2 MB
    // ≤ budget).
    fs::path p1 = scratch / "audio_plain1.wav";
    fs::path p2 = scratch / "audio_plain2.wav";
    fs::path p3 = scratch / "audio_plain3.wav";
    write_wav_blob(p1, kWavBytes); set_mtime(p1, base + 2);
    write_wav_blob(p2, kWavBytes); set_mtime(p2, base + 3);
    write_wav_blob(p3, kWavBytes); set_mtime(p3, base + 4);

    std::unordered_set<std::string> protected_paths = {
        j1.string(), j2.string(),
    };

    auto done = sweep_staging(scratch, protected_paths, kBudget);

    REQUIRE(fs::exists(j1));
    REQUIRE(fs::exists(j2));
    REQUIRE_FALSE(fs::exists(p1));
    REQUIRE_FALSE(fs::exists(p2));
    REQUIRE_FALSE(fs::exists(p3));
    REQUIRE(done.size() == 3);
    REQUIRE(total_wav_bytes(scratch) == 2 * kWavBytes);  // ≤ budget
}

// ---------------------------------------------------------------------------
// Test 6 — periodic sweep harness (pure-helper carve-out).
//
// The actual periodic sweep is registered via `g_timeout_add_seconds(600, …)`
// inside `tray.cpp::main()`, which we cannot exercise without booting GTK.
// Per the D.4 testable-carve-out pattern (`tray_status`), the policy lives
// in `sweep_staging(...)` — a pure function callable from tests. This test
// pins the contract that the same input → same output across repeated
// invocations (idempotent on a clean slate) so the timer can fire on the
// 10 min cadence without behavioral drift.
// ---------------------------------------------------------------------------

TEST_CASE("D.6: sweep_staging is idempotent across repeated calls (periodic harness)",
          "[d6][periodic-sweep]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    constexpr std::uintmax_t kWavBytes = 1024 * 1024;
    constexpr std::uintmax_t kBudget   = 3 * 1024 * 1024;
    constexpr int kCount = 5;

    std::time_t base = 1700000000;
    for (int i = 0; i < kCount; ++i) {
        fs::path p = scratch / ("audio_" + std::to_string(i) + ".wav");
        write_wav_blob(p, kWavBytes);
        set_mtime(p, base + i);
    }

    // First sweep — must bring total to budget (≤ 3 MB) by evicting 2 oldest.
    auto done1 = sweep_staging(scratch, /*protected=*/{}, kBudget);
    REQUIRE(done1.size() == 2);
    REQUIRE(total_wav_bytes(scratch) <= kBudget);

    // Second sweep — nothing to do; under budget → returns empty.
    auto done2 = sweep_staging(scratch, /*protected=*/{}, kBudget);
    REQUIRE(done2.empty());
    REQUIRE(total_wav_bytes(scratch) <= kBudget);

    // Third sweep — still nothing. Idempotent.
    auto done3 = sweep_staging(scratch, /*protected=*/{}, kBudget);
    REQUIRE(done3.empty());
}

// ---------------------------------------------------------------------------
// Test 7 — plan_evictions with extra_pending_bytes (synchronous-at-start path).
//
// Pins the projected-total math used by tray.cpp::start_capture: when a
// new recording is about to write ~460 MB, the sweep must account for
// that pending growth, not just the current on-disk total. Test by
// pre-filling at exactly the budget; an additional projected 1 byte
// should trigger at least one eviction.
// ---------------------------------------------------------------------------

TEST_CASE("D.6: extra_pending_bytes counts toward the budget (synchronous-at-start)",
          "[d6][budget-enforce]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    constexpr std::uintmax_t kWavBytes = 1024 * 1024;
    constexpr std::uintmax_t kBudget   = 4 * 1024 * 1024;
    constexpr int kCount = 4;  // total = budget exactly

    std::time_t base = 1700000000;
    for (int i = 0; i < kCount; ++i) {
        fs::path p = scratch / ("audio_" + std::to_string(i) + ".wav");
        write_wav_blob(p, kWavBytes);
        set_mtime(p, base + i);
    }

    // No extra → no eviction needed.
    auto none = sweep_staging(scratch, /*protected=*/{}, kBudget, /*extra=*/0);
    REQUIRE(none.empty());

    // Add 1 byte of projected pending → must evict the oldest to make room.
    auto with_extra =
        sweep_staging(scratch, /*protected=*/{}, kBudget, /*extra=*/1);
    REQUIRE(with_extra.size() == 1);
    REQUIRE(with_extra[0].wav_path.filename() == "audio_0.wav");  // oldest
}
