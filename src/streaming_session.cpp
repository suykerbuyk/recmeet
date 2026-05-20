// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.10a — Server-side streaming-caption session implementation.
//
// See streaming_session.h for the design narrative. The two non-obvious
// implementation choices, spelled out here because the architecture review
// asks for them:
//
// 1. CaptionEngine producer migration.
//    The legacy daemon caption path wired the engine's producer side via
//    `capture.set_audio_callback(&CaptionEngine::on_audio_chunk, engine)` —
//    a PipeWire Capture object pushed PCM into the engine's lock-free SPSC
//    ring. C.10a has no Capture object on the daemon side: the audio
//    arrives as `0x03` network frames on the IPC poll thread. So instead of
//    a Capture-driven producer we call `CaptionEngine::on_audio_chunk(...)`
//    DIRECTLY from `feed_audio()`. `on_audio_chunk` is the engine's public,
//    address-stable producer entry point (it is `static` precisely so its
//    address can be handed to `set_audio_callback`); calling it directly is
//    the documented "compatible with AudioChunkCallback" contract. The ring
//    buffer and the ASR worker thread are untouched — we replaced the
//    *producer*, not the ring. The SPSC contract still holds: `feed_audio()`
//    is the single producer (poll thread only), the engine's worker is the
//    single consumer.
//
// 2. Disk-backed temp WAV.
//    Each `0x03` payload is `sf_write_short()`-appended to an open
//    libsndfile handle (SFM_WRITE). libsndfile keeps the WAV header's
//    data-chunk size live and rewrites it on `sf_close()`, so the on-disk
//    file is always a valid (if header-stale) WAV and the final close
//    produces a correct one. In-memory buffering is explicitly ruled out:
//    4 hours of 16 kHz mono int16 is ~460 MB, unsafe against the project's
//    16 GB ceiling under concurrency. The temp WAV is both the frame sink
//    AND (in C.10b, not C.10a) the commit source for the postprocess
//    handoff — structurally the same as B.5's reprocess_dir staging.

#include "streaming_session.h"

#include "cli.h"          // caption_language_supported
#include "ipc_protocol.h" // IpcErrorCode
#include "log.h"
#include "meeting_index.h"
#include "pipeline.h"     // save_meeting_context, resolve_caption_model_dir

#include <sndfile.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <optional>
#include <random>
#include <system_error>
#include <vector>

