// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "ipc_server.h"
#include "ipc_client.h"
#include "ipc_protocol.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <signal.h>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

// Ignore SIGPIPE so writing to disconnected clients doesn't kill the process
static struct SigpipeIgnorer {
    SigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} s_ignore_sigpipe;

using namespace recmeet;

// ---------------------------------------------------------------------------
// DaemonSim — lightweight daemon simulator for integration tests
// ---------------------------------------------------------------------------

namespace {

const char* INTEGRATION_SOCK = "/tmp/recmeet_test_integration.sock";

enum SimState { SIdle = 0, SRecording = 1, SPostprocessing = 2, SDownloading = 3 };

struct DaemonSim {
    IpcServer server;
    std::thread srv_thread;
    std::atomic<int> state{SIdle};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> inject_error{false};
    std::thread worker;
    std::mutex mu;
    std::condition_variable cv;

    explicit DaemonSim(const char* sock = INTEGRATION_SOCK)
        : server(sock)
    {
        unlink(sock);

        server.on("status.get", [this](const IpcRequest&, IpcResponse& resp, IpcError&) {
            resp.result["state"] = std::string(state_str());
            return true;
        });

        server.on("record.start", [this](const IpcRequest&, IpcResponse& resp, IpcError& err) {
            int expected = SIdle;
            if (!state.compare_exchange_strong(expected, SRecording)) {
                err.code = static_cast<int>(IpcErrorCode::Busy);
                err.message = "Daemon is busy";
                return false;
            }

            {
                IpcEvent ev;
                ev.event = "state.changed";
                ev.data["state"] = std::string("recording");
                server.broadcast(ev);
            }

            if (worker.joinable()) worker.join();
            stop_requested.store(false);

            worker = std::thread([this]() {
                {
                    std::unique_lock<std::mutex> lk(mu);
                    cv.wait_for(lk, std::chrono::seconds(5), [this]() {
                        return stop_requested.load() || inject_error.load();
                    });
                }

                if (inject_error.load()) {
                    server.post([this]() {
                        state.store(SIdle);
                        IpcEvent ev;
                        ev.event = "state.changed";
                        ev.data["state"] = std::string("idle");
                        ev.data["error"] = std::string("simulated pipeline error");
                        server.broadcast(ev);
                    });
                    return;
                }

                server.post([this]() {
                    state.store(SPostprocessing);
                    IpcEvent ev;
                    ev.event = "state.changed";
                    ev.data["state"] = std::string("postprocessing");
                    server.broadcast(ev);
                });

                std::this_thread::sleep_for(std::chrono::milliseconds(30));

                server.post([this]() {
                    state.store(SIdle);
                    IpcEvent ev;
                    ev.event = "job.complete";
                    ev.data["note_path"] = std::string("/tmp/test.md");
                    ev.data["output_dir"] = std::string("/tmp/");
                    server.broadcast(ev);

                    IpcEvent sev;
                    sev.event = "state.changed";
                    sev.data["state"] = std::string("idle");
                    server.broadcast(sev);
                });
            });

            resp.result["ok"] = true;
            return true;
        });

        server.on("record.stop", [this](const IpcRequest&, IpcResponse& resp, IpcError& err) {
            if (state.load() != SRecording) {
                err.code = static_cast<int>(IpcErrorCode::NotRecording);
                err.message = "Not recording";
                return false;
            }
            stop_requested.store(true);
            cv.notify_all();
            resp.result["ok"] = true;
            return true;
        });

        server.on("models.ensure", [this](const IpcRequest&, IpcResponse& resp, IpcError& err) {
            int expected = SIdle;
            if (!state.compare_exchange_strong(expected, SDownloading)) {
                err.code = static_cast<int>(IpcErrorCode::Busy);
                err.message = "Daemon is busy";
                return false;
            }

            {
                IpcEvent ev;
                ev.event = "state.changed";
                ev.data["state"] = std::string("downloading");
                server.broadcast(ev);
            }

            if (worker.joinable()) worker.join();
            worker = std::thread([this]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                server.post([this]() {
                    state.store(SIdle);
                    IpcEvent ev;
                    ev.event = "state.changed";
                    ev.data["state"] = std::string("idle");
                    server.broadcast(ev);
                });
            });

            resp.result["ok"] = true;
            return true;
        });
    }

