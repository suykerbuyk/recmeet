// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.7 — Server-side job queue with typed slots.
//
// `JobQueue` is the daemon's central concurrency mechanism. It replaces the
// pre-C.7 ad-hoc model — three loose atomics (`g_recording` /
// `g_postprocessing` / `g_downloading`) guarding check-then-set admission,
// plus a separate single-FIFO `std::queue<PostprocessJob>` drained by one
// long-lived worker — with three independent, typed, capacity-1 slots:
//
//     slot.postprocess     = 1
//     slot.streaming       = 1
//     slot.model_download  = 1
//
// Each slot is its own FIFO + "running" marker. The three slots are
// independent: a postprocess job, a streaming job, and a model-download
// job can all be running at the same time. Two postprocess jobs queue
// serially behind the single postprocess slot.
//
// Scope (C.7 only): this is the queue *mechanism*. C.7 does NOT add new IPC
// verbs (process.submit/stream/fetch/cancel, job.status/list — those are
// C.2–C.6) and does NOT convert `broadcast()` sites to `send_to_client()`
// (that is C.3). C.7's job_id->client_id responsibility is purely to *own
// and populate* the binding so C.3 can consume it later.
//
// Threading: one mutex (`mu_`) protects ALL mutable state — every slot's
// FIFO, every slot's running marker, the job registry, and the cross-slot
// model-download dependency map. Per-slot condition variables wake the
// worker waiting on `dequeue(kind)`. This single-mutex model is the direct
// successor of the pre-C.7 `g_state_mu` ("guards multi-flag transitions").

#pragma once

#include "config.h"
#include "pipeline.h"   // PostprocessInput

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <string>

#include <sys/types.h>   // pid_t (C.13 — pid_for_running_job)
#include <vector>

namespace recmeet {

// ---------------------------------------------------------------------------
// Typed slot kinds
// ---------------------------------------------------------------------------

/// The three independent capacity-1 slots. `count()` is the array bound for
/// the slot table; keep `_count` last.
enum class JobKind {
    Postprocess = 0,
    Streaming = 1,
    ModelDownload = 2,
    _count = 3
};

const char* job_kind_name(JobKind k);

// ---------------------------------------------------------------------------
// Job lifecycle state
// ---------------------------------------------------------------------------

enum class JobState {
    Queued,             ///< In a slot FIFO, not yet dequeued.
    WaitingOnDownload,  ///< Dequeued, but blocked: a required model is
                        ///< downloading. The dependency is tracked inside
                        ///< JobQueue; the job re-enters the slot front once
                        ///< the download completes.
    WaitingForUpload,   ///< Phase C.2 reservation — id minted by
                        ///< `reserve_job_id()` so `process.submit` can return
                        ///< it to the client immediately; the job is NOT yet
                        ///< in any FIFO. `enqueue_reserved()` flips it to
                        ///< Queued once the binary upload finalizes; cancel()
                        ///< marks it Cancelled if the upload never completes.
    Running,            ///< Dequeued and executing (slot's running marker set).
    Done,               ///< Completed successfully.
    Failed,             ///< Completed with an error.
    Cancelled           ///< Cancelled (while queued or in-flight).
};

const char* job_state_name(JobState s);

// ---------------------------------------------------------------------------
// Job record
// ---------------------------------------------------------------------------

/// A unit of queued work. `PostprocessJob`'s pre-C.7 fields (job_id / input /
/// cfg) are folded in here; C.7 additionally threads `client_id` onto the
/// struct so the job_id->client_id binding has an authoritative home.
struct Job {
    int64_t          job_id = 0;
    JobKind          kind = JobKind::Postprocess;
    std::string      client_id;        ///< Owning client (may be empty for
                                       ///< daemon-internal / pre-session jobs).
    JobState         state = JobState::Queued;

    // --- Postprocess payload (populated for JobKind::Postprocess) ---
    PostprocessInput input;
    JobConfig        cfg;

