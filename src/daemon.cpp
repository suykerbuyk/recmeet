// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "backend_info.h"
#include "config.h"
#include "config_json.h"
#include "diarization_cache.h"
#include "fetch_artifacts.h"
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
static std::unique_ptr<JobQueue> g_jobs;

// Phase C.10a — the server-side streaming-caption session registry. Owns the
// `stream_token -> StreamingSession` map; routes inbound `0x03` frames; holds
// each session's CaptionEngine + disk-backed temp WAV. Constructed in main()
// after g_jobs + the IpcServer exist (it captures the server for the caption
// event sink). One streaming session at a time (C.7 streaming slot is
// capacity-1).
static std::unique_ptr<StreamingSessionManager> g_streaming;

// Phase C.2 — the server-side upload-session registry. Owns the
// `upload_token -> UploadSession` map; routes inbound `0x01` frames by
// client_id; holds each upload's per-job staging directory + JobQueue
// postprocess reservation. Constructed in main() after g_jobs exists.
static std::unique_ptr<UploadSessionManager> g_uploads;

// Phase C.11.4 — server-side `meeting_id -> meeting_dir_path` index. Owns
// the dedup state for the convergence-principle audio contract (see
// docs/V2-STRATEGY.md "Meeting identity and the client-server audio
// contract"). Constructed at daemon startup BEFORE the IPC listener
// accepts so the very first `process.submit` / `process.stream` sees a
// fully-populated index. Repopulated from on-disk `context.json` via
// `rebuild_from_disk(g_config.output_dir)` once at startup; thereafter
// updated incrementally by the upload finalize + streaming create paths.
static std::unique_ptr<MeetingIndex> g_meeting_index;

// Phase C.8 — server-resident per-job diarization cache. Populated by
// pp_worker_loop on every successful Postprocess job that produced
// diarization (the enroll-mode path always populates; future
// transcribe-mode jobs land here too when the subprocess writes a
// diarization.json — currently only enroll mode does, but the cache
// machinery is shape-compatible with adding a transcribe-side artifact
// in C.8.1+). Consumed by `enroll.finalize` to extract the user-picked
// cluster's embedding and append to the speakers DB.
static std::unique_ptr<DiarizationCache> g_diar_cache;

static Config g_config;
static std::mutex g_config_mu;

// Stop tokens — separate for independent cancellation. C.9 dropped
// `g_rec_stop` along with the legacy recording worker; only the postprocess
// kill-switch survives.
static StopToken g_pp_stop;

// Worker threads (C.9 retired g_rec_worker)
static std::thread g_pp_worker;   // long-lived, drains the postprocess slot
static std::thread g_dl_worker;   // long-lived, drains the model_download slot
static std::thread g_stream_worker;  // C.10a — drains the streaming slot

// PostprocessJob — pre-C.7 this was a standalone struct; the C.7/C.9 typed
// JobQueue surfaces postprocess work as `Job`. The alias is kept so the
// pp_worker / write_job_config call sites read unchanged.
using PostprocessJob = Job;

// Subprocess postprocessing state
static std::atomic<pid_t> g_pp_child_pid{-1};
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

