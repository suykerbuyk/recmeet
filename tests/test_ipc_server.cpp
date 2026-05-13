// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "ipc_server.h"
#include "ipc_client.h"
#include "ipc_protocol.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
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
    // Phase A.1: TCP listeners require a PSK. Both ends read the same value
    // (server via set_psk(), client via RECMEET_AUTH_TOKEN inside connect_tcp()).
    const char* prev = std::getenv("RECMEET_AUTH_TOKEN");
    std::string prev_str = prev ? prev : "";
    setenv("RECMEET_AUTH_TOKEN", "ipc_server_test_token", 1);

    IpcServer server("127.0.0.1:19876");
    server.set_psk("ipc_server_test_token");
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

    if (prev) setenv("RECMEET_AUTH_TOKEN", prev_str.c_str(), 1);
    else      unsetenv("RECMEET_AUTH_TOKEN");
}

TEST_CASE("IpcServer TCP: broadcast to 2 clients", "[ipc_server]") {
    // Phase A.1: same PSK pattern as the round-trip test above.
    const char* prev = std::getenv("RECMEET_AUTH_TOKEN");
    std::string prev_str = prev ? prev : "";
    setenv("RECMEET_AUTH_TOKEN", "ipc_server_test_token", 1);

    IpcServer server("127.0.0.1:19877");
    server.set_psk("ipc_server_test_token");
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

    if (prev) setenv("RECMEET_AUTH_TOKEN", prev_str.c_str(), 1);
    else      unsetenv("RECMEET_AUTH_TOKEN");
}

// ---------------------------------------------------------------------------
// Phase A.2 — NDJSON line cap + non-blocking send + per-fd outbound queue
// ---------------------------------------------------------------------------

namespace {

// Connect to a TCP port, returns fd or -1. Used by the A.2 slow-consumer
// tests that need a raw socket — IpcClient hides the readable/writable
// surface behind its own reader thread, which is exactly what we want to
// avoid here.
int a2_tcp_connect(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Send a single PSK frame to clear the auth gate. Returns true on success.
bool a2_send_psk(int fd, const std::string& token) {
    std::string line = "{\"type\":\"auth.token\",\"token\":\"" + token + "\"}\n";
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(line.size())) {
        ssize_t n = write(fd, line.data() + total, line.size() - total);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

// Block until a single NDJSON line arrives (or timeout). Returns the line
// content without the trailing `\n`. Empty on EOF or timeout.
std::string a2_recv_line(int fd, int timeout_ms = 2000) {
    std::string buf;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        char c;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;
        int r = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(fd, &rfds)) {
            ssize_t n = read(fd, &c, 1);
            if (n <= 0) break;
            if (c == '\n') return buf;
            buf += c;
        }
    }
    return buf;
}

// Wait for the fd to read EOF (server closed) or hit timeout. Returns true
// on a clean EOF, false on timeout / unexpected bytes.
bool a2_wait_eof(int fd, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    char throwaway[64];
    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd pfd{fd, POLLIN, 0};
        int r = poll(&pfd, 1, 50);
        if (r > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(fd, throwaway, sizeof(throwaway));
            if (n == 0) return true;
            if (n < 0) {
                if (errno == ECONNRESET) return true;
                return false;
            }
            // We tolerate any pending bytes (auth.error frame, response
            // text, etc.) and keep draining until EOF or timeout.
        }
    }
    return false;
}

// RAII helper for RECMEET_AUTH_TOKEN scoped to one TEST_CASE.
struct A2ScopedAuthToken {
    bool had_prev;
    std::string prev_val;
    explicit A2ScopedAuthToken(const std::string& value) {
        const char* p = std::getenv("RECMEET_AUTH_TOKEN");
        had_prev = p != nullptr;
        prev_val = had_prev ? p : "";
        setenv("RECMEET_AUTH_TOKEN", value.c_str(), 1);
    }
    ~A2ScopedAuthToken() {
        if (had_prev) setenv("RECMEET_AUTH_TOKEN", prev_val.c_str(), 1);
        else          unsetenv("RECMEET_AUTH_TOKEN");
    }
};

} // anonymous namespace

