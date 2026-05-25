// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.10a — Server-side streaming-caption session.
//
// Re-establishes the tray->daemon live-caption path under the v2 thin-client
// architecture. Before C.10a the daemon-side `CaptionEngine` was only ever
// constructed inside `run_recording()`'s live-capture branch — dead code,
// since Phase B made the tray submit exclusively via `record.start` with
// `reprocess_dir` set. C.10a swaps the `CaptionEngine` *producer side* from
// the (now tray-side) PipeWire callback to a network `0x03`-frame queue.
//
// One `StreamingSession` per active `process.stream` job. Lifecycle:
//
//   process.stream         -> StreamingSessionManager::create()
//                             - mints a crypto-random stream_token
//                             - acquires the JobQueue `streaming` slot
//                             - opens a disk-backed temp WAV (frame sink)
//                             - starts a CaptionEngine
//   0x03 audio frames      -> StreamingSessionManager::feed_audio()
//                             - routed by stream_token to the session
//                             - PCM appended to the temp WAV AND pushed
//                               into the CaptionEngine's SPSC ring buffer
//   caption events         <- CaptionEngine worker thread -> CaptionSink
//                             - the daemon broadcasts caption /
//                               caption.degraded IPC events
//   process.stream.cancel  -> StreamingSessionManager::cancel()
//   process.stream.commit  -> StreamingSessionManager::commit()
//                             (C.10b) — flush + close the temp WAV, reserve
//                             a Postprocess job_id, enqueue a postprocess Job
//                             whose `input.out_dir` / `cfg.reprocess_dir` point
//                             at the temp WAV's parent directory, release the
//                             streaming slot (mark the streaming job Done).
//                             The temp WAV is NOT unlinked — the postprocess
//                             subprocess will read it. The client monitors
//                             the new postprocess job_id via `progress.job` +
//                             `process.fetch`.
//   TCP drop mid-stream    -> StreamingSessionManager::on_client_disconnect()
//                             - marks the JobQueue job failed
//                             - discards the ASR session
//                             - unlinks the temp WAV
//                             - releases the streaming slot
//
// The `CaptionEngine`'s ASR internals are UNCHANGED — C.10a migrates only
// its input producer. The engine still owns its lock-free SPSC ring buffer
// and worker thread; this module is simply the new producer that pushes the
// network-delivered PCM into that ring via `_push_samples_for_test()`'s
// public sibling path (see streaming_session.cpp for the wiring rationale).
//
// Threading: `StreamingSessionManager` is guarded by one internal mutex.
// `feed_audio()` runs on the IPC poll thread (the binary-frame handler).
// `create()` / `cancel()` / `on_client_disconnect()` also run on the poll
// thread (IPC handlers + `remove_client()` are all poll-thread). The
// CaptionEngine's caption callbacks fire on the engine's *own* worker
// thread and are marshalled back onto the poll thread by the daemon's
// `server.post()` before any `broadcast()` — exactly as the legacy
// `run_recording()` caption path did.

#pragma once

#include "caption_engine.h"
#include "config.h"
#include "job_queue.h"
#include "util.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace recmeet {

// Forward declaration: StreamingSession holds a back-pointer to its owning
// manager (used by the CaptionEngine callbacks), and the manager is defined
// after StreamingSession in this header.
class StreamingSessionManager;
class MeetingIndex;

// ---------------------------------------------------------------------------
// process.stream request parameters
// ---------------------------------------------------------------------------

/// Parsed `process.stream` request shape. `speaker_hints` is reserved for v2
/// multi-server and is accepted-and-ignored in C.10a (no field here).
struct StreamRequest {
    std::string format = "s16le";   ///< only "s16le" supported in v1
    int32_t     sample_rate = 16000;
    int32_t     channels = 1;
    std::string context;            ///< meeting context (reserved for C.10b)
    std::string language = "en";    ///< English-only guard rejects others
    bool        captions_enabled = true;
    int         latency_budget_ms = 500;  ///< [200,2000], drives emit cadence
    /// Phase C.11 — client-minted UUID v4 identifying the meeting (see
    /// docs/V2-STRATEGY.md "Meeting identity"). Empty for v1-shaped
    /// clients. Validated at the wire boundary by the daemon. The
    /// streaming session carries this through to the postprocess Job
    /// `process.stream.commit` enqueues — commit itself does not need a
    /// separate meeting_id arg because the session already knows it.
    std::string meeting_id;
};