namespace recmeet {

namespace {

// ---------------------------------------------------------------------------
// stream_token entropy
// ---------------------------------------------------------------------------
//
// The token is the wire-facing routing key for `0x03` frames: a client sends
// `process.stream`, gets a `stream_token` back, then tags its audio stream
// with it. It MUST NOT be guessable — a misbehaving (or hostile) client must
// not be able to forge another client's token and inject audio into its
// session. `mint_client_id()` in ipc_server.cpp is explicitly documented as
// non-cryptographic (rand()-seeded hex, "a log-friendly tag, not a security
// primitive"), so we do NOT reuse it. Instead we draw 16 bytes from
// `std::random_device` — on Linux this is /dev/urandom-backed — and hex-encode
// them to a 32-char token. 128 bits of entropy makes the token space
// unsearchable.
std::string mint_stream_token() {
    std::random_device rd;
    std::array<uint32_t, 4> words{};
    for (auto& w : words) w = rd();
    static const char* hex = "0123456789abcdef";
    std::string tok;
    tok.reserve(32);
    for (uint32_t w : words) {
        for (int shift = 28; shift >= 0; shift -= 4)
            tok.push_back(hex[(w >> shift) & 0xF]);
    }
    return tok;
}

// Milliseconds since the steady clock epoch — the same monotonic basis the
// daemon's legacy caption path uses for `caption.degraded` timestamps.
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Unlink a file, swallowing ENOENT. Logs at warn on a real failure.
void unlink_quiet(const fs::path& p) {
    if (p.empty()) return;
    std::error_code ec;
    fs::remove(p, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        log_warn("[streaming] failed to unlink temp WAV %s: %s",
                 p.string().c_str(), ec.message().c_str());
    }
}

} // anonymous namespace

// ===========================================================================
// StreamingSession
// ===========================================================================

StreamingSession::~StreamingSession() {
    // Defensive teardown — StreamingSessionManager::teardown_locked() is the
    // normal path and will have already done this, but a session that is
    // dropped any other way (e.g. a create() that fails after the engine
    // started) must still release its resources cleanly.
    if (engine_) {
        engine_->stop();
        engine_.reset();
    }
    if (wav_) {
        sf_close(static_cast<SNDFILE*>(wav_));
        wav_ = nullptr;
    }
    // Phase C.10b — a committed session must NOT unlink its WAV: the file is
    // the postprocess subprocess's input and the new Postprocess Job's
    // `input.out_dir` directory owns it. The commit() path closes the WAV
    // BEFORE setting `committed_` (so libsndfile flushes the header) and
    // erases the session from the manager's map; this destructor still runs
    // for the unique_ptr's value, and the flag tells us to skip the unlink.
    //
    // Phase C.11.4 — a session with a non-empty `meeting_id_` that lands
    // here uncommitted (TCP drop mid-stream, or `cancel()`) still has its
    // WAV preserved: per the convergence-principle, the operator's local
    // copy can later overwrite the partial via `process.submit` (pattern
    // 2). For v1-shaped streams (empty meeting_id) the partial is
    // unreachable — no key to look it up by — so we keep the legacy
    // unlink. The owning manager's teardown_locked() applies the same
    // rule so both teardown paths agree.
    if (!committed_ && meeting_id_.empty()) {
        unlink_quiet(wav_path_);
    }
}

// ===========================================================================
// StreamingSessionManager
// ===========================================================================

StreamingSessionManager::StreamingSessionManager(JobQueue& jobs,
                                                 const StreamingCaptionSink& sink,
                                                 std::string caption_model_dir,
                                                 MeetingIndex* meeting_index,
                                                 fs::path meetings_root)
    : jobs_(jobs), sink_(sink),
      caption_model_dir_(std::move(caption_model_dir)),
      meeting_index_(meeting_index),
      meetings_root_(std::move(meetings_root)) {
    // C.11.4 — symmetry with UploadSessionManager: half-wired dedup is a
    // configuration bug, not a soft fallback. Log once and force the
    // legacy path.
    const bool have_idx  = (meeting_index_ != nullptr);
    const bool have_root = !meetings_root_.empty();
    if (have_idx != have_root) {
        log_warn("[streaming] manager constructed with partial dedup "
                 "wiring (meeting_index=%s, meetings_root=%s) — "
                 "convergence-principle dedup disabled; streams will use "
                 "legacy temp-WAV-becomes-meeting-dir",
                 have_idx ? "set" : "null",
                 have_root ? meetings_root_.string().c_str() : "empty");
        meeting_index_ = nullptr;
        meetings_root_.clear();
    }
}

StreamingSessionManager::~StreamingSessionManager() {
    // Abort every still-active session on daemon shutdown. The map owns the
    // sessions; clearing it runs ~StreamingSession on each, which stops the
    // engine and unlinks the WAV. We do NOT touch the JobQueue here — by the
    // time the manager is destroyed the daemon has already called
    // JobQueue::shutdown().
    std::lock_guard<std::mutex> lk(mu_);
    sessions_.clear();
}

void StreamingSessionManager::teardown_locked(StreamingSession* sess) {
    // Order matters: stop the engine FIRST so its worker thread has joined
    // and can never touch the (about-to-close) state, THEN close the WAV
    // handle, THEN unlink the file. Mirrors ActiveCaptionEngine's dtor
    // ordering in pipeline.cpp.
    if (sess->engine_) {
        sess->engine_->stop();
        sess->engine_.reset();
    }
    if (sess->wav_) {
        sf_close(static_cast<SNDFILE*>(sess->wav_));
        sess->wav_ = nullptr;
    }
    // Phase C.10b — `cancel()` / disconnect paths reach here with
    // `committed_ == false`, so they unlink as before. `commit()` does NOT
    // call teardown_locked — it has its own do-not-unlink finalize path —
    // but we keep this guard symmetric with the destructor's so a future
    // caller can't accidentally trip a double-teardown that would unlink a
    // committed WAV out from under the postprocess subprocess.
    //
    // Phase C.11.4 — symmetric with ~StreamingSession: a non-empty
    // meeting_id_ also suppresses the unlink so the partial WAV survives
    // a TCP drop / cancel and can be overwritten by a follow-up
    // process.submit (pattern 2). v1-shaped streams (empty meeting_id)
    // still unlink as before.
    if (!sess->committed_ && sess->meeting_id_.empty()) {
        unlink_quiet(sess->wav_path_);
        sess->wav_path_.clear();
    }
}

StreamingSessionManager::CreateResult
StreamingSessionManager::create(const std::string& client_id,
                                const StreamRequest& req,
                                const fs::path& temp_dir,
                                const JobConfig& pp_cfg) {
    CreateResult res;

    // --- Validate request shape (cheap checks first, before any allocation).

    // English-only guard — matches the existing CLI guard
    // (cli.cpp caption_language_supported, used by the V1 streaming-zipformer
    // English-only lock). Reject server-side rather than silently degrade so
    // a thin client gets an explicit error instead of a silent no-captions.
    if (!caption_language_supported(req.language)) {
        res.code = static_cast<int>(IpcErrorCode::InvalidParams);
        res.error = "process.stream: only English is supported in V1 "
                    "(got language=" + req.language + ")";
        return res;
    }

    // Format guard — V1 streams raw S16LE mono 16 kHz only.
    if (req.format != "s16le") {
        res.code = static_cast<int>(IpcErrorCode::InvalidParams);
        res.error = "process.stream: unsupported format '" + req.format +
                    "' (only s16le supported in V1)";
        return res;
    }
    if (req.channels != 1) {
        res.code = static_cast<int>(IpcErrorCode::InvalidParams);
        res.error = "process.stream: only mono (channels=1) supported in V1";
        return res;
    }
    if (req.sample_rate != 16000) {
        res.code = static_cast<int>(IpcErrorCode::InvalidParams);
        res.error = "process.stream: only sample_rate=16000 supported in V1";
        return res;
    }

    // latency_budget_ms — default 500, valid range [200, 2000]. Reject
    // out-of-range (the daemon's session.init handler rejects rather than
    // clamps caption_latency_ms, so we match that contract here).
    if (req.latency_budget_ms < kStreamLatencyMinMs ||
        req.latency_budget_ms > kStreamLatencyMaxMs) {
        res.code = static_cast<int>(IpcErrorCode::InvalidParams);
        res.error = "process.stream: latency_budget_ms must be in ["
                  + std::to_string(kStreamLatencyMinMs) + ", "
                  + std::to_string(kStreamLatencyMaxMs) + "]";
        return res;
    }

    std::lock_guard<std::mutex> lk(mu_);

    // --- Acquire the JobQueue `streaming` slot.
    //
    // The streaming slot is capacity-1 (C.7). We enqueue a JobKind::Streaming
    // job; since the streaming slot has no model-download dependency the job
    // is immediately runnable. We do NOT spin a worker thread that blocks in
    // `dequeue()` (the postprocess/model_download pattern) — a streaming job
    // has no subprocess; its "work" is the CaptionEngine's own worker thread
    // plus the poll-thread feed path. Instead the daemon's streaming worker
    // thread calls `dequeue(Streaming)` to flip the slot's running marker and
    // observe the job; see daemon.cpp. Here we only need the slot to be free.
    if (jobs_.slot_busy(JobKind::Streaming) ||
        jobs_.queued_count(JobKind::Streaming) > 0) {
        res.code = static_cast<int>(IpcErrorCode::Busy);
        res.error = "process.stream: streaming slot is busy "
                    "(another stream is active)";
        return res;
    }

    Job job;
    job.kind = JobKind::Streaming;
    // C.11 — carry meeting_id onto the streaming Job so job.list / job.status
    // echo it back to the client. Per-session storage below holds the same
    // value for the postprocess job that commit() eventually enqueues.
    job.meeting_id = req.meeting_id;
    int64_t job_id = jobs_.enqueue(std::move(job), JobKind::Streaming, client_id);

    // --- Resolve where the WAV will live.
    //
    // Phase C.11.4 — when the dedup contract is wired (meeting_index_ +
    // meetings_root_ both set), open the WAV directly inside the real
    // meeting directory under ~/meetings/. The streaming accumulator
    // writes there from frame zero, so commit() does not have to relocate
    // anything (pattern 1 of the convergence-principle flow patterns; per
    // V2-STRATEGY.md "the canonical path was the target from frame
    // zero"). On the unwired path (tests), fall back to the legacy temp
    // WAV in `temp_dir` / `fs::temp_directory_path()`.
    fs::path     wav_path;
    fs::path     resolved_meeting_dir;
    std::string  resolved_timestamp;
    bool         using_real_meeting_dir = false;
    std::error_code ec;
    if (meeting_index_ != nullptr && !meetings_root_.empty()) {
        // Resolve target meeting dir under the manager mutex (already
        // held). Find-or-allocate-and-bind, same dedup contract as the
        // upload finalize path — concurrent process.stream + process.submit
        // with the same meeting_id will both land in the same dir; either
        // the WAV is open (this branch) or the audio is being relocated
        // (upload branch), and the LATER write wins atomically.
        std::optional<fs::path> hit;
        if (!req.meeting_id.empty()) {
            hit = meeting_index_->find(req.meeting_id);
        }
        if (hit.has_value() && fs::is_directory(*hit, ec)) {
            resolved_meeting_dir = *hit;
            resolved_timestamp = derive_meeting_timestamp(*hit);
            log_info("[streaming] dedup: meeting_id=%s -> existing dir %s "
                     "(stream into existing meeting)",
                     req.meeting_id.c_str(),
                     resolved_meeting_dir.string().c_str());
        } else {
            if (hit.has_value()) {
                log_warn("[streaming] dedup: meeting_id=%s bound to vanished "
                         "dir %s — unbinding and allocating fresh",
                         req.meeting_id.c_str(), hit->string().c_str());
                meeting_index_->unbind(req.meeting_id);
            }
            std::error_code mkec;
            fs::create_directories(meetings_root_, mkec);
            try {
                OutputDir od = create_output_dir(meetings_root_);
                resolved_meeting_dir = od.path;
                resolved_timestamp = od.timestamp;
            } catch (const std::exception& e) {
                jobs_.finish(job_id, /*ok=*/false,
                             std::string("meeting dir allocation failed: ") +
                             e.what());
                res.code = static_cast<int>(IpcErrorCode::InternalError);
                res.error = std::string("process.stream: could not allocate "
                                        "meeting dir: ") + e.what();
                return res;
            }
            // Bind happens AFTER sf_open succeeds below so an sf_open
            // failure leaves the index clean (no entry pointing at a dir
            // we just removed). The window between create_output_dir
            // and sf_open is microseconds under the mutex; no concurrent
            // submit can race us during it.
        }
        std::string fname = resolved_timestamp.empty()
            ? std::string("audio.wav")
            : (std::string("audio_") + resolved_timestamp + ".wav");
        wav_path = resolved_meeting_dir / fname;
        using_real_meeting_dir = true;
    } else {
        fs::path dir = temp_dir.empty() ? fs::temp_directory_path() : temp_dir;
        fs::create_directories(dir, ec);  // best-effort; sf_open reports a hard fail
        wav_path = dir / ("recmeet-stream-" + std::to_string(job_id)
                          + "-" + mint_stream_token().substr(0, 8) + ".wav");
    }

    SF_INFO info = {};
    info.samplerate = req.sample_rate;
    info.channels = req.channels;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* wav = sf_open(wav_path.string().c_str(), SFM_WRITE, &info);
    if (!wav) {
        std::string sf_err = sf_strerror(nullptr);
        // C.11.4 — clean up the allocated meeting dir on the wired path
        // so a sf_open failure doesn't leave a stray empty dir under
        // ~/meetings/. We only do this when we just created the dir
        // (allocate path); the overwrite path (hit existing dir) must
        // leave it alone — it has operator data.
        if (using_real_meeting_dir && fs::exists(resolved_meeting_dir)) {
            std::error_code is_empty_ec;
            if (fs::is_empty(resolved_meeting_dir, is_empty_ec) &&
                !is_empty_ec) {
                std::error_code rm_ec;
                fs::remove(resolved_meeting_dir, rm_ec);
            }
        }
        // Roll back the JobQueue slot — the job never really started.
        jobs_.finish(job_id, /*ok=*/false,
                     "temp WAV open failed");
        res.code = static_cast<int>(IpcErrorCode::InternalError);
        res.error = std::string("process.stream: could not open temp WAV: ") +
                    sf_err;
        return res;
    }

    // C.11.4 (wired path) — WAV is open, bind the index entry + persist
    // context.json so a future daemon restart can repopulate the binding
    // via rebuild_from_disk. Both are no-ops on the legacy/unwired path.
    if (using_real_meeting_dir) {
        if (!req.meeting_id.empty()) {
            meeting_index_->bind(req.meeting_id, resolved_meeting_dir);
            log_info("[streaming] dedup: meeting_id=%s -> fresh dir %s "
                     "(allocate + bind)",
                     req.meeting_id.c_str(),
                     resolved_meeting_dir.string().c_str());
        } else {
            log_info("[streaming] dedup: v1-shaped stream (no meeting_id) "
                     "-> fresh dir %s (no index binding)",
                     resolved_meeting_dir.string().c_str());
        }
        try {
            save_meeting_context(resolved_meeting_dir, req.context,
                                 /*context_file=*/fs::path{},
                                 resolved_timestamp, req.meeting_id);
        } catch (const std::exception& e) {
            log_warn("[streaming] create: save_meeting_context(%s) failed: "
                     "%s — binding will be lost across daemon restart",
                     resolved_meeting_dir.string().c_str(), e.what());
        }
    }

    // --- Build the session object first so the CaptionEngine callbacks can
    //     take a stable `StreamingSession*` as their userdata. The session
    //     lives in `sessions_` for its whole life and is only erased after
    //     teardown_locked() has joined the engine worker, so the pointer is
    //     valid for every callback the engine will ever fire.
    std::string token = mint_stream_token();

    auto sess = std::unique_ptr<StreamingSession>(new StreamingSession());
    sess->mgr_ = this;
    sess->job_id_ = job_id;
    sess->client_id_ = client_id;
    sess->stream_token_ = token;
    sess->latency_budget_ms_ = req.latency_budget_ms;
    sess->wav_path_ = wav_path;
    sess->wav_ = wav;
    sess->bytes_written_ = 0;
    // Phase C.11.4 — capture the resolved meeting dir + timestamp so
    // commit() can build the postprocess Job from these instead of
    // re-deriving the parent path. On the unwired path
    // resolved_meeting_dir is empty; commit() falls back to
    // wav_path_.parent_path() so the legacy contract is preserved.
    if (using_real_meeting_dir) {
        sess->meeting_dir_ = resolved_meeting_dir;
        sess->timestamp_ = resolved_timestamp;
    }
    // Phase C.10b — freeze the per-client postprocess config snapshot the
    // daemon passed in. We do NOT clear `reprocess_dir` here: the daemon
    // already passes a snapshot with whatever reprocess context it wants;
    // commit() unconditionally overwrites it with the streaming temp WAV's
    // parent directory, mirroring C.2's process.submit finalize.
    sess->pp_cfg_ = pp_cfg;
    sess->context_inline_ = req.context;
    sess->meeting_id_ = req.meeting_id; // C.11 — propagated to commit's pp Job
    StreamingSession* sess_ptr = sess.get();

    // --- Start the CaptionEngine (the producer migrates to feed_audio()).
    auto engine = std::make_unique<CaptionEngine>();
    CaptionEngine::Options opts;
    opts.model_dir = caption_model_dir_;
    opts.num_threads = 1;
    opts.sample_rate = req.sample_rate;

    // The CaptionEngine result/degraded callbacks fire on the engine's own
    // worker thread. We forward them through the manager-level sink, which
    // the daemon wires to marshal onto the poll thread before broadcast().
    // Userdata is the StreamingSession* — no separate heap-allocated context
    // to leak; the session owns the engine and outlives the worker.
    bool started = engine->start(
        opts,
        // on_result
        +[](const CaptionResult& r, void* ud) {
            auto* s = static_cast<StreamingSession*>(ud);
            if (s->mgr_->sink_.on_caption)
                s->mgr_->sink_.on_caption(s->job_id_, s->client_id_,
                                          r.text, r.is_partial,
                                          r.timestamp_ms);
        },
        sess_ptr,
        // on_degraded
        +[](CaptionDegradedReason reason, void* ud) {
            auto* s = static_cast<StreamingSession*>(ud);
            const char* reason_str =
                (reason == CaptionDegradedReason::BufferOverrun)
                    ? "buffer_overrun" : "unknown";
            if (s->mgr_->sink_.on_degraded)
                s->mgr_->sink_.on_degraded(s->job_id_, s->client_id_,
                                           reason_str, now_ms());
        },
        sess_ptr);

    if (!started) {
        // Engine failed to start (no-sherpa stub build, missing model, …).
        // C.10a contract: this is NOT fatal to the stream — the audio still
        // flows to the disk-backed WAV so C.10b's batch path has it. We log,
        // emit a one-shot degraded signal, and continue with engine_ == null.
        std::string err = engine->last_error();
        log_warn("[streaming] caption engine failed to start (%s) — "
                 "streaming audio to disk only (job=%ld)",
                 err.c_str(), (long)job_id);
        if (sink_.on_degraded)
            sink_.on_degraded(job_id, client_id, "engine_error", now_ms());
        engine.reset();      // null engine — feed_audio() skips the ring push
    }
    sess->engine_ = std::move(engine);

    // --- Register the session under its routing token.
    sessions_.emplace(token, std::move(sess));

    log_info("[streaming] session created: job=%ld client=%s token=%s "
             "wav=%s latency_budget_ms=%d",
             (long)job_id, client_id.c_str(), token.c_str(),
             wav_path.string().c_str(), req.latency_budget_ms);

    res.ok = true;
    res.job_id = job_id;
    res.stream_token = token;
    return res;
}

bool StreamingSessionManager::feed_audio(const std::string& stream_token,
                                         const std::string& pcm) {
    // A `0x03` payload is raw S16LE PCM — the byte count must be even.
    if (pcm.size() % sizeof(int16_t) != 0) {
        log_warn("[streaming] feed_audio: odd-length PCM payload (%zu bytes) "
                 "for token=%s — protocol violation",
                 pcm.size(), stream_token.c_str());
        return false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(stream_token);
    if (it == sessions_.end()) {
        // Unknown token: a `0x03` frame arrived with no live session. The
        // caller treats false as a protocol violation and closes the fd —
        // there is no legitimate way to send streaming audio without a
        // prior `process.stream` that handed back this exact token.
        log_warn("[streaming] feed_audio: unknown stream_token=%s "
                 "(no live session)", stream_token.c_str());
        return false;
    }
    StreamingSession* sess = it->second.get();

    const size_t n_samples = pcm.size() / sizeof(int16_t);
    if (n_samples == 0) return true;  // zero-length frame is legal & a no-op

    const int16_t* samples = reinterpret_cast<const int16_t*>(pcm.data());

    // 1. Disk-backed buffer: append to the temp WAV. Negligible RAM — the
    //    libsndfile handle streams straight to disk.
    if (sess->wav_) {
        sf_count_t wrote = sf_write_short(static_cast<SNDFILE*>(sess->wav_),
                                          samples,
                                          static_cast<sf_count_t>(n_samples));
        if (wrote != static_cast<sf_count_t>(n_samples)) {
            log_warn("[streaming] feed_audio: short WAV write "
                     "(%lld/%zu samples) for job=%ld",
                     (long long)wrote, n_samples, (long)sess->job_id_);
            // A short write is an I/O problem (disk full?). We do NOT abort
            // the stream here — captions are still useful — but we stop
            // accounting bytes we did not actually persist.
        }
        sess->bytes_written_ +=
            static_cast<int64_t>(std::max<sf_count_t>(wrote, 0))
            * static_cast<int64_t>(sizeof(int16_t));
    }

    // 2. Producer side of the CaptionEngine: push the PCM into the engine's
    //    SPSC ring via the engine's public producer entry point. This is the
    //    migration described in the file header — `on_audio_chunk` replaces
    //    the PipeWire-Capture callback as the ring's producer. Lock-free,
    //    non-allocating inside the engine; the engine's worker thread drains
    //    it. A null engine (failed start / stub build) just skips this — the
    //    audio still went to disk above.
    if (sess->engine_)
        CaptionEngine::on_audio_chunk(samples, n_samples, sess->engine_.get());

    return true;
}

bool StreamingSessionManager::cancel(const std::string& client_id,
                                     const std::string& stream_token) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(stream_token);
    if (it == sessions_.end()) {
        log_warn("[streaming] cancel: unknown stream_token=%s",
                 stream_token.c_str());
        return false;
    }
    StreamingSession* sess = it->second.get();
    // A client may only cancel its own stream — the token is crypto-random
    // so this should never trip in practice, but enforce ownership anyway.
    if (sess->client_id_ != client_id) {
        log_warn("[streaming] cancel: token=%s owned by client=%s, "
                 "not requester=%s — refusing",
                 stream_token.c_str(), sess->client_id_.c_str(),
                 client_id.c_str());
        return false;
    }

    log_info("[streaming] cancel: job=%ld client=%s token=%s",
             (long)sess->job_id_, client_id.c_str(), stream_token.c_str());
    cancel_session_locked(it);
    return true;
}

fs::path StreamingSessionManager::wav_path_for_job(int64_t job_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [tok, sess] : sessions_) {
        if (sess->job_id_ == job_id) return sess->wav_path_;
    }
    return {};
}

