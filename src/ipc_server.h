// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "ipc_protocol.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>   // std::pair (C.13 ResumeTokenResolver return)
#include <vector>

namespace recmeet {

// Phase A.6 session-handshake types. These live on the per-`client_id` slot
// inside `IpcServer::ClientState` and are populated by the `session.init` /
// `session.update_credentials` / `session.update_prefs` handlers. They are
// cleared automatically on disconnect via `remove_client()`. The daemon
// reads them at `enqueue_postprocess()` time via `IpcServer::get_session()`
// and merges them into the postprocess job's `Config` (see `merge_creds_for_job`)
// — the subprocess never re-reads env or daemon.yaml, so this merge is the
// only credential path from operator config into the subprocess JSON.
//
// Empty-string sentinels mean "unset" for every string field — empty and
// unset are equivalent at this layer. The merge in `merge_creds_for_job`
// treats a non-empty session string as "session-supplied" and applies the
// daemon-env > session > daemon.yaml precedence chain accordingly.
struct SessionCredentials {
    std::string provider;                          // "xai" / "openai" / "anthropic"
    std::string api_key;                           // legacy single-key field
    std::map<std::string, std::string> api_keys;   // per-provider keys
};

// Session preferences carried from `session.init`. `caption_latency_ms`
// defaults to 500 (C.10's planned ms cadence); the value is rejected outside
// [200, 2000] by the `session.init` and `session.update_prefs` handlers.
// `summarization_backend` is constrained to `"http"` or `"local"` — empty
// is allowed and treated as "use daemon.yaml fallback"; any other value is
// rejected at the handler.
struct SessionPreferences {
    std::string output_dir;
    std::string note_dir;
    std::string language;
    std::string vocabulary;
    std::string mic_source;
    std::string monitor_source;
    std::string whisper_model;
    std::string summarization_backend;   // "" / "http" / "local"
    std::string llm_model;
    bool captions_enabled = false;
    int  caption_latency_ms = 500;
};

// Method handler: receives a request, returns a response or error.
// On success, populate resp and return true.
// On failure, populate err and return false.
using MethodHandler = std::function<bool(const IpcRequest& req,
                                         IpcResponse& resp,
                                         IpcError& err)>;

// Phase C.10a: binary-frame handler. Invoked on the poll thread for every
// fully-assembled non-NDJSON frame (`0x01`/`0x02`/`0x03`) received from an
// Authed client. `client_id` is the server-issued id of the originating
// connection (the same value handlers see on `IpcRequest::client_id`);
// `type` is the frame discriminator; `payload` is the raw decoded body.
// C.10a wires this to route `0x03` streaming-audio payloads into the
// streaming session keyed by `stream_token`; C.2/C.4 will consume
// `0x01`/`0x02`. Returning false tears the connection down (protocol
// violation — e.g. a `0x03` frame with no live streaming session). When
// no handler is registered every binary frame is discarded with a debug
// trace, preserving the C.1 behavior.
using BinaryFrameHandler = std::function<bool(const std::string& client_id,
                                              FrameType type,
                                              const std::string& payload)>;

// Phase C.10a: client-disconnect handler. Invoked on the poll thread from
// `remove_client()` AFTER the fd is closed and the client maps are erased,
// for every client that had a server-issued `client_id` (i.e. completed
// auth). `client_id` is that id. C.10a wires this to abort any streaming
// session the disconnected client owned (mark the JobQueue job failed,
// unlink the temp WAV, release the streaming slot). The handler must not
// block — it runs inline on the poll loop. Unset → no-op (pre-C.10a
// behavior).
using ClientDisconnectHandler = std::function<void(const std::string& client_id)>;

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

    // Phase C.10a: register the binary-frame handler. Must be called before
    // start(). At most one handler — a second call replaces the first.
    void on_binary_frame(BinaryFrameHandler handler) {
        binary_frame_handler_ = std::move(handler);
    }

    // Phase C.10a: register the client-disconnect handler. Must be called
    // before start(). At most one — a second call replaces the first.
    void on_client_disconnect(ClientDisconnectHandler handler) {
        client_disconnect_handler_ = std::move(handler);
    }

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

    // Phase C.1: cap on a single binary frame's declared length (the
    // `0x01`/`0x02`/`0x03` discriminators). Distinct from the NDJSON line
    // cap above — binary frames are length-prefixed, not newline-delimited,
    // so the FrameReader rejects an oversized declared length before it
    // buffers any payload bytes. Default `kDefaultMaxBinaryFrameBytes`
    // (16 MiB). Applied to every ClientState's FrameReader at accept time;
    // C.2 / C.10a will wire this to a `[ipc] max_upload_bytes` config key.
    void set_max_binary_frame_bytes(size_t n) { max_binary_frame_bytes_ = n; }
    size_t max_binary_frame_bytes() const { return max_binary_frame_bytes_; }

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

