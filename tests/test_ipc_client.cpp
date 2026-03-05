// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "ipc_client.h"
#include "ipc_server.h"

#include <thread>
#include <chrono>
#include <unistd.h>

using namespace recmeet;

namespace {
const char* TEST_SOCK = "/tmp/recmeet_test_client.sock";
}

TEST_CASE("IpcClient: call() round-trip", "[ipc_client]") {
    unlink(TEST_SOCK);

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
    unlink(TEST_SOCK);

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
    unlink(TEST_SOCK);

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
    unlink(TEST_SOCK);
    IpcClient client(TEST_SOCK);
    CHECK_FALSE(client.connect());
    CHECK_FALSE(client.connected());
}

TEST_CASE("IpcClient: call() without connect returns error", "[ipc_client]") {
    IpcClient client("/tmp/recmeet_test_noexist.sock");
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client.call("status.get", resp, err));
    CHECK(err.message == "Not connected");
}

TEST_CASE("daemon_running: returns false when no daemon", "[ipc_client]") {
    unlink(TEST_SOCK);
    CHECK_FALSE(daemon_running(TEST_SOCK));
}

TEST_CASE("daemon_running: returns true with running server", "[ipc_client]") {
    unlink(TEST_SOCK);

    IpcServer server(TEST_SOCK);
    server.on("status.get", [](const IpcRequest&, IpcResponse& resp, IpcError&) {
        resp.result["state"] = std::string("idle");
        return true;
    });
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CHECK(daemon_running(TEST_SOCK));

    server.stop();
    srv.join();
}
