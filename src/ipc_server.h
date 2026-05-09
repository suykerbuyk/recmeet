// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "ipc_protocol.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace recmeet {

// Method handler: receives a request, returns a response or error.
// On success, populate resp and return true.
// On failure, populate err and return false.
using MethodHandler = std::function<bool(const IpcRequest& req,
                                         IpcResponse& resp,
                                         IpcError& err)>;

// Per-connection authentication state for the Phase A.1 PSK gate.
//
// Unix-socket clients are accepted as `Authed` immediately (kernel-enforced
// peer credentials are sufficient for local trust). TCP clients begin as
// `PendingPsk` and must send `{"type":"auth.token","token":"..."}` as the
// first frame. On match → `Authed`. On mismatch or wrong first frame →
// `Rejected` and the connection is closed after sending an `auth.error`.
//
// `PendingSession` is intentionally NOT included yet — A.6 (session.init
// handshake) introduces a second post-PSK gate and will extend this enum
// at that time. Keeping the v1 set minimal makes the dispatch chokepoint
// obvious: only `Authed` connections may dispatch arbitrary requests.
enum class AuthState { PendingPsk, Authed, Rejected };

class IpcServer {
public:
    explicit IpcServer(const std::string& socket_path);
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    // Register a method handler. Must be called before start().
    void on(const std::string& method, MethodHandler handler);

    // Configure the pre-shared key required for TCP connections.
    // Must be called before start() if the listener is TCP.
    // Empty string disables PSK auth (only safe for tests / Unix-only mode).
    // Phase A.1: PSK is global (one shared key for the daemon). Per-server
    // PSKs deferred to v2 multi-server work.
    void set_psk(const std::string& psk) { psk_ = psk; }

    // Start listening. Returns false on bind/listen failure.
    // For TCP listeners the daemon-side caller MUST set_psk() with a
    // non-empty value first; start() refuses to bring up an unauthenticated
    // TCP surface and logs a clear error.
    bool start();

    // Run the poll() loop. Blocks until stop() is called.
    void run();

    // Signal the poll loop to exit (thread-safe via self-pipe).
    void stop();

    // Broadcast an event to all connected clients.
    void broadcast(const IpcEvent& ev);

    // Wake the poll loop from a worker thread (e.g., after job completion).
    // The callback will be invoked on the poll thread.
    void post(std::function<void()> fn);

    int listen_fd() const { return listen_fd_; }

private:
    void accept_client();
    void handle_client_data(int fd);
    void remove_client(int fd);
    void send_to(int fd, const std::string& msg);
    void drain_wakeup();
    void run_posted();
    bool start_unix();
    bool start_tcp();

    IpcAddress addr_;
    int listen_fd_ = -1;
    int wakeup_read_ = -1;   // self-pipe read end
    int wakeup_write_ = -1;  // self-pipe write end
    bool running_ = false;

    struct ClientState {
        std::string read_buf;  // accumulates data until \n
        AuthState   auth_state = AuthState::Authed;  // Unix default; TCP overrides
        std::string peer_addr;  // "host:port" for TCP, "unix" for Unix; for log lines
    };
    std::unordered_map<int, ClientState> clients_;

    std::unordered_map<std::string, MethodHandler> handlers_;

    std::mutex post_mu_;
    std::vector<std::function<void()>> posted_;

    // Pre-shared key for TCP connections. Empty → PSK auth disabled.
    // Set via set_psk() before start(). Compared with constant-time
    // equality on the auth.token frame.
    std::string psk_;

    // Handle the first-frame PSK exchange for a TCP client. Returns true
    // when the connection should remain open after this line; false when
    // the caller should drop the client (rejected). Always consumes the
    // line regardless of outcome.
    bool handle_pending_psk(int fd, ClientState& cs, const std::string& line);
};

} // namespace recmeet
