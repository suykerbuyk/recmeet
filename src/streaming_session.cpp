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
#include "pipeline.h"     // resolve_caption_model_dir (not used directly here;
                          //   the daemon passes the resolved dir in)

#include <sndfile.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
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
    unlink_quiet(wav_path_);
}

// ===========================================================================
// StreamingSessionManager
// ===========================================================================

StreamingSessionManager::StreamingSessionManager(JobQueue& jobs,
                                                 const StreamingCaptionSink& sink,
                                                 std::string caption_model_dir)
    : jobs_(jobs), sink_(sink),
      caption_model_dir_(std::move(caption_model_dir)) {}

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
    unlink_quiet(sess->wav_path_);
    sess->wav_path_.clear();
}

StreamingSessionManager::CreateResult
StreamingSessionManager::create(const std::string& client_id,
                                const StreamRequest& req,
                                const fs::path& temp_dir) {
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
    int64_t job_id = jobs_.enqueue(std::move(job), JobKind::Streaming, client_id);

    // --- Open the disk-backed temp WAV (the frame sink).
    fs::path dir = temp_dir.empty() ? fs::temp_directory_path() : temp_dir;
    std::error_code ec;
    fs::create_directories(dir, ec);  // best-effort; sf_open reports a hard fail
    fs::path wav_path = dir / ("recmeet-stream-" + std::to_string(job_id)
                               + "-" + mint_stream_token().substr(0, 8) + ".wav");

    SF_INFO info = {};
    info.samplerate = req.sample_rate;
    info.channels = req.channels;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* wav = sf_open(wav_path.string().c_str(), SFM_WRITE, &info);
    if (!wav) {
        // Roll back the JobQueue slot — the job never really started.
        jobs_.finish(job_id, /*ok=*/false,
                     "temp WAV open failed");
        res.code = static_cast<int>(IpcErrorCode::InternalError);
        res.error = std::string("process.stream: could not open temp WAV: ")
                  + sf_strerror(nullptr);
        return res;
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

    int64_t job_id = sess->job_id_;
    log_info("[streaming] cancel: job=%ld client=%s token=%s",
             (long)job_id, client_id.c_str(), stream_token.c_str());

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
    teardown_locked(sess);
    jobs_.cancel(job_id);
    jobs_.finish(job_id, /*ok=*/false, "stream cancelled by client");
    sessions_.erase(it);
    return true;
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
