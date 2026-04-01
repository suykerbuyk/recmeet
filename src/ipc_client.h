// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "ipc_protocol.h"

#include <functional>
#include <string>

namespace recmeet {

// Callback for server-pushed events.
using EventCallback = std::function<void(const IpcEvent& ev)>;

class IpcClient {
public:
    explicit IpcClient(const std::string& socket_path = "");
    ~IpcClient();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    // Connect to the daemon. Returns true on success.
    bool connect();

    // Whether connected.
    bool connected() const { return fd_ >= 0; }

    // Send a request and block until the matching response arrives.
    // Events received while waiting are dispatched to the event callback.
    // Returns true for success (result populated), false for error (err populated).
    // timeout_ms: -1 = wait forever.
    bool call(const std::string& method, const JsonMap& params,
              IpcResponse& resp, IpcError& err, int timeout_ms = 30000);

    // Convenience: call with no params.
    bool call(const std::string& method, IpcResponse& resp, IpcError& err,
              int timeout_ms = 30000);

    // Set event callback. Called on the calling thread during call() or read_events().
    void set_event_callback(EventCallback cb) { event_cb_ = std::move(cb); }

    // Read and dispatch events until timeout or an event matching `until_event` arrives.
    // Returns true if the named event was seen, false on timeout.
    bool read_events(const std::string& until_event = "", int timeout_ms = -1);

    // Close the connection.
    void close_connection();

    // Change the target address. Only valid when disconnected.
    void set_address(const std::string& addr);

    // Whether this client targets a remote (TCP) daemon.
    bool is_remote() const { return addr_.transport == IpcTransport::Tcp; }

    // Get the underlying fd (for integration with external event loops).
    int fd() const { return fd_; }

    // Read data from socket, parse messages, dispatch events.
    // Returns false on disconnect.
    bool read_and_dispatch(int timeout_ms);

private:
    void process_line(const std::string& line);
    bool connect_unix();
    bool connect_tcp();

    IpcAddress addr_;
    int fd_ = -1;
    int64_t next_id_ = 1;
    std::string read_buf_;
    EventCallback event_cb_;

    // For blocking call(): stores the response/error for the pending request ID.
    int64_t pending_id_ = 0;
    IpcMessage pending_result_;
    bool pending_done_ = false;

    // Set when the target event for read_events() is seen.
    bool event_matched_ = false;
    std::string until_event_;
};

// Check whether a daemon is running (socket exists and is connectable).
bool daemon_running(const std::string& socket_path = "");

} // namespace recmeet