    // --- ModelDownload payload (populated for JobKind::ModelDownload) ---
    /// Logical model identifier the download targets, e.g. "whisper/base.en",
    /// "llama/<name>", "sherpa/diarization", "vad". Empty for non-download
    /// jobs. Auto-triggered downloads (from a parked postprocess job) and
    /// explicit `models.ensure` jobs both use this.
    std::string      model_id;

    /// When true, the model_download worker re-downloads even if the model
    /// is already cached (the `models.update` "refresh" semantic). When
    /// false (the default — auto-trigger and `models.ensure`), an
    /// already-cached model is a no-op.
    bool             force_download = false;

    /// Error message when `state == Failed`.
    std::string      error;

    /// Phase C.11 — meeting_id this job belongs to, when known. Threaded
    /// here from `SubmitRequest::meeting_id` (process.submit) or
    /// `StreamRequest::meeting_id` (process.stream → process.stream.commit's
    /// postprocess job inherits via the streaming session's frozen state).
    /// Empty for v1-shaped clients and for daemon-internal jobs (model
    /// downloads). The job.list / job.status serializer emits it
    /// unconditionally so the client can reconcile by content key.
    std::string      meeting_id;

    /// Phase C.14 — last observed pipeline phase + progress, cached on the
    /// registry so a D.3 reconnect re-sync can populate the UI immediately
    /// without waiting for the next `progress.job` / `phase` / `progress`
    /// event. The daemon's event-emission paths call
    /// `JobQueue::update_progress` to maintain these fields; the
    /// `serialize_job_object` paths in daemon.cpp emit `phase` / `progress`
    /// on the wire for both `job.status` and each `job.list` element. When
    /// `phase` is empty, the serializer falls back to a state-derived value
    /// (`default_phase_for_state(state)`) so an idle Queued job still wires
    /// a meaningful `"phase":"queued"` rather than an empty string. Default
    /// `progress = 0` is a sentinel for "no progress reported yet" — there
    /// is no way to distinguish "started but 0%" from "unknown" on the wire,
    /// but the only consumer (D.3 re-sync UI) treats both as "nothing to
    /// show yet" which is correct in either case.
    std::string      phase;
    int              progress = 0;
};

/// Phase C.14 — derive the canonical phase string for a job in `state` when
/// the registry has no cached `Job::phase` value (i.e. the job has emitted
/// no `phase` / `progress` events yet). Mirrors the wire-side enum from the
/// task plan: queued / downloading_model / uploading / running / complete /
/// failed / cancelled. `WaitingForUpload` derives "uploading" rather than
/// "waiting_for_upload" because the client-facing semantics is "an upload
/// is in flight"; `Running` derives a generic "running" that any pipeline
/// event will immediately overwrite once the subprocess emits its first
/// `phase` event.
const char* default_phase_for_state(JobState s);

// ---------------------------------------------------------------------------
// JobQueue
// ---------------------------------------------------------------------------

/// Thread-safe typed-slot job queue. See file header for the model.
///
/// Lifecycle of a postprocess job that needs a model:
///   1. enqueue(job, Postprocess, client_id)         -> Queued
///   2. dequeue(Postprocess) on the pp worker thread
///   3. JobQueue checks the model cache at dequeue time. Cached -> Running,
///      returned to the worker. Not cached -> JobQueue auto-enqueues a
///      ModelDownload job into the model_download slot, records the
///      dependency, leaves the postprocess job WaitingOnDownload, and
///      `dequeue` keeps blocking until the download completes.
///   4. The model-download worker dequeues the ModelDownload job, performs
///      the download, and calls finish_download() — which clears the
///      dependency and re-arms the dependent job so the still-blocked
///      `dequeue(Postprocess)` returns it as Running.
class JobQueue {
public:
    /// Slot capacities. C.7 wires these from `[server] slot.*` config keys;
    /// all three default to 1. Capacities other than 1 are accepted by the
    /// struct but C.7 only exercises capacity-1 — the typed-slot invariant.
    struct SlotCapacities {
        int postprocess = 1;
        int streaming = 1;
        int model_download = 1;
    };

