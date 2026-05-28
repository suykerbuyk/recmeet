// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.5 — submitted-but-incomplete jobs journal.
//
// Persists the tuples needed to resume tracking of every postprocess /
// streaming job the tray has handed to the daemon but for which it has
// not yet observed terminal status (`job.complete` / `job.failed` /
// cancel). On tray startup, the journal is loaded, each entry is
// resolved via `job.status` (D.3 wiring), and the local UI is rehydrated
// from the daemon's record-of-truth.
//
// File: `server_state_dir() / "pending_jobs.json"` →
//        `~/.local/state/recmeet-server/pending_jobs.json`.
//
// **Disjointness with the `.pending` sidecar** (D.5 plan, line 398):
// a WAV is either in-flight (journal entry, no sidecar) OR save-for-later
// (sidecar, no journal entry). The save-for-later path is the ONLY writer
// of sidecars; the submit-return path is the ONLY writer of journal
// entries (D.5 only provides the class; D.2 wires the call site). The
// class enforces this on the read side too: `load()` returns the
// snapshot verbatim and the caller treats sidecar-vs-journal as the
// authoritative dispatch.
//
// **Atomic-write contract**: every mutation goes through
// `util::atomic_write_file` so a crash mid-write cannot corrupt the
// journal — concurrent readers see either the prior snapshot or the new
// one, never a partial line.
//
// **File format**: a top-level JSON array of entry objects. Each entry
// carries the seven fields enumerated in `Entry` below. The format is
// flat (no nested objects) so the in-class hand-parser stays small;
// adding fields is additive (unknown keys are ignored on load).

#pragma once

#include "util.h"

#include <cstdint>
#include <string>
#include <vector>

namespace recmeet {

class PendingJobsJournal {
public:
    /// One submitted-but-incomplete job. Field names match the plan-body
    /// schema (`endpoint, meeting_id, job_id, staging_wav_path, kind,
    /// slot_kind, submitted_at_unix`) so on-disk format is self-documenting.
    struct Entry {
        /// Daemon address from which the resume_token was issued.
        /// Equality semantics per plan-body line 396: rename → break (the
        /// next reconnect surfaces the entry as unknown via D.6).
        std::string endpoint;

        /// Client-minted UUID v4 binding the recording to its server-side
        /// dedup index entry. Re-submitted as-is on the H-D3 retry path
        /// so the C.11.4 atomic-overwrite contract routes the bytes back
        /// to the same meeting directory.
        std::string meeting_id;

        /// Server-assigned `job_id` from the original `process.submit`
        /// (or `process.stream`) response. Used to call `job.status` /
        /// `process.fetch` / `process.cancel` post-restart.
        std::string job_id;

        /// Absolute path to the staging WAV on the local filesystem. Kept
        /// in the journal so the H-D3 retry path can re-stream the bytes
        /// without re-stating staging from `meeting_id`.
        std::string staging_wav_path;

        /// "submit" (postprocess upload via `process.submit`) or "stream"
        /// (live captions via `process.stream`). Streaming entries are
        /// rare in the journal — only present if the tray crashed
        /// mid-stream; D.3 wires the abort-and-fall-back-to-batch path.
        std::string kind;

        /// JobQueue slot the daemon parked this job in ("postprocess",
        /// "streaming", or "model_download"). Mirrors C.7's typed-slot
        /// shape so the recovery UI can attribute the per-slot status
        /// display correctly.
        std::string slot_kind;

        /// Unix seconds at submit-return time, captured by the writer.
        /// Sorting on this field gives the operator-facing "oldest first"
        /// view; D.6's eviction sweep also keys on it.
        int64_t submitted_at_unix = 0;
    };

    /// Default path: `~/.local/share/recmeet/pending_jobs.json`.
    PendingJobsJournal();

    /// Custom path for tests so each `TEST_CASE` writes to a scratch
    /// directory under `/tmp` instead of touching the operator's actual
    /// staging dir.
    explicit PendingJobsJournal(fs::path path);

    /// Return all entries currently on disk. Returns an empty vector if
    /// the file does not exist (fresh install, nothing in flight) or if
    /// the file is unreadable (logged at warn).
    std::vector<Entry> load() const;

    /// Replace the on-disk file with `entries` (atomic write). Order is
    /// preserved exactly; the writer does not sort.
    void save(const std::vector<Entry>& entries) const;

    /// Convenience: load → append → save. Equivalent to a load + push +
    /// save but spelled out so the call site reads as a single operation.
    void append(const Entry& entry) const;

    /// Convenience: load → remove all entries whose `job_id` matches →
    /// save. A `job_id` of "" matches nothing (defensive; tray should
    /// never call with an empty id). A no-op (no save) if no entry
    /// matched.
    void remove_by_job_id(const std::string& job_id) const;

    /// Path the journal reads/writes. Useful for tests.
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

} // namespace recmeet
