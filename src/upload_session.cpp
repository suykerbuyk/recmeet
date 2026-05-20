// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.2 — Server-side upload session for `process.submit`.
//
// See upload_session.h for the design narrative. Two non-obvious implementation
// choices spelled out here because the architecture review asks for them:
//
// 1. Split-enqueue via JobQueue::reserve_job_id / enqueue_reserved.
//    `process.submit`'s response must include a `job_id`, but the actual
//    postprocess work cannot become runnable until the binary upload
//    finalizes — otherwise `pp_worker_loop` would dequeue a job whose
//    `input.out_dir` does not yet contain the audio. Three options were
//    considered (see the orchestrator's brief): pre-allocate via a new
//    JobQueue API; add a new JobState::WaitingForUpload that
//    `pick_runnable_locked` skips; or run a manager-private id counter and
//    re-id at finalize time. The third confuses the protocol's `job_id`;
//    the second is invasive (the slot's FIFO is supposed to be runnable).
//    The first — `reserve_job_id` + `enqueue_reserved` — keeps the slot
//    FIFO's invariant ("a job in the FIFO is dequeueable") intact and
//    threads the binding through the registry from the moment the response
//    is sent. The new `JobState::WaitingForUpload` is a SENTINEL state for
//    the registry-only reservation; the FIFO never contains it.
//
// 2. Per-job staging directory; libsndfile vs std::ofstream by format.
//    Each upload gets `{staging_root}/recmeet-upload-{job_id}-{tok8}/`,
//    inside which the staging audio sits as `audio.<ext>`. For raw PCM
//    (s16le / f32le) we wrap the bytes in a WAV header via libsndfile —
//    same `SFM_WRITE` pattern as C.10a's streaming session — so the
//    postprocess pipeline (which expects a self-describing audio file in
//    `reprocess_dir`) does not need a separate decoder for "loose PCM".
//    For self-describing containers (wav / flac / mp3 / m4a / ogg) we
//    write the bytes verbatim to disk via `std::ofstream`; the existing
//    reprocess machinery already handles those via libsndfile's auto-
//    detection.

#include "upload_session.h"

#include "ipc_protocol.h"   // IpcErrorCode
#include "log.h"
#include "meeting_index.h"
#include "pipeline.h"       // save_meeting_context

#include <sndfile.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <system_error>

