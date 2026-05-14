// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.7 — JobQueue unit tests. Tagged [job_queue]. Covers:
//   - typed slots run concurrently (postprocess + streaming + model_download)
//   - two postprocess jobs queue serially behind the single postprocess slot
//   - the job_id -> client_id binding is populated and queryable
//   - auto-trigger enqueues a ModelDownload when a postprocess job's model is
//     uncached, and the dependent job is parked WaitingOnDownload
//   - finish_download re-arms the parked dependent (and fails it on a failed
//     download)
//   - cancel-by-id on a queued job and on an in-flight job
//   - list-by-client scoping
//   - clean shutdown wakes every dequeue() caller with std::nullopt
//   - the JobEventSink fires on the notable transitions

#include <catch2/catch_test_macros.hpp>

#include "job_queue.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace recmeet;
using namespace std::chrono_literals;

namespace {

// A postprocess Job with no model dependency (transcript_text already set,
// so the default ModelResolver returns nothing to download).
Job make_pp_job() {
    Job j;
    j.input.transcript_text = "already transcribed";
    return j;
}

} // namespace

TEST_CASE("JobQueue: typed slots run concurrently", "[job_queue]") {
    JobQueue q;

    int64_t pp = q.enqueue(make_pp_job(), JobKind::Postprocess, "c1");
    int64_t st = q.enqueue(Job{}, JobKind::Streaming, "c1");
    int64_t dl = q.enqueue(Job{}, JobKind::ModelDownload, "c1");

    // Each slot's dequeue returns its own job — independently, no blocking
    // across kinds.
    auto a = q.dequeue(JobKind::Postprocess);
    auto b = q.dequeue(JobKind::Streaming);
    auto c = q.dequeue(JobKind::ModelDownload);

    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    REQUIRE(a->job_id == pp);
    REQUIRE(b->job_id == st);
    REQUIRE(c->job_id == dl);

    // All three slots are simultaneously busy — true independence.
    REQUIRE(q.slot_busy(JobKind::Postprocess));
    REQUIRE(q.slot_busy(JobKind::Streaming));
    REQUIRE(q.slot_busy(JobKind::ModelDownload));

    REQUIRE(a->state == JobState::Running);
    REQUIRE(b->state == JobState::Running);
    REQUIRE(c->state == JobState::Running);
}

TEST_CASE("JobQueue: two postprocess jobs queue serially behind one slot",
          "[job_queue]") {
    JobQueue q;

    int64_t j1 = q.enqueue(make_pp_job(), JobKind::Postprocess, "c1");
    int64_t j2 = q.enqueue(make_pp_job(), JobKind::Postprocess, "c1");

    REQUIRE(q.queued_count(JobKind::Postprocess) == 2);

    auto first = q.dequeue(JobKind::Postprocess);
    REQUIRE(first.has_value());
    REQUIRE(first->job_id == j1);
    REQUIRE(q.slot_busy(JobKind::Postprocess));

    // The slot is capacity-1: a second dequeue must block until the first
    // job finishes. Run it on a thread and confirm it does not return early.
    std::atomic<bool> got_second{false};
    int64_t second_id = -1;
    std::thread worker([&] {
        auto second = q.dequeue(JobKind::Postprocess);
        if (second.has_value()) {
            second_id = second->job_id;
            got_second = true;
        }
    });

    std::this_thread::sleep_for(50ms);
    REQUIRE_FALSE(got_second.load());   // still blocked behind the slot.

    q.finish(j1, true);                 // release the slot.
    worker.join();

    REQUIRE(got_second.load());
    REQUIRE(second_id == j2);
}

TEST_CASE("JobQueue: job_id -> client_id binding is populated and queryable",
          "[job_queue]") {
    JobQueue q;

    int64_t a = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");
    int64_t b = q.enqueue(Job{}, JobKind::Streaming, "bob");

    REQUIRE(q.client_for_job(a).has_value());
    REQUIRE(*q.client_for_job(a) == "alice");
    REQUIRE(*q.client_for_job(b) == "bob");
    REQUIRE_FALSE(q.client_for_job(9999).has_value());

    // The binding survives the job reaching a terminal state.
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    q.finish(a, true);
    REQUIRE(q.client_for_job(a).has_value());
    REQUIRE(*q.client_for_job(a) == "alice");
}

