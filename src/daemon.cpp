// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "backend_info.h"
#include "config.h"
#include "config_json.h"
#include "daemon_handlers.h"          // Phase 2a — `register_daemon_handlers`
#include "daemon_handlers_internal.h" // Phase 2a — shared globals/helpers
#include "diarization_cache.h"
#include "fetch_artifacts.h"
#include "ipc_client.h"   // C.13 — `--evict` CLI dispatch routes through IpcClient
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"
#include "ndjson_parse.h"
#include "session_merge.h"
#include "speaker_id.h"
#include "summarize.h"
#include "log.h"
#include "model_manager.h"
#include "notify.h"
#include "pipeline.h"
#include "meeting_index.h"
#include "meetings_browse.h"
#include "session_manager.h"
#include "streaming_session.h"
#include "upload_session.h"
#include "util.h"
#include "version.h"

#include <whisper.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace recmeet;

// ---------------------------------------------------------------------------
// Daemon state — Phase C.7 + C.9. The pre-C.7 trio (g_recording /
// g_postprocessing / g_downloading) is folded into a typed-slot JobQueue;
// the postprocess and model_download slots' "running" markers are the
// authoritative successors of g_postprocessing / g_downloading.
//
// Phase C.9 retired the daemon-side live-recording path entirely: the
// `record.start` / `record.stop` / `job.context` handlers, the legacy
// `g_recording` atomic + `g_rec_mu` admission gate, the `g_pp_handoff`
// recording→postprocessing bridge, the `g_next_rec_job_id` counter, and the
// `g_rec_worker` thread are all gone. The tray now captures audio locally
// (see `recmeet_capture` / `tray_capture`) and submits batch jobs via
// `process.submit` + `0x01` upload frames; live captions arrive via
// `process.stream` (C.10a). The `streaming` slot reserves a different
// thing — a streaming caption session — and is unchanged by C.9.
// ---------------------------------------------------------------------------

// The typed-slot job queue (postprocess / streaming / model_download). Owns
// the job_id -> client_id binding C.3 will consume. Initialised in main()
// once Config is loaded so slot capacities reflect `[server] slot.*`.
std::unique_ptr<JobQueue> g_jobs;

// Phase C.10a — the server-side streaming-caption session registry. Owns the
// `stream_token -> StreamingSession` map; routes inbound `0x03` frames; holds
// each session's CaptionEngine + disk-backed temp WAV. Constructed in main()
// after g_jobs + the IpcServer exist (it captures the server for the caption
// event sink). One streaming session at a time (C.7 streaming slot is
// capacity-1).
std::unique_ptr<StreamingSessionManager> g_streaming;

// Phase C.2 — the server-side upload-session registry. Owns the
// `upload_token -> UploadSession` map; routes inbound `0x01` frames by
// client_id; holds each upload's per-job staging directory + JobQueue
// postprocess reservation. Constructed in main() after g_jobs exists.
std::unique_ptr<UploadSessionManager> g_uploads;

// Phase C.11.4 — server-side `meeting_id -> meeting_dir_path` index. Owns
// the dedup state for the convergence-principle audio contract (see
// docs/V2-STRATEGY.md "Meeting identity and the client-server audio
// contract"). Constructed at daemon startup BEFORE the IPC listener
// accepts so the very first `process.submit` / `process.stream` sees a
// fully-populated index. Repopulated from on-disk `context.json` via
// `rebuild_from_disk(g_server_config.meetings_root)` once at startup;
// thereafter updated incrementally by the upload finalize + streaming
// create paths.
std::unique_ptr<MeetingIndex> g_meeting_index;

// Phase C.8 — server-resident per-job diarization cache. Populated by
// pp_worker_loop on every successful Postprocess job that produced
// diarization (the enroll-mode path always populates; future
// transcribe-mode jobs land here too when the subprocess writes a
// diarization.json — currently only enroll mode does, but the cache
// machinery is shape-compatible with adding a transcribe-side artifact
// in C.8.1+). Consumed by `enroll.finalize` to extract the user-picked
// cluster's embedding and append to the speakers DB.
std::unique_ptr<DiarizationCache> g_diar_cache;

// Phase C.13 — server-side resume_token store. Owns the (in-memory only)
// `resume_token → ResumeSession{client_id, last_seen_epoch}` map and the
// `--evict` primitive. ResumeSession deliberately does NOT carry creds /
// prefs (H-2 from iter-161 review): those live on `ClientState` only and
// are dropped on disconnect; on resume the client MUST re-send
// `session.init`. Daemon restart invalidates all tokens (MC-1).
std::unique_ptr<SessionManager> g_sessions;


ServerConfig g_server_config;
std::mutex g_server_config_mu;

// Stop tokens — separate for independent cancellation. C.9 dropped
// `g_rec_stop` along with the legacy recording worker; only the postprocess
// kill-switch survives.
StopToken g_pp_stop;

// Phase C.13 — GC sweep thread shutdown signal. Pattern-matched on the
// worker_loop StopToken precedent above; the sweep thread waits on this
// condition variable (with a timeout for the sweep cadence) so SIGTERM
// does not have to wait a full interval.
static std::mutex             g_gc_mu;
static std::condition_variable g_gc_cv;
static bool                   g_gc_shutdown = false;

// Worker threads (C.9 retired g_rec_worker)
static std::thread g_pp_worker;   // long-lived, drains the postprocess slot
static std::thread g_dl_worker;   // long-lived, drains the model_download slot
static std::thread g_stream_worker;  // C.10a — drains the streaming slot
static std::thread g_gc_worker;       // C.13 — periodic resume_token sweep

// PostprocessJob — pre-C.7 this was a standalone struct; the C.7/C.9 typed
// JobQueue surfaces postprocess work as `Job`. The alias is kept so the
// pp_worker / write_job_config call sites read unchanged.
using PostprocessJob = Job;

// Subprocess postprocessing state
std::atomic<pid_t> g_pp_child_pid{-1};
static std::string g_self_exe;  // resolved at startup

// Global server pointer for signal handler
static IpcServer* g_server = nullptr;

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

// State reporting derives the postprocessing / downloading / streaming
// flags from the JobQueue slot "running" markers. Phase C.9 dropped the
// `recording` flag and its composite-state branches: the daemon no longer
// captures audio on its own thread.
static bool jq_postprocessing() {
    return g_jobs && g_jobs->slot_busy(JobKind::Postprocess);
}
static bool jq_downloading() {
    return g_jobs && g_jobs->slot_busy(JobKind::ModelDownload);
}
// Phase C.10a — true while a streaming-caption job occupies the JobQueue
// `streaming` slot (a `process.stream` is live). Independent of
// postprocessing / downloading: the slots may be busy simultaneously.
static bool jq_streaming() {
    return g_jobs && g_jobs->slot_busy(JobKind::Streaming);
}

static std::string composite_state_name() {
    bool pp  = jq_postprocessing();
    bool dl  = jq_downloading();
    bool st  = jq_streaming();
    if (pp)        return "postprocessing";
    if (st)        return "streaming";
    if (dl)        return "downloading";
    return "idle";
}

void fill_state_fields(JsonMap& data) {
    // C.9 wire change: the `recording` boolean is gone from the data map.
    // Clients that need to know whether something is in flight read
    // `state` (the composite name) or the per-slot booleans below.
    data["state"] = composite_state_name();
    data["postprocessing"] = jq_postprocessing();
    data["downloading"] = jq_downloading();
    data["streaming"] = jq_streaming();
}

// For worker threads — schedules broadcast on the poll thread
static void broadcast_state(IpcServer& server, const std::string& error = "") {
    server.post([&server, error]() {
        IpcEvent ev;
        ev.event = "state.changed";
        fill_state_fields(ev.data);
        if (!error.empty()) ev.data["error"] = error;
        server.broadcast(ev);
    });
}

// For handlers running on the poll thread — broadcasts immediately
void broadcast_state_inline(IpcServer& server, const std::string& error) {
    IpcEvent ev;
    ev.event = "state.changed";
    fill_state_fields(ev.data);
    if (!error.empty()) ev.data["error"] = error;
    server.broadcast(ev);
}

// Phase E.6.1 — path-traversal guard for the speakers.* IPC verbs.
// `name` becomes the basename of `<db_dir>/<name>.json`; rejecting `.`,
// `..`, and any path separator keeps the surface confined to db_dir.
// Matches the static helper in src/web.cpp:100 verbatim.
bool is_safe_dirname(const std::string& name) {
    if (name.empty()) return false;
    if (name == "." || name == "..") return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find('\0') != std::string::npos) return false;
    return true;
}

// Phase E.6.1 — `speakers.batch_reidentify` async bookkeeping. A second
// call while one is in flight is rejected with InvalidParams "batch in
// progress". Flag is set before the worker spawns and cleared in the
// worker epilogue (try/catch ensures it always clears).
std::atomic<bool> g_batch_reidentify_running{false};

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static void signal_handler(int sig) {
    if (sig == SIGHUP) {
        // Reload config — handled via post() in the main loop
        if (g_server) {
            g_server->post([] {
                try {
                    // Legacy config.yaml → daemon.yaml + client.yaml migration
                    // (no-op once daemon.yaml exists). Must run before
                    // load_server_config() so operator-edited legacy config
                    // takes effect on SIGHUP.
                    migrate_legacy_config_if_present();
                    std::lock_guard<std::mutex> lk(g_server_config_mu);
                    g_server_config = load_server_config();
                    log_info("daemon: config reloaded via SIGHUP");
                } catch (const std::exception& e) {
                    log_error("daemon: config reload failed: %s", e.what());
                }
                // Phase C.13 (C-2 fix) — also reload PSK from RECMEET_AUTH_TOKEN
                // env so an operator who rotates the env var and SIGHUPs the
                // daemon actually invalidates the old PSK for every NEW
                // connection (existing fds are already Authed and unaffected).
                // Pre-C.13 SIGHUP only reloaded Config, never psk_, so the
                // documented "PSK rotation" operator lever was a no-op.
                const char* env_psk = std::getenv("RECMEET_AUTH_TOKEN");
                if (env_psk) {
                    g_server->set_psk(env_psk);
                    log_info("daemon: PSK reloaded from RECMEET_AUTH_TOKEN env "
                             "via SIGHUP (new value takes effect on next "
                             "auth.token handshake; existing connections "
                             "unaffected)");
                }
            });
        }
        return;
    }
    // SIGINT/SIGTERM → stop all workers, then exit. C.9 retired the
    // legacy g_rec_stop along with the daemon-side recording worker;
    // only the postprocess kill-switch remains.
    g_pp_stop.request();
    if (g_server) g_server->stop();
}

// Suppress whisper log output in daemon mode
static void whisper_null_log(enum ggml_log_level, const char*, void*) {}

// ---------------------------------------------------------------------------
// Phase C.7 note: the pre-C.7 `ensure_models()` helper — which the
// record.start handler called inline-synchronously on the handler thread —
// has been removed. Model readiness is now resolved at *job-dequeue time*
// by the JobQueue: the ModelResolver / ModelCacheChecker wired in main()
// (mirroring the old ensure_models() decision logic) drive an auto-enqueued
// JobKind::ModelDownload in the model_download slot, and the dependent
// postprocess job is parked until the download completes.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Subprocess helpers
// ---------------------------------------------------------------------------