TEST_CASE("A.2: oversized NDJSON line without newline drops the connection",
          "[ipc][a2]") {
    // Set a small cap so we don't have to push 8 MB to trigger it. The
    // cap applies per-connection on the read accumulation buffer.
    unlink(TEST_SOCK);
    IpcServer server(TEST_SOCK);
    server.set_max_message_bytes(1024);  // 1 KB cap
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = connect_client(TEST_SOCK);
    REQUIRE(client >= 0);

    // Send 2 KB with no newline. The server admits the first read, then
    // notices the buffer is past the cap with no `\n` and closes.
    std::string blob(2048, 'A');
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(blob.size())) {
        ssize_t n = write(client, blob.data() + total, blob.size() - total);
        if (n <= 0) break;
        total += n;
    }

    // Expect EOF from the server.
    char throwaway[16];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool got_eof = false;
    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = read(client, throwaway, sizeof(throwaway));
        if (n == 0) { got_eof = true; break; }
        if (n < 0 && errno != EINTR && errno != EAGAIN) { got_eof = true; break; }
        if (n < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
    }
    CHECK(got_eof);

    close(client);
    server.stop();
    srv.join();
}

TEST_CASE("A.2: slowloris byte-at-a-time accumulates past the cap and drops",
          "[ipc][a2]") {
    // Same cap-trigger as the oversized test, but the bytes arrive one at
    // a time. Confirms the cap is evaluated on the cumulative read_buf
    // after every read, not just on a single oversized send.
    unlink(TEST_SOCK);
    IpcServer server(TEST_SOCK);
    server.set_max_message_bytes(512);
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = connect_client(TEST_SOCK);
    REQUIRE(client >= 0);

    bool got_eof = false;
    const int max_bytes = 4096;  // way past the 512-byte cap
    for (int i = 0; i < max_bytes; ++i) {
        char c = 'B';
        ssize_t n = write(client, &c, 1);
        if (n <= 0) { got_eof = true; break; }
        // Tiny sleep so the server gets repeated wakeups rather than
        // coalescing the whole stream into one read.
        if ((i % 16) == 15)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        // Poll for EOF without blocking — server may have closed already.
        struct pollfd pfd{client, POLLIN, 0};
        if (poll(&pfd, 1, 0) > 0) {
            if (pfd.revents & (POLLHUP | POLLERR | POLLIN)) {
                char tb;
                ssize_t r = read(client, &tb, 1);
                if (r <= 0) { got_eof = true; break; }
            }
        }
    }

    // If we did not see EOF mid-loop, give the server a moment and try
    // once more — write() to a closed socket may report EPIPE/SIGPIPE
    // rather than 0.
    if (!got_eof) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        char tb;
        ssize_t r = read(client, &tb, 1);
        if (r <= 0) got_eof = true;
    }
    CHECK(got_eof);

    close(client);
    server.stop();
    srv.join();
}

