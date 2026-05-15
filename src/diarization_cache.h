// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.8 — Server-side per-job diarization cache.
//
// `enroll.finalize` lets a thin client turn a previously-diarized recording
// into an enrolled-speaker voiceprint without re-uploading audio. It needs
// the cluster embeddings (centroids) the diarization pass already computed,
// keyed by the originating `job_id`. The diarized note that lands in the
// meeting directory does NOT carry the raw centroid vectors — they are
// computed in-memory by `pipeline.cpp` and discarded once the note + the
// per-meeting `speakers.json` are written. This cache is the missing
// store: every time the pp pipeline finishes a job that produced
// diarization, the daemon snapshots the centroids + per-cluster spans here.
//
// Design (per C.8 deliverable 1):
//
//   * Dedicated class, NOT a JobQueue extension. JobQueue is already
//     large; the cache lifetime is decoupled from job lifecycle (a Done
//     job's cache entry can expire while the job's registry entry stays
//     around forever for client_for_job() routing). Keeping it standalone
//     keeps JobQueue's mutex narrow and makes the cache testable in
//     isolation.
//
//   * In-memory only. The cache evaporates on daemon restart by design —
//     diarization output is large (one centroid per cluster, each ~192
//     floats for the sherpa-onnx embedding model), the disk surface is
//     not worth the persistence complexity, and clients can always re-run
//     a job if they need to enroll from a meeting older than the daemon's
//     uptime. Documented in the deliverables write-up.
//
//   * Lazy TTL eviction. Every `get()` checks `now() - inserted_at` and
//     erases stale entries. No background sweeper thread (deferred until
//     measurable memory pressure shows up — diarization runs are tens
//     per day on a busy daemon, each entry is a few KB).
//
//   * Clock injection seam. The default clock reads `std::chrono::
//     system_clock` epoch seconds. Tests inject a fake clock so a 24-h
//     TTL is exercised without `sleep_for(24h)`.
//
// Threading: all public methods take a single mutex. The class is
// reentrant-safe in the sense that handlers may call `get()` and
// `put()` from any thread; concurrent calls are serialized.

#pragma once

#include "util.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace recmeet {

/// One enrolled-candidate cluster, as seen by `enroll.finalize`.
///
/// `idx`           — 0-based cluster index, matches the diarization result's
///                   `seg.speaker` field. The client receives this in
///                   `job.complete.speakers[].idx`.
/// `embedding`     — the raw (non-normalized) centroid vector. The CLI's
///                   `--enroll` flow stores raw vectors too, so this matches
///                   the on-disk speakers DB format.
/// `duration_ms`   — total speaking time for the cluster, summed from the
///                   diarization segments. Surfaced to the client as a UI
///                   hint ("Speaker 1 has 32.4s of speech — likely Alice").
struct DiarizationCluster {
    int                  idx = 0;
    std::vector<float>   embedding;
    int64_t              duration_ms = 0;
};

/// One cache entry: the diarization snapshot for a single Postprocess job.
struct DiarizationCacheEntry {
    int64_t                          job_id = 0;
    std::vector<DiarizationCluster>  clusters;
    /// Insertion timestamp (seconds since epoch) from the cache's clock.
    int64_t                          inserted_at = 0;
};

/// In-memory keyed-by-job_id store of diarization output.
///
/// Default TTL is 24 hours; configurable via the constructor. A TTL of 0
/// is treated as "never expire" (used by tests that want deterministic
/// lifetimes without involving the clock at all).
class DiarizationCache {
public:
    /// Clock seam. Returns seconds since UNIX epoch. Defaults to
    /// `std::chrono::system_clock`. Tests inject a counter.
    using Clock = std::function<int64_t()>;

    /// @param ttl_secs  Per-entry TTL in seconds (default 24 h, matching the
    ///                  C.8 spec). 0 disables expiry.
    /// @param clock     Override for the time source. Default uses
    ///                  `std::chrono::system_clock`.
    explicit DiarizationCache(int64_t ttl_secs = 86400,
                              Clock clock = {});

    DiarizationCache(const DiarizationCache&) = delete;
    DiarizationCache& operator=(const DiarizationCache&) = delete;

    /// Insert (or replace) the entry for `job_id`. Stamps `inserted_at`
    /// with the current clock reading. `clusters` is moved in.
    void put(int64_t job_id, std::vector<DiarizationCluster> clusters);

    /// Fetch the entry for `job_id`. Returns nullopt if absent OR if the
    /// entry has expired (in which case the stale entry is erased as a
    /// side effect — lazy eviction).
    std::optional<DiarizationCacheEntry> get(int64_t job_id);

    /// Drop the entry for `job_id` regardless of state. Idempotent.
    /// Used by `process.cancel` paths to release memory if a job is
    /// terminated after its cache entry was populated.
    void erase(int64_t job_id);

    /// Total live entry count (post-eviction). Mostly for tests and logs.
    size_t size() const;

    /// TTL configured at construction. Surface for diagnostic logs and
    /// for the `enroll.finalize` error message that surfaces it back to
    /// the client when the entry is gone.
    int64_t ttl_secs() const { return ttl_secs_; }

    /// Force-evict every expired entry without a `get()` call. Optional —
    /// the lazy path covers the normal case; this exists for a future
    /// background sweeper or for tests that want to assert eviction
    /// happened deterministically.
    size_t sweep_expired();

    /// Parse the `diarization.json` artifact written by the enroll-mode
    /// pp_worker subprocess (see `write_diarization_artifact` in
    /// pipeline.cpp). Returns a populated `clusters` vector on success;
    /// throws RecmeetError on malformed input. The caller passes the
    /// resulting clusters to `put()`. Lives here (not in pipeline.cpp)
    /// because the daemon side is the consumer.
    static std::vector<DiarizationCluster>
    load_diarization_artifact(const fs::path& path);

private:
    /// Called with `mu_` held. Returns the current clock reading.
    int64_t now_locked() const;

    /// Called with `mu_` held. Returns true if `e` has expired.
    bool expired_locked(const DiarizationCacheEntry& e) const;

    mutable std::mutex                       mu_;
    int64_t                                  ttl_secs_;
    Clock                                    clock_;
    std::map<int64_t, DiarizationCacheEntry> entries_;
};

} // namespace recmeet
