// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.1 — per-slot-kind submission queue.
//
// Three small client-side queues (one per typed-slot kind:
// `postprocess`, `streaming`, `model_download`), each capacity-1-in-
// flight with an arbitrary-length FIFO backlog. All three are
// independent — a `postprocess` save-for-later batch upload does NOT
// block waiting for an active `streaming` session to commit. Mirrors
// C.7's server-side typed-slot model so the client does not artificially
// serialize what the server is happy to run concurrently.
//
// Retires `TrayState::last_pp_job_id` (a single scalar that only
// tracked the most-recent postprocess job): cancel-postprocess paths
// now read `slot_queues.postprocess.in_flight().job_id`, and the
// streaming + model_download slots gain analogous tracking that the
// old scalar never represented.
//
// **Drain semantics (D.2)**: on terminal status (`job.complete` /
// `job.failed`) of the in-flight entry, the slot's `complete_in_flight`
// clears the in-flight slot and returns the next backlog entry (if
// any). The caller dispatches that entry — keeping the dispatch shape
// outside the queue means we can unit-test queue behavior without
// linking the tray's IPC stack.
//
// Thread safety: the tray uses a single-threaded model (every
// `g_tray.*` access happens on the GTK main thread), so SlotQueue does
// NOT take a lock. Tests assume the same — none of the test cases below
// spawn worker threads. If a future caller needs cross-thread access,
// wrap a SlotQueues instance behind an explicit mutex at the call site.

#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace recmeet {

/// Slot identifier — mirrors the daemon-side `JobQueue::Slot` enum
/// (C.7) so the wire-side `slot_kind` field can be derived from this
/// and vice-versa.
enum class SlotKind {
    Postprocess,
    Streaming,
    ModelDownload,
};

/// Convert a SlotKind to the on-wire `slot_kind` string used in
/// `pending_jobs.json` and `process.submit` / `process.stream` params.
inline const char* to_string(SlotKind k) {
    switch (k) {
        case SlotKind::Postprocess:   return "postprocess";
        case SlotKind::Streaming:     return "streaming";
        case SlotKind::ModelDownload: return "model_download";
    }
    return "postprocess";  // unreachable; default keeps `-Wreturn-type` quiet
}

/// One queued submission. Carries only the fields the drain worker
/// (D.2) needs to either (a) re-dispatch from the backlog or (b)
/// resolve cancel/cleanup. The full per-job context (audio bytes,
/// captured prompt, etc.) lives in the staging WAV + `.pending`
/// sidecar; the queue itself is metadata-only.
struct JobEntry {
    /// Server-assigned job_id from the original `process.submit`
    /// (or `process.stream`) response. 0 → not yet dispatched (entry
    /// sits in the backlog).
    int64_t job_id = 0;

    /// Client-minted UUID v4 binding the recording to its server-side
    /// dedup index entry. Identifies the job in the journal even
    /// before `job_id` is assigned (a backlog entry has a meeting_id
    /// but no job_id yet).
    std::string meeting_id;

    /// Absolute path to the staging WAV (or other input). Empty for
    /// `model_download` entries, which dispatch via the
    /// `models.ensure` verb without a file body.
    std::string staging_wav_path;

    /// "submit" or "stream" — matches the journal `kind` field.
    /// Streaming entries arrive in the `streaming` slot, postprocess
    /// in `postprocess`, and model_download entries use the
    /// `model_download` slot with kind "model_download" for
    /// completeness (the kind string is mostly load-bearing for the
    /// recovery-side UI).
    std::string kind;
};

/// One per-slot queue: at most one `in_flight` job, arbitrary-length
/// `backlog`. Admission goes through `admit` (which places the entry
/// in `in_flight` if free, otherwise the backlog tail). Terminal
/// status goes through `complete_in_flight` (which clears `in_flight`
/// and returns the next-to-dispatch entry, if any).
class SlotQueue {
public:
    /// Push `entry` onto the queue. If the slot is idle (no in-flight
    /// job), the entry becomes the in-flight entry and `is_in_flight`
    /// returns true. Otherwise the entry joins the backlog tail.
    ///
    /// Returns true if the entry was promoted to in-flight (caller
    /// dispatches immediately); false if it joined the backlog
    /// (caller waits for the in-flight job to terminate, at which
    /// point `complete_in_flight` will surface it).
    bool admit(JobEntry entry) {
        if (!in_flight_.has_value()) {
            in_flight_ = std::move(entry);
            return true;
        }
        backlog_.push_back(std::move(entry));
        return false;
    }

    /// Mark the in-flight entry's job_id once the server's response
    /// returns it. Pre-dispatch (when `admit` runs before the
    /// `process.submit` round-trip), the entry has `job_id == 0`;
    /// post-response, the caller calls `set_in_flight_job_id` so
    /// later cancel/complete lookups can find the entry by id.
    void set_in_flight_job_id(int64_t job_id) {
        if (in_flight_.has_value()) {
            in_flight_->job_id = job_id;
        }
    }

    /// Pop the in-flight entry (terminal status). Returns the next
    /// backlog entry (now promoted to in-flight) if any, or
    /// std::nullopt if the slot is now fully idle. Caller dispatches
    /// the returned entry via the same path used at original
    /// admission.
    std::optional<JobEntry> complete_in_flight() {
        in_flight_.reset();
        if (!backlog_.empty()) {
            JobEntry next = std::move(backlog_.front());
            backlog_.pop_front();
            in_flight_ = next;
            return next;
        }
        return std::nullopt;
    }