    /// Cache-check hook. Given a logical model_id, returns true if the model
    /// is already on disk. Lifted to *dequeue time* from the pre-C.7
    /// download-worker-internal `is_*_model_cached()` calls. The daemon wires
    /// this to the real model_manager helpers; tests inject a stub.
    using ModelCacheChecker = std::function<bool(const std::string& model_id)>;

    /// Resolves the full set of logical model_ids a job requires (pure —
    /// "what does this job need", not "what is missing"). The daemon wires
    /// this to inspect `Job::cfg`; tests inject a stub. `dequeue` pairs it
    /// with `ModelCacheChecker` to find the first required-but-uncached
    /// model and auto-enqueue a download for it.
    using ModelResolver = std::function<std::vector<std::string>(const Job&)>;

    JobQueue();
    explicit JobQueue(SlotCapacities caps);
    ~JobQueue();

    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    /// Observer invoked (with the JobQueue mutex NOT held) whenever a job
    /// makes a notable state transition: Queued->WaitingOnDownload (a
    /// dependent parked on an auto-enqueued download), WaitingOnDownload->
    /// Queued (re-armed after the download finished), and the terminal
    /// transitions. The daemon wires this to emit `progress.job` events —
    /// in particular the `downloading_model` phase while a postprocess job
    /// is parked. Pure mechanism: JobQueue itself sends nothing on the wire.
    using JobEventSink = std::function<void(const Job& job)>;

    /// Wire the auto-download machinery. Optional: if unset, `dequeue` never
    /// auto-triggers a download and every job goes straight to Running.
    void set_model_cache_checker(ModelCacheChecker checker);
    void set_model_resolver(ModelResolver resolver);
    void set_job_event_sink(JobEventSink sink);

    /// Enqueue `job` into the slot for `kind`, owned by `client_id`. Assigns
    /// and returns a fresh monotonically-increasing job_id. `job.kind`,
    /// `job.client_id`, `job.job_id` and `job.state` are overwritten by this
    /// call; the caller fills the payload fields (`input`/`cfg` or
    /// `model_id`).
    int64_t enqueue(Job job, JobKind kind, const std::string& client_id);

    /// Phase C.2 — pre-allocate a job_id without enqueuing the job yet.
    /// `process.submit` needs to return a `job_id` to the client at request
    /// time, but the underlying postprocess job cannot become runnable until
    /// the upload completes (otherwise pp_worker_loop would dequeue an
    /// empty-input job). `reserve_job_id()` mints the id and creates a
    /// registry entry in a sentinel `WaitingForUpload` state so the binding
    /// is queryable (status / client_for_job) the instant the response is
    /// sent. The reserved id does NOT consume the slot's running marker or
    /// occupy the FIFO — `enqueue_reserved()` performs the real placement
    /// once the upload finalizes. `cancel(job_id)` is the matching teardown
    /// for a reservation that never gets finalized (e.g. process.submit.cancel
    /// or client disconnect mid-upload).
    int64_t reserve_job_id(JobKind kind, const std::string& client_id);

    /// Phase C.2 — finalize a reservation made by `reserve_job_id()`. Moves
    /// the existing registry entry from WaitingForUpload to Queued, places
    /// it at the back of the slot FIFO, and notifies the slot's worker. The
    /// caller passes the fully-populated `Job` payload (input/cfg etc.); the
    /// `job_id`, `kind`, and `client_id` fields on `job` are ignored — those
    /// were set by `reserve_job_id` and stay authoritative. Returns true on
    /// success; false if the reservation does not exist or is no longer in
    /// WaitingForUpload (e.g. cancelled meanwhile).
    bool enqueue_reserved(int64_t job_id, Job job);

