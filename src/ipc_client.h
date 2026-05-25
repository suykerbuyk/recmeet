// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "ipc_protocol.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

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

    // Phase D.5 — connect with explicit PSK + optional resume_token. When
    // `resume_token` is non-empty the TCP connect sends it as
    // `auth.token.resume_token` so the server-side SessionManager
    // resolves the prior `client_id` and remints (or re-issues) the
    // token. The server-side handler at ipc_server.cpp:146-154 reads
    // this optional field; the daemon's auth.ok emission at
    // ipc_server.cpp:87-111 echoes the resolved token back so the client
    // can persist it via `ResumeTokenStore::put`.
    //
    // When `psk` is empty, the same environment-variable fallback as the
    // zero-arg `connect()` is used (RECMEET_AUTH_TOKEN) — this overload
    // is the canonical entry point for the tray's reconnect path where
    // the resume_token comes from `ResumeTokenStore::get(addr)`.
    //
    // Unix-socket clients ignore `psk` (no PSK gate on AF_UNIX) but DO
    // honor `resume_token` for symmetry (a future test seam that swaps
    // a Unix-socket server with C.13 wiring will exercise resume there
    // too).
    bool connect(const std::string& psk,
                 const std::string& resume_token = "");

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

    // Phase D.5 — the server-issued `resume_token` parsed from the
    // `auth.ok` frame (C.13). Returns the EMPTY STRING (NOT throws) when
    // the daemon omits the field — legacy / test-path daemons, and any
    // daemon whose SessionManager resolver hook returned an empty token
    // — per ipc_server.cpp:104-108 conditional emission and the D.5
    // architecture-review checklist item #6. Reset on
    // `close_connection()` to prevent a stale token from being persisted
    // after a disconnect.
    const std::string& resume_token() const { return resume_token_; }

    // Get the underlying fd (for integration with external event loops).
    int fd() const { return fd_; }

    // Phase A.6 session handshake helpers. Each is a thin wrapper around
    // `call()` that builds the request params shape required by the
    // daemon's `session.init` / `session.update_credentials` /
    // `session.update_prefs` handlers. `creds` and `prefs` are flat
    // `JsonMap`s — keys match the plan-body JSON example
    // (`provider`, `api_key`, `api_keys.<provider>`, `output_dir`,
    // `note_dir`, `language`, `vocabulary`, `mic_source`, `monitor_source`,
    // `whisper_model`, `summarization_backend`, `llm_model`,
    // `caption_latency_ms`).
    //
    // Phase C (rev 5) — `captions_enabled` retired from the prefs payload:
    // captions liveness is server-owned (announced via
    // session.init.captions_supported). Legacy clients still emitting the
    // key are silently tolerated by the daemon parser.
    //
    // `session_init` is intended for use after `connect()` succeeds and
    // before any `record.start`. The daemon clears the slot on
    // disconnect, so a reconnect requires a fresh `session_init`.
    bool session_init(const JsonMap& creds, const JsonMap& prefs,
                      IpcResponse& resp, IpcError& err,
                      int timeout_ms = 30000);
    bool session_update_credentials(const JsonMap& creds,
                                    IpcResponse& resp, IpcError& err,
                                    int timeout_ms = 30000);
    bool session_update_prefs(const JsonMap& prefs,
                              IpcResponse& resp, IpcError& err,
                              int timeout_ms = 30000);

    // Read data from socket, parse messages, dispatch events.
    // Returns false on disconnect.
    bool read_and_dispatch(int timeout_ms);

    // Phase C.10a: send a `0x03` streaming-audio frame carrying raw PCM
    // (S16LE host-endian — the daemon and tray are the same architecture
    // on the v1 single-host dogfood path). `pcm` is the payload bytes;
    // the helper prepends the `0x03` discriminator + 4-byte big-endian
    // length via `frame_binary()`. Returns false on a write error (the
    // connection is closed) or when not connected. This is a fire-and-
    // forget send — there is no per-frame response; the daemon emits
    // `caption` / `caption.degraded` events asynchronously.
    bool send_stream_audio(const std::string& pcm);

    // Phase C.10a: convenience overload taking int16 samples directly.
    bool send_stream_audio(const int16_t* samples, std::size_t n);

    // Phase C.2: send a `0x01` upload-chunk frame carrying raw bytes for a
    // `process.submit` upload session. `bytes` is the payload; the helper
    // prepends the `0x01` discriminator + 4-byte big-endian length via
    // `frame_binary()`. Same blocking-write loop as `send_stream_audio`:
    // returns false on a write error (the connection is closed) or when
    // not connected. Fire-and-forget — the daemon emits `progress.job`
    // (phase=`uploading`) and a final `progress.job` (phase=`done`)
    // asynchronously as the upload finalizes and the postprocess runs.
    // C.2 does NOT add client-side queueing/retry — Phase D's job.
    bool send_upload_chunk(const std::string& bytes);

    // Phase C.4 — fetch artifacts for a completed postprocess job.
    //
    // Sends a `process.fetch {job_id}` request and waits for the metadata
    // response. On success, the daemon then writes N `0x02` BinaryArtifact
    // frames in the same order as the response's `artifacts[]` array.
    // `fetch_artifacts` reads exactly N binary frames from the connection
    // (dispatching any interleaved NDJSON events to the registered
    // `event_callback` so events keep flowing during the fetch), writes
    // each artifact's bytes to `output_dir / artifact.name`, and returns
    // the list of absolute paths written.
    //
    // On any error — request rejected by the daemon, wrong frame type in
    // the binary phase, size mismatch, write error — leaves `err` populated
    // with the failure reason and returns an empty vector. `output_dir` is
    // created if missing; existing files of the same name are overwritten.
    //
    // The fetch is a synchronous request/response/binary exchange — the
    // caller waits for the full set of artifacts before this function
    // returns. `timeout_ms` bounds the metadata-response wait; the binary
    // phase uses the same budget for the remaining frames.
    std::vector<std::filesystem::path> fetch_artifacts(
            int64_t job_id,
            const std::filesystem::path& output_dir,
            IpcError& err,
            int timeout_ms = 30000);

