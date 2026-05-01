// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "config.h"
#include "config_json.h"
#include "device_enum.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "ndjson_parse.h"
#include "speaker_id.h"
#include "summarize.h"
#include "log.h"
#include "model_manager.h"
#include "notify.h"
#include "pipeline.h"
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
// Daemon state — independent atomics for concurrent recording + postprocessing
// ---------------------------------------------------------------------------

static std::atomic<bool> g_recording{false};
static std::atomic<bool> g_postprocessing{false};
static std::atomic<bool> g_downloading{false};
static std::mutex g_state_mu;  // guards multi-flag transitions

static Config g_config;
static std::mutex g_config_mu;

// Stop tokens — separate for independent cancellation
static StopToken g_rec_stop;
static StopToken g_pp_stop;

// Worker threads
static std::thread g_rec_worker;
static std::thread g_pp_worker;   // long-lived, drains job queue
static std::thread g_dl_worker;

// Postprocessing job queue
static std::atomic<int64_t> g_next_job_id{1};

struct PostprocessJob {
    int64_t job_id;
    PostprocessInput input;
    Config cfg;
};

static std::mutex g_queue_mu;
static std::queue<PostprocessJob> g_job_queue;
static std::condition_variable g_queue_cv;
static bool g_queue_shutdown{false};

// Whether the current recording is a reprocess (poll-thread-only variable,
// set via server.post() from the recording worker)
static bool g_is_reprocess = false;

// Pending context from tray dialog (sent via job.context before record.stop)
static std::mutex g_context_mu;
static std::string g_pending_context;
static std::string g_pending_vocab;

// Subprocess postprocessing state
static std::atomic<pid_t> g_pp_child_pid{-1};
static std::string g_self_exe;  // resolved at startup

// Global server pointer for signal handler
static IpcServer* g_server = nullptr;

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

static std::string composite_state_name() {
    bool rec = g_recording.load();
    bool pp  = g_postprocessing.load();
    bool dl  = g_downloading.load();
    if (rec && pp) return g_is_reprocess ? "reprocessing+postprocessing" : "recording+postprocessing";
    if (rec)       return g_is_reprocess ? "reprocessing" : "recording";
    if (pp)        return "postprocessing";
    if (dl)        return "downloading";
    return "idle";
}

static void fill_state_fields(JsonMap& data) {
    data["state"] = composite_state_name();
    data["recording"] = g_recording.load();
    data["postprocessing"] = g_postprocessing.load();
    data["downloading"] = g_downloading.load();
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
    // SIGINT/SIGTERM → stop all workers, then exit
    g_rec_stop.request();
    g_pp_stop.request();
    if (g_server) g_server->stop();
}

// Suppress whisper log output in daemon mode
static void whisper_null_log(enum ggml_log_level, const char*, void*) {}

// ---------------------------------------------------------------------------
// Helper: ensure models are ready (non-interactive)
// ---------------------------------------------------------------------------