static fs::path write_job_config(const PostprocessJob& job) {
    auto path = fs::temp_directory_path() / ("recmeet-pp-" + std::to_string(job.job_id) + ".json");
    std::ofstream out(path);
    out << config_to_json(job.cfg);
    return path;
}

// ---------------------------------------------------------------------------
// T1C.2 helpers — cgroup-aware kill grace machine
// ---------------------------------------------------------------------------
//
// Background: under cgroup MemoryHigh-driven reclaim with no swap, all of
// the postprocess child's worker threads can park in D (uninterruptible) state
// simultaneously. SIGKILL is queued but signal delivery stalls until the
// kernel can reclaim memory — which it can't, because the cgroup is at the
// soft cap and there's no swap. T1A field measurements (2026-04-30) observed
// SIGKILL stall for ~30 minutes until MemoryHigh was raised manually.
//
// The kill grace machine: SIGTERM → 5s grace → SIGKILL → 30s grace →
// MemoryHigh=infinity → 30s grace → restore. Liveness uses waitpid(WNOHANG)
// so we both detect death AND reap the zombie atomically (avoids the
// kill(pid, 0) anti-pattern which reports zombies as alive).

// Read a systemd memory property via `systemctl show`. Returns the parsed
// value or -1 on failure. Parsing logic is in recmeet::parse_memory_property_line
// (util.cpp) so unit tests can drive synthetic input without spawning systemctl.
static long read_systemd_memory_property(const char* prop) {
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd),
                  "systemctl --user show recmeet-daemon.service -p %s 2>/dev/null", prop);
    FILE* f = ::popen(cmd, "r");
    if (!f) return -1;
    char buf[256] = {0};
    char* got = std::fgets(buf, sizeof(buf), f);
    int rc = ::pclose(f);
    if (!got || rc != 0) return -1;
    return recmeet::parse_memory_property_line(buf);
}

// Set a systemd memory property via `systemctl set-property`. Returns the
// shell exit code; non-zero indicates failure (e.g. unknown property,
// invalid value, dbus unreachable).
static int set_systemd_memory_property(const char* prop, const char* value) {
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd),
                  "systemctl --user set-property recmeet-daemon.service %s=%s",
                  prop, value);
    return std::system(cmd);
}

// Liveness check that ALSO reaps the zombie if the child has exited.
// Returns true if the child is still running, false if dead (reaped or
// already gone). On false, callers MUST NOT call waitpid() again for this
// pid.
static bool poll_child_alive(pid_t pid) {
    int status = 0;
    pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) return false;       // dead and reaped
    if (r == 0)  return true;         // still alive (no state change yet)
    return false;                      // ECHILD or error → treat as dead
}

// Helpers for the deferred-restore guard on the MemoryHigh bump.
// pp_worker_loop is currently single-threaded sequential, so checking
// queue + child running covers all in-flight postprocess work.
static bool pp_queue_empty() {
    // C.7: the postprocess slot is empty when nothing is queued and nothing
    // is running. (The pp slot is capacity-1 and sequential, so this still
    // covers all in-flight postprocess work.)
    return g_jobs &&
           g_jobs->queued_count(JobKind::Postprocess) == 0 &&
           !g_jobs->slot_busy(JobKind::Postprocess);
}
static bool pp_child_running() {
    return g_pp_child_pid.load() > 0;
}

