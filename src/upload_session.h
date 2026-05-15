// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.2 — Server-side upload session for `process.submit`.
//
// `process.submit` is the batch sibling of `process.stream` (C.10a): a thin
// client uploads a recorded audio file to the daemon for post-processing
// (transcribe + diarize + summarize). The audio arrives in `0x01` binary
// upload frames rather than `0x03` streaming frames; the work lands in the
// JobQueue postprocess slot (capacity-1) rather than the streaming slot.
//
// Lifecycle:
//
//   process.submit         -> UploadSessionManager::create()
//                             - mints a crypto-random `upload_token`
//                             - reserves a JobQueue postprocess job_id
//                               (the job is parked WaitingForUpload — not yet
//                                runnable; the slot stays free until finalize)
//                             - opens a per-job staging directory and the
//                               staging audio file (libsndfile for raw s16le,
//                               std::ofstream for self-describing containers)
//                             - returns {job_id, upload_token, max_size}
//   0x01 upload frames     -> UploadSessionManager::feed_chunk()
//                             - routed by client_id to the session
//                             - bytes appended to the staging file
//                             - emits a `progress.job` event with phase
//                               "uploading" via the JobQueue event sink
//                             - on bytes_received == audio_size: finalize ->
//                               close staging, call JobQueue::enqueue_reserved
//                               so pp_worker_loop can dequeue it
//   process.submit.cancel  -> UploadSessionManager::cancel()
//                             - tears down the staging file, marks the
//                               reservation Cancelled, releases the entry
//   TCP drop mid-upload    -> UploadSessionManager::on_client_disconnect()
//                             - aborts every in-flight upload for the client:
//                               unlinks the staging file, marks the
//                               reservation Failed (the client did not ask
//                               to stop)
//
// `upload_token` is the routing key in the manager's map (`upload_token ->
// UploadSession`). The wire `0x01` frame carries no token field (it is
// `<len><bytes>` only — see ipc_protocol.h), so the daemon's binary-frame
// handler routes by `client_id`: the capacity-1 postprocess-upload invariant
// means a client has at most one outstanding upload, and `token_for_client()`
// resolves the right session. The token is still the anti-forgery handle for
// `process.submit.cancel`. 128 bits of `std::random_device` entropy — same
// pattern as C.10a's `mint_stream_token` — so it cannot be guessed.
//
// The token does NOT survive reconnect. On a TCP drop the manager aborts the
// session entirely; a fresh `process.submit` after reconnect mints a new
// token and reserves a fresh job_id. C.2 is intentionally not designing a
// resume-after-disconnect path — Phase D will revisit upload retry.

#pragma once

#include "config.h"
#include "job_queue.h"
#include "util.h"

#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace recmeet {

// ---------------------------------------------------------------------------
// Upload-progress sink
// ---------------------------------------------------------------------------

/// The manager invokes this from `feed_chunk()` on the IPC poll thread (the
/// binary-frame handler) every time bytes are appended to the staging file.
/// The daemon wires it to broadcast a `progress.job` event with phase
/// `uploading` so the tray can render an upload progress bar. C.2 uses
/// `broadcast()` exactly like C.10a's caption events; C.3/C.10b convert to
/// `send_to_client()`.
struct UploadProgressSink {
    /// `bytes_received` is the running total persisted so far; `audio_size`
    /// is the total the client declared at `process.submit` time.
    std::function<void(int64_t job_id, const std::string& client_id,
                       int64_t bytes_received, int64_t audio_size)>
        on_progress;
};

// ---------------------------------------------------------------------------
// process.submit request parameters
// ---------------------------------------------------------------------------

/// Parsed `process.submit` request shape. `speaker_hints` is reserved for v2
/// multi-server and is accepted-and-ignored in C.2 (no field here).
struct SubmitRequest {
    int64_t     audio_size = 0;       ///< Total bytes the client will upload.
    std::string format = "s16le";     ///< s16le / f32le / wav / flac / mp3 / m4a / ogg
    int32_t     sample_rate = 16000;
    int32_t     channels = 1;
    std::string context;              ///< Meeting context forwarded to summarize.
    std::string mode = "transcribe";  ///< "transcribe" only in C.2; "enroll" is C.8.
};

/// Supported formats. Raw PCM formats are wrapped as WAV on the staging side
/// via libsndfile; container formats are written byte-for-byte to disk with
/// the matching extension (reprocess pipeline handles the rest).
inline constexpr const char* kSubmitFormatS16le = "s16le";
inline constexpr const char* kSubmitFormatF32le = "f32le";
inline constexpr const char* kSubmitFormatWav   = "wav";
inline constexpr const char* kSubmitFormatFlac  = "flac";
inline constexpr const char* kSubmitFormatMp3   = "mp3";
inline constexpr const char* kSubmitFormatM4a   = "m4a";
inline constexpr const char* kSubmitFormatOgg   = "ogg";

