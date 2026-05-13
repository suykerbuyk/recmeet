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

    // Phase A.4: the server-issued `client_id` for this connection.
    // Populated when `connect()` succeeds, from the `auth.ok` frame
    // (TCP: read inline after the PSK exchange; Unix: read on the
    // first `read_and_dispatch()` after connect — `auth.ok` arrives
    // unsolicited from the daemon for Unix clients). Empty before
    // connect or after `close_connection()`. Stable for the lifetime
    // of a connection: reconnect mints a fresh id on the server.
    const std::string& client_id() const { return client_id_; }

    // Phase A.5: the server-reported `protocol_version` parsed from the
    // `auth.ok` frame. Zero before connect, after `close_connection()`,
    // or if the daemon emits a frame without the field (pre-A.5 daemon —
    // also treated as a mismatch and the connect is failed). On a
    // matching version this returns `IPC_PROTOCOL_VERSION`.
    int protocol_version() const { return protocol_version_; }

    // Phase A.5: true when the most recent `connect()` attempt observed
    // a `protocol_version` mismatch (including a missing field). Set
    // alongside the failure return from `connect()`; cleared on the
    // next successful connect. Tests use this to distinguish a clean
    // connect failure from a version-rejection failure without parsing
    // log output.
    bool protocol_mismatch() const { return protocol_mismatch_; }

    // Get the underlying fd (for integration with external event loops).
    int fd() const { return fd_; }

    // Read data from socket, parse messages, dispatch events.
    // Returns false on disconnect.
    bool read_and_dispatch(int timeout_ms);

private:
    void process_line(const std::string& line);
    bool connect_unix();
    bool connect_tcp();

    // Phase A.5: parse an auth.ok reply, capture client_id +
    // protocol_version, and enforce the version invariant. Returns true
    // when the reply is acceptable (matching version), false when the
    // version is absent or mismatched. Callers close the fd on a false
    // return; the helper only mutates per-client state.
    bool verify_auth_ok_and_capture(const std::string& reply);

    IpcAddress addr_;
    int fd_ = -1;
    int64_t next_id_ = 1;
    std::string read_buf_;
    EventCallback event_cb_;

    // Phase A.4: server-issued client identifier, extracted from the
    // `auth.ok` frame. Cleared on `close_connection()` so a reconnect
    // never surfaces a stale id. Read by `client_id()`.
    std::string client_id_;

    // Phase A.5: server-reported wire protocol version, parsed from the
    // same `auth.ok` frame. Zero when absent, on a fresh client, or
    // after `close_connection()`. Read by `protocol_version()`.
    int protocol_version_ = 0;

    // Phase A.5: latched true when a `connect()` attempt rejected the
    // server's `auth.ok` because the `protocol_version` did not match
    // (including the "field missing" case). Surfaced via
    // `protocol_mismatch()`. Cleared on the next successful connect.
    bool protocol_mismatch_ = false;

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