// Kill the postprocess child with the cgroup-aware grace ladder.
// On return, the child has been reaped (or logged as unkillable). Caller
// MUST NOT call waitpid() for this pid afterward.
static void kill_pp_child_with_grace(pid_t pid) {
    log_warn("daemon: watchdog timeout — killing pp child pid=%d", (int)pid);

    // Stage 1: SIGTERM, 5 s grace.
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 5; ++i) {
        if (!poll_child_alive(pid)) return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Stage 2: SIGKILL, 30 s grace. Under cgroup MemoryHigh throttling all
    // worker threads can park in D state; signal delivery stalls until
    // reclaim releases. The cgroup may also OOM-kill during this window —
    // poll_child_alive reaps either way.
    log_warn("daemon: SIGTERM ignored — escalating to SIGKILL");
    ::kill(pid, SIGKILL);
    for (int i = 0; i < 30; ++i) {
        if (!poll_child_alive(pid)) return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Stage 3: SIGKILL stalled — release reclaim pressure. Save current
    // MemoryHigh first so we can restore it (operator may have customized).
    log_warn("daemon: SIGKILL stalled 30s — bumping MemoryHigh=infinity to unblock D-state threads");
    long original_high = read_systemd_memory_property("MemoryHigh");
    int rc = set_systemd_memory_property("MemoryHigh", "infinity");
    if (rc != 0) log_warn("daemon: set-property MemoryHigh=infinity rc=%d", rc);

    // Stage 4: post-bump 30 s grace. Signal should land within seconds.
    bool reaped = false;
    for (int i = 0; i < 30; ++i) {
        if (!poll_child_alive(pid)) { reaped = true; break; }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Stage 5: restore MemoryHigh — but only if no concurrent postprocess
    // job would suddenly hit reclaim throttling. If we can't safely restore
    // now, leave it at infinity; the cgroup MemoryMax (hard cap) still
    // bounds the unit.
    if (original_high > 0 && pp_queue_empty() && !pp_child_running()) {
        if (original_high == LONG_MAX) {
            rc = set_systemd_memory_property("MemoryHigh", "infinity");
        } else {
            char val[32];
            std::snprintf(val, sizeof(val), "%ld", original_high);
            rc = set_systemd_memory_property("MemoryHigh", val);
        }
        if (rc != 0) log_warn("daemon: restore MemoryHigh rc=%d", rc);
    } else {
        log_info("daemon: deferring MemoryHigh restore (queue_nonempty=%d, child_running=%d)",
                 (int)!pp_queue_empty(), (int)pp_child_running());
    }

    if (!reaped) {
        log_error("daemon: child pid=%d unkillable after MemoryHigh bump — manual intervention required",
                  (int)pid);
        // Don't block on waitpid(); leave it as a zombie under the process
        // table rather than risk hanging the watchdog forever. Operator-
        // visible via `ps` and the daemon error log.
    }
}

// ---------------------------------------------------------------------------
// Phase C.13 — orphan-WAV archive helper (M-2)
// ---------------------------------------------------------------------------
//
// When the GC sweep or `--evict` reaps a session that owned a non-terminal
// Postprocess (with a staging WAV) or Streaming (with an in-flight WAV)
// job, the WAV gets archived to `<meetings_root>/.orphan-<prefix>-<ts>/`
// BEFORE the per-kind teardown unlinks anything. This preserves operator
// forensics — the operator can `ls ~/meetings/.orphan-*` to recover
// audio that was in-flight when its owning session expired. The directory
// naming intentionally mirrors `--evict` ergonomics: the 8-char token
// prefix is the same handle the operator typed.
//
// The archive uses an atomic rename pattern: same-filesystem rename is one
// syscall (atomic by construction); cross-filesystem (e.g. /tmp -> ~) falls
// back to copy + rename + remove-src. fsync of parent directory makes the
// rename durable across crash. Failures are logged but never crash the
// daemon — losing the forensic copy is acceptable; losing the GC sweep is
// not. The function returns `true` when archive was attempted (regardless
// of outcome — the caller still proceeds with the teardown); it returns
// `false` when the input path was empty / non-existent and there was
// nothing to archive.
//
// Helper kept ~30 lines + comment inline in daemon.cpp per the C.13 plan;
// extracted to a separate translation unit only if it grows past that.

namespace {

/// Build the orphan archive dir name: `.orphan-<prefix8>-<YYYY-MM-DD_HH-MM>`.
/// The prefix is the resume_token's first 8 hex chars (matches `--evict`).
std::string make_orphan_dir_name(const std::string& token_prefix8) {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min);
    return std::string(".orphan-") + token_prefix8 + "-" + buf;
}

/// Atomic-rename `src_file` into `dst_dir/dst_name`. Returns nullopt on
/// success, an error message on failure. Mirrors the C.11.4 helper in
/// upload_session.cpp (write-tmp + fsync + rename + fsync(dir) + EXDEV
/// fallback). Kept local to daemon.cpp so the orphan-archive path does
/// not pull a header-level dependency on upload_session internals.
std::optional<std::string>
orphan_atomic_rename(const fs::path& src_file,
                     const fs::path& dst_dir,
                     const std::string& dst_name) {
    std::error_code ec;
    fs::create_directories(dst_dir, ec);
    if (ec) {
        return std::string("create_directories(") + dst_dir.string() + "): "
             + ec.message();
    }
    fs::path dst_final = dst_dir / dst_name;
    fs::path dst_tmp = dst_final;
    dst_tmp += ".tmp";
    fs::remove(dst_tmp, ec); ec.clear();  // best-effort

    // Try same-filesystem rename first.
    fs::rename(src_file, dst_tmp, ec);
    if (ec) {
        if (ec != std::errc::cross_device_link) {
            return std::string("rename: ") + ec.message();
        }
        // EXDEV — buffered copy then rename on destination FS.
        ec.clear();
        std::ifstream in(src_file, std::ios::binary);
        if (!in.is_open())
            return std::string("open src for cross-fs copy failed");
        std::ofstream out(dst_tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return std::string("open dst tmp for cross-fs copy failed");
        out << in.rdbuf();
        out.flush();
        if (!out.good() || !in.good()) {
            std::error_code rm; fs::remove(dst_tmp, rm);
            return std::string("cross-fs copy failed mid-stream");
        }
        out.close(); in.close();
        fs::remove(src_file, ec);
        if (ec) {
            log_warn("daemon: orphan archive cross-fs copy succeeded but "
                     "could not remove src %s: %s",
                     src_file.string().c_str(), ec.message().c_str());
            ec.clear();
        }
    }

    // fsync the file before the final rename.
    {
        int fd = ::open(dst_tmp.string().c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { (void)::fsync(fd); (void)::close(fd); }
    }
    fs::rename(dst_tmp, dst_final, ec);
    if (ec) {
        std::error_code rm; fs::remove(dst_tmp, rm);
        return std::string("rename(tmp->final): ") + ec.message();
    }
    // fsync(parent dir) so the rename entry is durable across crash.
    {
        int dfd = ::open(dst_dir.string().c_str(),
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) { (void)::fsync(dfd); (void)::close(dfd); }
    }
    return std::nullopt;
}

/// Best-effort archive of a single WAV (and optional sibling context.json)
/// to `<meetings_root>/.orphan-<prefix8>-<ts>/`. The caller passes the
/// 8-char token prefix and the absolute WAV path; the helper picks the
/// timestamp suffix, makes the dir, moves the files, and logs the outcome.
/// Returns true when at least one file was archived; false when there was
/// nothing to do or every rename failed.
bool archive_orphan_wav(const fs::path& meetings_root,
                        const std::string& token_prefix8,
                        const fs::path& wav_path) {
    if (wav_path.empty()) return false;
    std::error_code ec;
    if (!fs::exists(wav_path, ec)) return false;

    fs::path dst_dir = meetings_root / make_orphan_dir_name(token_prefix8);
    auto err = orphan_atomic_rename(wav_path, dst_dir, "audio.wav");
    if (err) {
        log_warn("daemon: orphan archive failed for wav=%s -> %s: %s",
                 wav_path.string().c_str(), dst_dir.string().c_str(),
                 err->c_str());
        return false;
    }
    log_info("daemon: orphan archive ok wav=%s -> %s/audio.wav",
             wav_path.string().c_str(), dst_dir.string().c_str());

    // Best-effort: also archive a sibling context.json so a future
    // operator-recovery flow has the metadata. Failure here is non-fatal.
    fs::path ctx_src = wav_path.parent_path() / "context.json";
    if (fs::exists(ctx_src, ec)) {
        auto ctx_err = orphan_atomic_rename(ctx_src, dst_dir, "context.json");
        if (ctx_err) {
            log_warn("daemon: orphan archive context.json failed: %s",
                     ctx_err->c_str());
        }
    }
    return true;
}

} // namespace  (Phase 2a — close anon namespace so teardown_orphan_jobs has
  //               external linkage and matches daemon_handlers_internal.h)

/// Per-evicted-session orphan-job teardown body. Shared by the GC sweep
/// and by `admin.evict`. Walks the JobQueue for jobs owned by
/// `client_id`; for each non-terminal job:
///   - Archive the WAV (Postprocess: input.audio_path; Streaming:
///     wav_path_for_job) BEFORE cancel — cancel unlinks the WAV.
///   - Cancel via the JobQueue / streaming-manager path that mirrors
///     process.cancel ordering.
///   - For Postprocess Running with a bound pid that matches
///     g_pp_child_pid, also dispatch SIGTERM. Pid mismatch → cancel-only
///     (C-1 race guard).
/// Returns the list of job_ids that were canceled, for the response
/// payload of `admin.evict` (`owned_jobs_failed`).
std::vector<int64_t>
teardown_orphan_jobs(const std::string& token_prefix8,
                     const std::string& client_id) {
    std::vector<int64_t> canceled;
    if (!g_jobs) return canceled;

    fs::path meetings_root;
    {
        std::lock_guard<std::mutex> lk(g_server_config_mu);
        meetings_root = g_server_config.meetings_root;
    }

    auto owned = g_jobs->list_by_client(client_id);
    for (const auto& job : owned) {
        // Skip terminal — nothing to tear down. Done/Failed/Cancelled
        // artifacts are retained per the 24h diarization-cache TTL.
        if (job.state == JobState::Done ||
            job.state == JobState::Failed ||
            job.state == JobState::Cancelled) {
            continue;
        }
        switch (job.kind) {
        case JobKind::Postprocess: {
            // Archive staging WAV (best-effort; Postprocess jobs always
            // have an audio_path even on the wired meeting-dir path).
            archive_orphan_wav(meetings_root, token_prefix8, job.input.audio_path);
            if (job.state == JobState::WaitingForUpload) {
                if (g_uploads) g_uploads->cancel_by_job_id(job.job_id);
                g_jobs->cancel(job.job_id);
            } else if (job.state == JobState::Running) {
                // C-1 — cancel FIRST so the Cancelled-state guard sticks,
                // THEN signal the child IFF the bound pid matches the
                // current pp_worker child. A mismatch means the slot's
                // current occupant is a different job that replaced ours
                // between the sweep snapshot and this teardown — sending
                // SIGTERM to the wrong job would corrupt unrelated work.
                g_jobs->cancel(job.job_id);
                auto bound_pid = g_jobs->pid_for_running_job(job.job_id);
                pid_t live_pid = g_pp_child_pid.load();
                if (bound_pid.has_value() && *bound_pid == live_pid && live_pid > 0) {
                    g_pp_stop.request();
                    ::kill(live_pid, SIGTERM);
                    log_info("daemon: GC orphan-teardown pp-running job=%ld "
                             "client=%s pid=%d cancel+SIGTERM",
                             (long)job.job_id, client_id.c_str(), (int)live_pid);
                } else {
                    log_info("daemon: GC orphan-teardown pp-running job=%ld "
                             "client=%s — pid mismatch (bound=%d live=%d), "
                             "cancel-only (C-1 guard)",
                             (long)job.job_id, client_id.c_str(),
                             bound_pid.has_value() ? (int)*bound_pid : -1,
                             (int)live_pid);
                }
            } else {
                // Queued / WaitingOnDownload — lazy FIFO removal at next
                // pick / finish_download Cancelled-guard.
                g_jobs->cancel(job.job_id);
            }
            canceled.push_back(job.job_id);
            break;
        }
        case JobKind::Streaming: {
            if (g_streaming) {
                fs::path wav = g_streaming->wav_path_for_job(job.job_id);
                archive_orphan_wav(meetings_root, token_prefix8, wav);
                g_streaming->cancel_by_job_id(job.job_id);
            }
            canceled.push_back(job.job_id);
            break;
        }
        case JobKind::ModelDownload:
            // No WAV; just mark Cancelled. The download worker observes
            // via finish_download's Cancelled guard and any dependents
            // fail cleanly through the existing finish_download path.
            g_jobs->cancel(job.job_id);
            canceled.push_back(job.job_id);
            break;
        case JobKind::_count:
            break;
        }
    }
    if (!canceled.empty()) {
        log_info("daemon: GC orphan-teardown evicted-prefix=%s client=%s "
                 "canceled %zu job(s)",
                 token_prefix8.c_str(), client_id.c_str(), canceled.size());
    }
    return canceled;
}

// ---------------------------------------------------------------------------
// Phase C.13 — GC sweep worker (resume_token TTL eviction)
// ---------------------------------------------------------------------------
static void gc_worker_loop(IpcServer& /*server*/, int interval_minutes) {
    log_debug("daemon: gc_worker_loop ENTER (interval=%dm)", interval_minutes);
    using namespace std::chrono;
    const auto cadence = minutes(interval_minutes > 0 ? interval_minutes : 5);
    while (true) {
        {
            std::unique_lock<std::mutex> lock(g_gc_mu);
            g_gc_cv.wait_for(lock, cadence,
                             []() { return g_gc_shutdown; });
            if (g_gc_shutdown) {
                log_debug("daemon: gc_worker_loop EXIT (shutdown)");
                return;
            }
        }

        if (!g_sessions) continue;  // pre-init defensive

        // (1) Drop expired resume_token bindings. Returns the list of
        // (token, client_id) pairs whose TTL elapsed.
        auto evicted = g_sessions->sweep_expired();
        if (evicted.empty()) continue;

        log_info("daemon: GC sweep evicted %zu session(s)", evicted.size());

        // (2) Per evicted (token, client_id): walk owned jobs and tear them
        // down. Per-kind dispatch + WAV archive lives in
        // `teardown_orphan_jobs` so admin.evict can reuse it verbatim.
        for (const auto& [token, client_id] : evicted) {
            const std::string prefix8 = SessionManager::log_prefix(token);
            (void)teardown_orphan_jobs(prefix8, client_id);
        }
    }
}

// ---------------------------------------------------------------------------
// Postprocessing worker loop (long-lived thread, fork/exec subprocess)
// ---------------------------------------------------------------------------

static void pp_worker_loop(IpcServer& server) {
    log_debug("daemon: pp_worker_loop ENTER (tid=%d)", (int)syscall(SYS_gettid));
    while (true) {
        // Phase C.7: block on the JobQueue's postprocess slot. `dequeue`
        // returns the job already marked Running (slot "running" marker set
        // — the successor of g_postprocessing). It auto-triggers a model
        // download and parks the job internally if a required model is
        // uncached; the daemon's job_event_sink emits `progress.job` with
        // phase `downloading_model` while parked. `dequeue` returns
        // std::nullopt only on shutdown.
        std::optional<Job> dq = g_jobs->dequeue(JobKind::Postprocess);
        if (!dq.has_value()) {
            log_debug("daemon: pp_worker_loop EXIT (shutdown)");
            return;
        }
        PostprocessJob job = std::move(*dq);
        log_debug("daemon: pp_worker dequeued job=%ld (slot_queued=%zu)",
                  (long)job.job_id, g_jobs->queued_count(JobKind::Postprocess));

        // The job is now owned by this worker and the slot's "running"
        // marker is set. C.9 retired the recording→postprocessing handoff
        // bridge (`g_pp_handoff`) along with the legacy record.start path;
        // all v2 producers (process.submit, enroll.finalize, etc.) hand
        // off through JobQueue::enqueue() directly.
        broadcast_state(server);
        g_pp_stop.reset();

        // C.7 job outcome — fed to g_jobs->finish() at clear_state. Default
        // to failure so the early `goto clear_state` paths (pipe/fork
        // failure) report the slot job as Failed; the result-interpretation
        // block below flips this to success or a specific error.
        bool job_ok = false;
        std::string job_err = "postprocessing did not start";

        // Subprocess must always reprocess (never record new audio).
        // The daemon already captured the audio; point the subprocess at it.
        job.cfg.reprocess_dir = job.input.out_dir;

        // Write config to temp file
        log_debug("daemon: writing job config (reprocess_dir=%s)", job.cfg.reprocess_dir.c_str());
        auto config_path = write_job_config(job);

        // Build argv
        std::string out_dir_str = job.input.out_dir.string();
        std::string config_path_str = config_path.string();

        std::vector<std::string> argv_strs = {
            g_self_exe,
            "--reprocess", out_dir_str,
            "--config-json", config_path_str,
            "--progress-json",
            "--no-daemon"
        };
        std::vector<char*> argv_ptrs;
        for (auto& s : argv_strs) argv_ptrs.push_back(s.data());
        argv_ptrs.push_back(nullptr);

        // Create pipes
        int stdout_pipe[2], stderr_pipe[2];
        if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
            log_error("daemon: pipe() failed: %s", strerror(errno));
            notify("Postprocessing failed", "Could not create pipes");
            broadcast_state(server, "pipe() failed");
            std::error_code ec;
            fs::remove(config_path, ec);
            goto clear_state;
        }

        {
            log_debug("daemon: forking subprocess");
            pid_t pid = fork();

            if (pid == 0) {
                // Child — reset signal handlers immediately
                signal(SIGINT, SIG_DFL);
                signal(SIGTERM, SIG_DFL);
                signal(SIGHUP, SIG_DFL);

                close(stdout_pipe[0]);
                close(stderr_pipe[0]);
                dup2(stdout_pipe[1], STDOUT_FILENO);
                dup2(stderr_pipe[1], STDERR_FILENO);
                close(stdout_pipe[1]);
                close(stderr_pipe[1]);

                // Close leaked daemon FDs (pid lock, log, IPC sockets)
                closefrom(3);

                execv(g_self_exe.c_str(), argv_ptrs.data());
                _exit(127);
            }

            if (pid < 0) {
                log_error("daemon: fork() failed: %s", strerror(errno));
                notify("Postprocessing failed", "Could not launch subprocess");
                broadcast_state(server, "fork() failed");
                close(stdout_pipe[0]); close(stdout_pipe[1]);
                close(stderr_pipe[0]); close(stderr_pipe[1]);
                std::error_code ec;
                fs::remove(config_path, ec);
                goto clear_state;
            }

            // Parent
            close(stdout_pipe[1]);
            close(stderr_pipe[1]);
            g_pp_child_pid.store(pid);
            // C.13 — publish the job_id ↔ pid binding so the future GC sweep
            // can verify `pid_for_running_job(victim) == g_pp_child_pid` BEFORE
            // sending SIGTERM (prevents killing an unrelated job that happens
            // to have replaced this one in the single-capacity pp slot by the
            // time the sweep fires). Paired with the unbind() in the reap
            // path below.
            g_jobs->bind_running_pid(job.job_id, pid);
            log_info("daemon: subprocess launched (pid=%d, job=%ld, dir=%s)",
                     (int)pid, (long)job.job_id, out_dir_str.c_str());

            // Per-job progress throttle state
            auto last_broadcast = std::chrono::steady_clock::time_point{};
            int last_percent = -1;
            std::string last_stderr_line;
            auto last_heartbeat = std::chrono::steady_clock::now();
            auto last_progress = last_heartbeat;  // track forward motion separately
            // T1C.1: track most recent phase event so the heartbeat handler
            // can recognize the identify-speakers phase (which has no
            // progress events of its own — sherpa-onnx's embedding extractor
            // exposes no progress callback) and reset last_progress on
            // heartbeat. The 120s heartbeat-staleness watchdog still catches
            // a child that has actually stopped emitting heartbeats.
            std::string last_known_phase;
            bool killed_stale = false;
            std::string captured_note_path;
            std::string captured_output_dir;

            // RSS tracking from heartbeat events — thresholded logging only,
            // no behavior change. The cgroup MemoryMax= in the systemd unit
            // is the real backstop.
            int64_t last_rss_kb = 0;
            int64_t peak_rss_kb = 0;
            auto last_rss_log = std::chrono::steady_clock::time_point{};
            constexpr int64_t RSS_WARN_KB  = 8L * 1024L * 1024L;   //  8 GiB
            constexpr int64_t RSS_ERROR_KB = 9L * 1024L * 1024L + 512L * 1024L;  // 9.5 GiB
            bool warned_rss = false;
            bool errored_rss = false;

            // Poll loop on both pipes
            struct pollfd pfds[2] = {
                {stdout_pipe[0], POLLIN, 0},
                {stderr_pipe[0], POLLIN, 0}
            };
            int nfds = 2;
            std::string stdout_buf, stderr_buf;
            int poll_iter = 0;

            while (nfds > 0) {
                int ret = poll(pfds, nfds, 1000);
                ++poll_iter;

                // Check cancel
                if (g_pp_stop.stop_requested()) {
                    kill(pid, SIGTERM);
                    g_pp_stop.reset();
                }

                // Staleness watchdog — two checks:
                // 1. No events at all for 120s → pipe/process dead
                // 2. No progress/phase for 300s → processing stuck (heartbeat alive but no work)
                if (!killed_stale) {
                    auto now_hb = std::chrono::steady_clock::now();
                    auto hb_stale = std::chrono::duration_cast<std::chrono::seconds>(
                        now_hb - last_heartbeat).count();
                    auto progress_stale = std::chrono::duration_cast<std::chrono::seconds>(
                        now_hb - last_progress).count();
                    if (poll_iter % 30 == 0) {
                        log_debug("daemon: subprocess poll (hb_age=%lds, progress_age=%lds)",
                                  (long)hb_stale, (long)progress_stale);
                    }
                    if (hb_stale >= 120) {
                        log_error("daemon: child stale for %lds (no events), killing pid %d",
                                  (long)hb_stale, (int)pid);
                        killed_stale = true;
                    } else if (progress_stale >= 300) {
                        log_error("daemon: child stuck for %lds (no progress), killing pid %d",
                                  (long)progress_stale, (int)pid);
                        killed_stale = true;
                    }
                    if (killed_stale) {
                        kill_pp_child_with_grace(pid);
                        for (auto& pfd : pfds) {
                            if (pfd.fd >= 0) close(pfd.fd);
                            pfd.fd = -1;
                        }
                        break;
                    }
                }

                if (ret <= 0) continue;

                // Process stdout
                if (pfds[0].fd >= 0 && (pfds[0].revents & (POLLIN | POLLHUP))) {
                    char buf[4096];
                    ssize_t n = read(pfds[0].fd, buf, sizeof(buf));
                    if (n > 0) {
                        stdout_buf.append(buf, n);
                        // Process complete lines
                        size_t pos = 0;
                        size_t nl;
                        while ((nl = stdout_buf.find('\n', pos)) != std::string::npos) {
                            std::string line = stdout_buf.substr(pos, nl - pos);
                            pos = nl + 1;

                            std::string event = parse_ndjson_string(line, "event");
                            // Any event proves pipe is alive
                            last_heartbeat = std::chrono::steady_clock::now();
                            // Phase/progress events prove forward motion
                            if (event == "phase" || event == "progress")
                                last_progress = last_heartbeat;
                            if (event == "heartbeat") {
                                int64_t rss_kb = parse_ndjson_int(line, "rss_kb");
                                if (rss_kb > 0) {
                                    last_rss_kb = rss_kb;
                                    if (rss_kb > peak_rss_kb) peak_rss_kb = rss_kb;
                                    auto now_rss = std::chrono::steady_clock::now();
                                    int since_log_s = std::chrono::duration_cast<std::chrono::seconds>(
                                        now_rss - last_rss_log).count();
                                    bool first_log = (last_rss_log.time_since_epoch().count() == 0);
                                    if (first_log || since_log_s >= 60) {
                                        last_rss_log = now_rss;
                                        log_info("daemon: pp child RSS=%lld MB (peak=%lld MB)",
                                                 (long long)(rss_kb / 1024),
                                                 (long long)(peak_rss_kb / 1024));
                                    }
                                    if (!errored_rss && rss_kb > RSS_ERROR_KB) {
                                        errored_rss = true;
                                        log_error("daemon: pp child RSS=%lld MB exceeds 9.5 GB - approaching MemoryMax cap",
                                                  (long long)(rss_kb / 1024));
                                    } else if (!warned_rss && rss_kb > RSS_WARN_KB) {
                                        warned_rss = true;
                                        log_warn("daemon: pp child RSS=%lld MB exceeds 8 GB - past MemoryHigh soft cap",
                                                 (long long)(rss_kb / 1024));
                                    }
                                }
                                // T1C.1: identify-speakers emits no progress
                                // events of its own (per-cluster blocking
                                // calls into sherpa-onnx with no callback
                                // hook). Treat any heartbeat received during
                                // this phase as a liveness signal and reset
                                // the progress-staleness watchdog. The 120s
                                // heartbeat-staleness watchdog above is
                                // independent and still catches a child that
                                // has stopped emitting heartbeats entirely.
                                if (last_known_phase == "identifying speakers") {
                                    last_progress = last_heartbeat;
                                }
                            } else if (event == "phase") {
                                last_percent = -1;  // Reset throttle on phase change
                                std::string name = parse_ndjson_string(line, "name");
                                last_known_phase = name;  // T1C.1
                                // C.14 — cache the new phase on the registry
                                // entry before fanning out the event so a D.3
                                // reconnect that races the event sees the
                                // updated phase. progress=0 resets the
                                // percentage now that we're in a new phase.
                                g_jobs->update_progress(job.job_id, name, 0);
                                // Phase C.3: route phase events to the owning
                                // client. The Job's `client_id` was captured
                                // when the pp_worker dequeued this job; we
                                // look it up via `client_for_job(job.job_id)`
                                // (the C.7 binding survives the whole job
                                // lifecycle). Empty → broadcast fallback.
                                int64_t jid = job.job_id;
                                server.post([&server, jid, name]() {
                                    IpcEvent ev;
                                    ev.event = "phase";
                                    ev.data["name"] = name;
                                    ev.data["job_id"] = jid;
                                    if (auto cid = g_jobs->client_for_job(jid);
                                        cid && !cid->empty()) {
                                        server.send_to_client(*cid, std::move(ev));
                                    } else {
                                        log_debug("daemon: phase event for "
                                                  "job=%lld has empty/missing "
                                                  "client_id — broadcast fallback",
                                                  (long long)jid);
                                        server.broadcast(ev);
                                    }
                                });
                            } else if (event == "progress") {
                                std::string phase = parse_ndjson_string(line, "phase");
                                int64_t percent = parse_ndjson_int(line, "percent");
                                // C.14 — always cache the latest phase/percent
                                // (independent of the throttled wire emission
                                // below) so a job.status / job.list during the
                                // throttle window still sees the freshest
                                // value. The throttle only gates wire traffic;
                                // the registry cache stays current.
                                g_jobs->update_progress(job.job_id, phase,
                                                        static_cast<int>(percent));
                                // Apply throttle
                                using clock = std::chrono::steady_clock;
                                auto now = clock::now();
                                int elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                    now - last_broadcast).count();
                                int jump = static_cast<int>(percent) - last_percent;
                                if (last_percent < 0 || elapsed >= 120 || jump >= 10) {
                                    last_broadcast = now;
                                    last_percent = static_cast<int>(percent);
                                    // Phase C.3 — same routing pattern as phase.
                                    int64_t jid = job.job_id;
                                    server.post([&server, jid, phase, percent]() {
                                        IpcEvent ev;
                                        ev.event = "progress";
                                        ev.data["phase"] = phase;
                                        ev.data["percent"] = percent;
                                        ev.data["job_id"] = jid;
                                        if (auto cid = g_jobs->client_for_job(jid);
                                            cid && !cid->empty()) {
                                            server.send_to_client(*cid, std::move(ev));
                                        } else {
                                            log_debug("daemon: progress event "
                                                      "for job=%lld has "
                                                      "empty/missing client_id "
                                                      "— broadcast fallback",
                                                      (long long)jid);
                                            server.broadcast(ev);
                                        }
                                    });
                                }
                            } else if (event == "job.complete") {
                                captured_note_path = parse_ndjson_string(line, "note_path");
                                captured_output_dir = parse_ndjson_string(line, "output_dir");
                            } else if (event.empty() && !line.empty()) {
                                log_debug("daemon: subprocess stdout unparseable: %.*s",
                                          (int)std::min(line.size(), (size_t)200), line.c_str());
                            }
                        }
                        stdout_buf.erase(0, pos);
                    } else if (n == 0) {
                        close(pfds[0].fd);
                        pfds[0].fd = -1;
                        pfds[0].events = 0;
                    }
                }

                // Process stderr
                if (pfds[1].fd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP))) {
                    char buf[4096];
                    ssize_t n = read(pfds[1].fd, buf, sizeof(buf));
                    if (n > 0) {
                        stderr_buf.append(buf, n);
                        size_t pos = 0;
                        size_t nl;
                        while ((nl = stderr_buf.find('\n', pos)) != std::string::npos) {
                            std::string line = stderr_buf.substr(pos, nl - pos);
                            pos = nl + 1;
                            if (!line.empty()) {
                                log_info("pp-child: %s", line.c_str());
                                last_stderr_line = line;
                            }
                        }
                        stderr_buf.erase(0, pos);
                    } else if (n == 0) {
                        close(pfds[1].fd);
                        pfds[1].fd = -1;
                        pfds[1].events = 0;
                    }
                }

                // Recount active fds
                nfds = 0;
                for (auto& pfd : pfds) {
                    if (pfd.fd >= 0) ++nfds;
                }
                // Compact: move valid fds to front
                if (nfds == 1 && pfds[0].fd < 0) {
                    pfds[0] = pfds[1];
                    pfds[1].fd = -1;
                }
            }

            // Reap child
            int status;
            waitpid(pid, &status, 0);
            g_pp_child_pid.store(-1);
            // C.13 — clear the pid binding paired with bind_running_pid()
            // above. After this point, a GC sweep that finds the victim still
            // in the registry sees pid_for_running_job → nullopt and falls
            // into the cancel-only path (no SIGTERM dispatch).
            g_jobs->unbind_running_pid(job.job_id);
            if (WIFEXITED(status))
                log_info("daemon: subprocess exited (pid=%d, exit=%d, job=%ld)",
                         (int)pid, WEXITSTATUS(status), (long)job.job_id);
            else if (WIFSIGNALED(status))
                log_info("daemon: subprocess killed (pid=%d, signal=%d, job=%ld)",
                         (int)pid, WTERMSIG(status), (long)job.job_id);

            // Clean up config file
            {
                std::error_code ec;
                fs::remove(config_path, ec);
            }

            // Interpret result
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                job_ok = true;
                job_err.clear();
                int64_t jid = job.job_id;
                bool batch_job = job.cfg.batch_mode;
                bool enroll_job = job.cfg.enroll_mode;

                // Phase C.8 — enroll-mode aftercare. Read the
                // `diarization.json` artifact the subprocess wrote, populate
                // the server-side DiarizationCache so `enroll.finalize` can
                // consume it, and build the speakers[] event payload. On
                // any failure here we log and emit a stripped event — the
                // job still counts as Done (the audio was processed), but
                // the client will see an empty speakers[] and can re-submit.
                std::string speakers_array_json;
                if (enroll_job && g_diar_cache) {
                    fs::path diar_path =
                        fs::path(captured_output_dir).empty()
                            ? job.input.out_dir / "diarization.json"
                            : fs::path(captured_output_dir) / "diarization.json";
                    // Fall back to job's out_dir if the subprocess did not
                    // emit an output_dir field (it does in practice, but
                    // the daemon should be defensive — the artifact lives
                    // in input.out_dir by spec).
                    if (!fs::exists(diar_path))
                        diar_path = job.input.out_dir / "diarization.json";
                    try {
                        auto clusters =
                            DiarizationCache::load_diarization_artifact(diar_path);
                        // Build speakers[] JSON before move-into-cache.
                        speakers_array_json = "[";
                        for (size_t i = 0; i < clusters.size(); ++i) {
                            if (i > 0) speakers_array_json += ",";
                            speakers_array_json +=
                                "{\"idx\":" + std::to_string(clusters[i].idx)
                                + ",\"duration_ms\":"
                                + std::to_string(clusters[i].duration_ms)
                                + "}";
                        }
                        speakers_array_json += "]";
                        g_diar_cache->put(jid, std::move(clusters));
                        log_info("daemon: enroll-mode job=%ld cached "
                                 "diarization (path=%s)", (long)jid,
                                 diar_path.c_str());
                    } catch (const std::exception& e) {
                        log_warn("daemon: enroll-mode job=%ld could not load "
                                 "diarization.json (%s) — emitting empty "
                                 "speakers[]", (long)jid, e.what());
                        speakers_array_json = "[]";
                    }
                }

                // Phase C.3 — route job.complete to the originating client.
                // The Job's `client_id` was bound at enqueue time and the C.7
                // binding is retained even after the job lands in a terminal
                // state, so `client_for_job(jid)` resolves here even though
                // `g_jobs->finish()` may have already been called by the time
                // the posted lambda runs (in practice it has not — finish()
                // happens at `clear_state:` below, after this post). Empty
                // client_id → broadcast fallback so legacy / pre-session jobs
                // still surface a completion event.
                server.post([&server, captured_note_path, captured_output_dir,
                             jid, batch_job, enroll_job,
                             speakers_array_json]() {
                    IpcEvent ev;
                    ev.event = "job.complete";
                    ev.data["job_id"] = jid;
                    ev.data["batch_job"] = batch_job;
                    if (enroll_job) {
                        // Phase C.8 — enroll-mode shape: speakers[] in
                        // place of note_path / output_dir. `enroll_mode`
                        // discriminator lets the thin client switch
                        // handlers off a single event verb.
                        ev.data["enroll_mode"] = true;
                        ev.data["speakers"] = speakers_array_json;
                    } else {
                        ev.data["note_path"] = captured_note_path;
                        ev.data["output_dir"] = captured_output_dir;
                    }
                    if (auto cid = g_jobs->client_for_job(jid);
                        cid && !cid->empty()) {
                        server.send_to_client(*cid, std::move(ev));
                    } else {
                        log_debug("daemon: job.complete for job=%lld has "
                                  "empty/missing client_id — broadcast fallback",
                                  (long long)jid);
                        server.broadcast(ev);
                    }
                });
            } else if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
                // Exit code 2 = cancelled. Leave the slot job's verdict to
                // g_jobs->finish(): if cancel() already marked it Cancelled
                // that verdict is preserved; otherwise it lands as Failed.
                job_ok = false;
                job_err = "cancelled";
                log_info("Postprocessing cancelled for job %ld", (long)job.job_id);
            } else {
                std::string msg;
                if (killed_stale) {
                    msg = "Processing stalled (no progress) — likely onnxruntime deadlock";
                } else if (WIFSIGNALED(status)) {
                    char sigbuf[128];
                    snprintf(sigbuf, sizeof(sigbuf), "Processing crashed (signal %d: %s)",
                             WTERMSIG(status), strsignal(WTERMSIG(status)));
                    msg = sigbuf;
                } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
                    msg = "Failed to launch postprocessing subprocess";
                } else {
                    char exitbuf[64];
                    snprintf(exitbuf, sizeof(exitbuf), "Processing failed (exit %d)",
                             WEXITSTATUS(status));
                    msg = exitbuf;
                    if (!last_stderr_line.empty())
                        msg += ": " + last_stderr_line;
                }
                msg += " — audio preserved at " + job.input.out_dir.string();
                log_error("daemon: %s", msg.c_str());
                notify("Postprocessing failed", msg);
                broadcast_state(server, msg);
                job_ok = false;
                job_err = msg;
            }
        }

    clear_state:
        // Phase C.7: release the postprocess slot. g_jobs->finish() clears the
        // slot's "running" marker (successor of g_postprocessing=false) so the
        // next queued postprocess job can be dequeued; a Cancelled verdict set
        // by cancel() is preserved over `job_ok`.
        g_jobs->finish(job.job_id, job_ok, job_err);
        log_debug("daemon: pp_worker released slot for job=%ld (ok=%d)",
                  (long)job.job_id, job_ok ? 1 : 0);
        broadcast_state(server);
    }
}

