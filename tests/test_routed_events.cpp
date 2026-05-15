// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.3 — Routed progress events.
//
// These tests verify the acceptance criterion of the thin-client plan: with
// two clients on one daemon, each client sees ONLY the events for jobs IT
// submitted. We do not stand up the full daemon binary here — too coupled to
// model files, subprocess fork/exec, and audio capture. Instead each test
// constructs an IpcServer + JobQueue and mirrors the exact daemon-side
// sink/lambda call patterns from src/daemon.cpp's set_job_event_sink,
// StreamingCaptionSink, UploadProgressSink, pp_worker_loop (phase/progress/
// job.complete), and model_dl_worker_loop (model.downloading). The routing
// path under test is the same code in src/ipc_server.cpp::send_to_client
// regardless of which sink the daemon wires up.
//
// Thread hygiene (orchestrator rule 5): every std::thread is owned by a RAII
// guard that joins on destruction so a REQUIRE between thread construction
// and `.join()` cannot call std::terminate().
//
// Acceptance criterion test: see "C.3 acceptance — two clients, postprocess
// job from A: A sees its events, B's stream stays SILENT". That test
// explicitly asserts B's event counter is zero for the time window during
// which A's job emits — the failure mode being guarded against is "B ALSO
// got A's events," not "A got nothing."

#include <catch2/catch_test_macros.hpp>

#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <unistd.h>

using namespace recmeet;

namespace {

// Ignore SIGPIPE so a write to a dropped socket does not kill the process.
struct SigpipeIgnorer {
    SigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} s_ignore_sigpipe;

const char* ROUTED_SOCK = "/tmp/recmeet_test_routed_events.sock";

// ServerGuard — joins the IpcServer poll thread on destruction, even on
// REQUIRE-thrown exception. Same pattern as test_streaming_session.cpp.
struct ServerGuard {
    IpcServer& server;
    std::thread thr;
    explicit ServerGuard(IpcServer& s) : server(s) {
        thr = std::thread([this]() { server.run(); });
    }
    ~ServerGuard() {
        server.stop();
        if (thr.joinable()) thr.join();
    }
};

// JqGuard — drives the JobQueue Postprocess and ModelDownload slots the way
// the daemon's pp_worker_loop / model_dl_worker_loop do: a worker thread that
// flips the slot's "running" marker so slot_busy() is authoritative. Joined
// on destruction via JobQueue::shutdown().
struct JqGuard {
    JobQueue& q;
    std::thread pp_worker;
    std::thread dl_worker;
    explicit JqGuard(JobQueue& q_) : q(q_) {
        pp_worker = std::thread([this]() {
            for (;;) {
                auto dq = q.dequeue(JobKind::Postprocess);
                if (!dq.has_value()) return;
                // The test driver calls finish() when it's done with the job.
            }
        });
        dl_worker = std::thread([this]() {
            for (;;) {
                auto dq = q.dequeue(JobKind::ModelDownload);
                if (!dq.has_value()) return;
            }
        });
    }
    ~JqGuard() {
        q.shutdown();
        if (pp_worker.joinable()) pp_worker.join();
        if (dl_worker.joinable()) dl_worker.join();
    }
};

// Wait until pred() is true or the deadline passes. Returns pred()'s final
// value. Used to ride out poll-thread scheduling without sleeping a fixed
// amount.
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

// Per-client event counter that records every event seen, keyed by event
// name. Used by every test in this file: clients install one of these,
// trigger events, then assert exact counts per event name on A and B.
struct EventCounter {
    std::mutex mtx;
    std::map<std::string, int> counts;
    std::vector<std::pair<std::string, int64_t>> log;  // (event, job_id)

    EventCallback callback() {
        return [this](const IpcEvent& ev) {
            std::lock_guard<std::mutex> lk(mtx);
            counts[ev.event]++;
            auto jit = ev.data.find("job_id");
            int64_t jid = (jit != ev.data.end())
                          ? json_val_as_int(jit->second, -1)
                          : -1;
            log.emplace_back(ev.event, jid);
        };
    }

