// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.7 — Server-side job queue with typed slots. See job_queue.h for
// the design.

#include "job_queue.h"

#include "log.h"

#include <algorithm>

namespace recmeet {

// ---------------------------------------------------------------------------
// Enum name helpers
// ---------------------------------------------------------------------------

const char* job_kind_name(JobKind k) {
    switch (k) {
        case JobKind::Postprocess:   return "postprocess";
        case JobKind::Streaming:     return "streaming";
        case JobKind::ModelDownload: return "model_download";
        case JobKind::_count:        break;
    }
    return "unknown";
}

const char* job_state_name(JobState s) {
    switch (s) {
        case JobState::Queued:            return "queued";
        case JobState::WaitingOnDownload: return "waiting_on_download";
        case JobState::Running:           return "running";
        case JobState::Done:              return "done";
        case JobState::Failed:            return "failed";
        case JobState::Cancelled:         return "cancelled";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Construction / teardown
// ---------------------------------------------------------------------------

JobQueue::JobQueue() : JobQueue(SlotCapacities{}) {}

JobQueue::JobQueue(SlotCapacities caps) {
    slots_[static_cast<int>(JobKind::Postprocess)].capacity =
        caps.postprocess > 0 ? caps.postprocess : 1;
    slots_[static_cast<int>(JobKind::Streaming)].capacity =
        caps.streaming > 0 ? caps.streaming : 1;
    slots_[static_cast<int>(JobKind::ModelDownload)].capacity =
        caps.model_download > 0 ? caps.model_download : 1;
}

JobQueue::~JobQueue() {
    shutdown();
}

// ---------------------------------------------------------------------------
// Slot accessors
// ---------------------------------------------------------------------------

JobQueue::Slot& JobQueue::slot_for(JobKind k) {
    return slots_[static_cast<int>(k)];
}

const JobQueue::Slot& JobQueue::slot_for(JobKind k) const {
    return slots_[static_cast<int>(k)];
}

// ---------------------------------------------------------------------------
// Hook wiring
// ---------------------------------------------------------------------------

void JobQueue::set_model_cache_checker(ModelCacheChecker checker) {
    std::lock_guard<std::mutex> lock(mu_);
    cache_checker_ = std::move(checker);
}

void JobQueue::set_model_resolver(ModelResolver resolver) {
    std::lock_guard<std::mutex> lock(mu_);
    model_resolver_ = std::move(resolver);
}

void JobQueue::set_job_event_sink(JobEventSink sink) {
    std::lock_guard<std::mutex> lock(mu_);
    event_sink_ = std::move(sink);
}

// ---------------------------------------------------------------------------
// Event sink plumbing
// ---------------------------------------------------------------------------

void JobQueue::record_event_locked(const Job& job) {
    if (event_sink_) pending_events_.push_back(job);
}

void JobQueue::fire_pending_events() {
    std::vector<Job> events;
    JobEventSink sink;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (pending_events_.empty() || !event_sink_) {
            pending_events_.clear();
            return;
        }
        events.swap(pending_events_);
        sink = event_sink_;
    }
    for (const Job& j : events) sink(j);
}

// ---------------------------------------------------------------------------
// enqueue
// ---------------------------------------------------------------------------

void JobQueue::place_job_locked(Job&& job) {
    int64_t id = job.job_id;
    JobKind kind = job.kind;
    job.state = JobState::Queued;
    slot_for(kind).fifo.push(id);
    jobs_[id] = std::move(job);
}

int64_t JobQueue::enqueue(Job job, JobKind kind, const std::string& client_id) {
    std::unique_lock<std::mutex> lock(mu_);
    int64_t id = next_job_id_++;
    job.job_id = id;
    job.kind = kind;
    job.client_id = client_id;
    place_job_locked(std::move(job));
    log_debug("job_queue: enqueue job=%ld kind=%s client=%s (slot_queued=%zu)",
              (long)id, job_kind_name(kind), client_id.c_str(),
              slot_for(kind).fifo.size());
    Slot& slot = slot_for(kind);
    lock.unlock();
    slot.cv.notify_all();
    return id;
}

// ---------------------------------------------------------------------------
// pick_runnable_locked — the heart of the typed-slot + auto-download logic
// ---------------------------------------------------------------------------

int64_t JobQueue::pick_runnable_locked(JobKind kind) {
    Slot& slot = slot_for(kind);
    if (slot.running) return -1;          // capacity-1: slot occupied.
    if (slot.fifo.empty()) return -1;

    int64_t head_id = slot.fifo.front();
    auto it = jobs_.find(head_id);
    if (it == jobs_.end()) {
        // Stale id (cancelled-and-removed). Drop it and retry the next head.
        slot.fifo.pop();
        return pick_runnable_locked(kind);
    }
    Job& head = it->second;

    if (head.state == JobState::Cancelled ||
        head.state == JobState::Done ||
        head.state == JobState::Failed) {
        // Terminal head — e.g. cancelled while queued, or a dependent that
        // finish_download() failed. Discard from the FIFO and try the next.
        // (The job stays in the registry so its binding/status survives.)
        slot.fifo.pop();
        return pick_runnable_locked(kind);
    }

    if (head.state == JobState::WaitingOnDownload) {
        // The head is parked on an in-flight model download (it stays at the
        // FIFO front so it keeps its place). Not runnable until
        // finish_download() flips it back to Queued. Capacity-1 + FIFO
        // ordering means nothing behind it can run either, so report -1.
        return -1;
    }

    // Auto-download dependency check (postprocess jobs only). Lifted here
    // from the pre-C.7 download-worker-internal cache checks: resolve the
    // job's required models and, for the first one not on disk, auto-enqueue
    // a ModelDownload and park the job until the download completes.
    if (kind == JobKind::Postprocess && head.state == JobState::Queued
        && cache_checker_ && model_resolver_) {
        std::vector<std::string> required = model_resolver_(head);
        std::optional<std::string> need;
        for (const std::string& m : required) {
            if (!cache_checker_(m)) { need = m; break; }
        }
        if (need.has_value()) {
            // Park the head; auto-enqueue a ModelDownload it depends on.
            head.state = JobState::WaitingOnDownload;
            record_event_locked(head);   // -> daemon emits downloading_model

            int64_t dl_id = next_job_id_++;
            Job dl;
            dl.job_id = dl_id;
            dl.kind = JobKind::ModelDownload;
            dl.client_id = head.client_id;   // download inherits the owner.
            dl.model_id = *need;
            place_job_locked(std::move(dl));
            download_deps_[dl_id].push_back(head_id);

            log_info("job_queue: job=%ld needs uncached model '%s' — "
                     "auto-enqueued download job=%ld",
                     (long)head_id, need->c_str(), (long)dl_id);

            // Wake the model_download worker.
            slot_for(JobKind::ModelDownload).cv.notify_all();
            return -1;   // head not runnable until the download finishes.
        }
    }

    // Head is runnable. Pop it, mark Running, set the slot marker.
    slot.fifo.pop();
    head.state = JobState::Running;
    slot.running = true;
    return head_id;
}

// ---------------------------------------------------------------------------
// dequeue
// ---------------------------------------------------------------------------

std::optional<Job> JobQueue::dequeue(JobKind kind) {
    std::unique_lock<std::mutex> lock(mu_);
    Slot& slot = slot_for(kind);
    for (;;) {
        if (shutdown_) return std::nullopt;
        int64_t id = pick_runnable_locked(kind);
        if (id >= 0) {
            Job running = jobs_.at(id);   // copy of the Running job.
            log_debug("job_queue: dequeue job=%ld kind=%s",
                      (long)id, job_kind_name(kind));
            lock.unlock();
            fire_pending_events();        // e.g. a parked dependent's event.
            return running;
        }
        // pick_runnable_locked may have parked a dependent and auto-enqueued
        // a download — flush that event before sleeping so the daemon can
        // emit `downloading_model` promptly.
        if (!pending_events_.empty()) {
            lock.unlock();
            fire_pending_events();
            lock.lock();
            continue;
        }
        // Nothing runnable: a job may be parked WaitingOnDownload, the slot
        // may be busy, or the FIFO empty. Wait for enqueue / finish /
        // finish_download / shutdown to change the picture.
        slot.cv.wait(lock);
    }
}

// ---------------------------------------------------------------------------
// finish / finish_download
// ---------------------------------------------------------------------------

void JobQueue::finish(int64_t job_id, bool ok, const std::string& error) {
    std::unique_lock<std::mutex> lock(mu_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        log_warn("job_queue: finish() on unknown job=%ld", (long)job_id);
        return;
    }
    Job& job = it->second;
    JobKind kind = job.kind;

    // Preserve a Cancelled verdict: a worker that finishes a job it was
    // told to cancel must not flip the state back to Done/Failed.
    if (job.state != JobState::Cancelled) {
        job.state = ok ? JobState::Done : JobState::Failed;
        if (!ok) job.error = error;
    }
    record_event_locked(job);

    Slot& slot = slot_for(kind);
    slot.running = false;
    log_debug("job_queue: finish job=%ld kind=%s state=%s",
              (long)job_id, job_kind_name(kind), job_state_name(job.state));
    lock.unlock();
    slot.cv.notify_all();   // next queued job for this slot can now run.
    fire_pending_events();
}

void JobQueue::finish_download(int64_t download_job_id, bool ok,
                               const std::string& error) {
    std::unique_lock<std::mutex> lock(mu_);
    auto it = jobs_.find(download_job_id);
    if (it == jobs_.end()) {
        log_warn("job_queue: finish_download() on unknown job=%ld",
                 (long)download_job_id);
        return;
    }
    Job& dl = it->second;
    if (dl.state != JobState::Cancelled) {
        dl.state = ok ? JobState::Done : JobState::Failed;
        if (!ok) dl.error = error;
    }
    record_event_locked(dl);
    slot_for(JobKind::ModelDownload).running = false;

    // Re-arm (or fail) every job parked on this download.
    std::vector<int64_t> dependents;
    auto dit = download_deps_.find(download_job_id);
    if (dit != download_deps_.end()) {
        dependents = std::move(dit->second);
        download_deps_.erase(dit);
    }

    for (int64_t dep_id : dependents) {
        auto jt = jobs_.find(dep_id);
        if (jt == jobs_.end()) continue;
        Job& dep = jt->second;
        if (dep.state == JobState::Cancelled) continue;  // cancelled meanwhile.
        if (dep.state != JobState::WaitingOnDownload) continue;

        if (ok) {
            // Re-insert at the FRONT of the slot FIFO so the dependent job
            // keeps its place ahead of anything enqueued while it waited.
            dep.state = JobState::Queued;
            std::queue<int64_t>& fifo = slot_for(dep.kind).fifo;
            std::queue<int64_t> rebuilt;
            rebuilt.push(dep_id);
            while (!fifo.empty()) { rebuilt.push(fifo.front()); fifo.pop(); }
            fifo = std::move(rebuilt);
            log_info("job_queue: download job=%ld done — re-armed dependent "
                     "job=%ld", (long)download_job_id, (long)dep_id);
        } else {
            dep.state = JobState::Failed;
            dep.error = error.empty()
                ? std::string("required model download failed")
                : ("model download failed: " + error);
            log_warn("job_queue: download job=%ld failed — dependent job=%ld "
                     "failed", (long)download_job_id, (long)dep_id);
        }
        record_event_locked(dep);
    }

    log_debug("job_queue: finish_download job=%ld state=%s dependents=%zu",
              (long)download_job_id, job_state_name(dl.state),
              dependents.size());

    // Notify both the model_download slot (its running marker cleared) and
    // the postprocess slot (re-armed dependents are now Queued there).
    Slot& dl_slot = slot_for(JobKind::ModelDownload);
    Slot& pp_slot = slot_for(JobKind::Postprocess);
    lock.unlock();
    dl_slot.cv.notify_all();
    pp_slot.cv.notify_all();
    fire_pending_events();
}

// ---------------------------------------------------------------------------
// cancel
// ---------------------------------------------------------------------------

bool JobQueue::cancel(int64_t job_id) {
    std::unique_lock<std::mutex> lock(mu_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) return false;
    Job& job = it->second;

    switch (job.state) {
        case JobState::Done:
        case JobState::Failed:
        case JobState::Cancelled:
            return false;   // already terminal — nothing to cancel.

        case JobState::Queued:
        case JobState::WaitingOnDownload:
        case JobState::Running:
            // Mark Cancelled. A Queued/Waiting job is lazily removed from its
            // slot FIFO by pick_runnable_locked(); a Running job's worker is
            // expected to observe the verdict (via status()) and stop —
            // finish() then preserves the Cancelled verdict.
            job.state = JobState::Cancelled;
            record_event_locked(job);
            log_info("job_queue: cancel job=%ld kind=%s",
                     (long)job_id, job_kind_name(job.kind));
            break;
    }

    // Wake every slot: the cancelled job may have been the head holding up a
    // dequeue, and a cancelled WaitingOnDownload dependent should let its
    // slot re-evaluate.
    lock.unlock();
    for (int i = 0; i < static_cast<int>(JobKind::_count); ++i)
        slots_[i].cv.notify_all();
    fire_pending_events();
    return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<Job> JobQueue::status(int64_t job_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) return std::nullopt;
    return it->second;
}

std::vector<Job> JobQueue::list_by_client(const std::string& client_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Job> out;
    for (const auto& [id, job] : jobs_) {
        if (job.client_id == client_id) out.push_back(job);
    }
    // jobs_ is a std::map keyed by ascending job_id, so `out` is already
    // newest-id last.
    return out;
}

std::optional<std::string> JobQueue::client_for_job(int64_t job_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) return std::nullopt;
    return it->second.client_id;
}

bool JobQueue::slot_busy(JobKind kind) const {
    std::lock_guard<std::mutex> lock(mu_);
    return slot_for(kind).running;
}

size_t JobQueue::queued_count(JobKind kind) const {
    std::lock_guard<std::mutex> lock(mu_);
    return slot_for(kind).fifo.size();
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------

void JobQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (shutdown_) return;
        shutdown_ = true;
        log_debug("job_queue: shutdown");
    }
    for (int i = 0; i < static_cast<int>(JobKind::_count); ++i)
        slots_[i].cv.notify_all();
}

bool JobQueue::is_shutdown() const {
    std::lock_guard<std::mutex> lock(mu_);
    return shutdown_;
}

} // namespace recmeet