    void start() {
        REQUIRE(server.start());
        srv_thread = std::thread([this]() { server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void shutdown() {
        stop_requested.store(true);
        cv.notify_all();
        if (worker.joinable()) worker.join();
        server.stop();
        if (srv_thread.joinable()) srv_thread.join();
    }

    ~DaemonSim() { shutdown(); }

    const char* state_str() const {
        switch (state.load()) {
            case SIdle:            return "idle";
            case SRecording:       return "recording";
            case SPostprocessing:  return "postprocessing";
            case SDownloading:     return "downloading";
        }
        return "unknown";
    }

    bool wait_for_state(int target, int timeout_ms = 3000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (state.load() != target) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
};

struct EventCollector {
    std::mutex mu;
    std::vector<IpcEvent> events;

    void add(const IpcEvent& ev) {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(ev);
    }

    EventCallback callback() {
        return [this](const IpcEvent& ev) { add(ev); };
    }

    size_t count() {
        std::lock_guard<std::mutex> lk(mu);
        return events.size();
    }

    std::vector<IpcEvent> snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return events;
    }

    bool has_event(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : events)
            if (e.event == name) return true;
        return false;
    }

    bool has_state_event(const std::string& state_val) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : events) {
            if (e.event == "state.changed") {
                auto it = e.data.find("state");
                if (it != e.data.end() && json_val_as_string(it->second) == state_val)
                    return true;
            }
        }
        return false;
    }
};

void drain_events(IpcClient& client, int ms = 200) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        client.read_and_dispatch(10);
    }
}

std::unique_ptr<IpcClient> make_client(const char* sock = INTEGRATION_SOCK) {
    auto c = std::make_unique<IpcClient>(sock);
    REQUIRE(c->connect());
    return c;
}

} // anonymous namespace

// ===========================================================================
// Category 1: State Broadcast Completeness
// ===========================================================================

TEST_CASE("record.start broadcasts state.changed: recording", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto observer = make_client();
    EventCollector obs_events;
    observer->set_event_callback(obs_events.callback());

    auto starter = make_client();
    IpcResponse resp;
    IpcError err;
    REQUIRE(starter->call("record.start", resp, err));

    drain_events(*observer, 200);

    CHECK(obs_events.has_state_event("recording"));

    sim.stop_requested.store(true);
    sim.cv.notify_all();
}

TEST_CASE("recording->postprocessing broadcasts state.changed", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto client = make_client();
    EventCollector events;
    client->set_event_callback(events.callback());

    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("record.start", resp, err));
    REQUIRE(client->call("record.stop", resp, err));

    drain_events(*client, 500);

    CHECK(events.has_state_event("postprocessing"));
}

TEST_CASE("postprocessing->idle broadcasts state.changed + job.complete", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto client = make_client();
    EventCollector events;
    client->set_event_callback(events.callback());

    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("record.start", resp, err));
    REQUIRE(client->call("record.stop", resp, err));

    drain_events(*client, 500);

    CHECK(events.has_event("job.complete"));
    CHECK(events.has_state_event("idle"));
}

TEST_CASE("pipeline error broadcasts state.changed: idle with error", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto client = make_client();
    EventCollector events;
    client->set_event_callback(events.callback());

    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("record.start", resp, err));

    sim.inject_error.store(true);
    sim.cv.notify_all();

    drain_events(*client, 500);

    auto snap = events.snapshot();
    bool found_error = false;
    for (auto& e : snap) {
        if (e.event == "state.changed") {
            auto it = e.data.find("error");
            if (it != e.data.end()) {
                found_error = true;
                CHECK(json_val_as_string(it->second) == "simulated pipeline error");
            }
        }
    }
    CHECK(found_error);
}

TEST_CASE("full lifecycle event sequence is ordered", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto client = make_client();
    EventCollector events;
    client->set_event_callback(events.callback());

    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("record.start", resp, err));
    REQUIRE(client->call("record.stop", resp, err));

    REQUIRE(sim.wait_for_state(SIdle, 3000));
    drain_events(*client, 300);

    auto snap = events.snapshot();

    std::vector<std::string> seq;
    for (auto& e : snap) {
        if (e.event == "state.changed") {
            auto it = e.data.find("state");
            if (it != e.data.end())
                seq.push_back("state:" + json_val_as_string(it->second));
        } else if (e.event == "job.complete") {
            seq.push_back("job.complete");
        }
    }

    REQUIRE(seq.size() >= 4);
    CHECK(seq[0] == "state:recording");
    CHECK(seq[1] == "state:postprocessing");
    CHECK(seq[2] == "job.complete");
    CHECK(seq[3] == "state:idle");
}