    /// Block until a job for `kind` is available and ready to run, then
    /// return it with `state == Running` (the slot's running marker is set).
    /// Returns std::nullopt when the queue is shutting down.
    ///
    /// "Ready to run" folds in the auto-download dependency: a postprocess
    /// job whose model is uncached is held back (WaitingOnDownload) and a
    /// ModelDownload job is auto-enqueued; `dequeue(Postprocess)` keeps
    /// blocking until `finish_download()` re-arms it.
    std::optional<Job> dequeue(JobKind kind);

    /// Mark the running job `job_id` finished. `ok==true` -> Done, else
    /// Failed (with `error`). Clears the slot's running marker so the next
    /// queued job for that slot can be dequeued.
    void finish(int64_t job_id, bool ok, const std::string& error = "");

    /// Completion hook for a ModelDownload job. In addition to `finish()`
    /// semantics for the download job itself, this clears the cross-slot
    /// dependency: every job WaitingOnDownload on this `download_job_id` is
    /// re-armed (re-inserted at the FRONT of its slot FIFO as Queued) and the
    /// relevant slot CV is notified. On `ok==false`, dependents are failed.
    void finish_download(int64_t download_job_id, bool ok,
                         const std::string& error = "");

    /// Cancel `job_id`. A Queued job is removed from its slot FIFO; a Running
    /// or WaitingOnDownload job is marked Cancelled (the worker is expected to
    /// observe this and stop — C.7 does not own subprocess signalling).
    /// Returns true if the job existed and was cancellable.
    bool cancel(int64_t job_id);

    /// Phase C.14 — cache the most-recently-observed pipeline phase and
    /// progress percentage on the registry entry for `job_id`. Daemon-side
    /// event emission sites (the pp_worker_loop `phase` / `progress` handlers
    /// and the UploadProgressSink) call this so a later `job.list` /
    /// `job.status` on the SAME job carries the same `phase` / `progress`
    /// without waiting for a fresh event — the load-bearing requirement for
    /// D.3 reconnect re-sync to render meaningful UI immediately. Silent
    /// no-op on unknown job_id and on jobs already in a terminal state
    /// (Done/Failed/Cancelled — phase is locked by state and percent is
    /// moot once a job finishes). `progress` is clamped to [0, 100].
    void update_progress(int64_t job_id, const std::string& phase, int progress);

    /// Phase C.13 — Running-Postprocess `job_id ↔ pid` binding. The GC sweep
    /// (orphan-eviction path inside the future SessionManager) needs to
    /// verify `pid_for_running_job(victim) == g_pp_child_pid.load()` BEFORE
    /// signaling `kill(g_pp_child_pid, SIGTERM)` so it does not SIGTERM an
    /// unrelated job that happens to occupy the (single-capacity) Postprocess
    /// slot at sweep time. The pre-C.13 daemon kept the pid in a file-static
    /// `std::atomic<pid_t> g_pp_child_pid` only — sufficient for the
    /// owning-client-initiated `process.cancel` path because the cancel
    /// request races nothing — but unsafe for a periodic background sweep.
    ///
    /// Wiring: `pp_worker_loop` calls `bind_running_pid(job.job_id, pid)`
    /// immediately after `g_pp_child_pid.store(pid)` (the existing fork-and-
    /// stamp site at `src/daemon.cpp:460`+). The reap path calls
    /// `unbind_running_pid(job.job_id)` before `finish()` so the binding
    /// goes away with the running marker.
    ///
    /// Thread-safety: mutex-guarded; lookups and mutations are O(log N) on a
    /// `std::map` keyed by job_id. The binding is a transient overlay on the
    /// existing registry — it does NOT extend `Job`'s lifetime semantics,
    /// and a binding for a job not in the registry is benign (treated as
    /// "no binding"). Empty by default; only Postprocess Running jobs ever
    /// populate it. Streaming jobs use `g_streaming->cancel_by_job_id` for
    /// orchestration (job-id-routed, owns its own engine + WAV teardown);
    /// ModelDownload has no SIGTERM target.
    void bind_running_pid(int64_t job_id, pid_t pid);
    void unbind_running_pid(int64_t job_id);
    std::optional<pid_t> pid_for_running_job(int64_t job_id) const;