/// Default / bounds for `latency_budget_ms`.
inline constexpr int kStreamLatencyDefaultMs = 500;
inline constexpr int kStreamLatencyMinMs     = 200;
inline constexpr int kStreamLatencyMaxMs     = 2000;

// ---------------------------------------------------------------------------
// Caption-event sink
// ---------------------------------------------------------------------------

/// The manager invokes this on the CaptionEngine worker thread whenever a
/// session emits a caption result or a degraded signal. The daemon wires it
/// to marshal onto the poll thread and route a `caption` / `caption.degraded`
/// IPC event to the owning client only. `job_id` identifies the streaming
/// job (stamped into the event payload); `client_id` is the per-session
/// originator that the daemon uses for `send_to_client()` (C.3). The sink
/// carries `client_id` rather than re-looking-up through
/// `JobQueue::client_for_job(job_id)` — same result, one fewer lock acquired
/// on every caption emission.
struct StreamingCaptionSink {
    /// is_partial=true → mid-utterance hypothesis; false → endpointed final.
    std::function<void(int64_t job_id, const std::string& client_id,
                       const std::string& text, bool is_partial,
                       int64_t timestamp_ms)> on_caption;
    /// reason is a stable string ("buffer_overrun", "engine_error").
    std::function<void(int64_t job_id, const std::string& client_id,
                       const std::string& reason,
                       int64_t timestamp_ms)> on_degraded;
};

// ---------------------------------------------------------------------------
// StreamingSession — one active process.stream job
// ---------------------------------------------------------------------------

/// One streaming-caption session. Owns the CaptionEngine, the disk-backed
/// temp WAV, and the binding to a JobQueue `streaming`-slot job. Not copyable.
/// Created and destroyed exclusively through StreamingSessionManager — the
/// destructor closes the WAV, unlinks it, and stops the engine, so dropping
/// a session from the manager's map is a complete cleanup.
class StreamingSession {
public:
    ~StreamingSession();
    StreamingSession(const StreamingSession&) = delete;
    StreamingSession& operator=(const StreamingSession&) = delete;

    int64_t job_id() const { return job_id_; }
    const std::string& client_id() const { return client_id_; }
    const std::string& stream_token() const { return stream_token_; }
    const fs::path& wav_path() const { return wav_path_; }

    /// Bytes of PCM appended to the temp WAV so far. Test introspection.
    int64_t bytes_written() const { return bytes_written_; }

    /// A3.4 — test introspection. Returns whether the session has a live
    /// CaptionEngine instance. False when the manager was constructed with
    /// `captions_enabled_at_startup=false` (engine block short-circuited
    /// — see streaming_session.cpp), or when an engine that started up
    /// failed and was reset to nullptr. No new mutating API surface.
    bool has_engine() const { return engine_ != nullptr; }

private:
    friend class StreamingSessionManager;
    StreamingSession() = default;

    /// Back-pointer to the owning manager. Used by the CaptionEngine
    /// result/degraded callbacks (which receive `this` as their userdata)
    /// to reach the manager-level StreamingCaptionSink. Process-lifetime
    /// stable — the manager outlives every session it owns.
    StreamingSessionManager* mgr_ = nullptr;

    int64_t      job_id_ = 0;
    std::string  client_id_;
    std::string  stream_token_;
    int          latency_budget_ms_ = kStreamLatencyDefaultMs;