    // Phase A.5 test seam: override the `protocol_version` value stamped
    // into outgoing `auth.ok` frames. Production code never calls this —
    // the default is `IPC_PROTOCOL_VERSION`. Tests use it to simulate a
    // server speaking a different version (mismatch path) or a pre-A.5
    // server that omits the field entirely (pass any negative value to
    // suppress the field). Must be called before clients connect; reads
    // of the value run on the poll thread at auth-completion time.
    void set_protocol_version_for_test(int v) { protocol_version_ = v; }
    int  protocol_version_for_test() const     { return protocol_version_; }

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

    // Phase C.4: route a binary frame (`0x01`/`0x02`/`0x03`) to a specific
    // client by `client_id`. Wraps `payload` via `frame_binary(type, ...)`
    // and enqueues it on the per-fd outbound queue, observing the same
    // back-pressure semantics as every other write.
    //
    // `cls` controls the overflow policy:
    //   - `MessageClass::Response` (the C.4 `process.fetch` policy) closes
    //     the client on outbound-queue overflow. Fetch is a request/response
    //     exchange — silently dropping an artifact frame would leave the
    //     client awaiting bytes that never arrive, so closing is the correct
    //     overflow behavior.
    //   - `MessageClass::Event` would drop-oldest on overflow — appropriate
    //     for future server-pushed binary streams (none today). Not used by
    //     C.4.
    //
    // Best-effort delivery on a missing client (raced disconnect): the
    // frame is dropped with a debug trace. Caller does not hold any lock.
    void send_binary_to_client(const std::string& client_id,
                               FrameType type,
                               std::string payload,
                               MessageClass cls = MessageClass::Response);

    // Wake the poll loop from a worker thread (e.g., after job completion).
    // The callback will be invoked on the poll thread.
    void post(std::function<void()> fn);

    int listen_fd() const { return listen_fd_; }

    // Phase A.6 session-state accessors. Handlers that need to read or
    // write the per-client session slot use these instead of reaching
    // into the private client map directly. All three are O(1) via the
    // `client_id → fd` reverse map.
    //
    // `get_session`: copies the current credentials + preferences into
    // the caller-provided outputs. Returns `false` (and leaves outputs
    // untouched) when `client_id` is unknown (disconnected, or a fresh
    // connection that has not yet called `session.init`). The daemon
    // calls this on the rec_worker thread at enqueue time; the lookup
    // itself is single-threaded because `clients_` is owned by the poll
    // thread and the daemon's rec_worker only runs after the poll thread
    // has dispatched `record.start`, but writes to the slot happen on the
    // poll thread, so callers that read off-thread accept "best-effort,
    // last-write-wins" semantics.
    //
    // `set_session_credentials` / `set_session_preferences`: wholesale
    // replacement of the slot's credentials / preferences. Handlers
    // implement the partial-update contract by first calling
    // `get_session()` to snapshot the existing slot, overlaying only the
    // fields actually present in the incoming JSON params, then calling
    // these setters with the merged result. Booleans and integers have
    // no "unset" sentinel at the struct level, so the merge logic lives
    // at the handler layer where the raw JSON keys are visible. Returns
    // `false` when `client_id` is unknown.
    bool get_session(const std::string& client_id,
                     SessionCredentials& out_creds,
                     SessionPreferences& out_prefs) const;
    bool set_session_credentials(const std::string& client_id,
                                 const SessionCredentials& creds);
    bool set_session_preferences(const std::string& client_id,
                                 const SessionPreferences& prefs);

    // Phase C.13 — resume_token resolver hook. Called from `handle_pending_psk`
    // immediately after the PSK check passes, with the `resume_token` field
    // the client supplied on `auth.token` (empty string when the client sent
    // no field). Returns `(client_id, resume_token_to_echo)`:
    //   - Resume path: provided token resolved → server-side `client_id` from
    //     the prior session, same token echoed back so the client knows to
    //     keep using it. No re-`mint_client_id()` call.
    //   - Fresh path: provided token empty or unknown/expired → resolver
    //     calls `mint_client_id()` (public below) + `g_sessions->mint(...)`
    //     and returns the fresh pair. Client overwrites its persisted token.
    // When the resolver is unset (tests, pre-C.13 callers) the legacy
    // fresh-mint-only path runs and `auth.ok` carries no `resume_token`
    // field (additive — L-2). Sole site that sees the raw resume_token in
    // ipc_server.cpp; everywhere else, only the public resolver result and
    // SessionManager's redacted log_prefix are observable.
    using ResumeTokenResolver = std::function<
        std::pair<std::string /*client_id*/, std::string /*resume_token*/>(
            const std::string& provided_resume_token)>;
    void set_resume_token_resolver(ResumeTokenResolver r);