    int count(const std::string& event_name) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = counts.find(event_name);
        return it == counts.end() ? 0 : it->second;
    }

    int total() {
        std::lock_guard<std::mutex> lk(mtx);
        int n = 0;
        for (auto& kv : counts) n += kv.second;
        return n;
    }
};

// ClientReader — RAII reader thread that loops `read_and_dispatch(50)` on a
// connected IpcClient so the per-client event callback actually fires.
// IpcClient::read_and_dispatch dispatches at most one batch's worth of
// frames per call; tests need a continuous reader to observe asynchronous
// events posted from the IpcServer poll thread.
struct ClientReader {
    IpcClient& c;
    std::atomic<bool> stop{false};
    std::thread thr;
    explicit ClientReader(IpcClient& cl) : c(cl) {
        thr = std::thread([this]() {
            while (!stop.load()) {
                if (!c.read_and_dispatch(50)) return;  // disconnect
            }
        });
    }
    ~ClientReader() {
        stop.store(true);
        if (thr.joinable()) thr.join();
    }
};

} // anonymous namespace

// ===========================================================================
// 1. ACCEPTANCE — two clients, postprocess job from A. A sees its events,
//    B's stream stays silent (the plan's exact criterion).
//
// Mirrors daemon.cpp's pp_worker_loop emission pattern: `phase`, `progress`,
// `job.complete` events all route via `client_for_job(job_id)`. The events
// here are synthesized to match the production wire shape; the routing path
// they exercise is the same `send_to_client(*cid, ev)` the daemon calls.
// ===========================================================================
TEST_CASE("C.3 acceptance — postprocess events route to originator only; "
          "B's stream is silent",
          "[c3][routing]") {
    ::unlink(ROUTED_SOCK);

    JobQueue q;
    JqGuard jqg(q);
    IpcServer server(ROUTED_SOCK);
    REQUIRE(server.start());
    ServerGuard sg(server);

    // Two clients, A and B. A's client_id is the routing target.
    IpcClient a(ROUTED_SOCK), b(ROUTED_SOCK);
    REQUIRE(a.connect());
    REQUIRE(b.connect());
    REQUIRE_FALSE(a.client_id().empty());
    REQUIRE_FALSE(b.client_id().empty());
    REQUIRE(a.client_id() != b.client_id());

    EventCounter a_counter, b_counter;
    a.set_event_callback(a_counter.callback());
    b.set_event_callback(b_counter.callback());
    ClientReader ar(a), br(b);

    // Enqueue a postprocess job owned by A. The Job's `client_id` is the
    // C.7 binding the per-job sink consults.
    Job job;
    int64_t jid = q.enqueue(std::move(job), JobKind::Postprocess, a.client_id());

    // Drive the pp_worker side: dequeue (so the slot's "running" marker is
    // set). The JqGuard pp_worker handles that. Now emit the same lambdas
    // the daemon's pp subprocess output handler posts.

    auto& srv = server;
    auto& jobs = q;
    // 1) phase event.
    srv.post([&srv, &jobs, jid]() {
        IpcEvent ev;
        ev.event = "phase";
        ev.data["name"] = std::string("transcribing");
        ev.data["job_id"] = jid;
        if (auto cid = jobs.client_for_job(jid); cid && !cid->empty())
            srv.send_to_client(*cid, std::move(ev));
        else
            srv.broadcast(ev);
    });
    // 2) progress event.
    srv.post([&srv, &jobs, jid]() {
        IpcEvent ev;
        ev.event = "progress";
        ev.data["phase"] = std::string("transcribing");
        ev.data["percent"] = static_cast<int64_t>(42);
        ev.data["job_id"] = jid;
        if (auto cid = jobs.client_for_job(jid); cid && !cid->empty())
            srv.send_to_client(*cid, std::move(ev));
        else
            srv.broadcast(ev);
    });
    // 3) job.complete event.
    srv.post([&srv, &jobs, jid]() {
        IpcEvent ev;
        ev.event = "job.complete";
        ev.data["note_path"] = std::string("/tmp/note.md");
        ev.data["output_dir"] = std::string("/tmp/out");
        ev.data["job_id"] = jid;
        ev.data["batch_job"] = false;
        if (auto cid = jobs.client_for_job(jid); cid && !cid->empty())
            srv.send_to_client(*cid, std::move(ev));
        else
            srv.broadcast(ev);
    });

    // Give the poll thread a window to dispatch all three events. We then
    // drain whatever lands on both clients. The acceptance check is on B:
    // its event stream is silent for these per-job events.
    CHECK(wait_until([&]() { return a_counter.total() >= 3; },
                     std::chrono::milliseconds(2000)));
    // Drain any tardy events still in flight on B.
    // Give B's reader an extra window to pick up any stray events that
    // might be in flight. If B were going to receive A's events, they'd
    // arrive in this window. (Reader thread continues dispatching here.)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // A received exactly one of each event we routed.
    CHECK(a_counter.count("phase") == 1);
    CHECK(a_counter.count("progress") == 1);
    CHECK(a_counter.count("job.complete") == 1);

    // ACCEPTANCE — B saw NONE of A's per-job events. This is the routing
    // failure mode the C.3 plan calls out explicitly: "B ALSO got A's
    // events" would be a routing bug. The assertions below are the load-
    // bearing ones; a weak "A got something" pass is not enough.
    CHECK(b_counter.count("phase") == 0);
    CHECK(b_counter.count("progress") == 0);
    CHECK(b_counter.count("job.complete") == 0);
    CHECK(b_counter.total() == 0);

    // Clean up the slot so JqGuard pp_worker can re-block in dequeue() on
    // shutdown rather than spinning on a still-busy slot.
    q.finish(jid, /*ok=*/true);

    // ClientReader destructors run before IpcClient destructors (reverse
    // declaration order), stopping the reader threads cleanly. The
    // IpcClient destructor then calls close_connection().
    ::unlink(ROUTED_SOCK);
}