static bool ensure_models(Config& cfg) {
    try {
        ensure_whisper_model(cfg.whisper_model);
    } catch (const RecmeetError& e) {
        log_error("daemon: whisper model: %s", e.what());
        return false;
    }

    if (!cfg.no_summary && !cfg.llm_model.empty()) {
        try {
            ensure_llama_model(cfg.llm_model);
        } catch (const RecmeetError& e) {
            log_error("daemon: llama model: %s", e.what());
            cfg.no_summary = true;
        }
    }

#if RECMEET_USE_SHERPA
    if (cfg.diarize && !is_sherpa_model_cached()) {
        try {
            ensure_sherpa_models();
        } catch (const RecmeetError& e) {
            log_error("daemon: sherpa models: %s", e.what());
            cfg.diarize = false;
        }
    }
    if (cfg.vad && !is_vad_model_cached()) {
        try {
            ensure_vad_model();
        } catch (const RecmeetError& e) {
            log_error("daemon: VAD model: %s", e.what());
            cfg.vad = false;
        }
    }
#else
    cfg.diarize = false;
    cfg.vad = false;
#endif

    return true;
}

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
    std::lock_guard<std::mutex> lk(g_queue_mu);
    return g_job_queue.empty();
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
        PostprocessJob job;
        bool already_flagged = false;
        {
            std::unique_lock<std::mutex> lock(g_queue_mu);
            g_queue_cv.wait(lock, [] {
                return !g_job_queue.empty() || g_queue_shutdown;
            });
            if (g_queue_shutdown) {
                log_debug("daemon: pp_worker_loop EXIT (shutdown)");
                return;
            }
            job = std::move(g_job_queue.front());
            g_job_queue.pop();
            log_debug("daemon: pp_worker dequeued job=%ld (queue_size=%zu)", (long)job.job_id, g_job_queue.size());

            // Check if recording worker already set g_postprocessing
            // (atomic handoff — avoids transient idle)
            already_flagged = g_postprocessing.load();
        }

        if (!already_flagged) {
            {
                std::lock_guard<std::mutex> lock(g_state_mu);
                g_postprocessing.store(true);
                log_debug("daemon: state postprocessing=true");
            }
            broadcast_state(server);
        }
        g_pp_stop.reset();

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
                                server.post([&server, name]() {
                                    IpcEvent ev;
                                    ev.event = "phase";
                                    ev.data["name"] = name;
                                    server.broadcast(ev);
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
                                    server.post([&server, phase, percent]() {
                                        IpcEvent ev;
                                        ev.event = "progress";
                                        ev.data["phase"] = phase;
                                        ev.data["percent"] = percent;
                                        server.broadcast(ev);
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
                int64_t jid = job.job_id;
                server.post([&server, captured_note_path, captured_output_dir, jid]() {
                    IpcEvent ev;
                    ev.event = "job.complete";
                    ev.data["note_path"] = captured_note_path;
                    ev.data["output_dir"] = captured_output_dir;
                    ev.data["job_id"] = jid;
                    server.broadcast(ev);
                });
            } else if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
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
            }
        }

    clear_state:
        {
            std::lock_guard<std::mutex> lock(g_state_mu);
            g_postprocessing.store(false);
            log_debug("daemon: state postprocessing=false");
        }
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

    // --- Method handlers ---

    server.on("status.get", [](const IpcRequest& req, IpcResponse& resp, IpcError&) {
        fill_state_fields(resp.result);
        {
            std::lock_guard<std::mutex> lock(g_queue_mu);
            resp.result["queue_depth"] = static_cast<int64_t>(g_job_queue.size());
        }
        return true;
    });

    server.on("sources.list", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        try {
            auto sources = list_sources();
            // Return as a JSON array string in result
            std::string arr = "[";
            for (size_t i = 0; i < sources.size(); ++i) {
                if (i > 0) arr += ",";
                arr += "{\"name\":\"" + json_escape(sources[i].name)
                    + "\",\"description\":\"" + json_escape(sources[i].description)
                    + "\",\"is_monitor\":" + (sources[i].is_monitor ? "true" : "false") + "}";
            }
            arr += "]";
            resp.result["sources"] = arr;
            resp.result["count"] = static_cast<int64_t>(sources.size());
            return true;
        } catch (const std::exception& e) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = e.what();
            return false;
        }
    });

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

    server.on("config.update", [](const IpcRequest& req, IpcResponse& resp, IpcError&) {
        std::lock_guard<std::mutex> lock(g_config_mu);
        // Apply params as config overrides
        JsonMap merged = config_to_map(g_config);
        for (const auto& [k, v] : req.params)
            merged[k] = v;
        g_config = config_from_map(merged);
        resp.result["ok"] = true;
        return true;
    });

    server.on("record.start", [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        bool is_reprocess = false;

        // Check if this is a reprocess request (peek at params before full config merge)
        {
            auto it = req.params.find("reprocess_dir");
            if (it != req.params.end()) {
                std::string val = json_val_as_string(it->second);
                is_reprocess = !val.empty();
            }
        }

        // Guard: !g_recording && !g_downloading (allow during postprocessing)
        {
            std::lock_guard<std::mutex> lock(g_state_mu);
            if (g_recording.load() || g_downloading.load()) {
                err.code = static_cast<int>(IpcErrorCode::Busy);
                err.message = "Daemon is busy (" + composite_state_name() + ")";
                return false;
            }
            g_recording.store(true);
        }

        // Snapshot config with any per-request overrides
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(g_config_mu);
            cfg = g_config;
        }
        JsonMap merged = config_to_map(cfg);
        for (const auto& [k, v] : req.params)
            merged[k] = v;
        cfg = config_from_map(merged);

        // Client-sent API key takes precedence (already merged above).
        // Fall back to daemon's key if client didn't send one.
        if (cfg.api_key.empty()) {
            std::lock_guard<std::mutex> lock2(g_config_mu);
            cfg.api_key = g_config.api_key;
        }

        if (!ensure_models(cfg)) {
            std::lock_guard<std::mutex> lock(g_state_mu);
            g_recording.store(false);
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "Model setup failed";
            return false;
        }

        // Assign job_id for this recording
        int64_t job_id = g_next_job_id.fetch_add(1);
        log_info("daemon: record.start (mode=%s, job=%ld)",
                 is_reprocess ? "reprocess" : "recording", (long)job_id);

        // Set reprocess flag on poll thread and broadcast
        g_is_reprocess = is_reprocess;
        broadcast_state_inline(server);

        g_rec_stop.reset();

        // Join any previous recording worker
        if (g_rec_worker.joinable()) g_rec_worker.join();

        g_rec_worker = std::thread([&server, cfg, is_reprocess, job_id]() {
            log_debug("daemon: rec_worker ENTER (tid=%d, job=%ld)", (int)syscall(SYS_gettid), (long)job_id);
            auto on_phase = [&server](const std::string& phase) {
                server.post([&server, phase]() {
                    IpcEvent ev;
                    ev.event = "phase";
                    ev.data["name"] = phase;
                    server.broadcast(ev);
                });
            };

            try {
                auto input = run_recording(cfg, g_rec_stop, on_phase);

                // Apply any context sent by tray before stop
                Config job_cfg = cfg;
                {
                    std::lock_guard<std::mutex> ctx_lock(g_context_mu);
                    if (!g_pending_context.empty()) {
                        job_cfg.context_inline = g_pending_context;
                        g_pending_context.clear();
                    }
                    if (!g_pending_vocab.empty()) {
                        if (!job_cfg.vocabulary.empty()) job_cfg.vocabulary += ", ";
                        job_cfg.vocabulary += g_pending_vocab;
                        g_pending_vocab.clear();
                    }
                }

                // Enqueue postprocessing job
                PostprocessJob job;
                job.job_id = job_id;
                job.input = std::move(input);
                job.cfg = job_cfg;

                log_debug("daemon: rec_worker handoff to pp (job=%ld)", (long)job_id);
                {
                    std::lock_guard<std::mutex> qlock(g_queue_mu);
                    g_job_queue.push(std::move(job));
                }

                // Atomic handoff: set postprocessing BEFORE clearing recording
                {
                    std::lock_guard<std::mutex> lock(g_state_mu);
                    g_postprocessing.store(true);
                    log_debug("daemon: state postprocessing=true");
                    g_recording.store(false);
                }

                // Clear reprocess flag and broadcast new state
                server.post([&server]() {
                    g_is_reprocess = false;
                });
                broadcast_state(server);

                // Wake the pp worker
                g_queue_cv.notify_one();
                log_debug("daemon: rec_worker EXIT (job=%ld)", (long)job_id);

            } catch (const std::exception& e) {
                fprintf(stderr, "Recording error: %s\n", e.what());
                log_error("daemon: recording error: %s", e.what());

                {
                    std::lock_guard<std::mutex> lock(g_state_mu);
                    g_recording.store(false);
                }
                server.post([&server]() {
                    g_is_reprocess = false;
                });
                broadcast_state(server, e.what());
                log_debug("daemon: rec_worker EXIT (job=%ld)", (long)job_id);
            }
        });

        resp.result["ok"] = true;
        resp.result["job_id"] = job_id;
        return true;
    });

    server.on("record.stop", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        // Parse optional target param: "recording", "postprocessing", "all" (default)
        std::string target = "all";
        {
            auto it = req.params.find("target");
            if (it != req.params.end()) {
                std::string val = json_val_as_string(it->second);
                if (!val.empty()) target = val;
            }
        }

        bool rec = g_recording.load();
        bool pp  = g_postprocessing.load();

        if (!rec && !pp) {
            err.code = static_cast<int>(IpcErrorCode::NotRecording);
            err.message = "Not recording";
            return false;
        }

        if (target == "recording") {
            if (!rec) {
                err.code = static_cast<int>(IpcErrorCode::NotRecording);
                err.message = "Not recording";
                return false;
            }
            g_rec_stop.request();
        } else if (target == "postprocessing") {
            if (!pp) {
                err.code = static_cast<int>(IpcErrorCode::NotRecording);
                err.message = "Not postprocessing";
                return false;
            }
            g_pp_stop.request();
            { pid_t child = g_pp_child_pid.load(); if (child > 0) kill(child, SIGTERM); }
        } else {
            // "all" — stop everything
            if (rec) g_rec_stop.request();
            if (pp) {
                g_pp_stop.request();
                pid_t child = g_pp_child_pid.load();
                if (child > 0) kill(child, SIGTERM);
            }
        }

        resp.result["ok"] = true;
        return true;
    });

    server.on("job.context", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        if (!g_recording.load()) {
            err.code = static_cast<int>(IpcErrorCode::NotRecording);
            err.message = "No active recording";
            return false;
        }
        std::lock_guard<std::mutex> lock(g_context_mu);
        auto ctx_it = req.params.find("context_inline");
        if (ctx_it != req.params.end())
            g_pending_context = json_val_as_string(ctx_it->second);
        auto vocab_it = req.params.find("vocabulary_append");
        if (vocab_it != req.params.end())
            g_pending_vocab = json_val_as_string(vocab_it->second);
        resp.result["ok"] = true;
        return true;
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

    server.on("models.ensure", [&server](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        // Guard: !g_recording && !g_downloading (allow during postprocessing)
        {
            std::lock_guard<std::mutex> lock(g_state_mu);
            if (g_recording.load() || g_downloading.load()) {
                err.code = static_cast<int>(IpcErrorCode::Busy);
                err.message = "Daemon is busy (" + composite_state_name() + ")";
                return false;
            }
            g_downloading.store(true);
        }

        // Determine what to download
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(g_config_mu);
            cfg = g_config;
        }
        auto it = req.params.find("whisper_model");
        std::string whisper_model = (it != req.params.end())
            ? json_val_as_string(it->second) : cfg.whisper_model;
        bool want_sherpa = cfg.diarize;
        bool want_vad = cfg.vad;

        if (g_dl_worker.joinable()) g_dl_worker.join();

        broadcast_state_inline(server);

        g_dl_worker = std::thread([&server, whisper_model, want_sherpa, want_vad]() {
            log_debug("daemon: dl_worker ENTER (tid=%d)", (int)syscall(SYS_gettid));
            auto broadcast_dl = [&server](const std::string& model, const std::string& status) {
                server.post([&server, model, status]() {
                    IpcEvent ev;
                    ev.event = "model.downloading";
                    ev.data["model"] = model;
                    ev.data["status"] = status;
                    server.broadcast(ev);
                });
            };

            try {
                if (!is_whisper_model_cached(whisper_model)) {
                    broadcast_dl("whisper/" + whisper_model, "downloading");
                    ensure_whisper_model(whisper_model);
                    broadcast_dl("whisper/" + whisper_model, "complete");
                }

#if RECMEET_USE_SHERPA
                if (want_sherpa && !is_sherpa_model_cached()) {
                    broadcast_dl("sherpa/diarization", "downloading");
                    ensure_sherpa_models();
                    broadcast_dl("sherpa/diarization", "complete");
                }
                if (want_vad && !is_vad_model_cached()) {
                    broadcast_dl("vad", "downloading");
                    ensure_vad_model();
                    broadcast_dl("vad", "complete");
                }
#endif
            } catch (const std::exception& e) {
                log_error("daemon: model download failed: %s", e.what());
                server.post([&server, what = std::string(e.what())]() {
                    IpcEvent ev;
                    ev.event = "model.downloading";
                    ev.data["status"] = std::string("error");
                    ev.data["error"] = what;
                    server.broadcast(ev);
                });
            }

            {
                std::lock_guard<std::mutex> lock(g_state_mu);
                g_downloading.store(false);
            }
            log_debug("daemon: dl_worker EXIT");
            broadcast_state(server);
        });

        resp.result["ok"] = true;
        return true;
    });

    server.on("models.update", [&server](const IpcRequest&, IpcResponse& resp, IpcError& err) {
        // Guard: !g_recording && !g_downloading (allow during postprocessing)
        {
            std::lock_guard<std::mutex> lock(g_state_mu);
            if (g_recording.load() || g_downloading.load()) {
                err.code = static_cast<int>(IpcErrorCode::Busy);
                err.message = "Daemon is busy (" + composite_state_name() + ")";
                return false;
            }
            g_downloading.store(true);
        }

        if (g_dl_worker.joinable()) g_dl_worker.join();

        broadcast_state_inline(server);

        g_dl_worker = std::thread([&server]() {
            log_debug("daemon: dl_worker ENTER (tid=%d)", (int)syscall(SYS_gettid));
            auto broadcast_dl = [&server](const std::string& model, const std::string& status) {
                server.post([&server, model, status]() {
                    IpcEvent ev;
                    ev.event = "model.downloading";
                    ev.data["model"] = model;
                    ev.data["status"] = status;
                    server.broadcast(ev);
                });
            };

            auto models = list_cached_models();
            bool sherpa_updated = false;

            for (const auto& m : models) {
                if (!m.cached) continue;

                try {
                    std::string label = m.category + "/" + m.name;
                    if (m.category == "whisper") {
                        broadcast_dl(label, "downloading");
                        download_whisper_model(m.name);
                        broadcast_dl(label, "complete");
                    }
#if RECMEET_USE_SHERPA
                    else if (m.category == "sherpa" && !sherpa_updated) {
                        broadcast_dl("sherpa/diarization", "downloading");
                        download_sherpa_models();
                        broadcast_dl("sherpa/diarization", "complete");
                        sherpa_updated = true;
                    } else if (m.category == "vad") {
                        broadcast_dl("vad", "downloading");
                        download_vad_model();
                        broadcast_dl("vad", "complete");
                    }
#endif
                } catch (const std::exception& e) {
                    log_error("daemon: model update failed: %s", e.what());
                    server.post([&server, what = std::string(e.what())]() {
                        IpcEvent ev;
                        ev.event = "model.downloading";
                        ev.data["status"] = std::string("error");
                        ev.data["error"] = what;
                        server.broadcast(ev);
                    });
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_state_mu);
                g_downloading.store(false);
            }
            log_debug("daemon: dl_worker EXIT");
            broadcast_state(server);
        });

        resp.result["ok"] = true;
        return true;
    });

    // --- Start server ---

    if (!server.start()) {
        fprintf(stderr, "Failed to start daemon on %s\n", socket_path.c_str());
        log_shutdown();
        notify_cleanup();
        return 1;
    }

    // Start the long-lived postprocessing worker
    g_pp_worker = std::thread([&server]() { pp_worker_loop(server); });

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

    // Cleanup — shut down all workers
    log_info("daemon: shutting down");
    g_rec_stop.request();
    g_pp_stop.request();
    { pid_t child = g_pp_child_pid.load(); if (child > 0) kill(child, SIGTERM); }

    // Shut down the pp worker queue
    {
        std::lock_guard<std::mutex> lock(g_queue_mu);
        g_queue_shutdown = true;
    }
    g_queue_cv.notify_one();

    log_debug("daemon: shutdown: joining rec_worker...");
    if (g_rec_worker.joinable()) g_rec_worker.join();
    log_debug("daemon: shutdown: joining pp_worker...");
    if (g_pp_worker.joinable()) g_pp_worker.join();
    if (g_dl_worker.joinable()) g_dl_worker.join();
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
