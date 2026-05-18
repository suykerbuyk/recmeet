// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.3 — pure helpers for the tray's reconnect-with-backoff path.
//
// Carved out of `tray.cpp` so the jitter formula, cap behavior, and
// post-reconnect job-resync analysis can be unit-tested without booting
// GTK or the IPC stack. The tray glues these helpers into `try_reconnect`
// and the post-`connect_to_daemon` resync handler.
//
// Three orthogonal concerns live here:
//
//   1. `next_nominal_backoff` — exponential doubling (1, 2, 4, 8, 16, 30,
//      30, ...) capped at `cap_secs`. Stateless; the caller threads the
//      current nominal through.
//
//   2. `jittered_delay_secs` — apply ±jitter_fraction uniform jitter to a
//      nominal delay, with a strict invariant that the returned value
//      stays within [nominal * (1 - jitter_fraction), nominal *
//      (1 + jitter_fraction)] and never undershoots 1. The cap is
//      applied AFTER the jitter (jittered values that exceed cap_secs
//      get clamped) so the wire never schedules > cap_secs delays.
//
//      Jitter is the standard one-line mitigation for thundering-herd
//      reconnect when many clients lose the daemon at the same moment
//      (network blip): without it every client retries on the same
//      lockstep schedule (1s, 2s, 4s, ...) and the daemon takes
//      simultaneous accept() bursts. ±25% jitter desynchronizes them
//      with a single uniform draw — see
//      <https://aws.amazon.com/builders-library/timeouts-retries-and-backoff-with-jitter/>.
//
//   3. `JobResyncAction` + `classify_resynced_job` — interpret one
//      `job.list` array element (server-issued job state at reconnect)
//      into the local action the tray must take:
//
//        * Done                → Fetch (issue `process.fetch`)
//        * Failed / Cancelled  → NotifyFailed (dbus notify; D.6 hook)
//        * Running / Queued    → Monitor (event pump catches up; no work)
//        * (synthetic) Unknown → NotifyFailed
//
//      Streaming jobs that the daemon reports as Failed get the
//      `streaming_aborted` flag set so the tray-side dispatcher can
//      fall back to convergence-principle pattern 2 — batch-upload the
//      local WAV via `process.submit` carrying the same `meeting_id`.
//      The "DO NOT resume streaming across reconnect" policy is locked
//      per plan line 362-363; this helper expresses it.

#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace recmeet {

/// Step the nominal (un-jittered) backoff schedule forward by one
/// attempt. `cap_secs` is the saturation ceiling (30 s per D.3 spec).
/// `current_nominal` of <= 0 is treated as 1 (the schedule starts at 1
/// s; a 0 or negative input is a caller-side bug we tolerate).
int next_nominal_backoff(int current_nominal, int cap_secs);

/// Apply ±`jitter_fraction` jitter to `nominal_secs`. The returned
/// value is guaranteed to lie in
///   [max(1, floor(nominal * (1 - jitter_fraction))),
///    min(cap_secs, ceil(nominal * (1 + jitter_fraction)))].
///
/// The post-jitter `min(..., cap_secs)` clamp matters because a
/// jittered nominal at the cap (e.g. `nominal=30, jitter=0.25`) would
/// otherwise yield up to 37.5 s. The cap is the contract; jitter never
/// breaks it.
///
/// `jitter_fraction` must be in [0, 1); a value outside is silently
/// clamped (defense-in-depth). The RNG is caller-owned so the tray can
/// thread a single seeded `std::mt19937` through all calls — tests pin
/// the jitter range over 100 trials using their own seeded RNG.
int jittered_delay_secs(int nominal_secs,
                        double jitter_fraction,
                        std::mt19937& rng,
                        int cap_secs);

/// One re-sync action the tray should take after `job.list` returns a
/// job's state on reconnect.
enum class JobResyncAction {
    /// Job is `done` server-side; tray issues `process.fetch` to pull
    /// artifacts.
    Fetch,
    /// Job is still active server-side; the event pump catches up via
    /// the next `progress.job` / `job.complete` event. No tray work.
    Monitor,
    /// Job is `failed` / `cancelled` server-side, OR the daemon
    /// reports `unknown` (the synthetic state surfaced by the tray
    /// when `job.status` returns `InvalidParams`/`PermissionDenied`
    /// for a known-to-us job_id — server restarted and lost the
    /// registry, OR resume_token expired and the fresh client_id no
    /// longer owns this job). Tray fires a dbus notify (the D.6 hook).
    NotifyFailed,
};

/// Result of classifying one re-synced job. The full set drives the
/// tray's post-reconnect dispatch: a list of {action, job_id,
/// meeting_id, kind, streaming_aborted} tuples.
struct JobResyncClassification {
    JobResyncAction action = JobResyncAction::Monitor;
    /// True iff the server-reported state implied a streaming session
    /// that did not survive the disconnect. The tray's caller falls
    /// back to convergence-principle pattern 2: batch-upload the local
    /// WAV via `process.submit` with the same `meeting_id`. Only
    /// meaningful when `action == NotifyFailed && kind == "streaming"`;
    /// false for every other combination.
    bool streaming_aborted = false;
};

/// Classify a single re-synced job. The input fields are parsed from
/// one element of the `job.list` `jobs[]` array (or one `job.status`
/// response — the shape is byte-equivalent per the daemon-side
/// `serialize_job_object` contract).
///
/// `state` MUST be one of: "queued", "waiting_on_download",
/// "waiting_for_upload", "running", "done", "failed", "cancelled", or
/// the synthetic "unknown" (used by the tray when `job.status` returns
/// PermissionDenied/InvalidParams — the server forgot us).
///
/// `kind` MUST be one of: "postprocess", "streaming", "model_download".
/// Empty `kind` is treated as "postprocess" defensively (legacy
/// pre-C.7 paths).
JobResyncClassification classify_resynced_job(const std::string& state,
                                              const std::string& kind);

/// One job parsed from a `job.list` `jobs[]` array element. Fields
/// match the daemon's `serialize_job_object` (C.6 + C.11 + C.14): the
/// seven fields the tray actually consumes during a re-sync. Phase +
/// progress survive the re-sync into the local UI per C.14 so the
/// status row populates synchronously.
struct ParsedJobListEntry {
    int64_t job_id = 0;
    std::string kind;
    std::string state;
    std::string meeting_id;
    std::string phase;
    int progress = 0;
    std::string error;
};

/// Parse the `jobs[]` array string returned by `job.list`. The daemon
/// stuffs the array as a raw JSON-array substring into a JsonMap value
/// (the C.4 raw-substring pattern), so the client receives it as a
/// single string like `[{...},{...}]`. This helper walks the top-level
/// objects (depth-tracking brace counter) and extracts the seven D.3
/// fields per element.
///
/// Robust to whitespace, additive unknown keys, and missing fields
/// (missing → empty string / 0). Malformed JSON returns the entries
/// parsed so far without raising — callers fall back to per-job
/// `job.status` queries if the count looks wrong.
std::vector<ParsedJobListEntry> parse_job_list_jobs(const std::string& jobs_array_json);

} // namespace recmeet