// ---------------------------------------------------------------------------
// Phase C.7 — model_download slot worker
// ---------------------------------------------------------------------------
//
// `model_id` convention (logical identifiers carried on JobKind::ModelDownload
// jobs, both auto-triggered and explicit `models.ensure` / `models.update`):
//
//     whisper/<name>        -> ensure_whisper_model / download_whisper_model
//     llama/<name>          -> ensure_llama_model (download not supported)
//     sherpa/diarization    -> ensure_sherpa_models / download_sherpa_models
//     vad                   -> ensure_vad_model / download_vad_model
//
// `force` (Job::force_download) selects ensure_* (no-op if cached) vs
// download_* (always re-fetch — the models.update refresh semantic).
// Per-model atomic-rename on the actual file write lives in
// model_manager.cpp's download_file(); this worker only dispatches.

static void perform_model_download(const std::string& model_id, bool force) {
    if (model_id.rfind("whisper/", 0) == 0) {
        std::string name = model_id.substr(8);
        if (force) download_whisper_model(name);
        else       ensure_whisper_model(name);
        return;
    }
    if (model_id.rfind("llama/", 0) == 0) {
#if RECMEET_USE_LLAMA
        // llama models have no download URL registry — ensure_llama_model
        // resolves a local path or throws. `force` has no effect.
        ensure_llama_model(model_id.substr(6));
#else
        throw RecmeetError("llama support not built in");
#endif
        return;
    }
#if RECMEET_USE_SHERPA
    if (model_id == "sherpa/diarization") {
        if (force) download_sherpa_models();
        else       ensure_sherpa_models();
        return;
    }
    if (model_id == "vad") {
        if (force) download_vad_model();
        else       ensure_vad_model();
        return;
    }
#endif
    throw RecmeetError("Unknown model id: " + model_id);
}