// ===========================================================================
// Category 2: Multi-Client Event Delivery
// ===========================================================================

TEST_CASE("broadcast reaches all connected clients", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto c1 = make_client();
    auto c2 = make_client();
    auto c3 = make_client();

    EventCollector ev2, ev3;
    c2->set_event_callback(ev2.callback());
    c3->set_event_callback(ev3.callback());

    // Ensure server has accepted all clients before triggering broadcast
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    IpcResponse resp;
    IpcError err;
    REQUIRE(c1->call("record.start", resp, err));

    // Drain both clients — interleave to avoid one consuming all timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < deadline) {
        c2->read_and_dispatch(10);
        c3->read_and_dispatch(10);
    }

    CHECK(ev2.has_state_event("recording"));
    CHECK(ev3.has_state_event("recording"));

    sim.stop_requested.store(true);
    sim.cv.notify_all();
}

TEST_CASE("client connected mid-recording receives subsequent events", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto starter = make_client();
    IpcResponse resp;
    IpcError err;
    REQUIRE(starter->call("record.start", resp, err));

    auto late = make_client();
    EventCollector late_events;
    late->set_event_callback(late_events.callback());

    REQUIRE(starter->call("record.stop", resp, err));

    drain_events(*late, 500);

    CHECK(late_events.has_state_event("postprocessing"));
    CHECK(late_events.has_state_event("idle"));
}

TEST_CASE("disconnected client doesn't block broadcast", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto c1 = make_client();
    auto c2 = make_client();
    auto c3 = make_client();

    EventCollector ev1, ev3;
    c1->set_event_callback(ev1.callback());
    c3->set_event_callback(ev3.callback());

    c2->close_connection();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    IpcResponse resp;
    IpcError err;
    REQUIRE(c1->call("record.start", resp, err));

    drain_events(*c1, 200);
    drain_events(*c3, 200);

    CHECK(ev1.has_state_event("recording"));
    CHECK(ev3.has_state_event("recording"));

    sim.stop_requested.store(true);
    sim.cv.notify_all();
}

// ===========================================================================
// Category 3: Late-Join State Sync
// ===========================================================================

TEST_CASE("status.get during recording returns recording", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto starter = make_client();
    IpcResponse resp;
    IpcError err;
    REQUIRE(starter->call("record.start", resp, err));

    auto checker = make_client();
    REQUIRE(checker->call("status.get", resp, err));
    CHECK(json_val_as_string(resp.result["state"]) == "recording");

    sim.stop_requested.store(true);
    sim.cv.notify_all();
}

TEST_CASE("status.get during postprocessing returns postprocessing", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto client = make_client();
    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("record.start", resp, err));
    REQUIRE(client->call("record.stop", resp, err));

    REQUIRE(sim.wait_for_state(SPostprocessing, 2000));

    auto checker = make_client();
    REQUIRE(checker->call("status.get", resp, err));
    CHECK(json_val_as_string(resp.result["state"]) == "postprocessing");
}

TEST_CASE("status.get during downloading returns downloading", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto client = make_client();
    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("models.ensure", resp, err));

    REQUIRE(sim.wait_for_state(SDownloading, 2000));

    auto checker = make_client();
    REQUIRE(checker->call("status.get", resp, err));
    CHECK(json_val_as_string(resp.result["state"]) == "downloading");
}

// ===========================================================================
// Category 4: Buffered Event Integrity
// ===========================================================================

TEST_CASE("events during blocking call() are all delivered", "[ipc][integration]") {
    const char* sock = "/tmp/recmeet_test_buffered1.sock";
    unlink(sock);

    IpcServer server(sock);
    server.on("multi_event", [&server](const IpcRequest&, IpcResponse& resp, IpcError&) {
        for (int i = 0; i < 3; i++) {
            IpcEvent ev;
            ev.event = "progress";
            ev.data["step"] = static_cast<int64_t>(i);
            server.broadcast(ev);
        }
        resp.result["ok"] = true;
        return true;
    });
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    IpcClient client(sock);
    REQUIRE(client.connect());

    EventCollector events;
    client.set_event_callback(events.callback());

    IpcResponse resp;
    IpcError err;
    REQUIRE(client.call("multi_event", resp, err));

    drain_events(client, 100);

    CHECK(events.count() >= 3);

    server.stop();
    srv.join();
}