// ===========================================================================
// 2. JobQueue::set_job_event_sink emissions route to the originator.
//    Mirrors daemon.cpp's set_job_event_sink lambda — captures
//    `job.client_id` synchronously, then posts a `progress.job` event
//    that routes via that captured id.
// ===========================================================================
TEST_CASE("C.3 — JobEventSink progress.job routes to originator",
          "[c3][routing]") {
    ::unlink(ROUTED_SOCK);

    JobQueue q;
    JqGuard jqg(q);
    IpcServer server(ROUTED_SOCK);

    // Wire the JobEventSink the same way daemon.cpp does. Capture
    // job.client_id on the calling thread; route via send_to_client in the
    // posted lambda; fall back to broadcast on empty.
    server.set_max_clients(8);
    q.set_job_event_sink([&server](const Job& job) {
        // Only emit on terminal-or-parked transitions — the daemon's exact
        // switch arm set.
        std::string phase;
        switch (job.state) {
            case JobState::WaitingOnDownload: phase = "downloading_model"; break;
            case JobState::Queued:            phase = "resumed";          break;
            case JobState::Done:              phase = "done";             break;
            case JobState::Failed:            phase = "failed";           break;
            case JobState::Cancelled:         phase = "cancelled";        break;
            default: return;
        }
        int64_t jid = job.job_id;
        std::string cid = job.client_id;
        server.post([&server, jid, phase, cid]() {
            IpcEvent ev;
            ev.event = "progress.job";
            ev.data["job_id"] = jid;
            ev.data["phase"] = phase;
            if (!cid.empty()) server.send_to_client(cid, std::move(ev));
            else              server.broadcast(ev);
        });
    });

    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient a(ROUTED_SOCK), b(ROUTED_SOCK);
    REQUIRE(a.connect());
    REQUIRE(b.connect());

    EventCounter a_counter, b_counter;
    a.set_event_callback(a_counter.callback());
    b.set_event_callback(b_counter.callback());
    ClientReader ar(a), br(b);

    // Enqueue a postprocess job owned by A. enqueue() does NOT call the
    // sink for the initial Queued state (the sink fires on transitions
    // recorded by finish/cancel/etc.). The simplest reliable trigger is
    // to dequeue (slot marker → Running, no sink call), then finish() →
    // Done sink call.
    int64_t jid = q.enqueue(Job{}, JobKind::Postprocess, a.client_id());
    REQUIRE(wait_until([&]() { return q.slot_busy(JobKind::Postprocess); },
                       std::chrono::milliseconds(1000)));
    q.finish(jid, /*ok=*/true);

    CHECK(wait_until([&]() { return a_counter.count("progress.job") >= 1; },
                     std::chrono::milliseconds(2000)));
    // Give B's reader an extra window to pick up any stray events that
    // might be in flight. If B were going to receive A's events, they'd
    // arrive in this window. (Reader thread continues dispatching here.)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CHECK(a_counter.count("progress.job") == 1);
    CHECK(b_counter.count("progress.job") == 0);
    CHECK(b_counter.total() == 0);

    // ClientReader destructors run before IpcClient destructors (reverse
    // declaration order), stopping the reader threads cleanly. The
    // IpcClient destructor then calls close_connection().
    ::unlink(ROUTED_SOCK);
}

