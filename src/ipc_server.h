// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "ipc_protocol.h"

#include <climits>
#include <cstddef>
#include <cstdint>
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

    // Phase A.3: cap concurrent connected clients. When `clients_.size()`
    // is already at or past this cap, `accept_client()` still drains the
    // kernel queue (so the listening socket stops waking poll), writes a
    // single-line `{"event":"error","kind":"server_full",...}` JSON
    // refusal frame on the new fd, logs a warning with the peer address,
    // and closes the fd before it is registered. The listen backlog is
    // sized as `max_clients * 2` so kernel-queue back-pressure does not
    // bottleneck before the daemon-side cap engages. Must be called
    // before start() to influence the backlog; the runtime cap is read
    // live so post-start changes are honored on the next accept.
    void set_max_clients(size_t n) { max_clients_ = n > 0 ? n : 1; }
    size_t max_clients() const { return max_clients_; }

    // Phase A.3 test accessor: the listen backlog argument that start_unix()
    // / start_tcp() will pass (or did pass) to listen(2). Exposed so tests
    // can assert backlog tracks `max_clients * 2` without poking at
    // /proc/sys/net or relying on platform-specific getsockopt behavior.
    int backlog_for_test() const { return listen_backlog(); }

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
    //
    // Phase A.4 retains this for genuinely-global events. The per-`client_id`
    // routing primitive is `send_to_client()` — switching event call sites
    // from `broadcast()` to `send_to_client()` is C.7's job, not A.4's.
    void broadcast(const IpcEvent& ev);

    // Phase A.4: route an event to the specific client identified by
    // `client_id`. Looks up the client via the `client_id → fd` reverse
    // map. Best-effort: if the client has disconnected between event
    // generation and this call the event is dropped silently (debug-log
    // only). The caller does NOT need to hold any lock; lookup + send
    // happen on the poll thread when invoked from a posted callback.
    //
    // Wire shape: the event is serialized with `IpcEvent::client_id`
    // already populated to `client_id` so a downstream observer can
    // confirm the routing target from the payload alone. Callers may
    // pass an `IpcEvent` with `client_id` empty; this function stamps it
    // before serialization.
    void send_to_client(const std::string& client_id, IpcEvent ev);

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

    // Phase A.3: compute the listen backlog from `max_clients_`. Centralized
    // so `start_unix()` and `start_tcp()` cannot drift. Floor of 8 preserves
    // the historical behavior for the (untyped) case where `max_clients_`
    // is set very small — the kernel queue should never be the bottleneck
    // before the daemon-side cap engages.
    int listen_backlog() const {
        const size_t b = max_clients_ * 2;
        if (b < 8) return 8;
        if (b > static_cast<size_t>(INT_MAX)) return INT_MAX;
        return static_cast<int>(b);
    }

    // Phase A.4 mint a fresh client_id. Format: `c-<counter>-<6 hex chars>`.
    // The counter guarantees process-lifetime uniqueness; the hex suffix
    // makes the value non-guessable so a misbehaving Phase C.7 caller
    // cannot easily forge a routing target from a sequential id alone.
    // Hex chars come from `rand()` seeded once at first use — this is a
    // log-friendly tag, not a security primitive.
    std::string mint_client_id();

    // Phase A.3: reject a new fd because the daemon is at `max_clients_`.
    // Writes a single-line JSON `server_full` error frame to the fd
    // synchronously (this is a doomed fd — do NOT enqueue or arm POLLOUT),
    // logs a warning with the peer address, and closes it. The fd is
    // never registered in `clients_`.
    void reject_full(int fd);

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

        // Phase A.4 server-issued identifier. Assigned at the moment auth
        // completes — for Unix clients at `accept_client()` (since they
        // bypass PSK), for TCP clients when `handle_pending_psk()` flips
        // `auth_state` to `Authed`. Format: `c-<counter>-<6 hex chars>`,
        // short and log-friendly. Indexes the reverse-map
        // `client_id_to_fd_` for O(1) routing in `send_to_client()`.
        std::string client_id;

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

    // Phase A.4 reverse-map: assigned `client_id` → fd. Inserted at the
    // same moment `ClientState::client_id` is assigned and erased in
    // `remove_client()`. Read by `send_to_client()` for O(1) routing.
    std::unordered_map<std::string, int> client_id_to_fd_;

    // Phase A.4 monotonically increasing counter that mints unique
    // `client_id` values when combined with a short random hex suffix.
    // Bumped on every accept; never reset for the lifetime of the
    // process so disconnect-then-reconnect produces a fresh id (Phase
    // C.7 routing must not see a stale binding revive on a new fd).
    uint64_t next_client_id_ = 1;

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

    // Phase A.3 connection cap. Default 16; configurable via
    // `[ipc] max_clients`. Connections past this are refused at accept
    // with a JSON `server_full` error frame and immediate close. The
    // listen backlog tracks `max_clients_ * 2` (see listen_backlog()).
    size_t max_clients_ = 16;

    // Handle the first-frame PSK exchange for a TCP client. Returns true
    // when the connection should remain open after this line; false when
    // the caller should drop the client (rejected). Always consumes the
    // line regardless of outcome.
    bool handle_pending_psk(int fd, ClientState& cs, const std::string& line);
};

} // namespace recmeet