bool StreamingSessionManager::cancel_by_job_id(int64_t job_id) {
    std::lock_guard<std::mutex> lk(mu_);
    // Phase C.5: `process.cancel` enters here with a job_id (the wire-facing
    // routing key is the stream_token, but the new general-cancel verb only
    // carries job_id). Locate the session by its bound job_id; the C.7
    // capacity-1 streaming slot makes the scan at most one entry deep in
    // practice, but we iterate defensively in case that invariant ever
    // changes. Ownership is NOT checked here — the `process.cancel` handler
    // performs the check via `JobQueue::client_for_job(job_id)` BEFORE
    // calling us, so by the time we run the requester has already been
    // authorized for this job. Returns false when no active streaming
    // session is bound to `job_id` (e.g. the job is already in a terminal
    // state, or the streaming session was finalized between status() and
    // this call — a benign race that the handler treats as "no work left").
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (it->second->job_id_ != job_id) continue;
        log_info("[streaming] cancel_by_job_id: job=%ld token=%s",
                 (long)job_id, it->first.c_str());
        cancel_session_locked(it);
        return true;
    }
    log_debug("[streaming] cancel_by_job_id: no live session for job=%ld",
              (long)job_id);
    return false;
}

StreamingSessionManager::CommitResult
StreamingSessionManager::commit(const std::string& client_id,
                                const std::string& stream_token) {
    CommitResult res;

    // Build the postprocess Job OUTSIDE the JobQueue calls but UNDER the
    // manager's mutex — same shape as cancel_session_locked: lock the map,
    // do all the local teardown + Job assembly, then run the JobQueue
    // mutations. `enqueue_reserved` takes its own JobQueue lock; we keep
    // the manager lock around it so a concurrent disconnect / cancel
    // cannot race the commit. The two locks form a strict order
    // (manager mutex → JobQueue mutex) in this direction only; no other
    // call path inverts it.
    int64_t reserved_pp_id = 0;
    int64_t stream_job_id = 0;
    fs::path wav_dir;
    Job pp_job;
    bool need_finish_stream = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(stream_token);
        if (it == sessions_.end()) {
            res.code = static_cast<int>(IpcErrorCode::InvalidParams);
            res.error = "process.stream.commit: unknown stream_token";
            return res;
        }
        StreamingSession* sess = it->second.get();
        if (sess->client_id_ != client_id) {
            res.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            res.error = "process.stream.commit: stream_token is not owned "
                        "by this client";
            return res;
        }
        // Committable-state guard. Today the only way to reach here with a
        // session that is mid-teardown is a race we cannot create (cancel /
        // disconnect both erase under mu_, and commit() likewise erases at
        // the end of its own critical section). The check is defensive
        // against future code paths that might leave a session in the map
        // post-teardown (e.g. a partial-failure recovery). `wav_ == null`
        // means the engine was torn down out from under us — refuse rather
        // than enqueue a Job that points at a closed / unlinked file.
        if (sess->committed_) {
            res.code = static_cast<int>(IpcErrorCode::InvalidParams);
            res.error = "process.stream.commit: session already committed";
            return res;
        }
        if (!sess->wav_) {
            // Engine path was set up but the disk-backed WAV is gone — the
            // streaming session has no buffered audio to hand off.
            res.code = static_cast<int>(IpcErrorCode::InvalidParams);
            res.error = "process.stream.commit: session has no buffered "
                        "audio (WAV not open)";
            return res;
        }

        stream_job_id = sess->job_id_;
        // Phase C.11.4 — when the manager was wired with a MeetingIndex +
        // meetings_root, `meeting_dir_` is the canonical ~/meetings/{ts}/
        // path we opened the WAV in at create() time. Use it as the
        // postprocess out_dir directly (pattern 1: "the canonical path
        // was the target from frame zero"). On the unwired / v1 path
        // meeting_dir_ is empty; fall back to the WAV's parent so the
        // legacy temp-WAV-becomes-meeting-dir contract is preserved.
        wav_dir = sess->meeting_dir_.empty()
            ? sess->wav_path_.parent_path()
            : sess->meeting_dir_;

        // (a) Stop the CaptionEngine — flush + join its worker thread. Any
        //     final partial-caption tokens fire through the existing sink
        //     path on the way out, before this returns. Mirrors the
        //     teardown_locked ordering: engine before WAV close.
        if (sess->engine_) {
            sess->engine_->stop();
            sess->engine_.reset();
        }

        // (b) Close the temp WAV. libsndfile rewrites the data-chunk size
        //     header on close, so the file on disk is a valid WAV after
        //     this returns. We do this BEFORE the JobQueue handoff so the
        //     postprocess subprocess opens a file with a correct header.
        sf_close(static_cast<SNDFILE*>(sess->wav_));
        sess->wav_ = nullptr;

        // (c) Mark the session committed so ~StreamingSession (and the
        //     defensive teardown_locked branch) skip the unlink. We set
        //     this BEFORE reserving the postprocess id so even an
        //     enqueue_reserved failure leaves the WAV intact for retry
        //     visibility — the alternative (unlink on failure) would
        //     silently delete the operator's recording.
        sess->committed_ = true;

        // (d) Reserve a Postprocess job_id. The reservation creates a
        //     registry entry in WaitingForUpload (the C.2 sentinel —
        //     conceptually analogous: "the job exists but is not yet
        //     runnable"); enqueue_reserved flips it to Queued.
        reserved_pp_id = jobs_.reserve_job_id(JobKind::Postprocess, client_id);

        // (e) Build the Job payload. `input.out_dir` and `cfg.reprocess_dir`
        //     both point at the temp WAV's parent directory — exactly the
        //     shape C.2's UploadSession::feed_chunk produces at finalize.
        //     The pp_worker_loop will set `cfg.reprocess_dir` from
        //     `input.out_dir` if not set, but we set it explicitly so the
        //     contract is visible at the seam.
        pp_job.kind = JobKind::Postprocess;
        pp_job.input.out_dir = wav_dir;
        pp_job.input.audio_path = sess->wav_path_;
        pp_job.cfg = sess->pp_cfg_;
        pp_job.cfg.reprocess_dir = wav_dir.string();
        if (!sess->context_inline_.empty())
            pp_job.cfg.context_inline = sess->context_inline_;
        // C.11 — carry the meeting_id from the streaming session onto the
        // postprocess Job so the commit-spawned job is reconcilable by the
        // same content key as the original process.stream request.
        pp_job.meeting_id = sess->meeting_id_;

        // (f) Place the populated job into the postprocess FIFO. If the
        //     reservation was cancelled meanwhile (e.g. process.cancel
        //     hit the reserved id between (d) and (f) — vanishingly rare
        //     because we hold the manager mutex, but cancel() does not
        //     touch this manager, only JobQueue), enqueue_reserved
        //     returns false; we surface that as a clean error and the
        //     caller learns the commit did not stick.
        bool placed = jobs_.enqueue_reserved(reserved_pp_id, std::move(pp_job));
        if (!placed) {
            log_warn("[streaming] commit: reservation pp_job=%ld no longer "
                     "WaitingForUpload — stream commit failed",
                     (long)reserved_pp_id);
            // Best-effort: surface an internal error and let the next
            // commit attempt / cancel pick up. We do NOT roll back the
            // streaming slot's `running` marker here — see step (g).
            res.code = static_cast<int>(IpcErrorCode::InternalError);
            res.error = "process.stream.commit: postprocess reservation "
                        "was cancelled before placement";
            // Still finish the streaming job + erase the session so the
            // streaming slot frees up; the temp WAV stays around (we set
            // committed_ above) but is orphaned. The orphan is harmless —
            // the disk-temp directory is best-effort anyway.
            need_finish_stream = true;
            // fall through to the slot-release + erase block.
        } else {
            need_finish_stream = true;
            log_info("[streaming] commit: stream_job=%ld -> postprocess_job=%ld "
                     "client=%s wav_dir=%s",
                     (long)stream_job_id, (long)reserved_pp_id,
                     client_id.c_str(), wav_dir.string().c_str());
            res.ok = true;
            res.postprocess_job_id = reserved_pp_id;
        }

        // (g) Release the streaming slot. We mark the streaming job Done
        //     (ok=true) — the stream completed normally; the postprocess
        //     job is a SEPARATE job, not a state transition on the
        //     streaming one. JobQueue::finish clears the slot's running
        //     marker so the next process.stream can open immediately.
        //     We deliberately call finish() AFTER enqueue_reserved so an
        //     enqueue failure does not advance state past a broken handoff.
        if (need_finish_stream) {
            jobs_.finish(stream_job_id, /*ok=*/true, "");
        }

        // (h) Erase the session from the map. The unique_ptr destructor
        //     runs ~StreamingSession; `committed_` is set so the unlink
        //     branch is skipped. wav_ is already null (closed in step b).
        sessions_.erase(it);
    } // release manager mutex

    return res;
}