    /// True iff a job is currently in flight (admitted and not yet
    /// terminal). The cancel paths key on this to decide whether
    /// `process.cancel { job_id }` has a job to reference.
    bool is_in_flight() const { return in_flight_.has_value(); }

    /// Read-only access to the in-flight entry. Returns nullptr if
    /// nothing is in flight — caller checks before dereferencing.
    const JobEntry* in_flight() const {
        return in_flight_.has_value() ? &(*in_flight_) : nullptr;
    }

    /// Number of entries currently sitting in the backlog (not
    /// counting the in-flight entry). Exposed for the tray status UI
    /// (D.4) and tests.
    std::size_t backlog_size() const { return backlog_.size(); }

    /// Clear in-flight + backlog. Used by the dtor cleanup path when
    /// the tray exits with a pending cancel in flight.
    void clear() {
        in_flight_.reset();
        backlog_.clear();
    }

private:
    std::optional<JobEntry> in_flight_;
    std::deque<JobEntry> backlog_;
};

/// Drain-worker result emitted on terminal status of an in-flight
/// entry. Tells the caller whether the terminal id matched a slot
/// (so the journal entry should be removed) and, if so, whether a
/// backlog entry was promoted and now needs dispatch.
struct DrainResult {
    /// True iff `job_id` matched an in-flight entry in some slot.
    /// False → daemon emitted a terminal event for a job we did not
    /// track (different client_id, or already drained). Caller logs
    /// at debug and moves on.
    bool matched = false;
    /// Slot the matched entry lived in. Meaningful only when
    /// `matched=true`.
    SlotKind slot = SlotKind::Postprocess;
    /// Meeting id of the terminated entry — convenient for the
    /// journal-removal call (journal keys on job_id, but the recovery
    /// UI may want meeting_id too).
    std::string meeting_id;
    /// If a backlog entry was promoted to in-flight, it appears here
    /// and the caller must dispatch it (re-issue `process.submit` /
    /// `process.stream` for that entry). If the slot has no backlog,
    /// this is std::nullopt and the slot is now fully idle.
    std::optional<JobEntry> next_to_dispatch;
};

/// Three independent slots — one per typed-slot kind. Selector helper
/// returns a reference so the caller can chain `.admit()` /
/// `.complete_in_flight()` without an outer switch.
struct SlotQueues {
    SlotQueue postprocess;
    SlotQueue streaming;
    SlotQueue model_download;

    SlotQueue& select(SlotKind k) {
        switch (k) {
            case SlotKind::Postprocess:   return postprocess;
            case SlotKind::Streaming:     return streaming;
            case SlotKind::ModelDownload: return model_download;
        }
        return postprocess;  // unreachable
    }

    const SlotQueue& select(SlotKind k) const {
        switch (k) {
            case SlotKind::Postprocess:   return postprocess;
            case SlotKind::Streaming:     return streaming;
            case SlotKind::ModelDownload: return model_download;
        }
        return postprocess;  // unreachable
    }

    /// Locate the in-flight entry across all three slots whose
    /// job_id matches `job_id`. Used by the drain worker's terminal-
    /// status dispatch: the daemon's `job.complete` / `job.failed`
    /// event carries only the `job_id`, so the slot is inferred by
    /// searching the in-flight set. Returns SlotKind::Postprocess +
    /// `found_out=false` if no in-flight entry matches (the event is
    /// for a job we did not track — log and drop).
    SlotKind find_slot_by_in_flight_job_id(int64_t job_id,
                                           bool& found_out) const {
        found_out = false;
        if (postprocess.in_flight() &&
            postprocess.in_flight()->job_id == job_id) {
            found_out = true;
            return SlotKind::Postprocess;
        }
        if (streaming.in_flight() &&
            streaming.in_flight()->job_id == job_id) {
            found_out = true;
            return SlotKind::Streaming;
        }
        if (model_download.in_flight() &&
            model_download.in_flight()->job_id == job_id) {
            found_out = true;
            return SlotKind::ModelDownload;
        }
        return SlotKind::Postprocess;
    }
};

/// Phase D.2 drain worker — pure, side-effect-free on the wire (does
/// NOT call any dispatch). Locates the slot whose in-flight entry has
/// the given `job_id`, pops it, and returns the next backlog entry (if
/// any) via `DrainResult.next_to_dispatch`. The caller is responsible
/// for (a) removing the journal entry by `job_id`, and (b) dispatching
/// `next_to_dispatch` via the appropriate `process.submit` /
/// `process.stream` path. Test-friendly: takes `SlotQueues&` rather
/// than reaching into `g_tray`, so the [d2] tests can construct a
/// fresh `SlotQueues` and assert the drain transitions without
/// linking the GTK / IPC stack.
inline DrainResult drain_on_terminal(SlotQueues& q, int64_t job_id) {
    DrainResult r;
    bool found = false;
    SlotKind slot = q.find_slot_by_in_flight_job_id(job_id, found);
    if (!found) return r;
    r.matched = true;
    r.slot = slot;
    // Capture the meeting_id BEFORE complete_in_flight() invalidates
    // the in-flight pointer.
    if (const auto* inflight = q.select(slot).in_flight()) {
        r.meeting_id = inflight->meeting_id;
    }
    r.next_to_dispatch = q.select(slot).complete_in_flight();
    return r;
}

} // namespace recmeet