    fs::path     wav_path_;
    /// Phase C.11.4 — the meeting directory the WAV lives in. On the wired
    /// path this is `meetings_root/{ts}/` (real meeting dir from frame
    /// zero); on the legacy path (unwired manager) it's the WAV's parent
    /// (whatever temp dir was used). `commit()` reads this into the
    /// postprocess Job's `input.out_dir`.
    fs::path     meeting_dir_;
    /// Phase C.11.4 — derived timestamp matching the meeting dir name on
    /// the wired path. Empty on the legacy path. Drives the canonical
    /// audio + context filename naming.
    std::string  timestamp_;
    /// Open libsndfile handle (SFM_WRITE), or null. Held as `void*` so the
    /// header does not need to pull in <sndfile.h> — the .cpp casts to
    /// `SNDFILE*` at every use site. The disk-backed temp WAV is the frame
    /// sink: every `0x03` payload is sf_write_short()-appended to it.
    void*        wav_ = nullptr;
    int64_t      bytes_written_ = 0;

    std::unique_ptr<CaptionEngine> engine_;

    /// Phase C.10b — per-session config snapshot captured at create() time.
    /// Mirrors C.2's UploadSession::pp_cfg_snapshots_ pattern: the daemon
    /// freezes the live `Config` (which already folds in session.init
    /// preferences via session.update_* merges into g_config) so that a
    /// concurrent config.reload between `process.stream` and
    /// `process.stream.commit` cannot surprise the postprocess handoff.
    /// Used by `commit()` to populate `Job.cfg` for the new Postprocess
    /// job.
    JobConfig    pp_cfg_;

    /// Phase C.10b — the meeting context the client passed at
    /// `process.stream` time. Forwarded to the postprocess subprocess at
    /// commit via `Job.cfg.context_inline`, matching what
    /// process.submit does with its `context` parameter.
    std::string  context_inline_;

    /// Phase C.10b — true once `commit()` has rewritten `Job.cfg` and
    /// successfully enqueued a Postprocess job whose input is THIS
    /// session's temp WAV. When set:
    ///   * `~StreamingSession` must NOT unlink the WAV — it is the
    ///     postprocess subprocess's input, owned by the new Job's
    ///     `input.out_dir`.
    ///   * `teardown_locked()` likewise skips the unlink.
    /// We picked the bool-flag option (option ii in the C.10b plan) over
    /// option i (clear `wav_path_`) because tests and logs still want the
    /// committed path for assertions, and over option iii (restructure)
    /// because the smaller surface is safer this late in Phase C.
    bool         committed_ = false;

    /// Phase C.11 — meeting_id this stream belongs to (UUID v4 or empty for
    /// v1-shaped clients). Captured at create() from the StreamRequest,
    /// stamped on both the streaming Job (so job.list / job.status echoes
    /// it back) and the postprocess Job that `commit()` enqueues. Empty is
    /// the v1 fallback; non-empty is wire-validated before it reaches us.
    std::string  meeting_id_;
};

// ---------------------------------------------------------------------------
// StreamingSessionManager — owns the stream_token -> session map
// ---------------------------------------------------------------------------

