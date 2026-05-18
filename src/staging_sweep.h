// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.6 — staging-directory disk-budget eviction sweep.
//
// Pure helpers carved out of `tray.cpp` so the eviction policy can be
// unit-tested without booting GTK or the IPC stack. Mirrors the
// D.3 (`reconnect_backoff`) / D.4 (`tray_status`) pure-helper pattern.
//
// The sweep walks `~/.local/share/recmeet/staging/` (configurable for
// tests), classifies each `*.wav` file as either "protected" (referenced
// by `pending_jobs.json` OR has a sibling `.pending` sidecar) or
// "safe-to-evict", and unlinks safe entries oldest-first (by mtime) until
// the total staging size drops at or below the operator-configured
// `staging_max_bytes` budget. The default budget (500 GiB) matches the
// `journald` MaxUse model — operators size it per host according to the
// local storage budget; daemon enforces.
//
// **Protection contract** (plan line 407):
//   * `.pending` sidecar present       → save-for-later (D.5);   PROTECTED
//   * journal entry references the WAV → in-flight upload (D.5); PROTECTED
//   * neither                          → safe to evict (oldest first).
//
// Server-side copies are unaffected — D.6 is exclusively about the
// CLIENT staging tier. The server's retention policy is discussed in
// docs/V2-STRATEGY.md "Disk-space implications".
//
// **Concurrent-access safety with the D.2 drain worker**:
//   The drain worker writes the journal entry BEFORE it begins uploading
//   the WAV (plan line 354 — "Records [...] before audio upload begins
//   for `process.submit` (so mid-upload tray-crash recovery has a
//   meeting_id to resume by)"). The sweep loads the journal AFTER
//   stat'ing the directory, then re-checks protection per WAV before
//   `unlink()`. The atomic-write contract on the journal means the
//   sweep observes either the prior snapshot (entry absent — but in
//   that case the upload has not yet begun; the WAV mtime is fresh
//   and would not be the oldest) or the new snapshot (entry present
//   → PROTECTED → not evicted). On Linux, `unlink()` on an open file
//   keeps the inode alive until the last fd closes, so even a
//   pathological race window in which the sweep removes a WAV the
//   drain worker just opened for reading does not corrupt the upload
//   in flight — the upload reads the unlinked-but-still-open inode to
//   completion. This is defense in depth; the load-after-stat ordering
//   makes the race vanishingly unlikely in the first place.

#pragma once

#include "util.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <unordered_set>
#include <vector>