TEST_CASE("JobQueue: list_by_client scopes to the owning client",
          "[job_queue]") {
    JobQueue q;

    int64_t a1 = q.enqueue(make_pp_job(), JobKind::Postprocess, "alice");
    q.enqueue(Job{}, JobKind::Streaming, "bob");
    int64_t a2 = q.enqueue(Job{}, JobKind::ModelDownload, "alice");

    auto alice = q.list_by_client("alice");
    REQUIRE(alice.size() == 2);
    // std::map keyed by ascending job_id -> newest-id last.
    REQUIRE(alice.front().job_id == a1);
    REQUIRE(alice.back().job_id == a2);

    auto bob = q.list_by_client("bob");
    REQUIRE(bob.size() == 1);

    REQUIRE(q.list_by_client("nobody").empty());
}

TEST_CASE("JobQueue: auto-triggers a ModelDownload for an uncached model",
          "[job_queue]") {
    JobQueue q;

    // The resolver says every postprocess job needs "whisper/base.en"; the
    // cache checker says it is NOT on disk.
    q.set_model_resolver([](const Job&) -> std::vector<std::string> {
        return {"whisper/base.en"};
    });
    std::atomic<bool> cached{false};
    q.set_model_cache_checker([&](const std::string& m) -> bool {
        return cached.load() && m == "whisper/base.en";
    });

    // Observe transitions: the dependent should report downloading_model.
    std::atomic<int> waiting_events{0};
    q.set_job_event_sink([&](const Job& j) {
        if (j.state == JobState::WaitingOnDownload) waiting_events++;
    });

    int64_t pp = q.enqueue(make_pp_job(), JobKind::Postprocess, "c1");

    // A worker blocks on the postprocess slot. Because the model is uncached,
    // dequeue parks the job and auto-enqueues a ModelDownload — the worker
    // stays blocked.
    std::atomic<bool> pp_ran{false};
    std::thread pp_worker([&] {
        auto j = q.dequeue(JobKind::Postprocess);
        if (j.has_value()) pp_ran = true;
    });

    // The model_download worker should see an auto-enqueued download job.
    auto dl = q.dequeue(JobKind::ModelDownload);
    REQUIRE(dl.has_value());
    REQUIRE(dl->kind == JobKind::ModelDownload);
    REQUIRE(dl->model_id == "whisper/base.en");
    REQUIRE(dl->client_id == "c1");   // download inherits the owner.

    // The postprocess worker is still parked: the dependent is not runnable.
    std::this_thread::sleep_for(50ms);
    REQUIRE_FALSE(pp_ran.load());
    REQUIRE(waiting_events.load() >= 1);
    auto st = q.status(pp);
    REQUIRE(st.has_value());
    REQUIRE(st->state == JobState::WaitingOnDownload);

    // Complete the download — the dependent is re-armed and the still-blocked
    // dequeue(Postprocess) returns it.
    cached = true;
    q.finish_download(dl->job_id, true);
    pp_worker.join();

    REQUIRE(pp_ran.load());
    REQUIRE(q.status(pp)->state == JobState::Running);
    REQUIRE(q.status(dl->job_id)->state == JobState::Done);
}