void StreamingSessionManager::cancel_session_locked(
        std::map<std::string,
                 std::unique_ptr<StreamingSession>>::iterator it) {
    // Shared body of cancel() and cancel_by_job_id(). Caller holds `mu_`.
    // Release resources (engine stopped, WAV closed + unlinked), then settle
    // the JobQueue job. Two calls, in order:
    //   1. cancel()  — sets the job state to Cancelled (the authoritative
    //                  verdict; survives the finish() below).
    //   2. finish()  — clears the streaming slot's "running" marker so the
    //                  slot is free for the next process.stream. JobQueue's
    //                  finish() explicitly preserves a Cancelled verdict, so
    //                  the job stays Cancelled (not flipped to Failed).
    // C.7's cancel() alone does NOT clear the running marker for a Running
    // job — "the worker is expected to observe this and stop"; the streaming
    // model has no per-job worker, so the manager settles the slot itself.
    StreamingSession* sess = it->second.get();
    int64_t job_id = sess->job_id_;
    teardown_locked(sess);
    jobs_.cancel(job_id);
    jobs_.finish(job_id, /*ok=*/false, "stream cancelled by client");
    sessions_.erase(it);
}

int StreamingSessionManager::on_client_disconnect(const std::string& client_id) {
    std::lock_guard<std::mutex> lk(mu_);
    int aborted = 0;
    // A client owns at most one streaming session under the C.7 capacity-1
    // streaming slot, but iterate defensively in case that invariant ever
    // changes.
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second->client_id_ != client_id) {
            ++it;
            continue;
        }
        StreamingSession* sess = it->second.get();
        int64_t job_id = sess->job_id_;
        log_info("[streaming] client %s disconnected mid-stream — aborting "
                 "job=%ld token=%s",
                 client_id.c_str(), (long)job_id, it->first.c_str());
        // C.10a disconnect contract: mark the job FAILED (not Cancelled —
        // the client did not ask to stop), discard the ASR session, unlink
        // the temp WAV, release the slot. The batch fallback is C.10b — not
        // here. No mid-stream resume.
        teardown_locked(sess);
        jobs_.finish(job_id, /*ok=*/false, "client disconnected mid-stream");
        it = sessions_.erase(it);
        ++aborted;
    }
    return aborted;
}

std::string StreamingSessionManager::token_for_client(
        const std::string& client_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [token, sess] : sessions_) {
        if (sess->client_id_ == client_id) return token;
    }
    return {};
}

size_t StreamingSessionManager::active_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sessions_.size();
}

int64_t StreamingSessionManager::job_id_for_token(
        const std::string& stream_token) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(stream_token);
    return it == sessions_.end() ? -1 : it->second->job_id_;
}

} // namespace recmeet