// ===========================================================================
// 3. StreamingCaptionSink-shape emissions route to the session's client_id.
//    The sink carries client_id directly (not via client_for_job); this is
//    the daemon.cpp pattern for StreamingCaptionSink::on_caption /
//    on_degraded.
// ===========================================================================
TEST_CASE("C.3 — caption events route via session-owned client_id",
          "[c3][routing]") {
    ::unlink(ROUTED_SOCK);

    IpcServer server(ROUTED_SOCK);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient a(ROUTED_SOCK), b(ROUTED_SOCK);
    REQUIRE(a.connect());
    REQUIRE(b.connect());

    EventCounter a_counter, b_counter;
    a.set_event_callback(a_counter.callback());
    b.set_event_callback(b_counter.callback());
    ClientReader ar(a), br(b);

    // The streaming sink in daemon.cpp posts caption / caption.degraded
    // events with the session's client_id. We mirror that exact pattern.
    auto post_caption = [&](const std::string& cid, int64_t jid,
                            const std::string& text, bool is_partial) {
        std::string c = cid;
        server.post([&server, c, jid, text, is_partial]() {
            IpcEvent ev = make_caption_event(jid, text, is_partial, 0);
            if (!c.empty()) server.send_to_client(c, std::move(ev));
            else            server.broadcast(ev);
        });
    };
    auto post_caption_degraded = [&](const std::string& cid, int64_t jid,
                                     const std::string& reason) {
        std::string c = cid;
        server.post([&server, c, jid, reason]() {
            IpcEvent ev = make_caption_degraded_event(jid, reason, 0);
            if (!c.empty()) server.send_to_client(c, std::move(ev));
            else            server.broadcast(ev);
        });
    };

    // Stream owned by A — emit a partial caption, a final caption, and a
    // degraded event. All three must reach A and NOT reach B.
    post_caption(a.client_id(), /*jid=*/101, "hello ", /*is_partial=*/true);
    post_caption(a.client_id(), /*jid=*/101, "hello world.", false);
    post_caption_degraded(a.client_id(), /*jid=*/101, "buffer_overrun");

    CHECK(wait_until([&]() { return a_counter.total() >= 3; },
                     std::chrono::milliseconds(2000)));
    // Give B's reader an extra window to pick up any stray events that
    // might be in flight. If B were going to receive A's events, they'd
    // arrive in this window. (Reader thread continues dispatching here.)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CHECK(a_counter.count("caption") == 2);
    CHECK(a_counter.count("caption.degraded") == 1);
    CHECK(b_counter.count("caption") == 0);
    CHECK(b_counter.count("caption.degraded") == 0);
    CHECK(b_counter.total() == 0);

    // ClientReader destructors run before IpcClient destructors (reverse
    // declaration order), stopping the reader threads cleanly. The
    // IpcClient destructor then calls close_connection().
    ::unlink(ROUTED_SOCK);
}

