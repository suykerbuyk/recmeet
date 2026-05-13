// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "ipc_protocol.h"

#include <cstddef>
#include <deque>
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

// Per-message overflow class for the Phase A.2 per-fd outbound queue.
//
// `Event` frames (broadcast caption / progress / phase tickers) tolerate
// drop-oldest under back-pressure: if the queue is full, the oldest queued
// event is evicted to make room for the new one and a per-fd drop counter
// increments. Per-frame logging is intentionally NOT done — a flood that
// triggers the cap is the same flood that would amplify into log spam.
//
// `Response` frames (auth.ok / auth.error / handler responses / error
// frames) carry stateful semantics — silently dropping them produces a
// client that thinks its last request is still pending forever. On queue
// overflow for a Response, the fd is closed instead of dropping. The
// client is either gone or unresponsive; either way the slot is freed.
enum class MessageClass { Response, Event };

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

    // Phase A.2: cap NDJSON line length per connection. A read that grows
    // the per-fd accumulation buffer past this without finding `\n` drops
    // the fd (close + logged refusal). Default 8 MB; configurable via
    // `[ipc] max_message_bytes` in daemon.yaml. Cap applies to NDJSON line
    // length only; binary frame payloads (Phase C `0x01`/`0x02`/`0x03`)
    // are bounded separately by `max_upload_bytes` at the request level.
    void set_max_message_bytes(size_t n) { max_message_bytes_ = n; }
    size_t max_message_bytes() const { return max_message_bytes_; }

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

    // Enqueue an outbound NDJSON frame on the per-fd queue (Phase A.2).
    // `cls` selects the overflow policy: `Event` drops the oldest entry
    // and increments the drop counter; `Response` closes the fd if the
    // queue is full. The caller MUST NOT touch the client after a call
    // with `Response`-class overflow — the fd may already be gone.
    //
    // This function never blocks and never writes the network. Actual
    // bytes go out via drain_outbound() when POLLOUT fires (or
    // opportunistically immediately after enqueue if the queue was
    // previously empty — see implementation).
    void send_to(int fd, std::string msg, MessageClass cls);

    // Attempt to drain a client's outbound queue with non-blocking
    // writes. On `EAGAIN`/`EWOULDBLOCK` the caller arms POLLOUT for this
    // fd on the next poll() so it does not busy-spin. On a hard write
    // error the client is removed.
    void drain_outbound(int fd);

    void drain_wakeup();
    void run_posted();
    bool start_unix();
    bool start_tcp();

    IpcAddress addr_;
    int listen_fd_ = -1;
    int wakeup_read_ = -1;   // self-pipe read end
    int wakeup_write_ = -1;  // self-pipe write end
    bool running_ = false;

    // Per-fd outbound queue entry (Phase A.2). `payload` is the wire-form
    // NDJSON frame including the trailing `\n`. `bytes_sent` tracks how
    // many bytes have already gone out on the wire on prior partial
    // writes (non-blocking `write()` can short-write).
    struct OutboundFrame {
        std::string  payload;
        size_t       bytes_sent = 0;
        MessageClass cls = MessageClass::Event;
    };

    struct ClientState {
        std::string read_buf;  // accumulates data until \n
        AuthState   auth_state = AuthState::Authed;  // Unix default; TCP overrides
        std::string peer_addr;  // "host:port" for TCP, "unix" for Unix; for log lines

        // Per-fd outbound queue (Phase A.2). Capacity-bounded by entries
        // AND total queued bytes (whichever cap engages first — sized for
        // a 10 Hz caption stream surviving a 10 s consumer stall).
        std::deque<OutboundFrame> outbound;
        size_t outbound_bytes = 0;       // sum of queued payload.size() - bytes_sent
        uint64_t dropped_events = 0;     // count of evicted events (drop-oldest policy)
        bool want_pollout = false;       // EAGAIN on last write — arm POLLOUT next poll
        bool pending_close = false;      // Response-class overflow / fatal write error
    };
    std::unordered_map<int, ClientState> clients_;

    // Per-fd outbound queue caps (Phase A.2). Whichever cap engages first
    // triggers the overflow policy. Sized per the plan body:
    // 10 Hz captions × 10 s stall = ~100 frames worst case → 64-entry cap
    // is the tighter side; 256 KB byte cap protects against a single
    // pathologically large payload.
    static constexpr size_t kOutboundMaxEntries = 64;
    static constexpr size_t kOutboundMaxBytes   = 256 * 1024;

    std::unordered_map<std::string, MethodHandler> handlers_;

    std::mutex post_mu_;
    std::vector<std::function<void()>> posted_;

    // Pre-shared key for TCP connections. Empty → PSK auth disabled.
    // Set via set_psk() before start(). Compared with constant-time
    // equality on the auth.token frame.
    std::string psk_;

    // Phase A.2 NDJSON line cap. Default 8 MB; configurable via
    // `[ipc] max_message_bytes`. Reads that accumulate past this without
    // finding `\n` drop the connection.
    size_t max_message_bytes_ = 8 * 1024 * 1024;

    // Handle the first-frame PSK exchange for a TCP client. Returns true
    // when the connection should remain open after this line; false when
    // the caller should drop the client (rejected). Always consumes the
    // line regardless of outcome.
    bool handle_pending_psk(int fd, ClientState& cs, const std::string& line);
};

} // namespace recmeet