namespace recmeet {

namespace {

// ---------------------------------------------------------------------------
// upload_token entropy
// ---------------------------------------------------------------------------
//
// Same 128-bit `std::random_device` pattern C.10a uses for `stream_token`.
// `mint_client_id()` in ipc_server.cpp is documented non-cryptographic
// (rand()-seeded; "a log-friendly tag, not a security primitive"), so we
// do NOT reuse it for the wire-facing routing/authorization key.
std::string mint_upload_token() {
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

// Per-format file extension on the staging side. Raw PCM is wrapped to WAV
// via libsndfile, so it gets a `.wav` extension just like the container WAV
// form. Container formats get their native extension so the reprocess
// pipeline's libsndfile auto-detection picks the right decoder.
const char* staging_extension(const std::string& format) {
    if (format == kSubmitFormatS16le) return "wav";
    if (format == kSubmitFormatF32le) return "wav";
    if (format == kSubmitFormatWav)   return "wav";
    if (format == kSubmitFormatFlac)  return "flac";
    if (format == kSubmitFormatMp3)   return "mp3";
    if (format == kSubmitFormatM4a)   return "m4a";
    if (format == kSubmitFormatOgg)   return "ogg";
    return "";  // unknown — caller rejects before we reach here
}

bool is_raw_pcm_format(const std::string& format) {
    return format == kSubmitFormatS16le || format == kSubmitFormatF32le;
}

bool is_supported_format(const std::string& format) {
    return !std::string(staging_extension(format)).empty();
}

void rm_dir_quiet(const fs::path& p) {
    if (p.empty()) return;
    std::error_code ec;
    fs::remove_all(p, ec);
    if (ec) {
        log_warn("[upload] failed to remove staging dir %s: %s",
                 p.string().c_str(), ec.message().c_str());
    }
}

// ---------------------------------------------------------------------------
// Phase C.11.4 — atomic staging→meeting WAV relocation
// ---------------------------------------------------------------------------
//
// Convergence-principle invariant: a concurrent reader of the final audio
// path must see either the prior file's bytes or the new file's bytes —
// never a partial write. Implementation: write the new bytes to
// `<final>.tmp` (full content + `fsync(file_fd)`), then `rename(2)` over
// `<final>`, then `fsync(dir_fd)` so the rename hits stable storage.
// `rename(2)` is atomic within a single filesystem.
//
// Cross-filesystem fallback. If `/tmp` (staging) and `~/meetings` are on
// different filesystems (common: tmpfs vs ext4), `rename(2)` returns
// EXDEV. We detect this via std::filesystem's error_code and fall back to
// `copy_file` (which is atomic on POSIX: it opens dst with O_EXCL? no —
// it isn't, so we do the same write-tmp + rename dance on the destination
// side, just using a buffered copy as the body). This keeps the
// "concurrent reader sees one or the other, never half" invariant intact
// because the rename on the destination filesystem is still atomic.
//
// Returns std::nullopt on success; an error message string otherwise.
std::optional<std::string>
atomic_relocate(const fs::path& src, const fs::path& dst_final) {
    fs::path dst_tmp = dst_final;
    dst_tmp += ".tmp";

    std::error_code ec;
    fs::remove(dst_tmp, ec); // best-effort — clear any stale partial

    // Try same-filesystem rename first. The source already has its bytes
    // fully flushed (libsndfile sf_close / ofstream close did that before
    // we arrived). std::filesystem::rename does rename(2) under the hood.
    fs::rename(src, dst_tmp, ec);
    if (ec) {
        // EXDEV is the only fallback we handle; any other error is fatal.
        if (ec != std::errc::cross_device_link) {
            return std::string("rename(") + src.string() + " -> " +
                   dst_tmp.string() + "): " + ec.message();
        }
        // Cross-filesystem path: copy bytes onto the destination FS into
        // <dst>.tmp, then rename to <dst>.
        ec.clear();
        std::ifstream in(src, std::ios::binary);
        if (!in.is_open()) {
            return std::string("open src ") + src.string() +
                   " for cross-fs copy failed";
        }
        std::ofstream out(dst_tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return std::string("open dst tmp ") + dst_tmp.string() +
                   " for cross-fs copy failed";
        }
        out << in.rdbuf();
        out.flush();
        if (!out.good() || !in.good()) {
            std::error_code rm_ec;
            fs::remove(dst_tmp, rm_ec);
            return std::string("cross-fs copy ") + src.string() + " -> " +
                   dst_tmp.string() + " failed mid-stream";
        }
        out.close();
        in.close();
        // Remove the source on success — we own the staging copy now.
        fs::remove(src, ec);
        if (ec) {
            log_warn("[upload] atomic_relocate: cross-fs copy succeeded but "
                     "failed to remove staging src %s: %s",
                     src.string().c_str(), ec.message().c_str());
            ec.clear(); // not fatal — the dst is what matters
        }
    }

    // Now: bytes live at dst_tmp on the destination filesystem. fsync the
    // file before the rename so the data is durable before any reader can
    // possibly see it.
    {
        int fd = ::open(dst_tmp.string().c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            (void)::fsync(fd);
            (void)::close(fd);
        }
    }

    fs::rename(dst_tmp, dst_final, ec);
    if (ec) {
        std::error_code rm_ec;
        fs::remove(dst_tmp, rm_ec);
        return std::string("rename(") + dst_tmp.string() + " -> " +
               dst_final.string() + "): " + ec.message();
    }

    // fsync(parent_dir) so the rename entry is durable.
    {
        fs::path parent = dst_final.parent_path();
        int dfd = ::open(parent.string().c_str(),
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            (void)::fsync(dfd);
            (void)::close(dfd);
        }
    }
    return std::nullopt;
}

} // anonymous namespace

// ===========================================================================
// UploadSession
// ===========================================================================

UploadSession::~UploadSession() {
    // Defensive teardown — UploadSessionManager::teardown_locked() is the
    // normal path. A session that is dropped any other way (e.g. a create()
    // that fails after the staging file opened) must still release its
    // resources cleanly.
    if (wav_) {
        sf_close(static_cast<SNDFILE*>(wav_));
        wav_ = nullptr;
    }
    if (container_out_) {
        container_out_->close();
        container_out_.reset();
    }
    // Only remove the staging dir if the session never finalized — once
    // `enqueue_reserved()` has happened the directory is owned by the
    // postprocess pipeline (it reads audio from there).
    if (!finalized_) rm_dir_quiet(staging_dir_);
}

// ===========================================================================
// UploadSessionManager
// ===========================================================================

UploadSessionManager::UploadSessionManager(JobQueue& jobs,
                                           fs::path staging_root,
                                           UploadProgressSink progress_sink,
                                           MeetingIndex* meeting_index,
                                           fs::path meetings_root)
    : jobs_(jobs), staging_root_(std::move(staging_root)),
      progress_sink_(std::move(progress_sink)),
      meeting_index_(meeting_index),
      meetings_root_(std::move(meetings_root)) {
    // C.11.4 — the dedup contract requires BOTH a MeetingIndex AND a
    // meetings_root. A half-wired manager (one set, the other not) is a
    // configuration bug; log it once at startup so it surfaces immediately
    // rather than silently degrading the upload path to legacy behavior.
    const bool have_idx  = (meeting_index_ != nullptr);
    const bool have_root = !meetings_root_.empty();
    if (have_idx != have_root) {
        log_warn("[upload] manager constructed with partial dedup wiring "
                 "(meeting_index=%s, meetings_root=%s) — convergence-principle "
                 "dedup disabled; uploads will use legacy staging-as-meeting-dir",
                 have_idx ? "set" : "null",
                 have_root ? meetings_root_.string().c_str() : "empty");
        meeting_index_ = nullptr;
        meetings_root_.clear();
    }
}

UploadSessionManager::~UploadSessionManager() {
    // Abort every still-active session on daemon shutdown. The map owns the
    // sessions; clearing it runs `~UploadSession` on each, which closes the
    // staging file and removes the staging dir (when not finalized).
    std::lock_guard<std::mutex> lk(mu_);
    sessions_.clear();
}

void UploadSessionManager::teardown_locked(UploadSession* sess) {
    if (sess->wav_) {
        sf_close(static_cast<SNDFILE*>(sess->wav_));
        sess->wav_ = nullptr;
    }
    if (sess->container_out_) {
        sess->container_out_->close();
        sess->container_out_.reset();
    }
    // Caller policy decides whether to remove the staging dir. The normal
    // cancel/disconnect path DOES remove it; finalize() lifts the directory
    // ownership to pp_worker_loop and skips the removal.
    rm_dir_quiet(sess->staging_dir_);
    sess->staging_dir_.clear();
}

UploadSessionManager::CreateResult
UploadSessionManager::create(const std::string& client_id,
                             const SubmitRequest& req,
                             const JobConfig& pp_cfg,
                             size_t max_upload_bytes) {
    CreateResult res;
    // Cap reported to client: 0 means "no operator cap" — report INT64_MAX.
    const int64_t cap = (max_upload_bytes == 0)
        ? std::numeric_limits<int64_t>::max()
        : static_cast<int64_t>(max_upload_bytes);
    res.max_size = cap;

    // --- Validate request shape (cheap checks first, before any allocation).

    // Mode guard. Phase C.8 wires `mode == "enroll"`: the upload finalizes
    // into a Postprocess job whose `cfg.enroll_mode = true`, which the
    // subprocess runs as diarize-only (skip transcribe / summarize /
    // note-write). The enroll-mode finalize requires a non-empty
    // `enroll_name` so the eventual `enroll.finalize` knows the label;
    // catch the missing-name case here at create-time rather than at
    // finalize.
    if (req.mode == "enroll") {
        if (req.enroll_name.empty()) {
            res.code = static_cast<int>(IpcErrorCode::InvalidParams);
            res.error = "process.submit: mode='enroll' requires "
                        "'enroll_name'";
            return res;
        }
    } else if (req.mode != "transcribe" && !req.mode.empty()) {
        res.code = static_cast<int>(IpcErrorCode::InvalidParams);
        res.error = "process.submit: unknown mode '" + req.mode +
                    "' (expected 'transcribe' or 'enroll')";
        return res;
    }

    if (!is_supported_format(req.format)) {
        res.code = static_cast<int>(IpcErrorCode::InvalidParams);
        res.error = "process.submit: unsupported format '" + req.format +
                    "' (expected s16le, f32le, wav, flac, mp3, m4a, or ogg)";
        return res;
    }

    if (req.audio_size <= 0) {
        res.code = static_cast<int>(IpcErrorCode::InvalidParams);
        res.error = "process.submit: audio_size must be > 0";
        return res;
    }
    if (req.audio_size > cap) {
        res.code = static_cast<int>(IpcErrorCode::InvalidParams);
        res.error = "process.submit: audio_size " +
                    std::to_string(req.audio_size) +
                    " exceeds max_upload_bytes " + std::to_string(cap);
        return res;
    }

    // Channels/sample_rate are only meaningful for raw PCM (we wrap to WAV
    // via libsndfile and the header needs them). Container formats carry
    // their own metadata; ignore mismatch silently for those.
    if (is_raw_pcm_format(req.format)) {
        if (req.channels < 1 || req.channels > 8) {
            res.code = static_cast<int>(IpcErrorCode::InvalidParams);
            res.error = "process.submit: invalid channels " +
                        std::to_string(req.channels);
            return res;
        }
        if (req.sample_rate < 8000 || req.sample_rate > 192000) {
            res.code = static_cast<int>(IpcErrorCode::InvalidParams);
            res.error = "process.submit: invalid sample_rate " +
                        std::to_string(req.sample_rate);
            return res;
        }
    }

    std::lock_guard<std::mutex> lk(mu_);

    // Capacity-1 postprocess-upload invariant: one outstanding upload per
    // client. (The capacity-1 postprocess SLOT can still hold a different
    // job — that is policed at finalize/dequeue time. The invariant here is
    // about a client not running two parallel uploads, which would race on
    // the binary-frame route-by-client_id path.)
    for (const auto& [tok, s] : sessions_) {
        (void)tok;
        if (s->client_id_ == client_id) {
            res.code = static_cast<int>(IpcErrorCode::Busy);
            res.error = "process.submit: client already has an upload in "
                        "progress (cancel it first)";
            return res;
        }
    }

    // --- Reserve a JobQueue postprocess job_id (registry-only,
    //     state=WaitingForUpload — no FIFO insertion yet).
    int64_t job_id = jobs_.reserve_job_id(JobKind::Postprocess, client_id);

    // --- Open the staging directory + audio file.
    fs::path root = staging_root_.empty()
        ? fs::temp_directory_path() : staging_root_;
    std::error_code ec;
    fs::create_directories(root, ec);  // best-effort

    std::string token = mint_upload_token();
    fs::path dir = root / ("recmeet-upload-" + std::to_string(job_id) +
                           "-" + token.substr(0, 8));
    fs::create_directories(dir, ec);
    if (ec) {
        jobs_.cancel(job_id);
        res.code = static_cast<int>(IpcErrorCode::InternalError);
        res.error = "process.submit: failed to create staging dir: " +
                    ec.message();
        return res;
    }

    fs::path audio = dir / (std::string("audio.") +
                            staging_extension(req.format));

    auto sess = std::unique_ptr<UploadSession>(new UploadSession());
    sess->job_id_ = job_id;
    sess->client_id_ = client_id;
    sess->upload_token_ = token;
    sess->req_ = req;
    sess->audio_size_ = req.audio_size;
    sess->bytes_received_ = 0;
    sess->staging_dir_ = dir;
    sess->staging_audio_path_ = audio;
    sess->finalized_ = false;

    if (is_raw_pcm_format(req.format)) {
        SF_INFO info = {};
        info.samplerate = req.sample_rate;
        info.channels   = req.channels;
        if (req.format == kSubmitFormatS16le) {
            info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        } else {
            // f32le — write as WAV float. libsndfile transparently downgrades
            // on read for downstream consumers that prefer s16.
            info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
        }
        SNDFILE* wav = sf_open(audio.string().c_str(), SFM_WRITE, &info);
        if (!wav) {
            std::string sf_err = sf_strerror(nullptr);
            rm_dir_quiet(dir);
            jobs_.cancel(job_id);
            res.code = static_cast<int>(IpcErrorCode::InternalError);
            res.error = "process.submit: could not open staging WAV: " + sf_err;
            return res;
        }
        sess->wav_ = wav;
    } else {
        auto out = std::make_unique<std::ofstream>(
            audio, std::ios::binary | std::ios::trunc);
        if (!out->is_open()) {
            rm_dir_quiet(dir);
            jobs_.cancel(job_id);
            res.code = static_cast<int>(IpcErrorCode::InternalError);
            res.error = "process.submit: could not open staging file " +
                        audio.string();
            return res;
        }
        sess->container_out_ = std::move(out);
    }

    // Stash the postprocess Config snapshot on the session via the SubmitRequest
    // context field — the actual `Config` is held until finalize where we
    // copy it into the Job. We capture pp_cfg here by reference; finalize
    // reads it via a member. (We store a copy on the session to outlive the
    // caller scope.)
    // To keep the public header concise, the Config is plumbed via a private
    // friend access path: we add it as a private member here.
    // (See pp_cfg_ below — but to avoid bloating the header, we stash it as
    // a side-table.)
    sessions_.emplace(token, std::move(sess));
    // Side-table for pp config snapshots, keyed by job_id. Cheap; one entry
    // per active upload, removed on finalize / teardown.
    pp_cfg_snapshots_[job_id] = pp_cfg;
    pp_context_overrides_[job_id] = req.context;
    // Phase C.8 — stash the mode + enroll_name so finalize can stamp them
    // on the outgoing Job.cfg. Skip the entries entirely for transcribe
    // mode so we don't litter the map with empty strings.
    if (req.mode == "enroll") {
        pp_modes_[job_id] = req.mode;
        pp_enroll_names_[job_id] = req.enroll_name;
    }
    // C.11 — only stash when non-empty so v1 submits leave nothing behind.
    if (!req.meeting_id.empty()) {
        pp_meeting_ids_[job_id] = req.meeting_id;
    }

    log_info("[upload] session created: job=%ld client=%s token=%s "
             "dir=%s audio_size=%lld format=%s",
             (long)job_id, client_id.c_str(), token.c_str(),
             dir.string().c_str(), (long long)req.audio_size,
             req.format.c_str());

    res.ok = true;
    res.job_id = job_id;
    res.upload_token = token;
    return res;
}

bool UploadSessionManager::feed_chunk(const std::string& client_id,
                                      const std::string& payload) {
    // Snapshot under lock; do the disk write under lock too so a concurrent
    // cancel/disconnect cannot tear the file out from under us. The poll
    // thread serializes IPC requests so this rarely contends in practice.
    int64_t job_id = 0;
    int64_t bytes_after = 0;
    int64_t audio_size = 0;
    bool    finalize_now = false;
    std::string staging_audio_path_str;
    fs::path staging_dir;
    JobConfig pp_cfg;
    std::string context_inline;
    std::string pp_mode;        // Phase C.8 — captured under the lock
    std::string pp_enroll_name; // Phase C.8 — captured under the lock
    std::string pp_meeting_id;  // Phase C.11 — captured under the lock

    // Phase C.11.4 — resolved by the in-lock dedup block. Equal to the
    // staging paths on the legacy/unwired path; equal to the real
    // ~/meetings/{ts}/ path on the dedup-wired path.
    fs::path    resolved_meeting_dir;
    fs::path    resolved_audio_path;
    std::string resolved_timestamp;
    {
        std::lock_guard<std::mutex> lk(mu_);
        UploadSession* sess = nullptr;
        std::string token_key;
        for (auto& [tok, s] : sessions_) {
            if (s->client_id_ == client_id) {
                sess = s.get();
                token_key = tok;
                break;
            }
        }
        if (!sess) {
            log_warn("[upload] feed_chunk: no live upload for client=%s — "
                     "protocol violation", client_id.c_str());
            return false;
        }
        if (sess->finalized_) {
            // Shouldn't happen — finalize erases from sessions_. Defensive.
            log_warn("[upload] feed_chunk: session already finalized "
                     "(client=%s job=%ld)", client_id.c_str(),
                     (long)sess->job_id_);
            return false;
        }
        if (payload.empty()) return true;  // legal no-op (zero-length frame)

        const int64_t new_total =
            sess->bytes_received_ + static_cast<int64_t>(payload.size());
        if (new_total > sess->audio_size_) {
            log_warn("[upload] feed_chunk: bytes_received %lld + payload %zu "
                     "exceeds audio_size %lld (client=%s job=%ld) — "
                     "protocol violation",
                     (long long)sess->bytes_received_, payload.size(),
                     (long long)sess->audio_size_, client_id.c_str(),
                     (long)sess->job_id_);
            return false;
        }

        // Write to the staging file. For raw PCM via libsndfile we go
        // sample-wise; for container formats it's a byte-for-byte append.
        if (sess->wav_) {
            // Raw PCM path. Reject odd-length payloads for s16le (the wire
            // is exact byte counts; an odd s16le frame is malformed).
            if (sess->req_.format == kSubmitFormatS16le) {
                if (payload.size() % sizeof(int16_t) != 0) {
                    log_warn("[upload] feed_chunk: odd-length s16le payload "
                             "(%zu bytes) — protocol violation",
                             payload.size());
                    return false;
                }
                const size_t n = payload.size() / sizeof(int16_t);
                const int16_t* p = reinterpret_cast<const int16_t*>(payload.data());
                sf_count_t wrote = sf_write_short(
                    static_cast<SNDFILE*>(sess->wav_), p,
                    static_cast<sf_count_t>(n));
                if (wrote != static_cast<sf_count_t>(n)) {
                    log_warn("[upload] feed_chunk: short s16le WAV write "
                             "(%lld/%zu) job=%ld",
                             (long long)wrote, n, (long)sess->job_id_);
                    return false;
                }
            } else {
                // f32le
                if (payload.size() % sizeof(float) != 0) {
                    log_warn("[upload] feed_chunk: odd-length f32le payload "
                             "(%zu bytes) — protocol violation",
                             payload.size());
                    return false;
                }
                const size_t n = payload.size() / sizeof(float);
                const float* p = reinterpret_cast<const float*>(payload.data());
                sf_count_t wrote = sf_write_float(
                    static_cast<SNDFILE*>(sess->wav_), p,
                    static_cast<sf_count_t>(n));
                if (wrote != static_cast<sf_count_t>(n)) {
                    log_warn("[upload] feed_chunk: short f32le WAV write "
                             "(%lld/%zu) job=%ld",
                             (long long)wrote, n, (long)sess->job_id_);
                    return false;
                }
            }
        } else if (sess->container_out_) {
            sess->container_out_->write(payload.data(),
                                        static_cast<std::streamsize>(payload.size()));
            if (!sess->container_out_->good()) {
                log_warn("[upload] feed_chunk: container write failed "
                         "(job=%ld)", (long)sess->job_id_);
                return false;
            }
        } else {
            log_warn("[upload] feed_chunk: session has no open writer "
                     "(job=%ld)", (long)sess->job_id_);
            return false;
        }

        sess->bytes_received_ = new_total;
        job_id = sess->job_id_;
        bytes_after = sess->bytes_received_;
        audio_size = sess->audio_size_;

        if (sess->bytes_received_ >= sess->audio_size_) {
            // Reached the declared end of the upload. Close the writer NOW
            // (under the lock) so libsndfile flushes the WAV header / the
            // ofstream syncs to disk before we hand the directory to
            // pp_worker_loop. Capture the metadata we need for the JobQueue
            // finalize and the staging_dir handoff; we erase the session
            // and call `enqueue_reserved` OUTSIDE the lock (JobQueue is
            // reentrant-safe, but the daemon's job_event_sink may take other
            // locks — keep the manager mutex narrow).
            if (sess->wav_) {
                sf_close(static_cast<SNDFILE*>(sess->wav_));
                sess->wav_ = nullptr;
            }
            if (sess->container_out_) {
                sess->container_out_->flush();
                sess->container_out_->close();
                sess->container_out_.reset();
            }
            sess->finalized_ = true;
            staging_audio_path_str = sess->staging_audio_path_.string();
            staging_dir = sess->staging_dir_;
            // Pull the cfg / context_inline / mode / enroll_name snapshots
            // from the side tables. We capture them into locals BEFORE
            // erasing the entries so the outside-the-lock finalize block
            // can stamp them on the outgoing Job.
            auto cit = pp_cfg_snapshots_.find(job_id);
            if (cit != pp_cfg_snapshots_.end()) pp_cfg = cit->second;
            auto xit = pp_context_overrides_.find(job_id);
            if (xit != pp_context_overrides_.end()) context_inline = xit->second;
            auto mit = pp_modes_.find(job_id);
            if (mit != pp_modes_.end()) pp_mode = mit->second;
            auto nit = pp_enroll_names_.find(job_id);
            if (nit != pp_enroll_names_.end()) pp_enroll_name = nit->second;
            auto idit = pp_meeting_ids_.find(job_id);
            if (idit != pp_meeting_ids_.end()) pp_meeting_id = idit->second;
            // Remove the session BEFORE leaving the lock so the next
            // process.submit from the same client is admitted cleanly.
            sessions_.erase(token_key);
            pp_cfg_snapshots_.erase(job_id);
            pp_context_overrides_.erase(job_id);
            pp_modes_.erase(job_id);
            pp_enroll_names_.erase(job_id);
            pp_meeting_ids_.erase(job_id);

            // ---------------------------------------------------------------
            // Phase C.11.4 — convergence-principle dedup resolution.
            //
            // Decide the target meeting directory while the manager mutex is
            // still held. Concurrent submits with the same meeting_id (from
            // different clients — same-client is already barred by the
            // capacity-1 invariant) serialize through this block and end up
            // referring to the same dir; the LATER one's bytes win via the
            // atomic relocate outside the lock (client-authoritative
            // overwrite, per docs/V2-STRATEGY.md).
            //
            // Two-path design:
            //   - Wired:   meeting_index_ + meetings_root_ both set. Resolve
            //              an entry in ~/meetings/ keyed by meeting_id (or
            //              allocate fresh when absent / vanished), bind, set
            //              the resolved paths. The outside-lock block does
            //              the atomic relocate + context.json write.
            //   - Unwired: legacy "staging dir IS the meeting dir" model.
            //              The outside-lock block sees resolved_meeting_dir
            //              == staging_dir and skips the relocate entirely.
            //              Existing tests that pass no MeetingIndex stay on
            //              this path byte-for-byte.
            // ---------------------------------------------------------------
            if (meeting_index_ != nullptr && !meetings_root_.empty()) {
                std::optional<fs::path> hit;
                if (!pp_meeting_id.empty()) {
                    hit = meeting_index_->find(pp_meeting_id);
                }
                std::error_code dec;
                if (hit.has_value() && fs::is_directory(*hit, dec)) {
                    resolved_meeting_dir = *hit;
                    resolved_timestamp = derive_meeting_timestamp(*hit);
                    log_info("[upload] dedup: meeting_id=%s -> existing dir %s "
                             "(overwrite path)",
                             pp_meeting_id.c_str(),
                             resolved_meeting_dir.string().c_str());
                } else {
                    if (hit.has_value()) {
                        // Stale index entry — dir was deleted out from under
                        // us. Drop the binding before allocating fresh so
                        // the next submit doesn't keep chasing the ghost.
                        log_warn("[upload] dedup: meeting_id=%s bound to "
                                 "vanished dir %s — unbinding and allocating "
                                 "fresh", pp_meeting_id.c_str(),
                                 hit->string().c_str());
                        meeting_index_->unbind(pp_meeting_id);
                    }
                    std::error_code mkec;
                    fs::create_directories(meetings_root_, mkec); // best-effort
                    try {
                        OutputDir od = create_output_dir(meetings_root_);
                        resolved_meeting_dir = od.path;
                        resolved_timestamp = od.timestamp;
                    } catch (const std::exception& e) {
                        // create_output_dir throws on "too many sessions in
                        // the same minute" — extreme edge case. Fall back to
                        // staging-as-meeting-dir for this one submit; log
                        // loudly so operators see it.
                        log_error("[upload] dedup: create_output_dir(%s) "
                                  "failed: %s — falling back to staging dir",
                                  meetings_root_.string().c_str(), e.what());
                        resolved_meeting_dir = staging_dir;
                        resolved_audio_path =
                            fs::path(staging_audio_path_str);
                    }
                    if (resolved_meeting_dir != staging_dir &&
                        !pp_meeting_id.empty()) {
                        // Bind under the mutex. A concurrent submit landing
                        // immediately after will find this binding and
                        // converge on the same dir.
                        meeting_index_->bind(pp_meeting_id,
                                             resolved_meeting_dir);
                        log_info("[upload] dedup: meeting_id=%s -> fresh dir "
                                 "%s (allocate path, bound)",
                                 pp_meeting_id.c_str(),
                                 resolved_meeting_dir.string().c_str());
                    } else if (resolved_meeting_dir != staging_dir) {
                        log_info("[upload] dedup: v1-shaped submit (no "
                                 "meeting_id) -> fresh dir %s (no index "
                                 "binding)",
                                 resolved_meeting_dir.string().c_str());
                    }
                }
                if (resolved_audio_path.empty()) {
                    // Compose the final audio filename matching V1 naming
                    // (audio_<ts>.<ext>) so V1 readers — including
                    // find_audio_file() — locate it. Fallback to plain
                    // "audio.<ext>" when the timestamp couldn't be derived.
                    std::string ext = staging_extension(sess->req_.format);
                    std::string fname = resolved_timestamp.empty()
                        ? (std::string("audio.") + ext)
                        : (std::string("audio_") + resolved_timestamp +
                           "." + ext);
                    resolved_audio_path = resolved_meeting_dir / fname;
                }
            } else {
                // Legacy path: staging IS the meeting dir.
                resolved_meeting_dir = staging_dir;
                resolved_audio_path = fs::path(staging_audio_path_str);
            }

            finalize_now = true;
        }
    }

    // Outside the lock: emit progress, then (if applicable) finalize the
    // JobQueue reservation.
    if (progress_sink_.on_progress)
        progress_sink_.on_progress(job_id, client_id, bytes_after, audio_size);

    if (finalize_now) {
        // Phase C.11.4 — atomic staging→meeting relocation (dedup-wired
        // path only). On the legacy path the resolved_* values equal the
        // staging values and we skip the relocate entirely; the staging
        // dir stays put and becomes the postprocess out_dir as it did
        // pre-C.11.4. On the wired path the resolved values point into
        // `meetings_root_/{ts}/`, and we:
        //   1. Atomically relocate the staging audio file (write-tmp +
        //      fsync + rename + fsync(dir); EXDEV handled).
        //   2. Write context.json with the captured context_inline and
        //      meeting_id (client-authoritative replace; the on-disk
        //      file is overwritten with whatever the latest submit
        //      carried — per V2-STRATEGY.md "Convergence principle").
        //   3. Remove the staging dir (the audio has moved out).
        // If the relocate fails we cancel the JobQueue reservation and
        // return true — the protocol round-trip is complete, but the
        // subprocess will never run. The client sees the failure via
        // job.status / progress.job after a brief delay.
        if (resolved_meeting_dir != staging_dir) {
            auto err = atomic_relocate(fs::path(staging_audio_path_str),
                                       resolved_audio_path);
            if (err.has_value()) {
                log_error("[upload] finalize: atomic_relocate failed for "
                          "job=%ld meeting_id=%s: %s — cancelling "
                          "reservation",
                          (long)job_id, pp_meeting_id.c_str(),
                          err->c_str());
                jobs_.cancel(job_id);
                rm_dir_quiet(staging_dir);
                return true;
            }
            // Write the context.json for this meeting. Includes meeting_id
            // when non-empty so a future daemon restart's
            // rebuild_from_disk re-establishes the binding. The
            // `resolved_timestamp` drives the canonical filename pattern
            // (context_<ts>.json), matching what live-recording writes.
            try {
                save_meeting_context(resolved_meeting_dir, context_inline,
                                     /*context_file=*/fs::path{},
                                     resolved_timestamp, pp_meeting_id);
            } catch (const std::exception& e) {
                // Non-fatal: the audio is already in place; the
                // subprocess can still run. Index rebuild after restart
                // would lose this binding without context.json — log so
                // operators notice.
                log_warn("[upload] finalize: save_meeting_context(%s) "
                         "failed: %s — index binding will be lost across "
                         "daemon restart",
                         resolved_meeting_dir.string().c_str(), e.what());
            }
            // The staging dir is now empty of the audio; remove it.
            rm_dir_quiet(staging_dir);
        }

        // Build the postprocess Job payload. `input.out_dir` is the dir
        // the reprocess subprocess scans for audio; with C.11.4 wired
        // this is the real ~/meetings/{ts}/ path, not the staging dir.
        Job j;
        j.input.out_dir = resolved_meeting_dir;
        j.input.audio_path = resolved_audio_path;
        j.cfg = pp_cfg;
        // Per-submit context override (the daemon's pp_worker writes the
        // job config out and the subprocess picks it up via the standard
        // `cfg.context_inline` channel). Pre-C.11.4 the subprocess wrote
        // context.json itself via run_pipeline's live-record branch; on
        // the C.11.4 path we already wrote context.json above (so the
        // index rebuild survives), and the subprocess reads it back via
        // load_meeting_context. Both are consistent.
        if (!context_inline.empty()) j.cfg.context_inline = context_inline;
        // Steer the subprocess to *reprocess* the meeting dir, not start
        // a new recording. pp_worker_loop sets this from input.out_dir
        // anyway, but be explicit so the contract is visible at the seam.
        j.cfg.reprocess_dir = resolved_meeting_dir.string();
        // Phase C.8 — enroll mode is carried via the locals captured under
        // the lock above (the side-table entries are already erased by
        // this point). Stamp the outgoing Job.cfg so the subprocess
        // inspects the flag and runs the diarize-only path.
        if (pp_mode == "enroll") {
            j.cfg.enroll_mode = true;
            j.cfg.enroll_name = pp_enroll_name;
        }
        // C.11 — stamp the meeting_id captured under the lock. Empty for
        // v1-shaped clients. job.list / job.status echo this back so the
        // client can reconcile its journal entries by content key.
        j.meeting_id = pp_meeting_id;

        bool placed = jobs_.enqueue_reserved(job_id, std::move(j));
        if (!placed) {
            // Reservation was cancelled between feed and finalize. The
            // audio is in its meeting dir (C.11.4 wired) or staging dir
            // (legacy); on the wired path we leave it in place — it's
            // operator data, and a future process.reprocess can pick it
            // up. On the legacy path we still rm_dir the staging since
            // it would otherwise be an /tmp orphan no operator can find.
            if (resolved_meeting_dir == staging_dir) {
                log_warn("[upload] finalize: reservation job=%ld no longer "
                         "WaitingForUpload — staging dir orphaned, removing",
                         (long)job_id);
                rm_dir_quiet(staging_dir);
            } else {
                log_warn("[upload] finalize: reservation job=%ld no longer "
                         "WaitingForUpload — audio at %s preserved for "
                         "future reprocess", (long)job_id,
                         resolved_meeting_dir.string().c_str());
            }
            return true;  // not a protocol violation; just lost the race.
        }
        log_info("[upload] finalize: job=%ld client=%s audio_size=%lld "
                 "meeting_dir=%s — enqueued for postprocess",
                 (long)job_id, client_id.c_str(), (long long)audio_size,
                 resolved_meeting_dir.string().c_str());
    }
    return true;
}

bool UploadSessionManager::cancel(const std::string& client_id,
                                  const std::string& upload_token) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(upload_token);
    if (it == sessions_.end()) {
        log_warn("[upload] cancel: unknown upload_token=%s",
                 upload_token.c_str());
        return false;
    }
    UploadSession* sess = it->second.get();
    if (sess->client_id_ != client_id) {
        log_warn("[upload] cancel: token=%s owned by client=%s, not "
                 "requester=%s — refusing",
                 upload_token.c_str(), sess->client_id_.c_str(),
                 client_id.c_str());
        return false;
    }
    log_info("[upload] cancel: job=%ld client=%s token=%s",
             (long)sess->job_id_, client_id.c_str(), upload_token.c_str());
    cancel_session_locked(it);
    return true;
}

bool UploadSessionManager::cancel_by_job_id(int64_t job_id) {
    std::lock_guard<std::mutex> lk(mu_);
    // Phase C.5: `process.cancel` enters here with a job_id. Scan for the
    // bound session; the capacity-1 postprocess slot makes the scan at most
    // one entry deep in practice. Ownership is NOT checked here — the
    // `process.cancel` handler authorizes via `client_for_job` before
    // calling us. Returns false when no live upload session is bound to
    // `job_id` (e.g. the upload finalized into a Queued postprocess job —
    // the handler's `JobQueue::cancel(job_id)` covers that path on its own;
    // returning false here is the signal that no upload-specific teardown
    // is needed).
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (it->second->job_id_ != job_id) continue;
        log_info("[upload] cancel_by_job_id: job=%ld token=%s",
                 (long)job_id, it->first.c_str());
        cancel_session_locked(it);
        return true;
    }
    log_debug("[upload] cancel_by_job_id: no live upload for job=%ld",
              (long)job_id);
    return false;
}