    // Phase C.13 — exposed (was private at l.339) so the resume_token
    // resolver (which lives in daemon.cpp) can mint a fresh client_id on
    // the fresh-token path without a second mechanism. Stateful counter,
    // thread-safe only for poll-thread callers (which is the only caller
    // both today and post-C.13).
    std::string mint_client_id();

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
        // Phase C.1: inbound framing state machine. Replaces the raw
        // `read_buf` + `rbuf.find('\n')` scan — it owns the unconsumed
        // byte accumulator internally and yields fully-assembled `Frame`s
        // (NDJSON lines or binary payloads). Constructed at accept time
        // with the server's `max_binary_frame_bytes_` so a per-connection
        // FrameReader carries the binary-frame cap.
        FrameReader reader;
        AuthState   auth_state = AuthState::Authed;  // Unix default; TCP overrides
        std::string peer_addr;  // "host:port" for TCP, "unix" for Unix; for log lines

        // Phase A.4 server-issued identifier. Assigned at the moment auth
        // completes — for Unix clients at `accept_client()` (since they
        // bypass PSK), for TCP clients when `handle_pending_psk()` flips
        // `auth_state` to `Authed`. Format: `c-<counter>-<6 hex chars>`,
        // short and log-friendly. Indexes the reverse-map
        // `client_id_to_fd_` for O(1) routing in `send_to_client()`.
        std::string client_id;

        // Phase A.6 per-client session state. Populated by `session.init`
        // / `session.update_credentials` / `session.update_prefs`. Cleared
        // automatically when the ClientState entry is erased in
        // `remove_client()`. The daemon reads `creds` + `prefs` at
        // `enqueue_postprocess()` time via `IpcServer::get_session()` and
        // merges them into `PostprocessJob::cfg` before
        // `write_job_config()` writes the temp JSON the subprocess reads
        // — the subprocess never re-reads env or daemon.yaml.
        SessionCredentials creds;
        SessionPreferences prefs;

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

    // Phase C.10a: optional binary-frame handler (see BinaryFrameHandler).
    // Unset → binary frames are discarded with a debug trace (C.1 behavior).
    BinaryFrameHandler binary_frame_handler_;

    // Phase C.10a: optional client-disconnect handler (see
    // ClientDisconnectHandler). Unset → no-op.
    ClientDisconnectHandler client_disconnect_handler_;

    // Phase C.13: optional resume_token resolver (see ResumeTokenResolver).
    // Unset → handle_pending_psk falls back to the pre-C.13 mint_client_id-
    // only path and auth.ok carries no resume_token field.
    ResumeTokenResolver resume_resolver_;

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

    // Phase C.1 binary frame cap. Default `kDefaultMaxBinaryFrameBytes`
    // (16 MiB); will be configurable via `[ipc] max_upload_bytes` once
    // C.2 / C.10a land. Stamped into each ClientState's FrameReader at
    // accept time.
    size_t max_binary_frame_bytes_ = kDefaultMaxBinaryFrameBytes;

    // Phase A.3 connection cap. Default 16; configurable via
    // `[ipc] max_clients`. Connections past this are refused at accept
    // with a JSON `server_full` error frame and immediate close. The
    // listen backlog tracks `max_clients_ * 2` (see listen_backlog()).
    size_t max_clients_ = 16;

    // Phase A.5: protocol version stamped into outgoing `auth.ok` frames.
    // Defaults to the compile-time `IPC_PROTOCOL_VERSION`. Tests can
    // override via `set_protocol_version_for_test()` to drive mismatch
    // paths without crafting raw bytes; a negative value suppresses the
    // field entirely to simulate a pre-A.5 daemon.
    int protocol_version_ = IPC_PROTOCOL_VERSION;

    // Handle the first-frame PSK exchange for a TCP client. Returns true
    // when the connection should remain open after this line; false when
    // the caller should drop the client (rejected). Always consumes the
    // line regardless of outcome.
    bool handle_pending_psk(int fd, ClientState& cs, const std::string& line);
};

} // namespace recmeet