/// Thread-safe registry of active streaming sessions. One per daemon. Holds
/// the `stream_token -> StreamingSession` map and routes inbound `0x03`
/// frames to the right session. The daemon constructs this once, wires the
/// JobQueue + caption sink, and calls `create()` / `feed_audio()` / `cancel()`
/// / `on_client_disconnect()` from the IPC poll thread.
class StreamingSessionManager {
public:
    /// `jobs` and `sink` must outlive the manager (the daemon owns both).
    /// `caption_model_dir` is the resolved sherpa streaming-zipformer dir
    /// passed to every session's CaptionEngine; empty is tolerated (the
    /// engine reports a clean error and the session still streams audio to
    /// disk so C.10b's batch fallback has the WAV).
    ///
    /// Phase C.11.4 — `meeting_index` and `meetings_root` together wire the
    /// convergence-principle dedup contract on the streaming path (see
    /// docs/V2-STRATEGY.md). When BOTH are non-null/non-empty, `create()`
    /// resolves a real `meetings_root/{ts}/` directory and opens the WAV
    /// directly there, so the streaming accumulator writes into the
    /// canonical meeting dir from frame zero (pattern 1 of the four flow
    /// patterns). On TCP drop mid-stream the partial WAV is preserved (not
    /// unlinked) when `meeting_id` is set — operator can later re-upload
    /// the full local copy via `process.submit` to overwrite the partial
    /// (pattern 2). When EITHER is absent (test fixtures), the manager
    /// falls back to the legacy temp-WAV-becomes-meeting-dir model. The
    /// pointer + path are stored, not copied — both must outlive the
    /// manager.
    /// A3.2 — `captions_enabled_at_startup` is the runtime-effective server
    /// captions capability, computed at daemon startup in daemon.cpp under
    /// `g_server_config_mu` (the operator's config-file intent AND'd with
    /// runtime model-presence + RECMEET_USE_SHERPA build). When `false`,
    /// `create()` short-circuits the engine-start block entirely — no
    /// CaptionEngine allocated, no engine_error emitted; audio still
    /// flows to the disk-backed temp WAV. The manager caches this value
    /// for the daemon-process lifetime; config changes require a daemon
    /// restart. Defaulted to `true` so existing test fixtures that pre-date
    /// A3.2 continue to compile + exercise the engine path; placed last so
    /// the bool slot does not interleave with the MeetingIndex* parameter.
    StreamingSessionManager(JobQueue& jobs,
                            const StreamingCaptionSink& sink,
                            std::string caption_model_dir,
                            MeetingIndex* meeting_index = nullptr,
                            fs::path meetings_root = {},
                            bool captions_enabled_at_startup = true);
    ~StreamingSessionManager();

    StreamingSessionManager(const StreamingSessionManager&) = delete;
    StreamingSessionManager& operator=(const StreamingSessionManager&) = delete;

    /// Result of a `create()` call. `ok=false` carries a human-readable
    /// `error` and an `IpcErrorCode`-compatible `code` for the handler.
    struct CreateResult {
        bool        ok = false;
        int         code = 0;          ///< IpcErrorCode value on failure
        std::string error;
        int64_t     job_id = 0;        ///< populated on success
        std::string stream_token;      ///< populated on success
    };

    /// Handle a `process.stream` request from `client_id`. Validates the
    /// request (English-only guard, latency-budget bounds), acquires the
    /// JobQueue `streaming` slot, opens a temp WAV, starts a CaptionEngine,
    /// mints a crypto-random `stream_token`, and registers the session.
    /// On any failure nothing is left allocated — the slot is released, the
    /// WAV unlinked. `temp_dir` is where the staging WAV is created
    /// (defaults to the system temp dir when empty).
    ///
    /// Phase C.10b — `pp_cfg` is the per-client postprocess config snapshot
    /// the daemon captures at `process.stream` time (mirrors C.2's
    /// `process.submit` snapshot). The session freezes it for use at
    /// `commit()` time, so the postprocess Job built then reflects the
    /// preferences live at start-of-stream rather than a stale or
    /// concurrently-reloaded daemon Config. Tests can pass a default Config{}.
    CreateResult create(const std::string& client_id,
                        const StreamRequest& req,
                        const fs::path& temp_dir = {},
                        const JobConfig& pp_cfg = JobConfig{});

    /// Route a `0x03` streaming-audio payload (raw S16LE PCM bytes) to the
    /// session identified by `stream_token`. Appends the PCM to the
    /// session's temp WAV and pushes it into the CaptionEngine ring buffer.
    /// Returns false when `stream_token` is unknown or the payload is
    /// malformed (odd byte count) — the caller (binary-frame handler)
    /// treats false as a protocol violation and closes the connection.
    bool feed_audio(const std::string& stream_token, const std::string& pcm);