// ===========================================================================
// 4. UploadProgressSink-shape emissions route via the upload session's
//    client_id. Daemon.cpp's UploadProgressSink::on_progress fires from
//    feed_chunk() and posts a `progress.job` event with phase=`uploading`.
// ===========================================================================
TEST_CASE("C.3 — upload progress.job routes via upload-session client_id",
          "[c3][routing]") {
    ::unlink(ROUTED_SOCK);

    IpcServer server(ROUTED_SOCK);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient a(ROUTED_SOCK), b(ROUTED_SOCK);
    REQUIRE(a.connect());
    REQUIRE(b.connect());

    EventCounter a_counter, b_counter;
    a.set_event_callback(a_counter.callback());
    b.set_event_callback(b_counter.callback());
    ClientReader ar(a), br(b);

    // Upload from B — phase=uploading events go to B only.
    auto post_upload = [&](const std::string& cid, int64_t jid,
                           int64_t received, int64_t total) {
        std::string c = cid;
        server.post([&server, c, jid, received, total]() {
            IpcEvent ev;
            ev.event = "progress.job";
            ev.data["job_id"] = jid;
            ev.data["kind"] = std::string("postprocess");
            ev.data["phase"] = std::string("uploading");
            ev.data["bytes_received"] = received;
            ev.data["bytes_total"] = total;
            if (!c.empty()) server.send_to_client(c, std::move(ev));
            else            server.broadcast(ev);
        });
    };

    post_upload(b.client_id(), /*jid=*/202, 1000, 10000);
    post_upload(b.client_id(), /*jid=*/202, 5000, 10000);
    post_upload(b.client_id(), /*jid=*/202, 10000, 10000);

    CHECK(wait_until([&]() { return b_counter.count("progress.job") >= 3; },
                     std::chrono::milliseconds(2000)));
    // Symmetric to the other tests: pause briefly so A would have a
    // chance to receive any leaked events if routing were broken.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CHECK(b_counter.count("progress.job") == 3);
    CHECK(a_counter.count("progress.job") == 0);
    CHECK(a_counter.total() == 0);

    // ClientReader destructors run before IpcClient destructors (reverse
    // declaration order), stopping the reader threads cleanly. The
    // IpcClient destructor then calls close_connection().
    ::unlink(ROUTED_SOCK);
}

// ===========================================================================
// 5. Model download events route via the Job's client_id (the originator of
//    the dependent postprocess job, or of the explicit models.ensure).
//    Mirrors daemon.cpp model_dl_worker_loop's emit_dl lambda.
// ===========================================================================
TEST_CASE("C.3 — model.downloading routes via the model_id job's client_id",
          "[c3][routing]") {
    ::unlink(ROUTED_SOCK);

    JobQueue q;
    JqGuard jqg(q);
    IpcServer server(ROUTED_SOCK);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient a(ROUTED_SOCK), b(ROUTED_SOCK);
    REQUIRE(a.connect());
    REQUIRE(b.connect());

    EventCounter a_counter, b_counter;
    a.set_event_callback(a_counter.callback());
    b.set_event_callback(b_counter.callback());
    ClientReader ar(a), br(b);

    // Auto-enqueue a model download job owned by A (mimics the C.7 path:
    // a postprocess job for A asks for a model that is not cached, so the
    // JobQueue auto-enqueues a ModelDownload owned by A).
    Job dl_job;
    dl_job.kind = JobKind::ModelDownload;
    dl_job.model_id = "whisper/base";
    int64_t dl_jid = q.enqueue(std::move(dl_job), JobKind::ModelDownload,
                               a.client_id());

    // Now emit the three model.downloading transitions the same way
    // model_dl_worker_loop's emit_dl does, capturing client_id on the
    // calling thread.
    std::string dl_cid = a.client_id();
    auto emit_dl = [&](const std::string& model, const std::string& status,
                       const std::string& error = "") {
        std::string c = dl_cid;
        std::string m = model, s = status, e = error;
        server.post([&server, c, m, s, e]() {
            IpcEvent ev;
            ev.event = "model.downloading";
            ev.data["model"] = m;
            ev.data["status"] = s;
            if (!e.empty()) ev.data["error"] = e;
            if (!c.empty()) server.send_to_client(c, std::move(ev));
            else            server.broadcast(ev);
        });
    };
    emit_dl("whisper/base", "downloading");
    emit_dl("whisper/base", "complete");

    CHECK(wait_until([&]() { return a_counter.count("model.downloading") >= 2; },
                     std::chrono::milliseconds(2000)));
    // Give B's reader an extra window to pick up any stray events that
    // might be in flight. If B were going to receive A's events, they'd
    // arrive in this window. (Reader thread continues dispatching here.)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CHECK(a_counter.count("model.downloading") == 2);
    CHECK(b_counter.count("model.downloading") == 0);
    CHECK(b_counter.total() == 0);

    // Cleanup — let the model_download worker drain.
    q.finish_download(dl_jid, /*ok=*/true);

    // ClientReader destructors run before IpcClient destructors (reverse
    // declaration order), stopping the reader threads cleanly. The
    // IpcClient destructor then calls close_connection().
    ::unlink(ROUTED_SOCK);
}