static void model_dl_worker_loop(IpcServer& server) {
    log_debug("daemon: model_dl_worker_loop ENTER (tid=%d)",
              (int)syscall(SYS_gettid));
    while (true) {
        std::optional<Job> dq = g_jobs->dequeue(JobKind::ModelDownload);
        if (!dq.has_value()) {
            log_debug("daemon: model_dl_worker_loop EXIT (shutdown)");
            return;
        }
        Job job = std::move(*dq);
        log_debug("daemon: model_dl_worker dequeued job=%ld model=%s force=%d",
                  (long)job.job_id, job.model_id.c_str(),
                  job.force_download ? 1 : 0);
        broadcast_state(server);   // downloading -> visible in state.changed

        // Phase C.3 — route `model.downloading` to the originating client.
        // The Job's `client_id` is the authoritative routing target (C.7
        // populates it for auto-enqueued downloads as the dependent
        // postprocess job's client_id, and for explicit `models.ensure`
        // requests as the request's client_id). Empty client_id (a daemon-
        // internal download — pre-session or absent session.init) falls
        // back to broadcast so operator visibility is preserved.
        std::string dl_cid = job.client_id;
        auto emit_dl = [&server, dl_cid](const std::string& model,
                                         const std::string& status,
                                         const std::string& error = "") {
            server.post([&server, dl_cid, model, status, error]() {
                IpcEvent ev;
                ev.event = "model.downloading";
                ev.data["model"] = model;
                ev.data["status"] = status;
                if (!error.empty()) ev.data["error"] = error;
                if (!dl_cid.empty()) {
                    server.send_to_client(dl_cid, std::move(ev));
                } else {
                    log_debug("daemon: model.downloading for model=%s has "
                              "empty client_id — falling back to broadcast",
                              model.c_str());
                    server.broadcast(ev);
                }
            });
        };

        bool ok = true;
        std::string err;
        emit_dl(job.model_id, "downloading");
        try {
            perform_model_download(job.model_id, job.force_download);
            emit_dl(job.model_id, "complete");
        } catch (const std::exception& e) {
            ok = false;
            err = e.what();
            log_error("daemon: model download failed (%s): %s",
                      job.model_id.c_str(), err.c_str());
            emit_dl(job.model_id, "error", err);
        }

        // finish_download() clears the model_download slot AND re-arms (or
        // fails) every postprocess job parked on this download — the
        // cross-slot dependency resolution.
        g_jobs->finish_download(job.job_id, ok, err);
        log_debug("daemon: model_dl_worker released slot for job=%ld (ok=%d)",
                  (long)job.job_id, ok ? 1 : 0);
        broadcast_state(server);
    }
}

// ---------------------------------------------------------------------------
// Phase C.10a — streaming-slot worker loop
//
// Unlike the postprocess / model_download workers this loop does NOT perform
// the work itself — a streaming job's "work" is the per-session CaptionEngine
// worker thread plus the poll-thread `0x03`-frame feed path, both owned by
// the StreamingSessionManager. This loop exists only to drain the JobQueue
// `streaming` slot: `dequeue(Streaming)` flips the slot's "running" marker so
// `slot_busy(JobKind::Streaming)` is authoritative (the C.7 typed-slot
// invariant), and broadcasts the state change. The session's lifetime then
// belongs to the manager; the slot is released when the manager calls
// `JobQueue::finish()` / `JobQueue::cancel()` on the job (process.stream.cancel
// or a client disconnect). After dequeuing, the loop blocks again in
// `dequeue()` until the slot is free AND a new streaming job is queued, or
// shutdown wakes it with std::nullopt.
// ---------------------------------------------------------------------------