private:
    void process_line(const std::string& line);
    bool connect_unix();
    bool connect_tcp();

    // Phase C.1: drain every complete frame the FrameReader can assemble,
    // dispatching NDJSON frames through `process_line()` and discarding
    // binary frames (no client-side consumer in C.1). Returns false on a
    // terminal framing error (unknown discriminator / oversized frame);
    // the caller then closes the connection.
    //
    // Phase C.4: when `capture_binary_out` is non-null, `0x02` BinaryArtifact
    // frames are captured into it (in order) instead of discarded — this is
    // the synchronous mode used by `fetch_artifacts()` to demultiplex the
    // exact-N-frame binary phase. Non-`0x02` binary frames (`0x01`/`0x03`)
    // remain discarded for the capture mode too — they are not expected on
    // a client receive stream.
    bool drain_frames(std::vector<std::string>* capture_binary_out = nullptr);

    // Phase C.4 — synchronous "expect N more `0x02` frames" pump. Drives
    // `read_and_dispatch` in capture mode until `expected` BinaryArtifact
    // frames have been collected into `out`, or the deadline expires, or a
    // protocol-level failure tears the connection down. NDJSON frames
    // arriving mid-binary phase are dispatched through `process_line()` so
    // event callbacks keep firing — only the binary frames are captured.
    // Returns true on success (exactly `expected` frames in `out`), false
    // on error (`fail_reason` populated). Used only by `fetch_artifacts`.
    bool pump_binary_frames(size_t expected,
                            std::vector<std::string>& out,
                            int timeout_ms,
                            std::string& fail_reason);

    // Phase A.6: send a request whose params are a pre-serialized JSON
    // object (e.g. `{"credentials":{...},"preferences":{...}}`). The
    // wire-level `IpcRequest::params` shape is a flat JsonMap on this
    // codebase; nested objects in the daemon-side parser are stored as
    // raw substrings. To send nested data the client bypasses
    // `serialize(IpcRequest)` and emits the frame literally. This helper
    // mirrors `call()`'s response wait + error handling so the public
    // entry points (`session_init` / `session_update_*`) stay one-liners.
    bool call_with_raw_params_json(const std::string& method,
                                   const std::string& params_json,
                                   IpcResponse& resp, IpcError& err,
                                   int timeout_ms);

    // Phase A.5: parse an auth.ok reply, capture client_id +
    // protocol_version, and enforce the version invariant. Returns true
    // when the reply is acceptable (matching version), false when the
    // version is absent or mismatched. Callers close the fd on a false
    // return; the helper only mutates per-client state.
    bool verify_auth_ok_and_capture(const std::string& reply);

    IpcAddress addr_;
    int fd_ = -1;
    int64_t next_id_ = 1;
    // Phase C.1: inbound framing state machine. Replaces the raw
    // `read_buf_` + `find('\n')` scan — see docs/IPC-WIRE-PROTOCOL.md.
    // The daemon prefixes every NDJSON message with a `0x00` discriminator;
    // FrameReader strips it and yields the JSON line. Reset on
    // `close_connection()` by reconstruction so a reconnect starts at a
    // clean frame boundary.
    FrameReader reader_;
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

    // Phase D.5: per-connection resume_token captured from the `auth.ok`
    // frame (C.13). Empty when the daemon did not emit the field
    // (legacy / test paths). Cleared on `close_connection()` and on the
    // start of every new `connect()` attempt so a failed handshake can
    // never leak a token from a prior connection. Read via
    // `resume_token()`.
    std::string resume_token_;

    // Phase D.5: resume_token the caller wants to present on the NEXT
    // connect. Set by the `connect(psk, resume_token)` overload and
    // consumed by `connect_tcp()` when constructing the `auth.token`
    // frame. Cleared after consumption so a subsequent zero-arg
    // `connect()` does not accidentally replay it.
    std::string pending_resume_token_;

    // Phase D.5: explicit PSK override (empty → use RECMEET_AUTH_TOKEN
    // env var, matching the zero-arg `connect()` behavior).
    std::string pending_psk_override_;
    bool pending_psk_set_ = false;

    // Phase C.4: per-connection FIFO of received `0x02` BinaryArtifact frame
    // payloads. The C.1 path discards binary frames on the client side
    // (no consumer); C.4 needs them buffered because `process.fetch` is a
    // request/response/binary exchange — the binary frames can race the
    // metadata response and arrive in the same read(). On every drain pass
    // (in `read_and_dispatch` / `drain_frames`) any received `0x02` frame
    // is appended here; `fetch_artifacts` drains this stash first and then
    // pumps more from the socket as needed. Cleared on `close_connection`.
    std::vector<std::string> stashed_artifact_frames_;

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