    /// The `stream_token` of the session owned by `client_id`, or empty if
    /// the client has no live streaming session. The C.1 `0x03` wire frame
    /// carries no token field (it is `<len><PCM>` only), so the daemon's
    /// binary-frame handler routes by `client_id` — the capacity-1 streaming
    /// slot guarantees a client owns at most one session. The crypto-random
    /// `stream_token` is still the map key and the anti-forgery handle for
    /// the `process.stream.cancel` path.
    std::string token_for_client(const std::string& client_id) const;

    /// Handle `process.stream.cancel`. Discards the session keyed by
    /// `stream_token`: marks the JobQueue job Cancelled, stops the engine,
    /// closes + unlinks the temp WAV, releases the streaming slot. Returns
    /// false when the token is unknown OR when it belongs to a different
    /// client than `client_id` (a client may only cancel its own stream).
    bool cancel(const std::string& client_id, const std::string& stream_token);

    /// Phase C.5 — `process.cancel` adapter. Locates the session by
    /// `job_id` (rather than by wire-facing `stream_token`) and runs the
    /// same teardown path as `cancel()`: stops the engine, unlinks the
    /// temp WAV, marks the JobQueue job Cancelled, releases the streaming
    /// slot. Ownership is NOT checked here — the caller (the
    /// `process.cancel` handler) is responsible for the ownership check
    /// via `JobQueue::client_for_job(job_id)`. Returns false when no
    /// active session is bound to `job_id` (e.g. the streaming job is
    /// already finalized or in a terminal state).
    bool cancel_by_job_id(int64_t job_id);

    /// Phase C.13 (M-2) — return the on-disk WAV path of the live
    /// streaming session bound to `job_id`, or empty when no live
    /// session is bound. Used by the GC orphan-eviction path to archive
    /// the in-flight WAV to `~/meetings/.orphan-<prefix>-<ts>/` BEFORE
    /// calling `cancel_by_job_id` (which unlinks the WAV). Lookup is
    /// path-only and does not mutate session state.
    fs::path wav_path_for_job(int64_t job_id) const;

    /// Phase C.10b — `process.stream.commit` result. `ok=false` carries a
    /// human-readable `error` and an `IpcErrorCode`-compatible `code` for
    /// the handler. On success, `postprocess_job_id` is a fresh
    /// JobQueue::Postprocess job_id (different from the streaming job_id)
    /// the client uses to monitor the postprocess pipeline via
    /// `progress.job` + `process.fetch`.
    struct CommitResult {
        bool        ok = false;
        int         code = 0;              ///< IpcErrorCode value on failure
        std::string error;
        int64_t     postprocess_job_id = 0;
    };

    /// Phase C.10b — finalize the streaming session referenced by
    /// `stream_token` and hand the accumulated audio off to a normal
    /// postprocess job. Validation:
    ///   * unknown `stream_token`               → InvalidParams.
    ///   * session not owned by `client_id`     → PermissionDenied.
    ///   * session already committed / torn down → InvalidParams
    ///     (defensive — `commit()` erases the session from the map at the
    ///     end of its critical section, so a second commit hits the
    ///     "unknown stream_token" path. This branch covers a hypothetical
    ///     post-erase residue and matches the plan's "not in a
    ///     committable state" guard).
    /// Action (under the manager's mutex, in this order):
    ///   a. Stop the CaptionEngine (flush + join its worker thread). The
    ///      engine's final partial caption tokens fire through the existing
    ///      sink path on the way out.
    ///   b. sf_close() the temp WAV — header rewritten with the final
    ///      data-chunk size, file is now a valid WAV.
    ///   c. Do NOT unlink the temp WAV — the postprocess subprocess is
    ///      about to read it. The session's `committed_` flag is set so
    ///      `~StreamingSession` skips the unlink too.
    ///   d. Reserve a Postprocess job_id via
    ///      `JobQueue::reserve_job_id(Postprocess, client_id)`.
    ///   e. Build a `Job` with `input.out_dir` / `cfg.reprocess_dir` set
    ///      to the temp WAV's parent directory, `cfg` copied from the
    ///      session's preference snapshot (the daemon's pp_worker writes
    ///      the cfg JSON the subprocess consumes — same shape as C.2's
    ///      process.submit finalize).
    ///   f. `JobQueue::enqueue_reserved(reserved_id, std::move(job))` so
    ///      the job becomes runnable.
    ///   g. Call `JobQueue::finish(stream_job_id, /*ok=*/true)` to release
    ///      the streaming slot (the streaming-slot job lands as Done).
    ///   h. Erase the session from the map.
    /// Returns a `CommitResult` whose `postprocess_job_id` is the new
    /// Postprocess job's id; the client monitors it via `progress.job` +
    /// `process.fetch`. The streaming-side `job_id` is not returned —
    /// it is in terminal Done state and not useful afterwards.
    CommitResult commit(const std::string& client_id,
                        const std::string& stream_token);