// ===========================================================================
// 6. state.changed remains GLOBAL — both clients receive every emission.
//    This is the "do NOT convert" boundary; state.changed is the composite
//    daemon state and must reach every connected client.
// ===========================================================================
TEST_CASE("C.3 — state.changed stays GLOBAL (broadcast to all clients)",
          "[c3][routing]") {
    ::unlink(ROUTED_SOCK);

    IpcServer server(ROUTED_SOCK);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient a(ROUTED_SOCK), b(ROUTED_SOCK);
    REQUIRE(a.connect());
    REQUIRE(b.connect());

    EventCounter a_counter, b_counter;
    a.set_event_callback(a_counter.callback());
    b.set_event_callback(b_counter.callback());
    ClientReader ar(a), br(b);

    // Emit three state.changed events the way daemon.cpp's
    // broadcast_state_inline does. Each one MUST reach both clients.
    auto post_state = [&](const std::string& state_name) {
        std::string sn = state_name;
        server.post([&server, sn]() {
            IpcEvent ev;
            ev.event = "state.changed";
            ev.data["state"] = sn;
            ev.data["recording"] = false;
            ev.data["postprocessing"] = false;
            ev.data["downloading"] = false;
            ev.data["streaming"] = false;
            server.broadcast(ev);
        });
    };
    post_state("idle");
    post_state("recording");
    post_state("postprocessing");

    CHECK(wait_until([&]() {
        return a_counter.count("state.changed") >= 3 &&
               b_counter.count("state.changed") >= 3;
    }, std::chrono::milliseconds(2000)));

    CHECK(a_counter.count("state.changed") == 3);
    CHECK(b_counter.count("state.changed") == 3);

    // ClientReader destructors run before IpcClient destructors (reverse
    // declaration order), stopping the reader threads cleanly. The
    // IpcClient destructor then calls close_connection().
    ::unlink(ROUTED_SOCK);
}

