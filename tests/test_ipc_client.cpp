// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "daemon_test_harness.h"
#include "ipc_client.h"
#include "ipc_server.h"
#include "test_tmpdir.h"

#include <filesystem>
#include <thread>
#include <chrono>
#include <unistd.h>

using namespace recmeet;
using recmeet::test::DaemonTestHarness;

namespace {
const std::string TEST_SOCK =
    recmeet::test::tmp_path("recmeet_test_client.sock").string();
}

TEST_CASE("IpcClient: call() round-trip", "[ipc_client]") {
    unlink(TEST_SOCK.c_str());

    IpcServer server(TEST_SOCK);
    server.on("greet", [](const IpcRequest& req, IpcResponse& resp, IpcError&) {
        std::string name = json_val_as_string(req.params.at("name"));
        resp.result["greeting"] = std::string("Hello, " + name + "!");
        return true;
    });
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    IpcClient client(TEST_SOCK);
    REQUIRE(client.connect());

    IpcResponse resp;
    IpcError err;
    JsonMap params;
    params["name"] = std::string("world");
    REQUIRE(client.call("greet", params, resp, err));
    CHECK(json_val_as_string(resp.result["greeting"]) == "Hello, world!");

    server.stop();
    srv.join();
}

TEST_CASE("IpcClient: call() error response", "[ipc_client]") {
    unlink(TEST_SOCK.c_str());

    IpcServer server(TEST_SOCK);
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    IpcClient client(TEST_SOCK);
    REQUIRE(client.connect());

    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client.call("nonexistent", resp, err));
    CHECK(err.code == static_cast<int>(IpcErrorCode::MethodNotFound));

    server.stop();
    srv.join();
}

TEST_CASE("IpcClient: events dispatched during call()", "[ipc_client]") {
    unlink(TEST_SOCK.c_str());

    IpcServer server(TEST_SOCK);
    server.on("slow", [&server](const IpcRequest&, IpcResponse& resp, IpcError&) {
        // Broadcast an event before responding
        IpcEvent ev;
        ev.event = "progress";
        ev.data["pct"] = 50;
        server.broadcast(ev);
        resp.result["done"] = true;
        return true;
    });
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    IpcClient client(TEST_SOCK);
    REQUIRE(client.connect());

    bool got_event = false;
    client.set_event_callback([&got_event](const IpcEvent& ev) {
        if (ev.event == "progress") got_event = true;
    });

    IpcResponse resp;
    IpcError err;
    REQUIRE(client.call("slow", resp, err));
    CHECK(json_val_as_bool(resp.result["done"]));
    CHECK(got_event);

    server.stop();
    srv.join();
}

TEST_CASE("IpcClient: connect fails without server", "[ipc_client]") {
    unlink(TEST_SOCK.c_str());
    IpcClient client(TEST_SOCK);
    CHECK_FALSE(client.connect());
    CHECK_FALSE(client.connected());
}

TEST_CASE("IpcClient: call() without connect returns error", "[ipc_client]") {
    auto missing = recmeet::test::tmp_path("recmeet_test_noexist.sock");
    std::filesystem::remove_all(missing);
    IpcClient client(missing.string());
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client.call("status.get", resp, err));
    CHECK(err.message == "Not connected");
}

TEST_CASE("IpcClient: read_events exits when callback closes connection", "[ipc_client]") {
    unlink(TEST_SOCK.c_str());

    IpcServer server(TEST_SOCK);
    server.on("trigger", [&server](const IpcRequest&, IpcResponse& resp, IpcError&) {
        resp.result["ok"] = true;
        // Schedule the error event after the response is sent
        std::thread([&server]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            server.post([&server]() {
                IpcEvent ev;
                ev.event = "state.changed";
                ev.data["error"] = std::string("Transcription produced no text.");
                server.broadcast(ev);
            });
        }).detach();
        return true;
    });
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    IpcClient client(TEST_SOCK);
    REQUIRE(client.connect());

    bool got_error = false;
    client.set_event_callback([&client, &got_error](const IpcEvent& ev) {
        if (ev.event == "state.changed") {
            got_error = true;
            client.close_connection();  // simulate main.cpp error handler
        }
    });

    IpcResponse resp;
    IpcError err;
    REQUIRE(client.call("trigger", resp, err));

    // read_events must return promptly (not hang) even though until_event
    // "job.complete" is never sent — because the callback closed the fd.
    bool finished = client.read_events("job.complete", 3000);
    CHECK_FALSE(finished);
    CHECK(got_error);

    server.stop();
    srv.join();
}

TEST_CASE("daemon_running: returns false when no daemon", "[ipc_client]") {
    unlink(TEST_SOCK.c_str());
    CHECK_FALSE(daemon_running(TEST_SOCK));
}

TEST_CASE("daemon_running: returns true with running server", "[ipc_client]") {
    // Phase 2b: daemon_running probes `status.get` (see
    // src/ipc_client.cpp:1244), which is a production verb. Drive the
    // production handler via DaemonTestHarness so we exercise the real
    // status.get path instead of stubbing it locally.
    DaemonTestHarness harness;
    harness.start();
    CHECK(daemon_running(harness.socket_path()));
}

// ---------------------------------------------------------------------------
// TCP tests
// ---------------------------------------------------------------------------

TEST_CASE("IpcClient: TCP connect fails without server (fast timeout)", "[ipc_client]") {
    // Should return false quickly (5s timeout), not hang
    auto start = std::chrono::steady_clock::now();
    IpcClient client("127.0.0.1:19876");
    CHECK(client.is_remote());
    CHECK_FALSE(client.connect());
    auto elapsed = std::chrono::steady_clock::now() - start;
    // Should fail in well under 10 seconds (connect refused is instant on localhost)
    CHECK(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 10);
}

TEST_CASE("IpcClient: is_remote() false for Unix, true for TCP", "[ipc_client]") {
    IpcClient unix_client("/tmp/test.sock");
    CHECK_FALSE(unix_client.is_remote());

    IpcClient tcp_client("127.0.0.1:9090");
    CHECK(tcp_client.is_remote());
}

TEST_CASE("IpcClient: set_address() updates address when disconnected", "[ipc_client]") {
    IpcClient client;  // default Unix
    CHECK_FALSE(client.is_remote());

    client.set_address("127.0.0.1:9090");
    CHECK(client.is_remote());

    // Reset to Unix
    client.set_address("/tmp/other.sock");
    CHECK_FALSE(client.is_remote());
}