TEST_CASE("A.2: NDJSON line cap is configurable via set_max_message_bytes",
          "[ipc][a2]") {
    // Confirms a small operator-supplied cap (1024 bytes) triggers the
    // drop on a 2 KB line, AND that a line *under* the cap passes through
    // the dispatcher unchanged.
    unlink(TEST_SOCK);
    IpcServer server(TEST_SOCK);
    server.set_max_message_bytes(1024);

    std::atomic<bool> handler_ran{false};
    server.on("ping", [&handler_ran](const IpcRequest&, IpcResponse& resp,
                                     IpcError&) {
        handler_ran = true;
        resp.result["ok"] = true;
        return true;
    });
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 1) A small request well under the cap is dispatched normally.
    {
        int client = connect_client(TEST_SOCK);
        REQUIRE(client >= 0);
        IpcRequest req;
        req.id = 1;
        req.method = "ping";
        send_line(client, serialize(req));
        std::string reply = read_line(client);
        CHECK_FALSE(reply.empty());
        CHECK(handler_ran);
        close(client);
    }

    // 2) A 2 KB single line trips the cap and the server drops.
    {
        int client = connect_client(TEST_SOCK);
        REQUIRE(client >= 0);
        std::string blob(2048, 'C');
        ssize_t total = 0;
        while (total < static_cast<ssize_t>(blob.size())) {
            ssize_t n = write(client, blob.data() + total,
                              blob.size() - total);
            if (n <= 0) break;
            total += n;
        }
        // Expect EOF.
        char tb;
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::seconds(2);
        bool got_eof = false;
        while (std::chrono::steady_clock::now() < deadline) {
            ssize_t r = read(client, &tb, 1);
            if (r == 0) { got_eof = true; break; }
            if (r < 0 && errno != EAGAIN && errno != EINTR) {
                got_eof = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(got_eof);
        close(client);
    }

    server.stop();
    srv.join();
}

TEST_CASE("A.2: per-fd outbound queue drops oldest events under back-pressure",
          "[ipc][a2]") {
    // Hard-back-pressure test. We open a TCP client whose receive window
    // is intentionally tiny, never read, and watch the server enqueue
    // events into the per-fd queue until the cap engages. Once it does,
    // drop-oldest must kick in (Event class) and the server must NOT
    // close the fd (that would be Response-class behavior).
    A2ScopedAuthToken auth("a2-overflow-token");
    const uint16_t PORT = 19890;
    const std::string ADDR = "127.0.0.1:" + std::to_string(PORT);

    IpcServer server(ADDR);
    server.set_psk("a2-overflow-token");
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = a2_tcp_connect(PORT);
    REQUIRE(client >= 0);
    // Pin receive window small so the kernel buffers fill quickly. Linux
    // doubles SO_RCVBUF for accounting; the floor is ~2 KB. The actual
    // back-pressure point is well above this once TCP autotuning kicks
    // in, so we ALSO use big events below to push past the kernel buffer.
    int rcvbuf = 2048;
    setsockopt(client, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // Clear PSK on this raw fd.
    REQUIRE(a2_send_psk(client, "a2-overflow-token"));
    std::string auth_reply = a2_recv_line(client, 2000);
    REQUIRE(auth_reply.find("auth.ok") != std::string::npos);

    // Now NEVER read from the client. The server's send buffer + kernel
    // buffer to this fd will fill up; subsequent broadcasts must enqueue.
    //
    // To guarantee the queue overflows even if the kernel TCP send buffer
    // is large (Linux default ~4 MB autotuning), each event carries a
    // ~16 KB payload and we fire 200 of them — total wire weight is ~3 MB,
    // far past the per-fd queue's 256 KB byte cap.
    constexpr int N = 200;
    server.post([&server]() {
        for (int i = 0; i < N; ++i) {
            IpcEvent ev;
            ev.event = "caption";  // Event-class per A.2 (drop-oldest)
            ev.data["seq"] = static_cast<int64_t>(i);
            ev.data["text"] = std::string(16 * 1024, 'E');
            server.broadcast(ev);
        }
    });

    // Give the loop time to process the burst. The slow client never
    // reads — overflow must be drop-oldest, NOT close. We confirm
    // close-was-NOT-triggered by checking the fd stays writable.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // The fd MUST still be open. Send a no-op byte (writable check) —
    // a closed peer would surface as EPIPE/ECONNRESET on write or as
    // a POLLHUP on poll().
    struct pollfd pfd{client, POLLOUT | POLLIN, 0};
    int pr = poll(&pfd, 1, 50);
    REQUIRE(pr >= 0);
    CHECK_FALSE(pfd.revents & POLLHUP);
    CHECK_FALSE(pfd.revents & POLLERR);

    // Now drain the client and confirm we receive *fewer* than N events,
    // but at least some — proves drop-oldest evicted entries rather than
    // closing the fd or losing all events.
    int events_seen = 0;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string line = a2_recv_line(client, 100);
        if (line.empty()) break;
        if (line.find("\"caption\"") != std::string::npos) ++events_seen;
    }
    CHECK(events_seen > 0);
    CHECK(events_seen < N);  // some were dropped

    close(client);
    server.stop();
    srv.join();
}

TEST_CASE("A.2: per-fd outbound queue closes fd on response-class overflow",
          "[ipc][a2]") {
    // Synthetic response storm. The server's `blob.get` handler replies
    // with a large response on every call. The client sends many
    // requests back-to-back without reading replies — replies pile up in
    // the per-fd outbound queue and, on overflow, MUST close the fd
    // because responses are Response-class (close-on-overflow).
    A2ScopedAuthToken auth("a2-resp-token");
    const uint16_t PORT = 19891;
    const std::string ADDR = "127.0.0.1:" + std::to_string(PORT);

    IpcServer server(ADDR);
    server.set_psk("a2-resp-token");
    // A response is wire size ~ key+value+id; we make the value a 16 KB
    // string so 16 responses fills the 256 KB byte cap exactly.
    server.on("blob.get", [](const IpcRequest& req, IpcResponse& resp,
                             IpcError&) {
        resp.id = req.id;
        resp.result["blob"] = std::string(16 * 1024, 'R');
        return true;
    });
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int client = a2_tcp_connect(PORT);
    REQUIRE(client >= 0);
    // Lock the receive window tight BEFORE connect side handshake to keep
    // it small after TCP autotuning. On Linux SO_RCVBUF below 2304 is
    // clamped; we use the floor.
    int rcvbuf = 2048;
    setsockopt(client, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    REQUIRE(a2_send_psk(client, "a2-resp-token"));
    std::string auth_reply = a2_recv_line(client, 2000);
    REQUIRE(auth_reply.find("auth.ok") != std::string::npos);

    // Each response is 16 KB; we send 2000 requests to guarantee the
    // server queue overflows even with a generously-sized TCP send buffer
    // on the loopback. 2000 × 16 KB = ~32 MB worth, far past anything
    // the kernel will absorb before back-pressure.
    int sent_count = 0;
    for (int i = 0; i < 2000; ++i) {
        IpcRequest req;
        req.id = i + 1;
        req.method = "blob.get";
        std::string wire = serialize(req) + "\n";
        ssize_t total = 0;
        bool short_send = false;
        while (total < static_cast<ssize_t>(wire.size())) {
            ssize_t n = write(client, wire.data() + total,
                              wire.size() - total);
            if (n < 0 && errno == EPIPE) { short_send = true; break; }
            if (n <= 0) { short_send = true; break; }
            total += n;
        }
        if (short_send) break;
        ++sent_count;
    }
    // We expect to land at least *some* requests, but not necessarily
    // all 2000 if the server closes the fd mid-stream.
    CHECK(sent_count > 0);

    // Server MUST close the fd once the response queue overflows. Allow
    // up to 5 seconds — the response handler is dispatched on the poll
    // thread, so the close lands as soon as one outbound queue overflows.
    bool got_eof = a2_wait_eof(client, 5000);
    CHECK(got_eof);

    close(client);
    server.stop();
    srv.join();
}

TEST_CASE("A.2: slow-TCP-consumer does not stall the poll thread",
          "[ipc][a2]") {
    // The C-1 acceptance test. With the iter-139 spin-on-EAGAIN code, a
    // single slow client could pin the poll thread on a write() loop and
    // starve every other client. Phase A.2 must enqueue + arm POLLOUT
    // instead — so a second client connecting AFTER the slow one is
    // stuck must still get its requests dispatched.
    A2ScopedAuthToken auth("a2-stall-token");
    const uint16_t PORT = 19892;
    const std::string ADDR = "127.0.0.1:" + std::to_string(PORT);

    IpcServer server(ADDR);
    server.set_psk("a2-stall-token");
    server.on("ping", [](const IpcRequest& req, IpcResponse& resp,
                         IpcError&) {
        resp.id = req.id;
        resp.result["pong"] = true;
        return true;
    });
    REQUIRE(server.start());
    std::thread srv([&server]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ---- Slow client: connects, auths, then never reads. ----
    int slow = a2_tcp_connect(PORT);
    REQUIRE(slow >= 0);
    int slow_rcvbuf = 2048;
    setsockopt(slow, SOL_SOCKET, SO_RCVBUF, &slow_rcvbuf, sizeof(slow_rcvbuf));
    REQUIRE(a2_send_psk(slow, "a2-stall-token"));
    std::string slow_auth = a2_recv_line(slow, 2000);
    REQUIRE(slow_auth.find("auth.ok") != std::string::npos);

    // Flood the slow client with broadcasts so its outbound queue fills
    // up + back-pressures. We post the broadcast onto the server thread
    // so we know the event loop is actively trying to write.
    server.post([&server]() {
        for (int i = 0; i < 500; ++i) {
            IpcEvent ev;
            ev.event = "caption";
            ev.data["seq"] = static_cast<int64_t>(i);
            ev.data["text"] = std::string(128, 'S');
            server.broadcast(ev);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // ---- Healthy client: connects AFTER the slow client is stuck and
    // makes a real request. With the old spin-on-EAGAIN code, this would
    // never get serviced — the poll thread is pinned on `slow`. With the
    // A.2 non-blocking rewrite, this must complete in <2 s.
    int healthy = a2_tcp_connect(PORT);
    REQUIRE(healthy >= 0);
    REQUIRE(a2_send_psk(healthy, "a2-stall-token"));
    std::string h_auth = a2_recv_line(healthy, 2000);
    REQUIRE(h_auth.find("auth.ok") != std::string::npos);

    IpcRequest req;
    req.id = 42;
    req.method = "ping";
    std::string wire = serialize(req) + "\n";
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(wire.size())) {
        ssize_t n = write(healthy, wire.data() + total, wire.size() - total);
        REQUIRE(n > 0);
        total += n;
    }

    std::string reply = a2_recv_line(healthy, 2000);
    REQUIRE_FALSE(reply.empty());
    IpcMessage msg;
    REQUIRE(parse_ipc_message(reply, msg));
    CHECK(msg.type == IpcMessageType::Response);
    CHECK(msg.response.id == 42);

    close(healthy);
    close(slow);
    server.stop();
    srv.join();
}