// ===========================================================================
// 7. Empty client_id fallback: a job whose client_id is empty (a daemon-
//    internal job, or a pre-session legacy request) emits via broadcast()
//    so both clients still receive the event. This is the fallback
//    contract spelled out in the C.3 spec ("don't let pre-session /
//    daemon-internal jobs' events vanish").
//
// Without disabling A.6 globally (which would unwire production code),
// we directly synthesize a Job with empty client_id and exercise the
// per-event sink path's empty-id branch.
// ===========================================================================
TEST_CASE("C.3 — empty client_id falls back to broadcast",
          "[c3][routing]") {
    ::unlink(ROUTED_SOCK);

    JobQueue q;
    JqGuard jqg(q);
    IpcServer server(ROUTED_SOCK);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient a(ROUTED_SOCK), b(ROUTED_SOCK);
    REQUIRE(a.connect());
    REQUIRE(b.connect());

    EventCounter a_counter, b_counter;
    a.set_event_callback(a_counter.callback());
    b.set_event_callback(b_counter.callback());
    ClientReader ar(a), br(b);

    // Enqueue with empty client_id (daemon-internal / pre-session). The
    // C.7 binding stores empty, and client_for_job() returns an optional
    // populated with "" — the sink's `cid && !cid->empty()` guard must
    // route via broadcast().
    int64_t jid = q.enqueue(Job{}, JobKind::Postprocess, /*client_id=*/"");

    auto& srv = server;
    auto& jobs = q;
    srv.post([&srv, &jobs, jid]() {
        IpcEvent ev;
        ev.event = "phase";
        ev.data["name"] = std::string("fallback-test");
        ev.data["job_id"] = jid;
        if (auto cid = jobs.client_for_job(jid); cid && !cid->empty())
            srv.send_to_client(*cid, std::move(ev));
        else
            srv.broadcast(ev);  // fallback path under test
    });

    // Both clients receive the event (broadcast). This is the requested
    // contract: an empty client_id does NOT silently drop the event.
    CHECK(wait_until([&]() {
        return a_counter.count("phase") >= 1 &&
               b_counter.count("phase") >= 1;
    }, std::chrono::milliseconds(2000)));
    CHECK(a_counter.count("phase") == 1);
    CHECK(b_counter.count("phase") == 1);

    q.finish(jid, /*ok=*/true);
    // ClientReader destructors run before IpcClient destructors (reverse
    // declaration order), stopping the reader threads cleanly. The
    // IpcClient destructor then calls close_connection().
    ::unlink(ROUTED_SOCK);
}

// ===========================================================================
// 8. Late event for a terminal job: the C.7 invariant guarantees the
//    job_id -> client_id binding is retained AFTER the job lands in a
//    terminal state (registry retention), so a `job.complete` posted
//    after `finish()` has already run still routes via client_for_job.
// ===========================================================================
TEST_CASE("C.3 — late job.complete for a terminal job still routes",
          "[c3][routing]") {
    ::unlink(ROUTED_SOCK);

    JobQueue q;
    JqGuard jqg(q);
    IpcServer server(ROUTED_SOCK);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient a(ROUTED_SOCK), b(ROUTED_SOCK);
    REQUIRE(a.connect());
    REQUIRE(b.connect());

    EventCounter a_counter, b_counter;
    a.set_event_callback(a_counter.callback());
    b.set_event_callback(b_counter.callback());
    ClientReader ar(a), br(b);

    // Enqueue a job owned by A, drive it through Dequeued → finished. The
    // pp_worker (JqGuard) dequeues, then we explicitly finish() to push
    // the job into a terminal state. After that, emit the job.complete
    // event the way pp_worker_loop does — the binding must still resolve.
    int64_t jid = q.enqueue(Job{}, JobKind::Postprocess, a.client_id());
    REQUIRE(wait_until([&]() { return q.slot_busy(JobKind::Postprocess); },
                       std::chrono::milliseconds(1000)));
    q.finish(jid, /*ok=*/true);

    // Confirm the binding survives — this is the C.7 invariant we are
    // relying on.
    auto retained = q.client_for_job(jid);
    REQUIRE(retained.has_value());
    REQUIRE(*retained == a.client_id());

    // Now emit a "late" job.complete and assert it still reaches A only.
    auto& srv = server;
    auto& jobs = q;
    srv.post([&srv, &jobs, jid]() {
        IpcEvent ev;
        ev.event = "job.complete";
        ev.data["job_id"] = jid;
        if (auto cid = jobs.client_for_job(jid); cid && !cid->empty())
            srv.send_to_client(*cid, std::move(ev));
        else
            srv.broadcast(ev);
    });

    CHECK(wait_until([&]() { return a_counter.count("job.complete") >= 1; },
                     std::chrono::milliseconds(2000)));
    // Give B's reader an extra window to pick up any stray events that
    // might be in flight. If B were going to receive A's events, they'd
    // arrive in this window. (Reader thread continues dispatching here.)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CHECK(a_counter.count("job.complete") == 1);
    CHECK(b_counter.count("job.complete") == 0);

    // ClientReader destructors run before IpcClient destructors (reverse
    // declaration order), stopping the reader threads cleanly. The
    // IpcClient destructor then calls close_connection().
    ::unlink(ROUTED_SOCK);
}