// ---------------------------------------------------------------------------
// UploadSession — one active `process.submit` upload
// ---------------------------------------------------------------------------

/// One upload-in-flight. Owns the staging directory, the staging audio file
/// (libsndfile handle for raw PCM; plain ofstream for containers), and the
/// JobQueue reservation. Not copyable. Lifetime is `UploadSessionManager`'s
/// map — dropping a session runs `~UploadSession`, which closes any open file
/// and removes the staging directory if no audio reached disk. The session
/// stays in the map past finalize too (until the postprocess job completes
/// and the worker cleans it up — but C.2 does the lighter alternative: the
/// session is erased from the map at finalize time, and pp_worker_loop owns
/// the staging directory thereafter).
class UploadSession {
public:
    ~UploadSession();
    UploadSession(const UploadSession&) = delete;
    UploadSession& operator=(const UploadSession&) = delete;

    int64_t job_id() const { return job_id_; }
    const std::string& client_id() const { return client_id_; }
    const std::string& upload_token() const { return upload_token_; }
    const fs::path& staging_dir() const { return staging_dir_; }
    const fs::path& staging_audio_path() const { return staging_audio_path_; }
    int64_t audio_size() const { return audio_size_; }
    int64_t bytes_received() const { return bytes_received_; }

private:
    friend class UploadSessionManager;
    UploadSession() = default;

    int64_t     job_id_ = 0;
    std::string client_id_;
    std::string upload_token_;
    SubmitRequest req_;
    int64_t     audio_size_ = 0;
    int64_t     bytes_received_ = 0;

    fs::path    staging_dir_;
    fs::path    staging_audio_path_;

    /// libsndfile SNDFILE* for raw PCM formats (held as void* so the header
    /// does not pull in <sndfile.h>; the .cpp casts at every use site).
    /// Null for container formats — they use `container_out_` instead.
    void*       wav_ = nullptr;

    /// Plain stream for container formats (wav/flac/mp3/m4a/ogg). Null for
    /// raw-PCM formats — they use `wav_` instead.
    std::unique_ptr<std::ofstream> container_out_;

    /// True once `feed_chunk` has crossed audio_size — the manager has
    /// committed the staging file to JobQueue via `enqueue_reserved`. Any
    /// further `0x01` bytes for the same client are treated as a protocol
    /// violation (the session is gone by then; see `feed_chunk`).
    bool        finalized_ = false;
};

// ---------------------------------------------------------------------------
// UploadSessionManager — owns the upload_token -> session map
// ---------------------------------------------------------------------------

/// Thread-safe registry of active uploads. One per daemon. Constructed once
/// after the JobQueue exists; wired to a staging-root path so each upload
/// gets its own subdirectory the postprocess pipeline can consume directly.
class UploadSessionManager {
public:
    /// `jobs` must outlive the manager (the daemon owns both). `staging_root`
    /// is the parent directory under which each upload gets its own per-job
    /// subdirectory (`recmeet-upload-<job_id>-<token8>/`). Empty falls back
    /// to the system temp dir at create()-time. `progress_sink` is invoked
    /// from `feed_chunk()` for the `uploading` phase; empty sink is fine
    /// (tests use a no-op).
    UploadSessionManager(JobQueue& jobs, fs::path staging_root,
                         UploadProgressSink progress_sink = {});
    ~UploadSessionManager();

    UploadSessionManager(const UploadSessionManager&) = delete;
    UploadSessionManager& operator=(const UploadSessionManager&) = delete;

    struct CreateResult {
        bool        ok = false;
        int         code = 0;          ///< IpcErrorCode value on failure.
        std::string error;
        int64_t     job_id = 0;
        std::string upload_token;
        int64_t     max_size = 0;      ///< daemon's configured upload cap.
    };

    /// Handle a `process.submit` request from `client_id`. Validates the
    /// request shape (format / mode / audio_size against `max_upload_bytes`),
    /// reserves a JobQueue postprocess job_id (WaitingForUpload), opens the
    /// staging file, mints a crypto-random `upload_token`, registers the
    /// session, and returns `{job_id, upload_token, max_size}`. On any
    /// failure nothing is left allocated — the reservation is cancelled, the
    /// staging directory removed.
    ///
    /// `max_upload_bytes` is the daemon-configured cap (`[server]
    /// max_upload_bytes`). Pass 0 to mean "no cap" — the cap is reported as
    /// INT64_MAX. `pp_cfg` is the per-client postprocess config snapshot the
    /// daemon will pass through to the eventual postprocess job (captured at
    /// submit time, not finalize time, so it reflects the session.init
    /// preferences that were live when the client submitted).
    CreateResult create(const std::string& client_id,
                        const SubmitRequest& req,
                        const Config& pp_cfg,
                        size_t max_upload_bytes);