    /// Snapshot of a single job by id, or std::nullopt if unknown.
    std::optional<Job> status(int64_t job_id) const;

    /// All jobs owned by `client_id`, newest-id last. Used by C.6's job.list
    /// later; C.7 only needs it for the binding to be queryable.
    std::vector<Job> list_by_client(const std::string& client_id) const;

    /// The job_id->client_id binding C.3 will consume to route events.
    /// std::nullopt if the job_id is unknown. The binding outlives the job's
    /// active state (terminal jobs are retained in the registry) so a late
    /// event can still be routed.
    std::optional<std::string> client_for_job(int64_t job_id) const;

    /// True if the slot for `kind` currently has a Running job.
    bool slot_busy(JobKind kind) const;

    /// Count of Queued (not yet dequeued) jobs in the slot for `kind`.
    size_t queued_count(JobKind kind) const;

    /// Wake every `dequeue` caller and make them return std::nullopt. After
    /// this, `dequeue` always returns std::nullopt immediately. Idempotent;
    /// the daemon calls it during shutdown to drain the worker threads.
    void shutdown();

    bool is_shutdown() const;

private:
    struct Slot {
        std::queue<int64_t>     fifo;        ///< Queued job_ids, FIFO order.
        bool                    running = false;
        int                     capacity = 1;
        std::condition_variable cv;          ///< Woken on enqueue / finish /
                                             ///< finish_download / shutdown.
    };

    Slot& slot_for(JobKind k);
    const Slot& slot_for(JobKind k) const;

    /// Called with `mu_` held. Returns the next runnable job_id for `slot`,
    /// or -1 if none is currently runnable (FIFO empty, slot busy, or the
    /// head job is parked WaitingOnDownload). Side effect: when the head of
    /// a Postprocess FIFO needs an uncached model, this auto-enqueues a
    /// ModelDownload job, parks the head as WaitingOnDownload, and records
    /// the dependency — then reports -1 (head not runnable yet).
    int64_t pick_runnable_locked(JobKind kind);

    /// Called with `mu_` held. The shared body of enqueue() — appends an
    /// already-id-assigned job to its slot and registers it.
    void place_job_locked(Job&& job);

    /// Called with `mu_` held — queues a snapshot of `job` for the event
    /// sink. Snapshots are flushed by `fire_pending_events()` once the
    /// caller has released `mu_` (the sink must never be invoked under the
    /// lock — it re-enters the daemon and could deadlock).
    void record_event_locked(const Job& job);

    /// Called with `mu_` NOT held. Drains and dispatches queued snapshots.
    void fire_pending_events();

    mutable std::mutex          mu_;
    Slot                        slots_[static_cast<int>(JobKind::_count)];
    std::map<int64_t, Job>      jobs_;        ///< Registry: every job ever
                                              ///< enqueued, including
                                              ///< terminal ones (retained so
                                              ///< the client binding survives).
    int64_t                     next_job_id_ = 1;
    bool                        shutdown_ = false;

    ModelCacheChecker           cache_checker_;
    ModelResolver               model_resolver_;
    JobEventSink                event_sink_;

    /// Snapshots queued under `mu_` by `record_event_locked`, dispatched to
    /// `event_sink_` by `fire_pending_events()` after `mu_` is released.
    std::vector<Job>            pending_events_;

    /// download_job_id -> dependent postprocess job_ids parked on it.
    std::map<int64_t, std::vector<int64_t>> download_deps_;

    /// Phase C.13 — Running-Postprocess job_id → pid binding. Empty by
    /// default; populated by `bind_running_pid` from `pp_worker_loop` after
    /// fork, cleared by `unbind_running_pid` in the reap path. Read by the
    /// future GC sweep via `pid_for_running_job` to gate SIGTERM dispatch.
    std::map<int64_t, pid_t> running_pids_;
};

} // namespace recmeet