// ===========================================================================
// 9. Cross-routed: two clients each have their own job at the same time.
//    A's job emits go ONLY to A; B's job emits go ONLY to B. This is the
//    full bidirectional routing assertion.
// ===========================================================================
TEST_CASE("C.3 — cross-routed: A's job → A, B's job → B, no leakage either way",
          "[c3][routing]") {
    ::unlink(ROUTED_SOCK);

    JobQueue q;
    JqGuard jqg(q);
    IpcServer server(ROUTED_SOCK);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient a(ROUTED_SOCK), b(ROUTED_SOCK);
    REQUIRE(a.connect());
    REQUIRE(b.connect());

    EventCounter a_counter, b_counter;
    a.set_event_callback(a_counter.callback());
    b.set_event_callback(b_counter.callback());
    ClientReader ar(a), br(b);

    // Each client owns their own postprocess job. The JqGuard worker
    // sequentially dequeues them (slot capacity-1) — we finish() each
    // after dequeue so the next can run.
    int64_t a_jid = q.enqueue(Job{}, JobKind::Postprocess, a.client_id());
    REQUIRE(wait_until([&]() { return q.slot_busy(JobKind::Postprocess); },
                       std::chrono::milliseconds(1000)));
    q.finish(a_jid, /*ok=*/true);
    REQUIRE(wait_until([&]() { return !q.slot_busy(JobKind::Postprocess); },
                       std::chrono::milliseconds(1000)));
    int64_t b_jid = q.enqueue(Job{}, JobKind::Postprocess, b.client_id());
    REQUIRE(wait_until([&]() { return q.slot_busy(JobKind::Postprocess); },
                       std::chrono::milliseconds(1000)));
    q.finish(b_jid, /*ok=*/true);

    auto& srv = server;
    auto& jobs = q;
    auto post_phase = [&](int64_t jid, const std::string& name) {
        srv.post([&srv, &jobs, jid, name]() {
            IpcEvent ev;
            ev.event = "phase";
            ev.data["name"] = name;
            ev.data["job_id"] = jid;
            if (auto cid = jobs.client_for_job(jid); cid && !cid->empty())
                srv.send_to_client(*cid, std::move(ev));
            else
                srv.broadcast(ev);
        });
    };

    // Emit phase events for both jobs interleaved. Each must reach its
    // own client only.
    post_phase(a_jid, "transcribing");
    post_phase(b_jid, "transcribing");
    post_phase(a_jid, "summarizing");
    post_phase(b_jid, "summarizing");

    CHECK(wait_until([&]() {
        return a_counter.count("phase") >= 2 &&
               b_counter.count("phase") >= 2;
    }, std::chrono::milliseconds(2000)));

    CHECK(a_counter.count("phase") == 2);
    CHECK(b_counter.count("phase") == 2);

    // Verify the events landed on the right clients by checking job_ids.
    {
        std::lock_guard<std::mutex> lk(a_counter.mtx);
        for (auto& [name, jid] : a_counter.log) {
            if (name == "phase") CHECK(jid == a_jid);
        }
    }
    {
        std::lock_guard<std::mutex> lk(b_counter.mtx);
        for (auto& [name, jid] : b_counter.log) {
            if (name == "phase") CHECK(jid == b_jid);
        }
    }

    // ClientReader destructors run before IpcClient destructors (reverse
    // declaration order), stopping the reader threads cleanly. The
    // IpcClient destructor then calls close_connection().
    ::unlink(ROUTED_SOCK);
}