TEST_CASE("JobQueue: a failed model download fails the dependent job",
          "[job_queue]") {
    JobQueue q;
    q.set_model_resolver([](const Job&) -> std::vector<std::string> {
        return {"vad"};
    });
    q.set_model_cache_checker([](const std::string&) -> bool { return false; });

    int64_t pp = q.enqueue(make_pp_job(), JobKind::Postprocess, "c1");

    std::atomic<bool> pp_returned{false};
    std::atomic<bool> pp_got_job{false};
    std::thread pp_worker([&] {
        auto j = q.dequeue(JobKind::Postprocess);
        pp_returned = true;
        if (j.has_value()) pp_got_job = true;
    });

    auto dl = q.dequeue(JobKind::ModelDownload);
    REQUIRE(dl.has_value());

    // Download fails -> dependent goes Failed and is NOT handed to the worker.
    q.finish_download(dl->job_id, false, "network error");

    std::this_thread::sleep_for(50ms);
    REQUIRE_FALSE(pp_returned.load());   // worker still blocked, job not runnable.

    auto st = q.status(pp);
    REQUIRE(st.has_value());
    REQUIRE(st->state == JobState::Failed);
    REQUIRE(st->error.find("network error") != std::string::npos);

    // Shutdown to release the blocked worker.
    q.shutdown();
    pp_worker.join();
    REQUIRE(pp_returned.load());
    REQUIRE_FALSE(pp_got_job.load());
}

TEST_CASE("JobQueue: cancel a queued job removes it from the slot FIFO",
          "[job_queue]") {
    JobQueue q;

    int64_t j1 = q.enqueue(make_pp_job(), JobKind::Postprocess, "c1");
    int64_t j2 = q.enqueue(make_pp_job(), JobKind::Postprocess, "c1");

    // Cancel the head while it is still Queued.
    REQUIRE(q.cancel(j1));
    REQUIRE(q.status(j1)->state == JobState::Cancelled);

    // dequeue skips the cancelled head and returns j2.
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    REQUIRE(dq->job_id == j2);

    // Cancelling an already-terminal job returns false.
    REQUIRE_FALSE(q.cancel(j1));
    // Cancelling an unknown job returns false.
    REQUIRE_FALSE(q.cancel(123456));
}

TEST_CASE("JobQueue: cancel an in-flight job; finish preserves the verdict",
          "[job_queue]") {
    JobQueue q;

    int64_t j = q.enqueue(make_pp_job(), JobKind::Postprocess, "c1");
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    REQUIRE(q.slot_busy(JobKind::Postprocess));

    // Cancel while Running.
    REQUIRE(q.cancel(j));
    REQUIRE(q.status(j)->state == JobState::Cancelled);

    // The worker observes the cancellation and finishes — the Cancelled
    // verdict must be preserved over the worker's ok/err report.
    q.finish(j, true);
    REQUIRE(q.status(j)->state == JobState::Cancelled);

    // The slot is released so the next job can run.
    REQUIRE_FALSE(q.slot_busy(JobKind::Postprocess));
}

TEST_CASE("JobQueue: shutdown wakes every blocked dequeue with nullopt",
          "[job_queue]") {
    JobQueue q;

    std::atomic<int> nullopts{0};
    auto blocker = [&](JobKind k) {
        return std::thread([&, k] {
            auto j = q.dequeue(k);
            if (!j.has_value()) nullopts++;
        });
    };

    std::thread t1 = blocker(JobKind::Postprocess);
    std::thread t2 = blocker(JobKind::Streaming);
    std::thread t3 = blocker(JobKind::ModelDownload);

    std::this_thread::sleep_for(50ms);
    REQUIRE(nullopts.load() == 0);   // all three blocked, nothing queued.

    q.shutdown();
    t1.join();
    t2.join();
    t3.join();

    REQUIRE(nullopts.load() == 3);
    REQUIRE(q.is_shutdown());

    // Post-shutdown dequeue returns immediately.
    REQUIRE_FALSE(q.dequeue(JobKind::Postprocess).has_value());
}

TEST_CASE("JobQueue: status/finish error path marks the job Failed",
          "[job_queue]") {
    JobQueue q;
    int64_t j = q.enqueue(make_pp_job(), JobKind::Postprocess, "c1");
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());

    q.finish(j, false, "subprocess crashed");
    auto st = q.status(j);
    REQUIRE(st.has_value());
    REQUIRE(st->state == JobState::Failed);
    REQUIRE(st->error == "subprocess crashed");
    REQUIRE_FALSE(q.slot_busy(JobKind::Postprocess));
}