namespace recmeet {

/// One WAV file discovered by `scan_staging_dir`. Carries enough state
/// for the eviction policy to make a per-file decision without
/// re-stat'ing.
struct StagingEntry {
    fs::path     wav_path;          ///< absolute path to the WAV
    std::uintmax_t size_bytes = 0;  ///< file size in bytes (from stat)
    std::time_t  mtime = 0;         ///< last-modified time (from stat)
    bool sidecar_protected = false; ///< `.pending` sibling present
    bool journal_protected = false; ///< referenced in the in-flight journal
};

/// One eviction performed by `sweep_staging`. Logged at info; returned
/// so the caller (and tests) can attribute totals + assertions without
/// scraping the log.
struct EvictionRecord {
    fs::path       wav_path;
    std::uintmax_t size_bytes = 0;
    std::string    reason;   ///< human-readable; e.g. "over staging_max_bytes"
};

/// Default expected size for a fresh recording — used by the
/// synchronous-at-recording-start path to project current+new total
/// against the budget BEFORE opening the WAV. 4 hours of 16 kHz mono
/// int16 = 460,800,000 bytes (plan line 405). Constant rather than
/// configurable: the cap is a guardrail, not an operator-facing tunable.
constexpr std::uintmax_t DEFAULT_MAX_RECORDING_BYTES =
    static_cast<std::uintmax_t>(4) * 60 * 60 * 16000 * 2;  // 460,800,000

/// Default disk budget for client staging (plan line 404). 500 GiB,
/// matching the journald MaxUse default model. Single-source-of-truth
/// so the `staging_max_bytes` Config field and the sweep helper agree.
constexpr std::uintmax_t DEFAULT_STAGING_MAX_BYTES =
    static_cast<std::uintmax_t>(500) * 1024 * 1024 * 1024;  // 500 GiB

/// Scan `staging_dir` and return one `StagingEntry` per `*.wav` file
/// found at the top level (sub-directories are not recursed —
/// `tray_capture` writes flat). Each entry's `sidecar_protected` flag
/// is set when an `<wav_basename>.pending` sibling exists in the same
/// directory. `journal_protected` is set when `wav_path.string()` (or
/// its canonical form) appears in `protected_staging_paths`.
///
/// Robust to:
///   * `staging_dir` not existing  → returns empty vector (silent).
///   * `staging_dir` not readable  → returns empty vector + log_warn.
///   * Symlinks                    → followed (lstat would let an
///                                   operator with a symlinked WAV
///                                   evade the budget; not a useful
///                                   invariant for a single-operator
///                                   tray).
///   * Files we cannot stat        → skipped + log_warn (do not throw).
///
/// Pure (no logging side effects on the happy path) so tests can call
/// it freely without flooding the test runner output.
std::vector<StagingEntry>
scan_staging_dir(const fs::path& staging_dir,
                 const std::unordered_set<std::string>& protected_staging_paths);

/// Sum the on-disk size of every `*.wav` (regardless of protected
/// state). Cheaper than walking the entry list when the caller only
/// wants the current usage figure (e.g. the synchronous-at-start
/// projection).
std::uintmax_t current_staging_bytes(const fs::path& staging_dir);

/// Compute the eviction plan for `entries` against `max_bytes`. Pure;
/// no I/O. Filters out `sidecar_protected || journal_protected`,
/// sorts the remainder ascending by `mtime`, and returns the prefix
/// whose cumulative size brings the OVERALL total at or below
/// `max_bytes`. The OVERALL total counts ALL entries (protected
/// + safe) — protected WAVs cannot be evicted but they DO count
/// against the budget (the operator chose to retain them; the budget
/// is a hard cap on disk use, not a guideline). When even unlinking
/// every safe-to-evict entry would leave the total above `max_bytes`
/// the helper returns the full list of safe entries; the caller logs
/// a warning at the call site (sweep_staging does so).
///
/// `extra_pending_bytes` is added to the total before the budget
/// comparison, used by the synchronous-at-start projection to include
/// the about-to-be-written WAV in the calculation (the file does not
/// yet exist on disk so `scan_staging_dir` cannot see it). Default 0.
///
/// Stable ordering for equal-mtime ties: secondary key is
/// `wav_path.string()` lexicographic (so tests with mtime collisions
/// are deterministic across runs).
std::vector<StagingEntry>
plan_evictions(const std::vector<StagingEntry>& entries,
               std::uintmax_t max_bytes,
               std::uintmax_t extra_pending_bytes = 0);

/// Execute the eviction plan: unlink each WAV in `to_evict`, returning
/// the list of completed evictions (the path + size + a fixed reason
/// string for logging by the caller). On `unlink()` failure the entry
/// is skipped + log_warn'd; the sweep continues with the next entry
/// rather than aborting the whole pass — a partial sweep is strictly
/// better than no sweep when one stale WAV's mode is wrong.
std::vector<EvictionRecord>
apply_evictions(const std::vector<StagingEntry>& to_evict);

/// Convenience: scan + plan + apply, logging would-evict-but-protected
/// entries at debug. Returns the list of completed evictions so the
/// caller can attribute totals. Logs each eviction at info with path
/// + size + reason.
///
/// `extra_pending_bytes` (default 0) is the projected size of an
/// about-to-be-created WAV that should be included in the budget
/// comparison — see `plan_evictions`.
std::vector<EvictionRecord>
sweep_staging(const fs::path& staging_dir,
              const std::unordered_set<std::string>& protected_staging_paths,
              std::uintmax_t max_bytes,
              std::uintmax_t extra_pending_bytes = 0);

} // namespace recmeet