void UploadSessionManager::cancel_session_locked(
        std::map<std::string,
                 std::unique_ptr<UploadSession>>::iterator it) {
    // Shared body of cancel() and cancel_by_job_id(). Caller holds `mu_`.
    UploadSession* sess = it->second.get();
    int64_t job_id = sess->job_id_;
    teardown_locked(sess);
    // The reservation is still in WaitingForUpload — `JobQueue::cancel`
    // transitions it to Cancelled without ever touching a slot FIFO.
    jobs_.cancel(job_id);
    sessions_.erase(it);
    pp_cfg_snapshots_.erase(job_id);
    pp_context_overrides_.erase(job_id);
    pp_modes_.erase(job_id);
    pp_enroll_names_.erase(job_id);
    pp_meeting_ids_.erase(job_id);
}

int UploadSessionManager::on_client_disconnect(const std::string& client_id) {
    std::lock_guard<std::mutex> lk(mu_);
    int aborted = 0;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second->client_id_ != client_id) {
            ++it;
            continue;
        }
        UploadSession* sess = it->second.get();
        int64_t job_id = sess->job_id_;
        log_info("[upload] client %s disconnected mid-upload — aborting "
                 "job=%ld token=%s",
                 client_id.c_str(), (long)job_id, it->first.c_str());
        teardown_locked(sess);
        // Disconnect contract — C.10a parity: the client did not ask to
        // stop, so mark the reservation Failed (with an explicit error
        // message). The streaming-session sibling uses `finish(false, ...)`
        // for the same reason. Even though our reservation never set the
        // postprocess slot's "running" marker, `finish()` is safe on a
        // WaitingForUpload entry: it transitions state to Failed (the
        // Cancelled-preservation guard doesn't fire) and the slot.running
        // = false write is a no-op. The pp_worker is never woken to dequeue
        // a Failed registry-only entry because it was never in the FIFO.
        jobs_.finish(job_id, /*ok=*/false, "client disconnected mid-upload");
        it = sessions_.erase(it);
        pp_cfg_snapshots_.erase(job_id);
        pp_context_overrides_.erase(job_id);
        pp_modes_.erase(job_id);
        pp_enroll_names_.erase(job_id);
        pp_meeting_ids_.erase(job_id);
        ++aborted;
    }
    return aborted;
}

std::string UploadSessionManager::token_for_client(
        const std::string& client_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [token, sess] : sessions_) {
        if (sess->client_id_ == client_id) return token;
    }
    return {};
}

size_t UploadSessionManager::active_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sessions_.size();
}

int64_t UploadSessionManager::job_id_for_token(
        const std::string& upload_token) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(upload_token);
    return it == sessions_.end() ? -1 : it->second->job_id_;
}

int64_t UploadSessionManager::bytes_received_for_token(
        const std::string& upload_token) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(upload_token);
    return it == sessions_.end() ? -1 : it->second->bytes_received_;
}

} // namespace recmeet