static void fill_state_fields(JsonMap& data) {
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
static void broadcast_state_inline(IpcServer& server, const std::string& error = "") {
    IpcEvent ev;
    ev.event = "state.changed";
    fill_state_fields(ev.data);
    if (!error.empty()) ev.data["error"] = error;
    server.broadcast(ev);
}

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static void signal_handler(int sig) {
    if (sig == SIGHUP) {
        // Reload config — handled via post() in the main loop
        if (g_server) {
            g_server->post([] {
                try {
                    Config cfg = load_config();
                    if (cfg.llm_model.empty()) {
                        const auto* prov = find_provider(cfg.provider);
                        if (prov) {
                            std::string key = resolve_api_key(*prov, cfg.api_keys, cfg.api_key);
                            if (!key.empty()) cfg.api_key = key;
                        }
                    }
                    std::lock_guard<std::mutex> lock(g_config_mu);
                    g_config = cfg;
                    log_info("daemon: config reloaded via SIGHUP");
                } catch (const std::exception& e) {
                    log_error("daemon: config reload failed: %s", e.what());
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

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-h" || arg == "--help")) { print_usage(); return 0; }
        if ((arg == "-v" || arg == "--version")) { printf("recmeet-daemon %s\n", RECMEET_VERSION); return 0; }
        if (arg == "--socket" && i + 1 < argc) { socket_path = argv[++i]; continue; }
        if (arg == "--listen" && i + 1 < argc) { socket_path = argv[++i]; continue; }
        if (arg == "--log-level" && i + 1 < argc) { log_level_str = argv[++i]; continue; }
        if (arg == "--log-dir" && i + 1 < argc) { log_dir = argv[++i]; continue; }
        if (arg == "--log-retention" && i + 1 < argc) { log_retention_hours = std::atoi(argv[++i]); continue; }
        fprintf(stderr, "Unknown option: %s\n", arg.c_str());
        return 1;
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

    // Load config
    g_config = load_config();

    // Resolve API key
    if (g_config.llm_model.empty()) {
        const auto* prov = find_provider(g_config.provider);
        if (prov) {
            std::string key = resolve_api_key(*prov, g_config.api_keys, g_config.api_key);
            if (!key.empty()) g_config.api_key = key;
        }
    }
    // Note: don't auto-disable no_summary here — the decision is made per-job
    // based on the merged config (client may send an API key)

    // Phase C.7 — construct the typed-slot JobQueue. Slot capacities come
    // from `[server] slot.*` (all default 1); the auto-download machinery
    // (ModelResolver / ModelCacheChecker / JobEventSink) is wired below,
    // after the IpcServer exists (the event sink captures it).
    {
        JobQueue::SlotCapacities caps;
        caps.postprocess = g_config.slot_postprocess;
        caps.streaming = g_config.slot_streaming;
        caps.model_download = g_config.slot_model_download;
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
            g_config.diarization_cache_ttl_secs);
        log_info("daemon: diarization cache ready (ttl=%lld s)",
                 (long long)g_config.diarization_cache_ttl_secs);
    }

    // Phase C.11.4 — construct the meeting-id index BEFORE the upload /
    // streaming managers are wired so they can pass it into their
    // constructors. The startup `rebuild_from_disk` walks
    // `g_config.output_dir` once, reads each meeting dir's `context.json`,
    // and repopulates the map. Cost amortizes against startup; no on-disk
    // index file in v1.
    {
        g_meeting_index = std::make_unique<MeetingIndex>();
        std::size_t n = g_meeting_index->rebuild_from_disk(g_config.output_dir);
        log_info("daemon: meeting index ready (rebuilt %zu binding%s from %s)",
                 n, n == 1 ? "" : "s", g_config.output_dir.string().c_str());
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
        const Config& c = job.cfg;
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
            std::lock_guard<std::mutex> lock(g_config_mu);
            model_dir = resolve_caption_model_dir(g_config.caption_model).string();
        }
        g_streaming = std::make_unique<StreamingSessionManager>(
            *g_jobs, sink, model_dir,
            g_meeting_index.get(),    // C.11.4 — convergence-principle dedup
            g_config.output_dir);
        log_info("daemon: streaming session manager ready (caption_model_dir=%s, "
                 "meetings_root=%s)",
                 model_dir.c_str(), g_config.output_dir.string().c_str());
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
            g_config.output_dir);
        log_info("daemon: upload session manager ready (meetings_root=%s)",
                 g_config.output_dir.string().c_str());
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
    // is already resolved in g_config by load_config().
    server.set_max_message_bytes(g_config.max_message_bytes);

    // Phase A.3: apply the connection cap from config before start() so
    // the listen backlog (max_clients * 2) is sized off the configured
    // value rather than the struct default. Default is 16 / backlog 32;
    // override via `[ipc] max_clients` in daemon.yaml.
    server.set_max_clients(g_config.max_clients);

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

    // --- Method handlers ---

    server.on("status.get", [](const IpcRequest& req, IpcResponse& resp, IpcError&) {
        fill_state_fields(resp.result);
        // C.7: `queue_depth` reports the postprocess slot's queued count
        // (the successor of the pre-C.7 single g_job_queue size).
        resp.result["queue_depth"] = static_cast<int64_t>(
            g_jobs ? g_jobs->queued_count(JobKind::Postprocess) : 0);
        return true;
    });

    // Phase C.9 — `sources.list` is gone. Audio-device enumeration is a
    // client-local concern in the thin-client model: the tray (and the CLI
    // in standalone mode) link `recmeet_capture` and call its `list_sources()`
    // directly. The daemon no longer links PipeWire/PulseAudio so it could
    // not honour this verb anyway.

    server.on("config.reload", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        try {
            Config cfg = load_config();
            if (cfg.llm_model.empty()) {
                const auto* prov = find_provider(cfg.provider);
                if (prov) {
                    std::string key = resolve_api_key(*prov, cfg.api_keys, cfg.api_key);
                    if (!key.empty()) cfg.api_key = key;
                }
            }
            std::lock_guard<std::mutex> lock(g_config_mu);
            g_config = cfg;
            resp.result["ok"] = true;
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    // Phase A.6: `config.update` IPC removed. Per-request config overrides
    // are replaced by per-`client_id` session state established once at
    // connect via `session.init` and refreshed via `session.update_*`.
    // The handler is deliberately not re-registered; the IPC dispatcher
    // returns "Method not found" for any stray `config.update` request,
    // and tests pin that behavior (see test_ipc_integration.cpp).

    // -----------------------------------------------------------------
    // Phase A.6 session handshake handlers
    //
    // `session.init` is the post-auth handshake the thin-client tray and
    // CLI use to establish credentials + preferences on the per-`client_id`
    // slot the daemon will consult at `enqueue_postprocess()` time. Wire
    // shape per the plan body:
    //   {"credentials": {"provider", "api_key", "api_keys": {...}},
    //    "preferences": {"output_dir", "note_dir", "language", "vocabulary",
    //                    "mic_source", "monitor_source", "whisper_model",
    //                    "summarization_backend", "llm_model",
    //                    "captions_enabled", "caption_latency_ms"}}
    //
    // All fields optional. Validation enforced at this layer:
    //   * `summarization_backend ∈ {"", "http", "local"}`  → reject otherwise
    //   * `caption_latency_ms ∈ [200, 2000]`               → reject otherwise
    //
    // On success the response is {"ok": true, "session_active": true}.
    auto parse_credentials_into = [](const JsonMap& src, SessionCredentials& dst) {
        auto pit = src.find("provider");
        if (pit != src.end()) dst.provider = json_val_as_string(pit->second);
        auto kit = src.find("api_key");
        if (kit != src.end()) dst.api_key = json_val_as_string(kit->second);
        // Per-provider key map arrives as a nested object stringified by
        // the parser (parse_json_object stores nested objects as their
        // raw JSON for the outer map). We pick known providers out of
        // that raw string so a future addition only updates one site.
        auto m = src.find("api_keys");
        if (m != src.end()) {
            std::string raw = json_val_as_string(m->second);
            JsonMap nested;
            if (!raw.empty() && raw[0] == '{') {
                // Reuse the IPC protocol's parser via a fake response wrap.
                std::string wrapped = "{\"id\":0,\"result\":" + raw + "}";
                IpcMessage tmp;
                if (parse_ipc_message(wrapped, tmp) && tmp.type == IpcMessageType::Response)
                    nested = std::move(tmp.response.result);
            }
            for (const auto& [k, v] : nested) {
                std::string val = json_val_as_string(v);
                if (!val.empty()) dst.api_keys[k] = val;
            }
        }
    };

    // Validation helper that returns true on success; on failure populates
    // `err` and returns false. Used by all three session handlers so the
    // shape rules (latency range, backend value-set) cannot drift between
    // them.
    auto validate_prefs_payload = [](const JsonMap& src, IpcError& err) {
        auto bit = src.find("summarization_backend");
        if (bit != src.end()) {
            std::string b = json_val_as_string(bit->second);
            if (!b.empty() && b != "http" && b != "local") {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "summarization_backend must be 'http' or 'local'";
                return false;
            }
        }
        auto lit = src.find("caption_latency_ms");
        if (lit != src.end()) {
            int64_t v = json_val_as_int(lit->second);
            if (v < 200 || v > 2000) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "caption_latency_ms must be in [200, 2000]";
                return false;
            }
        }
        return true;
    };

    auto parse_preferences_into = [](const JsonMap& src, SessionPreferences& dst) {
        auto get_str = [&](const char* k, std::string& out) {
            auto it = src.find(k);
            if (it != src.end()) out = json_val_as_string(it->second);
        };
        get_str("output_dir",            dst.output_dir);
        get_str("note_dir",              dst.note_dir);
        get_str("language",              dst.language);
        get_str("vocabulary",            dst.vocabulary);
        get_str("mic_source",            dst.mic_source);
        get_str("monitor_source",        dst.monitor_source);
        get_str("whisper_model",         dst.whisper_model);
        get_str("summarization_backend", dst.summarization_backend);
        get_str("llm_model",             dst.llm_model);
        auto cit = src.find("captions_enabled");
        if (cit != src.end()) dst.captions_enabled = json_val_as_bool(cit->second);
        auto lit = src.find("caption_latency_ms");
        if (lit != src.end()) dst.caption_latency_ms =
            static_cast<int>(json_val_as_int(lit->second));
    };

    // Helper: pull a nested sub-object out of req.params at `key` into a
    // JsonMap. The IPC parser leaves nested objects as their raw JSON
    // string in the outer map; we re-parse via the response-wrap trick.
    auto pull_nested = [](const JsonMap& outer, const char* key, JsonMap& dst) {
        auto it = outer.find(key);
        if (it == outer.end()) return false;
        std::string raw = json_val_as_string(it->second);
        if (raw.empty() || raw[0] != '{') return false;
        std::string wrapped = "{\"id\":0,\"result\":" + raw + "}";
        IpcMessage tmp;
        if (!parse_ipc_message(wrapped, tmp) || tmp.type != IpcMessageType::Response)
            return false;
        dst = std::move(tmp.response.result);
        return true;
    };

    server.on("session.init", [&server, parse_credentials_into, parse_preferences_into,
                               validate_prefs_payload, pull_nested]
              (const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (req.client_id.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "session.init: no client_id stamped on request";
            return false;
        }

        // Validate first; on rejection the slot is unchanged so the
        // client can retry with a corrected payload without dropping a
        // prior good session state (this handler is also valid on a
        // fresh connection where there is no prior state to preserve,
        // so the symmetry is just for code clarity).
        JsonMap prefs_map;
        bool have_prefs = pull_nested(req.params, "preferences", prefs_map);
        if (have_prefs && !validate_prefs_payload(prefs_map, err)) {
            err.id = req.id;
            return false;
        }

        // Parse and apply.
        SessionCredentials creds;
        JsonMap creds_map;
        if (pull_nested(req.params, "credentials", creds_map))
            parse_credentials_into(creds_map, creds);

        SessionPreferences prefs;
        // Preserve the struct defaults (caption_latency_ms = 500) when
        // the client omits the field; only overwrite from the request.
        if (have_prefs)
            parse_preferences_into(prefs_map, prefs);

        server.set_session_credentials(req.client_id, creds);
        server.set_session_preferences(req.client_id, prefs);

        resp.result["ok"] = true;
        resp.result["session_active"] = true;
        return true;
    });

    server.on("session.update_credentials",
              [&server, parse_credentials_into]
              (const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (req.client_id.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "session.update_credentials: no client_id stamped on request";
            return false;
        }
        SessionCredentials creds;
        SessionPreferences prefs;
        // Snapshot prior slot so the merge preserves untouched fields.
        // If the slot does not exist yet, `creds` / `prefs` default-
        // construct — the update behaves like a partial session.init.
        server.get_session(req.client_id, creds, prefs);
        // Overlay only the fields actually present in the request.
        parse_credentials_into(req.params, creds);
        if (!server.set_session_credentials(req.client_id, creds)) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "session slot unavailable";
            return false;
        }
        resp.result["ok"] = true;
        return true;
    });

    server.on("session.update_prefs",
              [&server, parse_preferences_into, validate_prefs_payload]
              (const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (req.client_id.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "session.update_prefs: no client_id stamped on request";
            return false;
        }
        if (!validate_prefs_payload(req.params, err)) {
            err.id = req.id;
            return false;
        }
        SessionCredentials creds;
        SessionPreferences prefs;
        server.get_session(req.client_id, creds, prefs);
        // Overlay only the fields actually present in the request — for
        // strings this is "non-empty value overwrites"; for the boolean
        // and integer we explicitly check key presence so a request that
        // omits them preserves the prior value (and so a request that
        // sets caption_latency_ms but omits captions_enabled does NOT
        // implicitly set captions_enabled=false).
        auto bit = req.params.find("captions_enabled");
        auto lit = req.params.find("caption_latency_ms");
        SessionPreferences from_req;
        parse_preferences_into(req.params, from_req);
        // String fields: non-empty overlays.
        if (!from_req.output_dir.empty())            prefs.output_dir = from_req.output_dir;
        if (!from_req.note_dir.empty())              prefs.note_dir = from_req.note_dir;
        if (!from_req.language.empty())              prefs.language = from_req.language;
        if (!from_req.vocabulary.empty())            prefs.vocabulary = from_req.vocabulary;
        if (!from_req.mic_source.empty())            prefs.mic_source = from_req.mic_source;
        if (!from_req.monitor_source.empty())        prefs.monitor_source = from_req.monitor_source;
        if (!from_req.whisper_model.empty())         prefs.whisper_model = from_req.whisper_model;
        if (!from_req.summarization_backend.empty()) prefs.summarization_backend = from_req.summarization_backend;
        if (!from_req.llm_model.empty())             prefs.llm_model = from_req.llm_model;
        if (bit != req.params.end()) prefs.captions_enabled   = from_req.captions_enabled;
        if (lit != req.params.end()) prefs.caption_latency_ms = from_req.caption_latency_ms;
        if (!server.set_session_preferences(req.client_id, prefs)) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "session slot unavailable";
            return false;
        }
        resp.result["ok"] = true;
        return true;
    });

    // ---------------------------------------------------------------------------
    // Phase C.9 — REMOVED handlers: record.start, record.stop, job.context.
    //
    // Until C.9 the daemon owned a live-recording path: record.start spawned a
    // worker thread that called pipeline::run_recording (PipeWire/PulseAudio
    // capture + optional CaptionEngine), and record.stop / job.context fed it.
    // C.9 retired the entire path. The tray now captures audio locally (via
    // recmeet_capture) and either:
    //   - submits a finished WAV via  + 0x01 upload frames
    //     (the daemon enqueues a postprocess Job; pp_worker_loop drains it),
    //   - opens a streaming-caption session via  + 0x03 audio
    //     frames (the streaming_session manager owns the CaptionEngine).
    //
    // record.stop / job.context callers should migrate to process.cancel /
    // (per-submit) context fields in process.submit. The wire-side 
    // boolean on state.changed is also gone in C.9 (IPC_PROTOCOL_VERSION bump).
    // ---------------------------------------------------------------------------

    // --- Phase C.10a — streaming-caption handlers ---
    //
    // process.stream         — open a streaming-caption session. The client
    //                          then sends `0x03` audio frames; the daemon
    //                          feeds them into a CaptionEngine and broadcasts
    //                          `caption` / `caption.degraded` events.
    // process.stream.cancel  — discard the streaming job + its buffered audio
    //                          (unlinks the temp WAV), release the slot.
    //
    // C.10a scope: this is the streaming *core*. process.stream.commit and
    // the stream->postprocess handoff are C.10b; converting the caption
    // events to send_to_client() is C.3/C.10b.

    server.on("process.stream",
              [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_streaming) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "streaming subsystem unavailable";
            return false;
        }
        // Parse the request shape. Unknown / missing fields fall back to the
        // StreamRequest struct defaults. `speaker_hints` is reserved for v2
        // multi-server — accepted and ignored (we simply never read it).
        StreamRequest sr;
        auto gs = [&](const char* k, const std::string& def) {
            auto it = req.params.find(k);
            return it != req.params.end() ? json_val_as_string(it->second) : def;
        };
        auto gi = [&](const char* k, int64_t def) {
            auto it = req.params.find(k);
            return it != req.params.end() ? json_val_as_int(it->second) : def;
        };
        auto gb = [&](const char* k, bool def) {
            auto it = req.params.find(k);
            return it != req.params.end() ? json_val_as_bool(it->second) : def;
        };
        sr.format            = gs("format", sr.format);
        sr.sample_rate       = static_cast<int32_t>(gi("sample_rate", sr.sample_rate));
        sr.channels          = static_cast<int32_t>(gi("channels", sr.channels));
        sr.context           = gs("context", sr.context);
        sr.language          = gs("language", sr.language);
        sr.captions_enabled  = gb("captions_enabled", sr.captions_enabled);
        sr.latency_budget_ms = static_cast<int>(gi("latency_budget_ms",
                                                   sr.latency_budget_ms));
        // C.11 — optional meeting_id. Empty is the v1-client fallback;
        // non-empty must be a canonical UUID v4 or we reject at the wire
        // boundary (defense-in-depth so a malformed id never poisons the
        // MeetingIndex via the streaming session's frozen state).
        sr.meeting_id        = gs("meeting_id", "");
        if (!is_valid_meeting_id(sr.meeting_id)) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.stream: 'meeting_id' must be a canonical "
                          "lowercase UUID v4 (or absent)";
            return false;
        }

        // Phase C.10b — snapshot the per-client postprocess config at
        // `process.stream` time, exactly as `process.submit` does at
        // submit time. The streaming session freezes this snapshot so a
        // later `process.stream.commit` builds its postprocess Job from
        // the preferences live when the stream STARTED (not from a
        // concurrently-reloaded daemon Config). The default temp_dir
        // (system temp) is used here; production daemons can swap it
        // later via a config knob.
        Config stream_pp_cfg;
        {
            std::lock_guard<std::mutex> lock(g_config_mu);
            stream_pp_cfg = g_config;
        }
        // Mirror process.submit: clear any stale `reprocess_dir` from the
        // global Config snapshot; commit() sets it from the temp WAV's
        // parent directory.
        stream_pp_cfg.reprocess_dir.clear();

        auto cr = g_streaming->create(req.client_id, sr, {}, stream_pp_cfg);
        if (!cr.ok) {
            err.code = cr.code;
            err.message = cr.error;
            return false;
        }
        resp.result["job_id"] = cr.job_id;
        resp.result["stream_token"] = cr.stream_token;
        log_info("daemon: process.stream OK (job=%ld client=%s)",
                 (long)cr.job_id, req.client_id.c_str());
        return true;
    });

    server.on("process.stream.cancel",
              [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_streaming) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "streaming subsystem unavailable";
            return false;
        }
        auto it = req.params.find("stream_token");
        if (it == req.params.end() || json_val_as_string(it->second).empty()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.stream.cancel: missing 'stream_token'";
            return false;
        }
        std::string token = json_val_as_string(it->second);
        if (!g_streaming->cancel(req.client_id, token)) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.stream.cancel: unknown stream_token "
                          "(or not owned by this client)";
            return false;
        }
        broadcast_state_inline(server);
        resp.result["ok"] = true;
        return true;
    });

    // --- Phase C.10b — process.stream.commit (finalize handoff) ---
    //
    // process.stream.commit — finalize a streaming session: flush + close
    //                         the temp WAV, hand the accumulated audio off
    //                         to a fresh Postprocess job (transcribe +
    //                         diarize + summarize), release the streaming
    //                         slot. Request: { stream_token }. Response:
    //                         { job_id: <postprocess job_id>, ok: true }.
    //                         The client monitors the new job via
    //                         `progress.job` + `process.fetch` exactly as
    //                         it would for a `process.submit` job.
    //
    // Validation chain (narrowest reject first):
    //   1. Missing `stream_token`              → InvalidParams
    //   2. Unknown stream_token                → InvalidParams
    //   3. Stream not owned by req.client_id   → PermissionDenied
    //   4. Session not in a committable state  → InvalidParams
    //
    // The full action is documented at StreamingSessionManager::commit;
    // the daemon handler is a thin shell over it.
    server.on("process.stream.commit",
              [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_streaming) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "streaming subsystem unavailable";
            return false;
        }
        auto it = req.params.find("stream_token");
        if (it == req.params.end() || json_val_as_string(it->second).empty()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.stream.commit: missing 'stream_token'";
            return false;
        }
        std::string token = json_val_as_string(it->second);
        auto cr = g_streaming->commit(req.client_id, token);
        if (!cr.ok) {
            err.code = cr.code;
            err.message = cr.error;
            return false;
        }
        // `job_id` on the wire is the NEW postprocess job_id (not the
        // streaming job's id). The client uses it for progress.job +
        // process.fetch follow-ups.
        resp.result["job_id"] = cr.postprocess_job_id;
        resp.result["ok"] = true;
        broadcast_state_inline(server);
        log_info("daemon: process.stream.commit OK (postprocess_job=%ld "
                 "client=%s)",
                 (long)cr.postprocess_job_id, req.client_id.c_str());
        return true;
    });

    // --- Phase C.2 — process.submit (batch postprocess upload) ---
    //
    // process.submit         — open an upload session. The client then sends
    //                          `0x01` upload frames; on completion the daemon
    //                          enqueues a postprocess job (which the existing
    //                          pp_worker_loop drains exactly as a legacy
    //                          record.start handoff did).
    // process.submit.cancel  — discard an in-flight upload session (unlinks
    //                          the staging dir, releases the reservation).
    //                          C.5 will add cancel-for-running-postprocess.
    //
    // C.2 scope: opening and finalizing an UPLOAD. Cancelling a postprocess
    // job that is already enqueued/running is C.5. Fetching artifacts is
    // C.4. Removing record.start is C.9.
    server.on("process.submit",
              [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_uploads) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "upload subsystem unavailable";
            return false;
        }
        SubmitRequest sr;
        auto gs = [&](const char* k, const std::string& def) {
            auto it = req.params.find(k);
            return it != req.params.end() ? json_val_as_string(it->second) : def;
        };
        auto gi = [&](const char* k, int64_t def) {
            auto it = req.params.find(k);
            return it != req.params.end() ? json_val_as_int(it->second) : def;
        };
        sr.audio_size   = gi("audio_size", 0);
        sr.format       = gs("format", sr.format);
        sr.sample_rate  = static_cast<int32_t>(gi("sample_rate", sr.sample_rate));
        sr.channels     = static_cast<int32_t>(gi("channels", sr.channels));
        sr.context      = gs("context", sr.context);
        sr.mode         = gs("mode", sr.mode);
        sr.enroll_name  = gs("enroll_name", sr.enroll_name);  // C.8
        // C.11 — optional client-minted meeting_id (UUID v4). Empty is the
        // v1-client fallback; non-empty must validate or we reject at the
        // wire boundary so a malformed id never reaches UploadSession or
        // the MeetingIndex.
        sr.meeting_id   = gs("meeting_id", "");
        if (!is_valid_meeting_id(sr.meeting_id)) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.submit: 'meeting_id' must be a canonical "
                          "lowercase UUID v4 (or absent)";
            return false;
        }
        // `speaker_hints` is accepted-and-ignored in C.2 (reserved for v2
        // multi-server). We deliberately do not read it.

        // Snapshot the per-client postprocess config for the eventual Job.
        // The recipe matches what record.start does at handoff: take the
        // global Config (which already reflects session.init preferences
        // because session.update_* merges into it), then layer the
        // per-submit context. The full session-state weave will be revisited
        // in C.9 when record.start is removed; for now the cleanest path is
        // to pass the live Config snapshot through to the upload manager,
        // which freezes it for the upload's lifetime so a concurrent
        // config.reload between submit and finalize can't surprise us.
        Config cfg;
        size_t max_upload_bytes = 0;
        {
            std::lock_guard<std::mutex> lock(g_config_mu);
            cfg = g_config;
            max_upload_bytes = g_config.max_upload_bytes;
        }
        // Force the subprocess into reprocess mode (the staging dir IS the
        // out_dir). pp_worker_loop sets this from input.out_dir, but be
        // explicit so a stale reprocess_dir from the snapshot does not
        // leak into the staging job.
        cfg.reprocess_dir.clear();

        auto cr = g_uploads->create(req.client_id, sr, cfg, max_upload_bytes);
        if (!cr.ok) {
            err.code = cr.code;
            err.message = cr.error;
            return false;
        }
        resp.result["job_id"]       = cr.job_id;
        resp.result["upload_token"] = cr.upload_token;
        resp.result["max_size"]     = cr.max_size;
        log_info("daemon: process.submit OK (job=%ld client=%s "
                 "audio_size=%lld format=%s)",
                 (long)cr.job_id, req.client_id.c_str(),
                 (long long)sr.audio_size, sr.format.c_str());
        return true;
    });

    server.on("process.submit.cancel",
              [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_uploads) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "upload subsystem unavailable";
            return false;
        }
        auto it = req.params.find("upload_token");
        if (it == req.params.end() || json_val_as_string(it->second).empty()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.submit.cancel: missing 'upload_token'";
            return false;
        }
        std::string token = json_val_as_string(it->second);
        if (!g_uploads->cancel(req.client_id, token)) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.submit.cancel: unknown upload_token "
                          "(or not owned by this client)";
            return false;
        }
        broadcast_state_inline(server);
        resp.result["ok"] = true;
        return true;
    });

    // --- Phase C.5 — process.cancel (general, job_id-routed) ---
    //
    // process.cancel — unified cancellation surface. Request: { job_id }.
    //                  Cancels a job regardless of lifecycle state (Queued /
    //                  WaitingOnDownload / WaitingForUpload / Running) and
    //                  regardless of which slot it lives in. Routes to the
    //                  right teardown path by inspecting the job's kind +
    //                  state via JobQueue::status.
    //
    //   kind / state                        → action
    //   ─────────────────────────────────── ──────────────────────────────
    //   Postprocess + Queued                → g_jobs->cancel(job_id)
    //                                         lazy FIFO removal at next
    //                                         pick_runnable_locked.
    //   Postprocess + WaitingOnDownload     → g_jobs->cancel(job_id);
    //                                         finish_download will see the
    //                                         Cancelled verdict and skip
    //                                         re-arming.
    //   Postprocess + WaitingForUpload      → g_jobs->cancel(job_id) AND
    //                                         g_uploads->cancel_by_job_id —
    //                                         tear down staging file + slot
    //                                         reservation.
    //   Postprocess + Running               → g_jobs->cancel(job_id) AND
    //                                         g_pp_stop.request() AND
    //                                         kill(g_pp_child_pid, SIGTERM)
    //                                         if a child is alive (mirrors
    //                                         the record.stop path).
    //   Streaming + any non-terminal        → g_streaming->cancel_by_job_id
    //                                         (stops engine + WAV + slot).
    //   ModelDownload + non-terminal       → g_jobs->cancel(job_id); the
    //                                         download worker observes via
    //                                         finish_download's Cancelled
    //                                         guard and dependents fail
    //                                         cleanly through the existing
    //                                         finish_download path.
    //   any kind + terminal                → InvalidParams "already
    //                                         in terminal state".
    //
    // Scope discipline — process.cancel is NEW; the narrower verbs
    // process.stream.cancel and process.submit.cancel stay in place for
    // backward compatibility (and as token-anchored entry points). C.5 does
    // NOT remove them — they coexist with the general verb.
    //
    // record.stop (legacy) is left untouched — it remains the v1 path that
    // C.9 will retire. The two coexist until then; this handler is the v2
    // thin-client cancellation entry point.
    server.on("process.cancel",
              [&server](const IpcRequest& req, IpcResponse& resp,
                        IpcError& err) {
        if (!g_jobs) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "job queue unavailable";
            return false;
        }

        // (1) job_id required and positive.
        auto jit = req.params.find("job_id");
        if (jit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.cancel: missing 'job_id'";
            return false;
        }
        const int64_t job_id = json_val_as_int(jit->second, 0);
        if (job_id <= 0) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.cancel: 'job_id' must be a positive integer";
            return false;
        }

        // (2) Snapshot the job. We read kind/state under JobQueue's mutex
        // via status(); after we release that snapshot we are racing the
        // worker, but the manager-side teardown paths all re-validate.
        auto snap = g_jobs->status(job_id);
        if (!snap.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.cancel: unknown job_id "
                        + std::to_string(job_id);
            return false;
        }
        const JobKind  kind  = snap->kind;
        const JobState state = snap->state;

        // (3) Ownership: only the originating client may cancel its own job.
        // Uses the C.3 binding (job_id → client_id). An empty req.client_id
        // (defensive — the IPC server stamps a client_id on every accepted
        // connection) would fail this check naturally because no live job
        // is bound to "".
        auto owner = g_jobs->client_for_job(job_id);
        if (!owner.has_value() || *owner != req.client_id) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "process.cancel: job_id "
                        + std::to_string(job_id)
                        + " is not owned by this client";
            return false;
        }

        // (4) Terminal-state reject — there is nothing to cancel.
        if (state == JobState::Done ||
            state == JobState::Failed ||
            state == JobState::Cancelled) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.cancel: job is already in terminal "
                          "state " + std::string(job_state_name(state));
            return false;
        }

        // (5) Dispatch by kind + state.
        log_info("daemon: process.cancel job=%ld kind=%s state=%s "
                 "client=%s",
                 (long)job_id, job_kind_name(kind),
                 job_state_name(state), req.client_id.c_str());

        switch (kind) {
        case JobKind::Postprocess: {
            switch (state) {
            case JobState::Queued:
            case JobState::WaitingOnDownload:
                // Lazy FIFO removal at the next pick_runnable_locked, OR
                // observed by finish_download as a Cancelled dependent.
                g_jobs->cancel(job_id);
                break;
            case JobState::WaitingForUpload:
                // Tear down the in-flight upload first (so the staging
                // file is unlinked and the slot reservation released),
                // then mark the JobQueue entry Cancelled. The upload
                // manager's cancel_by_job_id() also calls g_jobs->cancel
                // internally; calling cancel() here is idempotent (the
                // second cancel hits the Cancelled-state guard and is a
                // no-op). Order matters: tear down resources before
                // settling state, so an observer racing on status()
                // sees a clean lifecycle.
                if (g_uploads) g_uploads->cancel_by_job_id(job_id);
                g_jobs->cancel(job_id);
                break;
            case JobState::Running: {
                // Mark Cancelled FIRST so the pp_worker_loop's exit-code-2
                // path (which calls finish(false, "cancelled")) preserves
                // the Cancelled verdict via finish()'s state guard. Then
                // poke the subprocess: g_pp_stop is the cooperative
                // signal; SIGTERM is the kick if a child PID is live.
                g_jobs->cancel(job_id);
                g_pp_stop.request();
                pid_t child = g_pp_child_pid.load();
                if (child > 0) ::kill(child, SIGTERM);
                break;
            }
            default:
                // Unreachable — terminal states reject above.
                break;
            }
            break;
        }
        case JobKind::Streaming: {
            // The streaming manager owns the slot + engine + temp WAV. Its
            // cancel_by_job_id runs the full teardown (engine stop, WAV
            // close+unlink, JobQueue::cancel, JobQueue::finish to release
            // the running marker). Returns false only if the session is
            // no longer live — a benign race; the JobQueue verdict is
            // either already Cancelled or terminal, which the post-status
            // re-check above caught.
            if (g_streaming) g_streaming->cancel_by_job_id(job_id);
            else             g_jobs->cancel(job_id);
            break;
        }
        case JobKind::ModelDownload: {
            // The model_download worker checks status before finishing —
            // a Cancelled verdict propagates through finish_download(),
            // and any parked Postprocess dependents land Failed via the
            // existing finish_download dependent path.
            g_jobs->cancel(job_id);
            break;
        }
        case JobKind::_count:
            // Unreachable — sentinel.
            break;
        }

        broadcast_state_inline(server);
        resp.result["ok"] = true;
        return true;
    });

    // --- Phase C.4 — process.fetch (artifact download) ---
    //
    // process.fetch — download the artifacts of a completed postprocess job.
    //                 Request:  { "job_id": int64 }
    //                 Response: { "job_id": int64,
    //                             "artifacts": [
    //                               {"name": "...", "size": int64,
    //                                "content_type": "..."}, ...],
    //                             "total_size": int64 }
    //                 Then, in the same `client_id`'s outbound stream, one
    //                 `0x02` BinaryArtifact frame per `artifacts[]` entry,
    //                 in the same order. NDJSON events for unrelated jobs
    //                 may interleave fine — the client demultiplexes by
    //                 counting `0x02` frames specifically.
    //
    // Validation chain (in this order — narrowest reject first):
    //   1. Missing/non-positive job_id        -> InvalidParams
    //   2. Unknown job_id                     -> InvalidParams
    //   3. job not owned by req.client_id     -> PermissionDenied
    //   4. job not in Done state              -> JobNotReady
    //   5. out_dir missing/unreadable         -> InternalError
    //   6. any artifact > max_binary_frame_bytes -> InternalError
    //                                            (chunking deferred — C.4
    //                                             rejects with a clear msg)
    //
    // Scope discipline (NOT in this handler — later phases):
    //   * Cancellation                        — C.5 (process.cancel)
    //   * job.status / job.list verbs         — C.6
    //   * Cleanup / retention of old artifacts— out of v1 scope
    //   * Chunked transfer for huge artifacts — future; today they reject
    server.on("process.fetch",
              [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_jobs) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "job queue unavailable";
            return false;
        }

        // (1) job_id is required and positive.
        auto jit = req.params.find("job_id");
        if (jit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.fetch: missing 'job_id'";
            return false;
        }
        const int64_t job_id = json_val_as_int(jit->second, 0);
        if (job_id <= 0) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.fetch: 'job_id' must be a positive integer";
            return false;
        }

        // (2) The job_id must be known.
        auto snap = g_jobs->status(job_id);
        if (!snap.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.fetch: unknown job_id "
                        + std::to_string(job_id);
            return false;
        }
        const Job& job = *snap;

        // (3) Ownership: the requester must be the originator. We do NOT
        // expose another client's artifacts even if the job_id is guessed.
        // The check goes through `client_for_job()` (the C.3 binding) so a
        // raced disconnect-then-reconnect mints a fresh client_id and is
        // correctly rejected — the new connection does not inherit the
        // departed client's job artifacts.
        auto owner = g_jobs->client_for_job(job_id);
        if (!owner.has_value() || *owner != req.client_id) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "process.fetch: job_id "
                        + std::to_string(job_id)
                        + " is not owned by this client";
            return false;
        }

        // (4) State must be terminal-Done. Other states are valid lifecycle
        // points but produce no artifacts to ship.
        if (job.state != JobState::Done) {
            err.code = static_cast<int>(IpcErrorCode::JobNotReady);
            err.message = std::string("process.fetch: fetch is only valid for "
                                      "Done jobs; current state=")
                        + job_state_name(job.state);
            return false;
        }

        // (5) `out_dir` must exist on disk. Enumeration handles the absent /
        // unreadable cases and returns an explanatory error string.
        const fs::path& out_dir = job.input.out_dir;
        if (out_dir.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "process.fetch: job has no out_dir on record";
            return false;
        }
        std::string enum_err;
        std::vector<ArtifactInfo> arts = enumerate_artifacts(out_dir, &enum_err);
        if (!enum_err.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "process.fetch: " + enum_err;
            return false;
        }

        // (6) Cap check: each artifact rides one `0x02` frame. C.1's transport
        // cap (default 16 MiB) bounds the payload of a single frame. A note
        // at typical sizes (<100 KB) clears this trivially, but a malformed
        // / huge artifact would not — reject rather than truncate.
        const size_t frame_cap = server.max_binary_frame_bytes();
        for (const auto& a : arts) {
            if (static_cast<size_t>(a.size) > frame_cap) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "process.fetch: artifact '" + a.name
                            + "' (" + std::to_string(a.size)
                            + " bytes) exceeds max_binary_frame_bytes ("
                            + std::to_string(frame_cap)
                            + "); raise [ipc] max_upload_bytes on the daemon "
                              "or omit this artifact";
                return false;
            }
        }

        // Read the bytes BEFORE sending the metadata response. If a read
        // fails (file vanished between enumerate and read) we want the
        // failure to surface as the response error, not as a torn binary
        // stream halfway through. This sequence preserves the wire invariant
        // "metadata first → exactly N `0x02` frames after". On success we
        // hand the buffers to the post-response binary fan-out below.
        std::vector<std::string> bodies;
        bodies.reserve(arts.size());
        int64_t total_size = 0;
        for (const auto& a : arts) {
            std::ifstream in(a.path, std::ios::binary);
            if (!in) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "process.fetch: cannot open artifact '"
                            + a.name + "' for read";
                return false;
            }
            std::string body;
            body.reserve(static_cast<size_t>(a.size));
            body.assign(std::istreambuf_iterator<char>(in),
                        std::istreambuf_iterator<char>());
            total_size += static_cast<int64_t>(body.size());
            bodies.push_back(std::move(body));
        }

        // Build the metadata JSON. We emit `artifacts` as a raw JSON array
        // string because the JsonMap value type is flat — nested arrays
        // are stored as their raw substring and round-trip cleanly through
        // the parser on the client side (see parse_json_object's nested
        // object/array branch in ipc_protocol.cpp).
        std::string arr = "[";
        for (size_t i = 0; i < arts.size(); ++i) {
            if (i > 0) arr += ",";
            arr += "{\"name\":\"" + json_escape(arts[i].name) + "\""
                +  ",\"size\":" + std::to_string(arts[i].size)
                +  ",\"content_type\":\"" + json_escape(arts[i].content_type)
                +  "\"}";
        }
        arr += "]";

        resp.result["job_id"]     = job_id;
        resp.result["artifacts"]  = arr;
        resp.result["total_size"] = total_size;

        // The metadata response is dispatched by the IPC machinery once we
        // return `true`. The binary fan-out must happen AFTER that response
        // hits the client's outbound queue (or at least, after the response
        // is enqueued on the same fd) so the client's "wait for response,
        // then expect N binary frames" pump path is happy. We `post()` the
        // binary sends onto the poll thread so they enqueue on the SAME
        // outbound queue AFTER this handler's response — preserving order
        // because `send_to()` appends to the per-fd `outbound` deque.
        //
        // We capture `bodies` + `arts` BY MOVE into the posted callback so
        // no copying happens for large artifacts.
        const std::string client_id = req.client_id;
        auto bodies_p = std::make_shared<std::vector<std::string>>(std::move(bodies));
        auto arts_p   = std::make_shared<std::vector<ArtifactInfo>>(std::move(arts));
        server.post([&server, client_id, bodies_p, arts_p]() {
            // Iterate `artifacts[]` order (which is `bodies` order — built
            // in lockstep above). Each `send_binary_to_client` enqueues one
            // `0x02` frame on the client's outbound queue; the queue
            // preserves enqueue order, so the wire sees them in the same
            // order as the metadata array.
            for (size_t i = 0; i < bodies_p->size(); ++i) {
                server.send_binary_to_client(client_id,
                                             FrameType::BinaryArtifact,
                                             (*bodies_p)[i],
                                             MessageClass::Response);
            }
            log_debug("daemon: process.fetch dispatched %zu artifact frame(s) "
                      "to client=%s", bodies_p->size(), client_id.c_str());
        });

        log_info("daemon: process.fetch OK (job=%ld client=%s "
                 "artifacts=%zu total_size=%lld)",
                 (long)job_id, req.client_id.c_str(),
                 arts_p->size(), (long long)total_size);
        return true;
    });

    // --- Phase C.6 — job.status / job.list (read-only queries) ---
    //
    // Two thin read-only verbs that surface the JobQueue registry to clients.
    // Both wire over the existing `JobQueue::status` / `JobQueue::list_by_client`
    // / `JobQueue::client_for_job` API — no new mutation paths and no new
    // wire-shape surface beyond the response body.
    //
    // job.status — request  : { "job_id": int64 }
    //              response : { "job_id"   : int64,
    //                           "kind"     : "postprocess"|"streaming"|"model_download",
    //                           "state"    : "queued"|"waiting_on_download"|
    //                                        "waiting_for_upload"|"running"|
    //                                        "done"|"failed"|"cancelled",
    //                           "client_id": "<owner>",
    //                           "model_id" : "<empty for non-download>",
    //                           "error"    : "<empty unless state==failed>" }
    //              Validation chain:
    //                1. Missing/non-positive job_id   -> InvalidParams
    //                2. Unknown job_id                -> InvalidParams
    //                3. job not owned by req.client_id -> PermissionDenied
    //              Use case (D.3): on reconnect a client re-syncs the state of
    //              a single known job_id. Terminal jobs are retained in the
    //              registry (C.7) so a Done/Failed/Cancelled verdict survives.
    //
    // job.list — request  : {} (no params; scoped server-side by req.client_id)
    //            response : { "jobs": [ ...same shape as job.status... ],
    //                         "count": int64 }
    //            Use case (D.5): on tray restart a client re-syncs every job
    //            it owns across all kinds, including terminal jobs. Ordering
    //            is ascending job_id (std::map iteration order inside
    //            `list_by_client`); we preserve it on the wire.
    //
    // Scope discipline: `Job::input` (PostprocessInput) and `Job::cfg` (Config)
    // are NOT serialized. They carry config secrets (api_keys, output_dir
    // paths) and are not part of the wire surface. The intentional positive
    // assertion in the test suite is that these keys are ABSENT from the
    // response.
    //
    // Serialization approach for the `jobs[]` array follows the same precedent
    // as `process.fetch`'s `artifacts[]` (a few hundred lines up): we build
    // a raw JSON-array substring and stash it as a JsonVal::string. The
    // JsonMap value type is flat — nested objects/arrays round-trip cleanly
    // through `parse_json_object`'s nested-substring branch — so emitting
    // pre-serialized JSON via a string is the established C.4 pattern. We
    // factor the per-job object out into a local `serialize_job_object`
    // helper so `job.status` (single object) and `job.list` (array element)
    // share the exact same field set, ordering, and escaping behavior. A
    // divergence between the two responses would be a Phase D re-sync
    // correctness bug.

    auto serialize_job_object = [](const Job& job) -> std::string {
        std::string out;
        out.reserve(200);
        out += "{\"job_id\":";
        out += std::to_string(job.job_id);
        out += ",\"kind\":\"";
        out += job_kind_name(job.kind);
        out += "\",\"state\":\"";
        out += job_state_name(job.state);
        out += "\",\"client_id\":\"";
        out += json_escape(job.client_id);
        out += "\",\"model_id\":\"";
        out += json_escape(job.model_id);
        out += "\",\"error\":\"";
        out += json_escape(job.error);
        // C.11 — meeting_id emitted unconditionally (empty string for
        // v1-shaped clients + daemon-internal jobs like model downloads).
        // The client uses this to reconcile by content key per
        // docs/V2-STRATEGY.md "Meeting identity".
        out += "\",\"meeting_id\":\"";
        out += json_escape(job.meeting_id);
        out += "\"}";
        return out;
    };

    server.on("job.status",
              [](const IpcRequest& req, IpcResponse& resp,
                 IpcError& err) {
        if (!g_jobs) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "job queue unavailable";
            return false;
        }

        // (1) job_id required and positive.
        auto jit = req.params.find("job_id");
        if (jit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "job.status: missing 'job_id'";
            return false;
        }
        const int64_t job_id = json_val_as_int(jit->second, 0);
        if (job_id <= 0) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "job.status: 'job_id' must be a positive integer";
            return false;
        }

        // (2) Snapshot — the registry retains terminal jobs (C.7), so a
        // Done/Failed/Cancelled job is fully queryable here.
        auto snap = g_jobs->status(job_id);
        if (!snap.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "job.status: unknown job_id "
                        + std::to_string(job_id);
            return false;
        }

        // (3) Ownership — same posture as process.fetch / process.cancel.
        // We do NOT leak another client's job state even on a guessed id.
        auto owner = g_jobs->client_for_job(job_id);
        if (!owner.has_value() || *owner != req.client_id) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "job.status: job_id "
                        + std::to_string(job_id)
                        + " is not owned by this client";
            return false;
        }

        // Single-job response — emit each field flat under `result`. The
        // IpcResponse top-level wrapper is `{"id":N,"result":{...}}`, so we
        // expose the per-job fields directly at the result level (not
        // nested under a "job" key) — that matches the doc-comment shape
        // above. We emit `model_id`/`error` unconditionally even when
        // empty: a missing key vs. an empty string would be an unnecessary
        // client-side branch and the wire byte cost is negligible.
        //
        // `serialize_job_object` produces a raw JSON-object string used by
        // job.list's array elements (raw-substring C.4 pattern). For
        // job.status the response wrapper is itself an object, so we use
        // the flat JsonMap path instead — the resulting wire shape is
        // byte-for-byte the same set of {kind/state/client_id/model_id/
        // error/job_id} fields the array-element form emits.
        resp.result["job_id"]    = static_cast<int64_t>(snap->job_id);
        resp.result["kind"]      = std::string(job_kind_name(snap->kind));
        resp.result["state"]     = std::string(job_state_name(snap->state));
        resp.result["client_id"] = snap->client_id;
        resp.result["model_id"]  = snap->model_id;
        resp.result["error"]     = snap->error;
        // C.11 — emit unconditionally; matches serialize_job_object so the
        // job.status object shape is byte-equivalent to a job.list entry.
        resp.result["meeting_id"] = snap->meeting_id;

        log_debug("daemon: job.status job=%ld kind=%s state=%s client=%s",
                  (long)job_id, job_kind_name(snap->kind),
                  job_state_name(snap->state), req.client_id.c_str());
        return true;
    });

    server.on("job.list",
              [serialize_job_object](const IpcRequest& req, IpcResponse& resp,
                                     IpcError& err) {
        if (!g_jobs) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "job queue unavailable";
            return false;
        }

        // No params required — server-side scoping is by req.client_id.
        // An empty req.client_id (defensive — the IPC server stamps a
        // client_id on every accepted connection) would naturally return
        // an empty list because no live job binding is "".
        std::vector<Job> jobs = g_jobs->list_by_client(req.client_id);

        // Build the `jobs[]` raw JSON array. Same C.4 raw-substring pattern
        // as `process.fetch`'s `artifacts[]` — pre-serialize the array, then
        // stash it on the response as a string. The client-side parser
        // (`parse_json_object`'s nested-`[...]` branch) treats it as a raw
        // substring and re-parses on the receive side. Ordering is the
        // ascending-job_id order `list_by_client` returns (std::map
        // iteration order); we preserve it byte-for-byte on the wire.
        std::string arr = "[";
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (i > 0) arr += ",";
            arr += serialize_job_object(jobs[i]);
        }
        arr += "]";

        resp.result["jobs"]  = arr;
        resp.result["count"] = static_cast<int64_t>(jobs.size());

        log_debug("daemon: job.list client=%s count=%zu",
                  req.client_id.c_str(), jobs.size());
        return true;
    });

    // --- Phase C.8 — enroll.finalize (voiceprint enrollment second step) ---
    //
    // Two flows feed `enroll.finalize`. Both end at the same code path here:
    //
    //   Flow (a) — fresh enrollment audio:
    //     client → process.submit { mode="enroll", enroll_name=Alice, ... }
    //                              → daemon reserves Postprocess job_id
    //     client → 0x01 upload frames (audio)
    //     daemon → enqueue Postprocess job with cfg.enroll_mode = true
    //              → subprocess runs diarize-only, writes diarization.json
    //              → daemon reads diarization.json, populates
    //                DiarizationCache[job_id], emits job.complete with
    //                speakers[] payload + enroll_mode=true.
    //     client picks `target_speaker` from speakers[].
    //     client → enroll.finalize { job_id, target_speaker, enroll_name }.
    //              → handler extracts embedding for target_speaker from
    //                cached centroids, appends to SpeakerProfile under
    //                enroll_name, persists speakers DB.
    //
    //   Flow (b) — reuse existing diarization from a prior postprocess job:
    //     client → enroll.finalize { existing_job_id, target_speaker,
    //                                enroll_name }.
    //              → handler uses the same cache lookup.
    //
    // Both flows share the cache-lookup + extract + persist body below.
    //
    // Validation chain (matches `process.fetch` posture):
    //   1. Missing job_id / target_speaker / enroll_name → InvalidParams.
    //   2. Unknown job_id                                → InvalidParams.
    //   3. Job not owned by req.client_id                → PermissionDenied.
    //   4. Job state not Done                            → JobNotReady.
    //   5. Diarization cache miss or expired             → InvalidParams
    //                                                      "diarization no
    //                                                       longer cached
    //                                                       (TTL=...)".
    //   6. target_speaker out of cluster range           → InvalidParams.
    //
    // Action: load the speakers DB, find or create the profile for
    // `enroll_name`, append the cluster's centroid as a new embedding,
    // bump `updated`, save. Returns `{ok, enroll_name, embedding_count}`.
    server.on("enroll.finalize",
              [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_jobs || !g_diar_cache) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "enroll.finalize: subsystem unavailable";
            return false;
        }
        auto jit = req.params.find("job_id");
        if (jit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: missing 'job_id'";
            return false;
        }
        const int64_t job_id = json_val_as_int(jit->second, 0);
        if (job_id <= 0) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: 'job_id' must be a positive integer";
            return false;
        }
        auto sit = req.params.find("target_speaker");
        if (sit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: missing 'target_speaker'";
            return false;
        }
        const int64_t target_speaker = json_val_as_int(sit->second, -1);
        if (target_speaker < 0) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: 'target_speaker' must be >= 0";
            return false;
        }
        auto nit = req.params.find("enroll_name");
        if (nit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: missing 'enroll_name'";
            return false;
        }
        std::string enroll_name = json_val_as_string(nit->second);
        if (enroll_name.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: 'enroll_name' must be non-empty";
            return false;
        }

        auto snap = g_jobs->status(job_id);
        if (!snap.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: unknown job_id "
                        + std::to_string(job_id);
            return false;
        }
        auto owner = g_jobs->client_for_job(job_id);
        if (!owner.has_value() || *owner != req.client_id) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "enroll.finalize: job_id "
                        + std::to_string(job_id)
                        + " is not owned by this client";
            return false;
        }
        if (snap->state != JobState::Done) {
            err.code = static_cast<int>(IpcErrorCode::JobNotReady);
            err.message = std::string(
                              "enroll.finalize: finalize is only valid for "
                              "Done jobs; current state=")
                        + job_state_name(snap->state);
            return false;
        }

        auto entry = g_diar_cache->get(job_id);
        if (!entry.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: diarization no longer cached "
                          "(TTL=" + std::to_string(g_diar_cache->ttl_secs())
                        + "s; re-submit the job)";
            return false;
        }
        if (target_speaker >= static_cast<int64_t>(entry->clusters.size())) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "enroll.finalize: target_speaker "
                        + std::to_string(target_speaker)
                        + " out of range (clusters=0.."
                        + std::to_string(entry->clusters.size()) + ")";
            return false;
        }
        const auto& cluster = entry->clusters[target_speaker];
        if (cluster.embedding.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "enroll.finalize: cluster "
                        + std::to_string(target_speaker)
                        + " has no cached embedding (subprocess may have "
                          "skipped extraction)";
            return false;
        }

        // Append the cluster centroid to the SpeakerProfile keyed by
        // `enroll_name`. Mirrors the CLI's `profile.embeddings.push_back(...)`
        // semantic — the on-disk format stays append-one-embedding.
        fs::path db_dir;
        {
            std::lock_guard<std::mutex> lock(g_config_mu);
            db_dir = g_config.speaker_db.empty()
                ? default_speaker_db_dir() : g_config.speaker_db;
        }
        try {
            auto profiles = load_speaker_db(db_dir);
            SpeakerProfile profile;
            for (const auto& p : profiles) {
                if (p.name == enroll_name) {
                    profile = p;
                    break;
                }
            }
            if (profile.name.empty()) {
                profile.name = enroll_name;
                profile.created = iso_now();
            }
            profile.embeddings.push_back(cluster.embedding);
            profile.updated = iso_now();
            if (profile.created.empty()) profile.created = profile.updated;
            save_speaker(db_dir, profile);

            resp.result["ok"] = true;
            resp.result["enroll_name"] = enroll_name;
            resp.result["embedding_count"] =
                static_cast<int64_t>(profile.embeddings.size());
            log_info("daemon: enroll.finalize OK (job=%ld client=%s "
                     "name=%s target=%d count=%zu)",
                     (long)job_id, req.client_id.c_str(),
                     enroll_name.c_str(), (int)target_speaker,
                     profile.embeddings.size());
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = std::string("enroll.finalize: ") + e.what();
            return false;
        }
    });

    // --- Speaker management handlers ---

    server.on("speakers.list", [](const IpcRequest&, IpcResponse& resp, IpcError& err) {
        try {
            fs::path db_dir;
            {
                std::lock_guard<std::mutex> lock(g_config_mu);
                db_dir = g_config.speaker_db.empty()
                    ? default_speaker_db_dir() : g_config.speaker_db;
            }
            auto profiles = load_speaker_db(db_dir);
            std::string arr = "[";
            for (size_t i = 0; i < profiles.size(); ++i) {
                if (i > 0) arr += ",";
                arr += "{\"name\":\"" + json_escape(profiles[i].name)
                    + "\",\"enrollments\":" + std::to_string(profiles[i].embeddings.size())
                    + ",\"created\":\"" + json_escape(profiles[i].created)
                    + "\",\"updated\":\"" + json_escape(profiles[i].updated) + "\"}";
            }
            arr += "]";
            resp.result["speakers"] = arr;
            resp.result["count"] = static_cast<int64_t>(profiles.size());
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    server.on("speakers.remove", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        auto it = req.params.find("name");
        if (it == req.params.end() || json_val_as_string(it->second).empty()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "Missing 'name' parameter";
            return false;
        }
        std::string name = json_val_as_string(it->second);
        fs::path db_dir;
        {
            std::lock_guard<std::mutex> lock(g_config_mu);
            db_dir = g_config.speaker_db.empty()
                ? default_speaker_db_dir() : g_config.speaker_db;
        }
        if (remove_speaker(db_dir, name)) {
            resp.result["ok"] = true;
            resp.result["name"] = name;
            return true;
        } else {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "Speaker '" + name + "' not found";
            return false;
        }
    });

    server.on("speakers.reset", [](const IpcRequest&, IpcResponse& resp, IpcError& err) {
        try {
            fs::path db_dir;
            {
                std::lock_guard<std::mutex> lock(g_config_mu);
                db_dir = g_config.speaker_db.empty()
                    ? default_speaker_db_dir() : g_config.speaker_db;
            }
            int count = reset_speakers(db_dir);
            resp.result["ok"] = true;
            resp.result["removed"] = static_cast<int64_t>(count);
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

    // --- Model management handlers ---

    server.on("models.list", [](const IpcRequest&, IpcResponse& resp, IpcError&) {
        auto models = list_cached_models();
        std::string arr = "[";
        for (size_t i = 0; i < models.size(); ++i) {
            if (i > 0) arr += ",";
            arr += "{\"name\":\"" + json_escape(models[i].name)
                + "\",\"category\":\"" + json_escape(models[i].category)
                + "\",\"cached\":" + (models[i].cached ? "true" : "false")
                + ",\"size_bytes\":" + std::to_string(models[i].size_bytes)
                + ",\"path\":\"" + json_escape(models[i].path) + "\"}";
        }
        arr += "]";
        resp.result["models"] = arr;
        return true;
    });

    // C.7: models.ensure / models.update no longer spawn an ad-hoc
    // g_dl_worker thread guarded by g_downloading. They enqueue
    // JobKind::ModelDownload jobs into the JobQueue's model_download slot;
    // the long-lived model_dl_worker_loop drains it. The slot's capacity-1
    // FIFO is the single admission point — explicit downloads queue behind
    // any auto-triggered one (and vice-versa) instead of racing.
    server.on("models.ensure", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        Config cfg;
        bool allow_dl;
        {
            std::lock_guard<std::mutex> lock(g_config_mu);
            cfg = g_config;
            allow_dl = g_config.allow_client_downloads;
        }
        // Download initiation policy (C.7): operator-disablable via
        // `[server] allow_client_downloads`. Any PSK-authenticated client
        // may trigger downloads when enabled (the default).
        if (!allow_dl) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "Client-initiated model downloads are disabled "
                          "([server] allow_client_downloads=false)";
            return false;
        }

        auto it = req.params.find("whisper_model");
        std::string whisper_model = (it != req.params.end())
            ? json_val_as_string(it->second) : cfg.whisper_model;

        std::vector<std::string> enqueued;
        auto enqueue_if_missing = [&](const std::string& model_id, bool missing) {
            if (!missing) return;
            Job j;
            j.model_id = model_id;
            j.force_download = false;   // ensure semantics: no-op if cached.
            g_jobs->enqueue(std::move(j), JobKind::ModelDownload, req.client_id);
            enqueued.push_back(model_id);
        };

        try {
            enqueue_if_missing("whisper/" + whisper_model,
                               !is_whisper_model_cached(whisper_model));
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = e.what();
            return false;
        }
#if RECMEET_USE_SHERPA
        if (cfg.diarize)
            enqueue_if_missing("sherpa/diarization", !is_sherpa_model_cached());
        if (cfg.vad)
            enqueue_if_missing("vad", !is_vad_model_cached());
#endif

        resp.result["ok"] = true;
        resp.result["enqueued"] = static_cast<int64_t>(enqueued.size());
        return true;
    });

    server.on("models.update", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        bool allow_dl;
        {
            std::lock_guard<std::mutex> lock(g_config_mu);
            allow_dl = g_config.allow_client_downloads;
        }
        if (!allow_dl) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "Client-initiated model downloads are disabled "
                          "([server] allow_client_downloads=false)";
            return false;
        }

        // Refresh every currently-cached model: one ModelDownload job per
        // model, force_download=true so it re-fetches even when cached.
        auto models = list_cached_models();
        std::vector<std::string> enqueued;
        bool sherpa_enqueued = false;
        for (const auto& m : models) {
            if (!m.cached) continue;
            std::string model_id;
            if (m.category == "whisper") {
                model_id = "whisper/" + m.name;
            }
#if RECMEET_USE_SHERPA
            else if (m.category == "sherpa") {
                if (sherpa_enqueued) continue;
                model_id = "sherpa/diarization";
                sherpa_enqueued = true;
            } else if (m.category == "vad") {
                model_id = "vad";
            }
#endif
            else {
                continue;
            }
            Job j;
            j.model_id = model_id;
            j.force_download = true;
            g_jobs->enqueue(std::move(j), JobKind::ModelDownload, req.client_id);
            enqueued.push_back(model_id);
        }

        resp.result["ok"] = true;
        resp.result["enqueued"] = static_cast<int64_t>(enqueued.size());
        return true;
    });

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