    /// Handle a client disconnect. Aborts every session owned by
    /// `client_id`: marks each JobQueue job Failed, stops the engine,
    /// unlinks the temp WAV, releases the slot. Returns the number of
    /// sessions aborted (0 is the common case — most clients have none).
    int on_client_disconnect(const std::string& client_id);

    /// Number of active sessions. Test introspection.
    size_t active_count() const;

    /// Look up the job_id bound to a stream_token, or -1 if unknown.
    /// Test introspection.
    int64_t job_id_for_token(const std::string& stream_token) const;

    /// A3.4 — test introspection: return the live `StreamingSession*` bound
    /// to `stream_token`, or nullptr if unknown. Lifetime is the session's
    /// lifetime in the manager's map — callers must not retain the pointer
    /// past a `cancel()` / `commit()` / `on_client_disconnect()` call.
    /// Used by T5 to verify the engine-startup short-circuit produced a
    /// session with `has_engine() == false`.
    StreamingSession* session_for_token(const std::string& stream_token) const;

private:
    /// Called with `mu_` held. Tears down `*sess`: stops the engine, closes
    /// + unlinks the temp WAV. Does NOT touch the JobQueue or the map — the
    /// caller decides the job verdict (Cancelled vs Failed) and erases the
    /// map entry. Centralizes the resource-release order so cancel() /
    /// disconnect() / a failed create() cannot drift.
    void teardown_locked(StreamingSession* sess);

    /// Phase C.5 — shared body of `cancel()` and `cancel_by_job_id()`.
    /// Called with `mu_` held. The iterator `it` must reference a live
    /// session in `sessions_`. Runs the full cancel-teardown sequence
    /// (teardown + JobQueue::cancel + JobQueue::finish + erase) and
    /// returns the JobQueue::cancel return for logging. After this call
    /// `it` is invalidated.
    void cancel_session_locked(
        std::map<std::string,
                 std::unique_ptr<StreamingSession>>::iterator it);

    mutable std::mutex mu_;
    JobQueue&          jobs_;
    StreamingCaptionSink sink_;
    std::string        caption_model_dir_;
    /// A3.2 — runtime-effective server captions capability cached at ctor
    /// time. See ctor doc for the AND-in derivation. `create()` reads this
    /// to short-circuit the engine-start block when the server can't run
    /// captions. Const-ish: written exactly once by the ctor, never
    /// mutated; readers do not need a lock.
    bool               captions_runtime_enabled_ = true;

    /// Phase C.11.4 — dedup contract dependencies. Null/empty when the
    /// manager is constructed for tests that don't exercise the
    /// meeting-dir resolution path; non-null/non-empty when the daemon
    /// wires the convergence-principle path. See the ctor doc.
    MeetingIndex*      meeting_index_ = nullptr;
    fs::path           meetings_root_;

    /// stream_token -> session. The token is the wire-facing routing key;
    /// it is crypto-random (see mint_stream_token in the .cpp) so a
    /// misbehaving client cannot forge another client's token. The map
    /// owns the sessions; erasing an entry runs ~StreamingSession.
    std::map<std::string, std::unique_ptr<StreamingSession>> sessions_;
};

} // namespace recmeet