    /// Route a `0x01` upload payload to the session owned by `client_id`.
    /// Appends bytes to the staging file. When `bytes_received` reaches
    /// `audio_size` the session is finalized: the staging file is closed,
    /// the postprocess job is finalized via `JobQueue::enqueue_reserved()`,
    /// and the session is erased from the manager's map. Emits a
    /// `progress.job` "uploading" event via the JobQueue event sink (the
    /// daemon's existing sink handles wire emission).
    ///
    /// Returns false on protocol violations:
    ///   * unknown client (no live upload)
    ///   * bytes_received + payload.size() would exceed audio_size
    ///   * the staging file write returned a short write
    /// The caller (binary-frame handler) treats false as a protocol
    /// violation and tears the connection down.
    bool feed_chunk(const std::string& client_id, const std::string& payload);

    /// The `upload_token` of the session owned by `client_id`, or empty if
    /// the client has no live upload. Mirrors C.10a's `token_for_client()` —
    /// the `0x01` wire frame carries no token field, so the binary-frame
    /// handler routes by `client_id`.
    std::string token_for_client(const std::string& client_id) const;

    /// Handle `process.submit.cancel`. Discards the session keyed by
    /// `upload_token`: closes + unlinks the staging file, marks the
    /// JobQueue reservation Cancelled. Returns false when the token is
    /// unknown OR belongs to a different client than `client_id`.
    bool cancel(const std::string& client_id, const std::string& upload_token);

    /// Phase C.5 — `process.cancel` adapter. Locates the upload session by
    /// `job_id` (rather than by wire-facing `upload_token`) and runs the
    /// same teardown path as `cancel()`: closes + unlinks the staging
    /// file, marks the JobQueue reservation Cancelled. Ownership is NOT
    /// checked here — the caller (the `process.cancel` handler) is
    /// responsible for the ownership check via
    /// `JobQueue::client_for_job(job_id)`. Returns false when no active
    /// upload session is bound to `job_id` (e.g. the upload finalized
    /// into a Queued postprocess job — the JobQueue::cancel() in the
    /// handler covers that path).
    bool cancel_by_job_id(int64_t job_id);

    /// Handle a client disconnect. Aborts every session owned by
    /// `client_id`: unlinks the staging file, marks the JobQueue
    /// reservation Failed (the client did not ask to stop). Returns the
    /// number of sessions aborted.
    int on_client_disconnect(const std::string& client_id);

    /// Number of active sessions. Test introspection.
    size_t active_count() const;

    /// Look up the job_id bound to an upload_token, or -1 if unknown.
    /// Test introspection.
    int64_t job_id_for_token(const std::string& upload_token) const;

    /// Look up bytes_received for an upload_token, or -1 if unknown.
    /// Test introspection — the wire round-trip tests assert progress.
    int64_t bytes_received_for_token(const std::string& upload_token) const;

private:
    /// Called with `mu_` held. Tears down `*sess`'s staging file/dir without
    /// touching the JobQueue or the map. Centralizes resource release order.
    void teardown_locked(UploadSession* sess);

    /// Phase C.5 — shared body of `cancel()` and `cancel_by_job_id()`.
    /// Called with `mu_` held. The iterator `it` must reference a live
    /// session in `sessions_`. Runs the full cancel-teardown sequence
    /// (teardown + JobQueue::cancel + erase + side-table cleanup).
    /// After this call `it` is invalidated.
    void cancel_session_locked(
        std::map<std::string,
                 std::unique_ptr<UploadSession>>::iterator it);

    mutable std::mutex mu_;
    JobQueue&          jobs_;
    fs::path           staging_root_;
    UploadProgressSink progress_sink_;

    /// upload_token -> session. Crypto-random key (anti-forgery for
    /// process.submit.cancel). The map owns sessions; erasing an entry runs
    /// `~UploadSession`.
    std::map<std::string, std::unique_ptr<UploadSession>> sessions_;

    /// Side tables keyed by job_id. The `create()` caller passes a `Config`
    /// snapshot (the per-client postprocess settings at submit time); we
    /// stash it here until finalize, where it lands on the `Job` payload
    /// that `enqueue_reserved` places in the FIFO. Same for the
    /// `context_inline` override the submit request carries (the daemon's
    /// pp_worker forwards it to the postprocess subprocess via the standard
    /// `cfg.context_inline` channel). Both are removed on finalize / cancel /
    /// disconnect so the side tables track sessions_ exactly.
    std::map<int64_t, Config>      pp_cfg_snapshots_;
    std::map<int64_t, std::string> pp_context_overrides_;
};

} // namespace recmeet