TEST_CASE("events after response in same buffer are not lost", "[ipc][integration]") {
    const char* sock = "/tmp/recmeet_test_buffered2.sock";
    unlink(sock);

    IpcServer server(sock);
    server.on("fast", [&server](const IpcRequest&, IpcResponse& resp, IpcError&) {
        resp.result["ok"] = true;
        server.post([&server]() {
            IpcEvent ev;
            ev.event = "follow_up";
            ev.data["val"] = std::string("after_response");
            server.broadcast(ev);
        });
        return true;
    });
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    IpcClient client(sock);
    REQUIRE(client.connect());

    EventCollector events;
    client.set_event_callback(events.callback());

    IpcResponse resp;
    IpcError err;
    REQUIRE(client.call("fast", resp, err));

    drain_events(client, 200);

    CHECK(events.has_event("follow_up"));

    server.stop();
    srv.join();
}

TEST_CASE("rapid broadcast burst: no events lost", "[ipc][integration]") {
    const char* sock = "/tmp/recmeet_test_burst.sock";
    unlink(sock);

    IpcServer server(sock);
    server.on("ping", [](const IpcRequest&, IpcResponse& resp, IpcError&) {
        resp.result["ok"] = true;
        return true;
    });
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    IpcClient client(sock);
    REQUIRE(client.connect());

    // Ensure server has accepted the client via a round-trip
    IpcResponse ping_resp;
    IpcError ping_err;
    REQUIRE(client.call("ping", ping_resp, ping_err));

    EventCollector events;
    client.set_event_callback(events.callback());

    server.post([&server]() {
        for (int i = 0; i < 20; i++) {
            IpcEvent ev;
            ev.event = "burst";
            ev.data["seq"] = static_cast<int64_t>(i);
            server.broadcast(ev);
        }
    });

    drain_events(client, 500);

    auto snap = events.snapshot();
    int burst_count = 0;
    for (auto& e : snap)
        if (e.event == "burst") burst_count++;

    CHECK(burst_count == 20);

    server.stop();
    srv.join();
}

// ===========================================================================
// Category 5: Concurrency & Error Handling
// ===========================================================================

TEST_CASE("concurrent record.start: one succeeds, one gets Busy", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto c1 = make_client();
    auto c2 = make_client();

    IpcResponse r1, r2;
    IpcError e1, e2;

    std::thread t1([&]() { c1->call("record.start", r1, e1); });
    std::thread t2([&]() { c2->call("record.start", r2, e2); });
    t1.join();
    t2.join();

    bool ok1 = r1.result.count("ok") && json_val_as_bool(r1.result["ok"]);
    bool ok2 = r2.result.count("ok") && json_val_as_bool(r2.result["ok"]);

    CHECK((ok1 != ok2));

    if (!ok1) CHECK(e1.code == static_cast<int>(IpcErrorCode::Busy));
    if (!ok2) CHECK(e2.code == static_cast<int>(IpcErrorCode::Busy));

    sim.stop_requested.store(true);
    sim.cv.notify_all();
}

TEST_CASE("record.stop from different client than starter", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto starter = make_client();
    auto stopper = make_client();

    EventCollector stop_events;
    stopper->set_event_callback(stop_events.callback());

    IpcResponse resp;
    IpcError err;
    REQUIRE(starter->call("record.start", resp, err));
    REQUIRE(stopper->call("record.stop", resp, err));

    drain_events(*stopper, 500);

    CHECK(stop_events.has_state_event("postprocessing"));
    CHECK(stop_events.has_state_event("idle"));
}

TEST_CASE("record.stop when idle returns NotRecording", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    auto client = make_client();
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client->call("record.stop", resp, err));
    CHECK(err.code == static_cast<int>(IpcErrorCode::NotRecording));
}

TEST_CASE("client disconnect during recording doesn't affect daemon", "[ipc][integration]") {
    DaemonSim sim;
    sim.start();

    {
        auto starter = make_client();
        IpcResponse resp;
        IpcError err;
        REQUIRE(starter->call("record.start", resp, err));
    }

    CHECK(sim.state.load() == SRecording);

    auto stopper = make_client();
    IpcResponse resp;
    IpcError err;
    REQUIRE(stopper->call("record.stop", resp, err));

    REQUIRE(sim.wait_for_state(SIdle, 3000));
}
