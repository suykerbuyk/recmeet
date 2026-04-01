// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "ipc_server.h"
#include "ipc_client.h"
#include "ipc_protocol.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace recmeet;

namespace {

const char* TEST_SOCK = "/tmp/recmeet_test_ipc.sock";

// Connect to a Unix socket. Returns fd or -1.
int connect_client(const char* path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Send a string over fd.
void send_line(int fd, const std::string& msg) {
    std::string wire = msg + "\n";
    write(fd, wire.data(), wire.size());
}

// Read until newline or timeout. Returns the line (without \n).
std::string read_line(int fd, int timeout_ms = 2000) {
    std::string buf;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        char c;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;  // 50ms
        int ret = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ret > 0 && FD_ISSET(fd, &rfds)) {
            ssize_t n = read(fd, &c, 1);
            if (n <= 0) break;
            if (c == '\n') return buf;
            buf += c;
        }
    }
    return buf;
}

} // anonymous namespace

TEST_CASE("IpcServer: request/response round-trip", "[ipc_server]") {
    unlink(TEST_SOCK);

    IpcServer server(TEST_SOCK);
    server.on("echo", [](const IpcRequest& req, IpcResponse& resp, IpcError&) {
        resp.result = req.params;
        return true;
    });
    REQUIRE(server.start());

    std::thread srv_thread([&server]() { server.run(); });

    // Give server a moment to enter poll()
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = connect_client(TEST_SOCK);
    REQUIRE(client >= 0);

    // Send echo request
    IpcRequest req;
    req.id = 1;
    req.method = "echo";
    req.params["hello"] = std::string("world");
    send_line(client, serialize(req));

    std::string reply = read_line(client);
    REQUIRE_FALSE(reply.empty());

    IpcMessage msg;
    REQUIRE(parse_ipc_message(reply, msg));
    REQUIRE(msg.type == IpcMessageType::Response);
    CHECK(msg.response.id == 1);
    CHECK(json_val_as_string(msg.response.result["hello"]) == "world");

    close(client);
    server.stop();
    srv_thread.join();
}

TEST_CASE("IpcServer: unknown method returns error", "[ipc_server]") {
    unlink(TEST_SOCK);

    IpcServer server(TEST_SOCK);
    REQUIRE(server.start());

    std::thread srv_thread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = connect_client(TEST_SOCK);
    REQUIRE(client >= 0);

    IpcRequest req;
    req.id = 99;
    req.method = "nonexistent.method";
    send_line(client, serialize(req));

    std::string reply = read_line(client);
    REQUIRE_FALSE(reply.empty());

    IpcMessage msg;
    REQUIRE(parse_ipc_message(reply, msg));
    REQUIRE(msg.type == IpcMessageType::Error);
    CHECK(msg.error.id == 99);
    CHECK(msg.error.code == static_cast<int>(IpcErrorCode::MethodNotFound));

    close(client);
    server.stop();
    srv_thread.join();
}

TEST_CASE("IpcServer: broadcast sends events to all clients", "[ipc_server]") {
    unlink(TEST_SOCK);

    IpcServer server(TEST_SOCK);
    server.on("ping", [](const IpcRequest&, IpcResponse& resp, IpcError&) {
        resp.result["pong"] = true;
        return true;
    });
    REQUIRE(server.start());

    std::thread srv_thread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int c1 = connect_client(TEST_SOCK);
    int c2 = connect_client(TEST_SOCK);
    REQUIRE(c1 >= 0);
    REQUIRE(c2 >= 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Broadcast from another thread via post()
    server.post([&server]() {
        IpcEvent ev;
        ev.event = "test.broadcast";
        ev.data["msg"] = std::string("hello");
        server.broadcast(ev);
    });

    // Both clients should receive the event
    std::string line1 = read_line(c1);
    std::string line2 = read_line(c2);

    IpcMessage msg1, msg2;
    REQUIRE(parse_ipc_message(line1, msg1));
    REQUIRE(parse_ipc_message(line2, msg2));
    CHECK(msg1.type == IpcMessageType::Event);
    CHECK(msg2.type == IpcMessageType::Event);
    CHECK(msg1.event.event == "test.broadcast");
    CHECK(msg2.event.event == "test.broadcast");

    close(c1);
    close(c2);
    server.stop();
    srv_thread.join();
}

TEST_CASE("IpcServer: handler returning false sends error", "[ipc_server]") {
    unlink(TEST_SOCK);

    IpcServer server(TEST_SOCK);
    server.on("fail", [](const IpcRequest&, IpcResponse&, IpcError& err) {
        err.code = 42;
        err.message = "intentional failure";
        return false;
    });
    REQUIRE(server.start());

    std::thread srv_thread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = connect_client(TEST_SOCK);
    REQUIRE(client >= 0);

    IpcRequest req;
    req.id = 5;
    req.method = "fail";
    send_line(client, serialize(req));

    std::string reply = read_line(client);
    IpcMessage msg;
    REQUIRE(parse_ipc_message(reply, msg));
    REQUIRE(msg.type == IpcMessageType::Error);
    CHECK(msg.error.id == 5);
    CHECK(msg.error.code == 42);
    CHECK(msg.error.message == "intentional failure");

    close(client);
    server.stop();
    srv_thread.join();
}

TEST_CASE("IpcServer: post() wakes poll loop", "[ipc_server]") {
    unlink(TEST_SOCK);

    IpcServer server(TEST_SOCK);
    REQUIRE(server.start());

    bool posted_ran = false;
    std::thread srv_thread([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    server.post([&posted_ran, &server]() {
        posted_ran = true;
        server.stop();
    });

    srv_thread.join();
    CHECK(posted_ran);
}

// ---------------------------------------------------------------------------
// TCP server tests
// ---------------------------------------------------------------------------

TEST_CASE("IpcServer TCP: listen + echo round-trip", "[ipc_server]") {
    IpcServer server("127.0.0.1:19876");
    server.on("echo", [](const IpcRequest& req, IpcResponse& resp, IpcError&) {
        resp.result["msg"] = req.params.at("msg");
        return true;
    });
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    IpcClient client("127.0.0.1:19876");
    REQUIRE(client.connect());
    CHECK(client.is_remote());

    JsonMap params;
    params["msg"] = std::string("hello-tcp");
    IpcResponse resp;
    IpcError err;
    REQUIRE(client.call("echo", params, resp, err));
    CHECK(json_val_as_string(resp.result["msg"]) == "hello-tcp");

    server.stop();
    srv.join();
}

TEST_CASE("IpcServer TCP: broadcast to 2 clients", "[ipc_server]") {
    IpcServer server("127.0.0.1:19877");
    server.on("ping", [](const IpcRequest&, IpcResponse& resp, IpcError&) {
        resp.result["ok"] = true;
        return true;
    });
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    IpcClient c1("127.0.0.1:19877"), c2("127.0.0.1:19877");
    REQUIRE(c1.connect());
    REQUIRE(c2.connect());

    // Give server time to accept both clients
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::atomic<int> events_seen{0};
    auto handler = [&events_seen](const IpcEvent& ev) {
        if (ev.event == "test.broadcast") events_seen++;
    };
    c1.set_event_callback(handler);
    c2.set_event_callback(handler);

    // Broadcast from server
    server.post([&server]() {
        IpcEvent ev;
        ev.event = "test.broadcast";
        ev.data["value"] = std::string("hello");
        server.broadcast(ev);
    });

    // Both clients should receive the event
    c1.read_events("test.broadcast", 2000);
    c2.read_events("test.broadcast", 2000);
    CHECK(events_seen == 2);

    server.stop();
    srv.join();
}