static void stream_worker_loop(IpcServer& server) {
    log_debug("daemon: stream_worker_loop ENTER (tid=%d)",
              (int)syscall(SYS_gettid));
    while (true) {
        std::optional<Job> dq = g_jobs->dequeue(JobKind::Streaming);
        if (!dq.has_value()) {
            log_debug("daemon: stream_worker_loop EXIT (shutdown)");
            return;
        }
        log_debug("daemon: stream_worker dequeued streaming job=%ld "
                  "(slot running marker now set)", (long)dq->job_id);
        // The slot's "running" marker is set; the StreamingSessionManager
        // owns the session from here. Reflect the new state and loop back to
        // dequeue() — which will block until finish()/cancel() releases the
        // slot and the next process.stream enqueues a job.
        broadcast_state(server);
    }
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage() {
    fprintf(stderr,
        "Usage: recmeet-daemon [OPTIONS]\n"
        "\n"
        "Run the recmeet daemon (IPC server for CLI and tray clients).\n"
        "\n"
        "Options:\n"
        "  --socket PATH       Unix socket path (default: $XDG_RUNTIME_DIR/recmeet/daemon.sock)\n"
        "  --listen ADDRESS    Listen address: Unix path or host:port for TCP\n"
        "  --log-level LEVEL   Log level: none, error, warn, info, debug (default: info)\n"
        "  --log-dir DIR       Log file directory\n"
        "  --log-retention N   Log retention in hours (default: 4)\n"
        "  --evict PREFIX      Operator session-revocation: connect to the running\n"
        "                      daemon and evict the resume_token matching PREFIX\n"
        "                      (8+ hex chars). Exits without starting a new daemon.\n"
        "  --check-backends    Print the ggml active-backend banner (CPU / Vulkan /\n"
        "                      HIP / CUDA / Metal) and exit 0, BEFORE opening any\n"
        "                      socket or starting workers. For operators running\n"
        "                      under systemd who want a one-shot GPU-vs-CPU answer\n"
        "                      without parsing journalctl.\n"
        "  -h, --help          Show this help\n"
        "  -v, --version       Show version\n"
    );
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::string socket_path = default_socket_path();
    std::string log_level_str = "info";
    fs::path log_dir;
    int log_retention_hours = 4;

    // Env var override (between default and CLI)
    if (const char* env = std::getenv("RECMEET_LOG_LEVEL"))
        log_level_str = env;

    // Phase C.13 — `--evict` is a CLI-mode flag, not a daemon-mode flag:
    // when present, this invocation acts as a client talking to the live
    // daemon, dispatches `admin.evict`, prints the result, and exits BEFORE
    // any pid-lock or listener setup. Parsed here as a separate string so
    // the early-dispatch branch below can fire without re-walking argv.
    std::string evict_prefix;

    // Phase E.4.1 — `--check-backends` is a diagnostic-mode flag: emit the
    // `ggml: active backend:` banner (load_backends + log_backend_summary
    // from src/backend_info.cpp) to stdout and exit 0 BEFORE any pid-lock,
    // socket bind, or worker thread. Lets operators answer "is the daemon
    // using my GPU?" without grepping journalctl. Mirrors the early-exit
    // pattern of --version and --help.
    bool check_backends = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-h" || arg == "--help")) { print_usage(); return 0; }
        if ((arg == "-v" || arg == "--version")) { printf("recmeet-daemon %s\n", RECMEET_VERSION); return 0; }
        if (arg == "--socket" && i + 1 < argc) { socket_path = argv[++i]; continue; }
        if (arg == "--listen" && i + 1 < argc) { socket_path = argv[++i]; continue; }
        if (arg == "--log-level" && i + 1 < argc) { log_level_str = argv[++i]; continue; }
        if (arg == "--log-dir" && i + 1 < argc) { log_dir = argv[++i]; continue; }
        if (arg == "--log-retention" && i + 1 < argc) { log_retention_hours = std::atoi(argv[++i]); continue; }
        // Phase C.13 — `--evict <prefix>` forces immediate eviction of a
        // resume_token by 8+ hex-char prefix. CLI-mode: talks to the live
        // daemon, NEVER acts as the daemon. See operator workflow guide in
        // agentctx/tasks/thin-client-recording-server.md C.13 body.
        if (arg == "--evict" && i + 1 < argc) { evict_prefix = argv[++i]; continue; }
        // Phase E.4.1 — diagnostic, no value.
        if (arg == "--check-backends") { check_backends = true; continue; }
        fprintf(stderr, "Unknown option: %s\n", arg.c_str());
        return 1;
    }

    // Phase E.4.1 — `--check-backends` early-exit: discover the runtime
    // ggml plugins, print the active-backend banner, exit 0. Logger is
    // intentionally NOT initialized (log_init never called → g_level is
    // NONE → log_info() inside banner_emit() is a no-op), so the only
    // surviving sink is backend_info.cpp's `fprintf(stderr, ...)`. We
    // dup2 stdout over stderr first so the banner lands on stdout per the
    // operator-facing contract (script-friendly capture: `recmeet-daemon
    // --check-backends | grep 'active backend'`).
    if (check_backends) {
        ::fflush(stderr);
        if (::dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
            // dup2 should never fail on healthy stdio; fall through to
            // backend_info's default stderr sink rather than abort.
            fprintf(stderr, "warning: dup2(stdout->stderr) failed: %s\n",
                    strerror(errno));
        }
        load_backends();
        log_backend_summary();
        ::fflush(stdout);
        ::fflush(stderr);
        return 0;
    }

    // Phase C.13 — `--evict` CLI dispatch. Mirrors the client_status /
    // client_stop pattern from src/main.cpp:143-176. Runs BEFORE pid-lock
    // acquire because the live daemon already holds it (this invocation is
    // not a second daemon — it is a one-shot client). Unix-socket peer-
    // credential trust gates the verb (the daemon's admin.evict handler
    // has no special privilege check; the security boundary is the socket
    // itself per src/ipc_server.cpp:501-513).
    if (!evict_prefix.empty()) {
        // Use the same listen address resolution the daemon itself would —
        // empty `socket_path` means default Unix socket (default_socket_path()).
        IpcClient client(socket_path);
        if (!client.connect()) {
            fprintf(stderr, "Daemon not running (cannot reach %s)\n",
                    socket_path.empty() ? "default socket" : socket_path.c_str());
            return 1;
        }
        IpcResponse resp;
        IpcError err;
        JsonMap params;
        params["prefix"] = evict_prefix;
        if (!client.call("admin.evict", params, resp, err)) {
            fprintf(stderr, "Evict failed: %s\n", err.message.c_str());
            return 1;
        }
        printf("Evicted session\n");
        printf("  token:     %s\n",
               json_val_as_string(resp.result["evicted"], "<missing>").c_str());
        printf("  client_id: %s\n",
               json_val_as_string(resp.result["client_id"], "<missing>").c_str());
        printf("  jobs:      %s\n",
               json_val_as_string(resp.result["owned_jobs_failed"], "[]").c_str());
        return 0;
    }

    // PID file with flock() to prevent duplicate daemons
    // For TCP addresses, use a fixed path under XDG_RUNTIME_DIR
    IpcAddress listen_addr;
    parse_ipc_address(socket_path, listen_addr);
    std::string pid_path;
    if (listen_addr.transport == IpcTransport::Tcp) {
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        std::string pid_dir_str = runtime
            ? std::string(runtime) + "/recmeet"
            : "/tmp/recmeet-" + std::to_string(getuid());
        mkdir(pid_dir_str.c_str(), 0700);
        pid_path = pid_dir_str + "/daemon-tcp.pid";
    } else {
        pid_path = socket_path + ".pid";
        std::string pid_dir = pid_path.substr(0, pid_path.rfind('/'));
        if (!pid_dir.empty()) mkdir(pid_dir.c_str(), 0700);
    }
    int pid_fd = open(pid_path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (pid_fd >= 0) {
        if (flock(pid_fd, LOCK_EX | LOCK_NB) < 0) {
            fprintf(stderr, "Another recmeet-daemon is already running.\n");
            close(pid_fd);
            return 1;
        }
        if (ftruncate(pid_fd, 0) < 0)
            fprintf(stderr, "warning: ftruncate pid file: %s\n", strerror(errno));
        std::string pid_str = std::to_string(getpid()) + "\n";
        if (write(pid_fd, pid_str.data(), pid_str.size()) < 0)
            fprintf(stderr, "warning: write pid file: %s\n", strerror(errno));
        // Keep fd open (holds the lock until process exits)
    }

    // Initialize logging (daemon always logs to stderr — journald or interactive)
    auto log_level = parse_log_level(log_level_str);
    log_init(log_level, log_dir, log_retention_hours, true);
    log_info("daemon: starting (socket=%s)", socket_path.c_str());

    // Discover runtime-loadable ggml backends (libggml-vulkan.so / libggml-cpu-*.so
    // installed alongside the binary) and surface which device the daemon will
    // use, so `journalctl --user -u recmeet-daemon.service` answers the GPU-or-CPU
    // question without an `ldd` round-trip. See agentctx/tasks/runtime-loadable-
    // gpu-backends.md (Step 4) and auto-detect-vulkan-backend.md (Step 5).
    load_backends();
    log_backend_summary();

    // Suppress whisper output
    whisper_log_set(whisper_null_log, nullptr);

    // Load config — Phase E.2 W2.2b. The server snapshot is the only
    // daemon-global config now; per-job JobConfig is assembled at each
    // enqueue seam via `make_job_config_with_real_env`.
    //
    // Legacy config.yaml → daemon.yaml + client.yaml migration runs first
    // (no-op once daemon.yaml exists). This keeps single-file legacy
    // installs (and tests that stage a config.yaml) working transparently.
    migrate_legacy_config_if_present();
    {
        std::lock_guard<std::mutex> lk(g_server_config_mu);
        g_server_config = load_server_config();
    }
    // Note: env-resolved API keys are now resolved per-job inside
    // `make_job_config()` from the requesting session's credentials and
    // the daemon's env, not at daemon startup. The daemon does not pick
    // "the one provider" anymore — each session picks its own.

    // Phase C.7 — construct the typed-slot JobQueue. Slot capacities come
    // from `[server] slot.*` (all default 1); the auto-download machinery
    // (ModelResolver / ModelCacheChecker / JobEventSink) is wired below,
    // after the IpcServer exists (the event sink captures it).
    {
        JobQueue::SlotCapacities caps;
        caps.postprocess = g_server_config.slot_postprocess;
        caps.streaming = g_server_config.slot_streaming;
        caps.model_download = g_server_config.slot_model_download;
        g_jobs = std::make_unique<JobQueue>(caps);
        log_info("daemon: JobQueue slots — postprocess=%d streaming=%d "
                 "model_download=%d",
                 caps.postprocess, caps.streaming, caps.model_download);
    }

    // Phase C.8 — construct the diarization cache. TTL comes from
    // `[server] diarization_cache_ttl_secs`, default 24h. In-memory only,
    // by design (see diarization_cache.h header for the rationale).
    {
        g_diar_cache = std::make_unique<DiarizationCache>(
            g_server_config.diarization_cache_ttl_secs);
        log_info("daemon: diarization cache ready (ttl=%lld s)",
                 (long long)g_server_config.diarization_cache_ttl_secs);
    }

    // Phase C.11.4 — construct the meeting-id index BEFORE the upload /
    // streaming managers are wired so they can pass it into their
    // constructors. The startup `rebuild_from_disk` walks
    // `g_server_config.meetings_root` once, reads each meeting dir's
    // `context.json`, and repopulates the map. Cost amortizes against
    // startup; no on-disk index file in v1.
    {
        g_meeting_index = std::make_unique<MeetingIndex>();
        std::size_t n = g_meeting_index->rebuild_from_disk(g_server_config.meetings_root);
        log_info("daemon: meeting index ready (rebuilt %zu binding%s from %s)",
                 n, n == 1 ? "" : "s", g_server_config.meetings_root.string().c_str());
    }

    // Phase C.13 — construct the resume_token store. In-memory only (MC-1);
    // daemon restart invalidates all tokens. TTL is the consolidated knob
    // `[server] retain_terminal_hours` (M-1); the config loader also
    // derives `diarization_cache_ttl_secs` from the same value so the two
    // coupled lifetimes track together. Clock seam (L-3) defaults to
    // std::chrono::system_clock; tests inject a fake.
    {
        const int64_t ttl_s =
            static_cast<int64_t>(g_server_config.retain_terminal_hours) * 3600;
        g_sessions = std::make_unique<SessionManager>(ttl_s);
        log_info("daemon: session manager ready (TTL=%dh, in-memory only — "
                 "restart invalidates all tokens)",
                 g_server_config.retain_terminal_hours);
    }

    notify_init();

    // Resolve path to recmeet binary for subprocess postprocessing
    {
        std::error_code ec;
        auto self = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            auto sibling = self.parent_path() / "recmeet";
            if (fs::exists(sibling, ec))
                g_self_exe = sibling.string();
        }
        if (g_self_exe.empty())
            g_self_exe = "recmeet";
    }

    // Create server
    IpcServer server(socket_path);
    g_server = &server;

    // Phase C.13 — wire the resume_token resolver. Called from
    // ipc_server.cpp's handle_pending_psk immediately after the PSK check
    // passes, with the `resume_token` field the client supplied on
    // auth.token (empty when the client sent no field). The resolver
    // owns the "resume an existing session OR mint fresh" decision:
    //   - Non-empty provided token + resolve() hit → reuse prior
    //     client_id; echo the SAME token back (client keeps using it).
    //   - Empty / unknown / expired → mint fresh client_id via the
    //     IpcServer's mint_client_id() + fresh resume_token via
    //     SessionManager::mint(). Client overwrites its persisted token.
    //
    // Per H-2: creds/prefs are NOT restored here — those live on
    // ClientState only and were dropped at the prior disconnect. The
    // resumed client MUST re-send session.init.
    //
    // Per L-1: only the 8-char prefix of any resume_token reaches log_*.
    server.set_resume_token_resolver(
        [&server](const std::string& provided_token)
            -> std::pair<std::string, std::string> {
        if (g_sessions && !provided_token.empty()) {
            if (auto cid = g_sessions->resolve(provided_token); cid) {
                log_info("daemon: resume_token RESUME (prefix=%s client=%s)",
                         SessionManager::log_prefix(provided_token).c_str(),
                         cid->c_str());
                return {*cid, provided_token};
            }
            log_info("daemon: resume_token unknown/expired (prefix=%s) — "
                     "falling through to fresh-mint",
                     SessionManager::log_prefix(provided_token).c_str());
        }
        std::string fresh_cid = server.mint_client_id();
        std::string fresh_token = g_sessions
            ? g_sessions->mint(fresh_cid)
            : std::string();
        log_info("daemon: resume_token FRESH (prefix=%s client=%s)",
                 SessionManager::log_prefix(fresh_token).c_str(),
                 fresh_cid.c_str());
        return {fresh_cid, fresh_token};
    });

    // Phase C.13 (M-4) — wire the per-handler last_seen bump. The
    // IpcServer dispatch site stamps this on every inbound IPC request
    // (skipping outbound events). One-line uniform coverage of every
    // verb. The token came from the auth.token handshake and is
    // persisted on ClientState.resume_token; we just forward it to the
    // session map. Silently no-ops on unknown tokens (which can happen
    // briefly if a SIGHUP-driven PSK rotation arrives mid-request — the
    // rotation itself doesn't invalidate the running connection's
    // resume_token binding, but a daemon-restart scenario would).
    server.set_request_dispatch_hook(
        [](const std::string& resume_token) {
            if (g_sessions) g_sessions->bump_last_seen(resume_token);
        });

    // Phase C.7 — wire the JobQueue auto-download machinery now that the
    // IpcServer exists (the JobEventSink captures it). The ModelResolver
    // enumerates the models a postprocess job requires (mirroring the old
    // ensure_models() decision logic); the ModelCacheChecker is the
    // model_manager `is_*_cached` helpers lifted to job-dequeue time; the
    // JobEventSink emits `progress.job` — in particular phase
    // `downloading_model` while a postprocess job is parked on a download.

    // ModelCacheChecker — returns true when a logical model_id is on disk.
    g_jobs->set_model_cache_checker([](const std::string& model_id) -> bool {
        try {
            if (model_id.rfind("whisper/", 0) == 0)
                return is_whisper_model_cached(model_id.substr(8));
#if RECMEET_USE_SHERPA
            if (model_id == "sherpa/diarization")
                return is_sherpa_model_cached();
            if (model_id == "vad")
                return is_vad_model_cached();
#endif
            // llama models have no cache-probe registry — ensure_llama_model
            // resolves a local path or throws. Treat as "cached" so we never
            // auto-enqueue a download that perform_model_download can't do.
            if (model_id.rfind("llama/", 0) == 0)
                return true;
        } catch (const std::exception& e) {
            log_warn("job_queue cache check failed for '%s': %s",
                     model_id.c_str(), e.what());
        }
        return true;  // unknown id — don't block the job on a phantom download.
    });

    // ModelResolver — enumerates the models a postprocess job requires.
    g_jobs->set_model_resolver([](const Job& job) -> std::vector<std::string> {
        std::vector<std::string> needed;
        if (job.kind != JobKind::Postprocess) return needed;
        const JobConfig& c = job.cfg;
        // Transcription needs the whisper model unless the recording phase
        // already produced transcript text.
        if (job.input.transcript_text.empty() && !c.whisper_model.empty())
            needed.push_back("whisper/" + c.whisper_model);
#if RECMEET_USE_SHERPA
        if (c.diarize) needed.push_back("sherpa/diarization");
        if (c.vad)     needed.push_back("vad");
#endif
        return needed;
    });

    // JobEventSink — emit `progress.job` for the notable transitions.
    // Phase C.3: route to the originating client via the C.7 binding. The
    // JobEventSink fires synchronously from JobQueue mutator paths; we
    // capture `job.client_id` here on the calling thread (NOT from the
    // posted lambda, which can race a `cancel`/`finish` that erases the
    // job entry on certain transitions). The capture is the authoritative
    // routing target — empty client_id (a pre-session or daemon-internal
    // job: e.g. a `models.ensure` triggered before the connection's
    // session.init landed) falls back to broadcast so the event still
    // reaches some operator-visible surface rather than disappearing.
    g_jobs->set_job_event_sink([&server](const Job& job) {
        std::string phase;
        switch (job.state) {
            case JobState::WaitingOnDownload: phase = "downloading_model"; break;
            case JobState::Queued:            phase = "resumed";          break;
            case JobState::Done:              phase = "done";             break;
            case JobState::Failed:            phase = "failed";           break;
            case JobState::Cancelled:         phase = "cancelled";        break;
            default:                          return;  // Running etc — no event.
        }
        int64_t jid = job.job_id;
        std::string kind = job_kind_name(job.kind);
        std::string model = job.model_id;
        std::string errmsg = job.error;
        std::string cid = job.client_id;
        server.post([&server, jid, phase, kind, model, errmsg, cid]() {
            IpcEvent ev;
            ev.event = "progress.job";
            ev.data["job_id"] = jid;
            ev.data["kind"] = kind;
            ev.data["phase"] = phase;
            if (!model.empty()) ev.data["model"] = model;
            if (!errmsg.empty()) ev.data["error"] = errmsg;
            if (!cid.empty()) {
                server.send_to_client(cid, std::move(ev));
            } else {
                log_debug("daemon: progress.job for job=%lld has empty "
                          "client_id — falling back to broadcast",
                          (long long)jid);
                server.broadcast(ev);
            }
        });
    });

    // Phase C.10a — construct the StreamingSessionManager. The caption event
    // sink marshals CaptionEngine worker-thread callbacks onto the IPC poll
    // thread (via server.post()) and emits `caption` / `caption.degraded`
    // events — exactly as the legacy run_recording() caption path did.
    //
    // Phase C.3 routes each event to the originating client via
    // `send_to_client(client_id, ev)` rather than `broadcast()`. The
    // `client_id` is delivered by the sink (the session owns it), so we do
    // NOT round-trip through `JobQueue::client_for_job(job_id)` — same
    // result, one fewer lock acquisition on every caption emission. An
    // empty `client_id` (defensive — should not occur for streaming because
    // process.stream requires a populated `req.client_id`) falls back to
    // broadcast() so the event still becomes visible.
    {
        StreamingCaptionSink sink;
        sink.on_caption = [&server](int64_t job_id,
                                    const std::string& client_id,
                                    const std::string& text,
                                    bool is_partial, int64_t ts) {
            // Fires on the CaptionEngine worker thread — marshal onto the
            // poll thread before touching server (not thread-safe).
            std::string t = text;
            std::string cid = client_id;
            server.post([&server, job_id, cid, t, is_partial, ts]() {
                IpcEvent ev = make_caption_event(job_id, t, is_partial, ts);
                if (!cid.empty()) {
                    server.send_to_client(cid, std::move(ev));
                } else {
                    log_debug("daemon: caption event for job=%lld has empty "
                              "client_id — falling back to broadcast",
                              (long long)job_id);
                    server.broadcast(ev);
                }
            });
        };
        sink.on_degraded = [&server](int64_t job_id,
                                     const std::string& client_id,
                                     const std::string& reason,
                                     int64_t ts) {
            std::string r = reason;
            std::string cid = client_id;
            server.post([&server, job_id, cid, r, ts]() {
                IpcEvent ev = make_caption_degraded_event(job_id, r, ts);
                if (!cid.empty()) {
                    server.send_to_client(cid, std::move(ev));
                } else {
                    log_debug("daemon: caption.degraded for job=%lld has "
                              "empty client_id — falling back to broadcast",
                              (long long)job_id);
                    server.broadcast(ev);
                }
            });
        };
        std::string model_dir;
        {
            std::lock_guard<std::mutex> lock(g_server_config_mu);
            model_dir = resolve_caption_model_dir(g_server_config.caption_model).string();
        }
        g_streaming = std::make_unique<StreamingSessionManager>(
            *g_jobs, sink, model_dir,
            g_meeting_index.get(),    // C.11.4 — convergence-principle dedup
            g_server_config.meetings_root);
        log_info("daemon: streaming session manager ready (caption_model_dir=%s, "
                 "meetings_root=%s)",
                 model_dir.c_str(), g_server_config.meetings_root.string().c_str());
    }

    // Phase C.2 — construct the UploadSessionManager. The progress sink
    // mirrors C.10a's caption-event sink: it marshals onto the poll thread
    // via server.post() and emits a `progress.job` event with phase
    // `uploading` so the tray can render an upload progress bar.
    //
    // Phase C.3 routes each event to the originating client via
    // `send_to_client(client_id, ev)`. The sink already carries the
    // upload session's `client_id` (UploadSession owns it), so we route
    // directly rather than re-looking-up through
    // `JobQueue::client_for_job(job_id)`. An empty client_id (defensive —
    // should not occur because process.submit requires a populated
    // `req.client_id`) falls back to broadcast.
    {
        UploadProgressSink upsink;
        upsink.on_progress = [&server](int64_t job_id,
                                       const std::string& client_id,
                                       int64_t bytes_received,
                                       int64_t audio_size) {
            // C.14 — cache phase="uploading" + percentage on the job registry
            // so a D.3 re-sync mid-upload reports the same value as the
            // throttled-or-not progress.job event below. audio_size==0 (the
            // "unknown total" case for streams) maps to progress=0 — no way
            // to compute a meaningful percentage without a known total.
            if (g_jobs) {
                int pct = 0;
                if (audio_size > 0) {
                    int64_t computed = (bytes_received * 100) / audio_size;
                    pct = static_cast<int>(computed > 100 ? 100 : computed);
                }
                g_jobs->update_progress(job_id, "uploading", pct);
            }
            // `feed_chunk` already runs on the poll thread (the binary-frame
            // handler), so we could route inline. We still go through
            // server.post() to keep the dispatch lane uniform with the rest
            // of the daemon's event emission and to avoid taking the manager
            // mutex while building the event (the sink fires AFTER the
            // mutex is released; this just keeps the post-thread invariant
            // explicit).
            std::string cid = client_id;
            server.post([&server, job_id, cid, bytes_received, audio_size]() {
                IpcEvent ev;
                ev.event = "progress.job";
                ev.data["job_id"] = job_id;
                ev.data["kind"] = std::string("postprocess");
                ev.data["phase"] = std::string("uploading");
                ev.data["bytes_received"] = bytes_received;
                ev.data["bytes_total"]    = audio_size;
                if (!cid.empty()) {
                    server.send_to_client(cid, std::move(ev));
                } else {
                    log_debug("daemon: upload progress.job for job=%lld has "
                              "empty client_id — falling back to broadcast",
                              (long long)job_id);
                    server.broadcast(ev);
                }
            });
        };
        // Staging root: empty falls back to system temp at create-time, which
        // mirrors the streaming-session policy. A future op-tunable knob
        // could expose this via `[server] upload_staging_root`; deferred.
        g_uploads = std::make_unique<UploadSessionManager>(
            *g_jobs, /*staging_root=*/fs::path{}, std::move(upsink),
            g_meeting_index.get(),    // C.11.4 — convergence-principle dedup
            g_server_config.meetings_root);
        log_info("daemon: upload session manager ready (meetings_root=%s)",
                 g_server_config.meetings_root.string().c_str());
    }

    // Phase C.10a — route inbound `0x03` streaming-audio frames into the
    // matching streaming session. The binary-frame handler runs on the poll
    // thread; it is the migrated *producer* side of the per-session
    // CaptionEngine (the audio used to come from a PipeWire Capture callback;
    // now it arrives as network frames). Returning false tears the
    // connection down — a `0x03` frame with no live session (unknown
    // stream_token) is a protocol violation.
    server.on_binary_frame([](const std::string& client_id,
                              FrameType type,
                              const std::string& payload) -> bool {
        if (type == FrameType::StreamAudio) {
            // `0x03` streaming audio. The wire framing carries no stream_token
            // — it is implicit: the capacity-1 streaming slot means a client
            // has at most one live streaming session, and the binary-frame
            // handler already knows the originating client_id. Route by
            // client_id to that client's session token.
            if (!g_streaming) return false;
            std::string token = g_streaming->token_for_client(client_id);
            if (token.empty()) {
                log_warn("daemon: 0x03 streaming frame from client=%s with no "
                         "live streaming session — protocol violation",
                         client_id.c_str());
                return false;
            }
            return g_streaming->feed_audio(token, payload);
        }
        if (type == FrameType::BinaryUpload) {
            // Phase C.2 — `0x01` postprocess-upload payload. The wire framing
            // carries no upload_token; it is implicit by client_id (the
            // capacity-1 postprocess-upload invariant means a client has at
            // most one outstanding upload). `feed_chunk` returns false on
            // protocol violations: unknown client, bytes-overflow vs
            // declared audio_size, short staging-write — any of which
            // should tear the connection down.
            if (!g_uploads) return false;
            return g_uploads->feed_chunk(client_id, payload);
        }
        // C.4 will consume 0x02 (artifact download); until then discard
        // without closing the connection — NDJSON traffic keeps flowing.
        log_debug("daemon: binary frame type=0x%02x from client=%s — "
                  "no handler (discarded)",
                  static_cast<unsigned>(type), client_id.c_str());
        return true;
    });

    // Phase C.10a — on client disconnect, abort any streaming session the
    // client owned: mark the JobQueue job failed, discard the ASR session,
    // unlink the temp WAV, release the streaming slot. Runs inline on the
    // poll thread from remove_client().
    server.on_client_disconnect([&server](const std::string& client_id) {
        // Both the streaming and upload managers need a disconnect callback:
        // a TCP drop must abort any in-flight session the client owned (a
        // streaming-caption session AND/OR a postprocess upload). We
        // ALWAYS call both — a client could legitimately have one of each
        // (one streaming + one upload, since they live in different slots).
        int stream_aborted = 0;
        int upload_aborted = 0;
        if (g_streaming) stream_aborted = g_streaming->on_client_disconnect(client_id);
        if (g_uploads)   upload_aborted = g_uploads->on_client_disconnect(client_id);
        if (stream_aborted > 0)
            log_info("daemon: aborted %d streaming session(s) for "
                     "disconnected client=%s", stream_aborted, client_id.c_str());
        if (upload_aborted > 0)
            log_info("daemon: aborted %d upload session(s) for "
                     "disconnected client=%s", upload_aborted, client_id.c_str());
        if (stream_aborted > 0 || upload_aborted > 0)
            broadcast_state_inline(server);
    });

    // Phase A.2: apply the NDJSON line cap from config before start().
    // Default is 8 MB; a daemon.yaml override via `[ipc] max_message_bytes`
    // is already resolved in g_server_config by load_server_config().
    server.set_max_message_bytes(g_server_config.max_message_bytes);

    // Phase A.3: apply the connection cap from config before start() so
    // the listen backlog (max_clients * 2) is sized off the configured
    // value rather than the struct default. Default is 16 / backlog 32;
    // override via `[ipc] max_clients` in daemon.yaml.
    server.set_max_clients(g_server_config.max_clients);

    // Phase A.1 PSK gate: TCP listeners require RECMEET_AUTH_TOKEN. Unix
    // listeners trust the kernel's peer credentials and skip the PSK check.
    if (listen_addr.transport == IpcTransport::Tcp) {
        const char* token_env = std::getenv("RECMEET_AUTH_TOKEN");
        std::string token = token_env ? token_env : "";
        if (token.empty()) {
            fprintf(stderr,
                    "recmeet-daemon: refusing to start TCP listener without "
                    "RECMEET_AUTH_TOKEN set.\n"
                    "Set RECMEET_AUTH_TOKEN to a strong shared secret, or use "
                    "--listen with a Unix socket path for local-only mode.\n");
            log_error("daemon: refusing TCP startup — RECMEET_AUTH_TOKEN unset");
            return 1;
        }
        server.set_psk(token);
        log_info("daemon: PSK auth enabled for TCP listener");
    }

    // --- Method handlers (extracted to daemon_handlers.cpp; Phase 2a) ---

    register_daemon_handlers(server);

    // --- Start server ---

    if (!server.start()) {
        fprintf(stderr, "Failed to start daemon on %s\n", socket_path.c_str());
        log_shutdown();
        notify_cleanup();
        return 1;
    }

    // Start the long-lived slot workers: one drains the JobQueue's
    // postprocess slot, one drains its model_download slot. Both block in
    // JobQueue::dequeue() until shutdown() wakes them with std::nullopt.
    g_pp_worker = std::thread([&server]() { pp_worker_loop(server); });
    g_dl_worker = std::thread([&server]() { model_dl_worker_loop(server); });
    // Phase C.10a — streaming-slot worker. Drains the JobQueue `streaming`
    // slot so its "running" marker is authoritative; the session lifetime
    // itself belongs to g_streaming.
    g_stream_worker = std::thread([&server]() { stream_worker_loop(server); });
    // Phase C.13 — GC sweep worker. SCAFFOLD: spawns thread + wakes
    // periodically + invokes g_sessions->sweep_expired(); orphan-job
    // teardown deferred to the C.13 implementation pass. Hard-coded 5-min
    // cadence; impl pass reads g_server_config.gc_interval_minutes.
    g_gc_worker = std::thread([&server]() { gc_worker_loop(server, 5); });

    // Signal handlers
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);

    log_info("daemon: listening on %s", socket_path.c_str());
    fprintf(stderr, "recmeet-daemon %s listening on %s\n", RECMEET_VERSION, socket_path.c_str());
    log_debug("daemon: entering event loop");

    server.run();

    // Cleanup — shut down all workers. C.9 retired the rec_worker (legacy
    // record.start path); only pp_worker / dl_worker / stream_worker remain.
    log_info("daemon: shutting down");
    g_pp_stop.request();
    { pid_t child = g_pp_child_pid.load(); if (child > 0) kill(child, SIGTERM); }

    // Shut down the JobQueue: wakes every dequeue() caller (pp_worker and
    // model_dl_worker) with std::nullopt so both slot workers exit their
    // loops. Successor of the pre-C.7 g_queue_shutdown + g_queue_cv model.
    if (g_jobs) g_jobs->shutdown();

    log_debug("daemon: shutdown: joining pp_worker...");
    if (g_pp_worker.joinable()) g_pp_worker.join();
    if (g_dl_worker.joinable()) g_dl_worker.join();
    // Phase C.10a — join the streaming-slot worker (woken by g_jobs->shutdown()
    // returning std::nullopt from dequeue). Then tear down the session
    // manager, which aborts any still-active streaming session (stops each
    // CaptionEngine, closes + unlinks the temp WAV).
    if (g_stream_worker.joinable()) g_stream_worker.join();
    g_streaming.reset();
    // Phase C.13 — signal + join the GC sweep thread. The sweep loop waits
    // on g_gc_cv with a per-cadence timeout; setting g_gc_shutdown lets it
    // exit immediately rather than wait the full interval.
    {
        std::lock_guard<std::mutex> lock(g_gc_mu);
        g_gc_shutdown = true;
    }
    g_gc_cv.notify_all();
    if (g_gc_worker.joinable()) g_gc_worker.join();
    g_sessions.reset();
    g_server = nullptr;

    // Clean up PID file
    if (pid_fd >= 0) {
        unlink(pid_path.c_str());
        close(pid_fd);
    }

    log_shutdown();
    notify_cleanup();
    return 0;
}
