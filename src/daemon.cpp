// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "config.h"
#include "config_json.h"
#include "device_enum.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
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
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <sys/file.h>
#include <sys/stat.h>
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
// Postprocessing worker loop (long-lived thread)
// ---------------------------------------------------------------------------

static void pp_worker_loop(IpcServer& server) {
    while (true) {
        PostprocessJob job;
        bool already_flagged = false;
        {
            std::unique_lock<std::mutex> lock(g_queue_mu);
            g_queue_cv.wait(lock, [] {
                return !g_job_queue.empty() || g_queue_shutdown;
            });
            if (g_queue_shutdown) return;
            job = std::move(g_job_queue.front());
            g_job_queue.pop();

            // Check if recording worker already set g_postprocessing
            // (atomic handoff — avoids transient idle)
            already_flagged = g_postprocessing.load();
        }

        if (!already_flagged) {
            // Direct enqueue (shouldn't normally happen, but be safe)
            {
                std::lock_guard<std::mutex> lock(g_state_mu);
                g_postprocessing.store(true);
            }
            broadcast_state(server);
        }
        g_pp_stop.reset();

        try {
            // Per-job progress throttle state (NOT static)
            auto last_broadcast = std::chrono::steady_clock::time_point{};
            int last_percent = -1;

            auto on_phase = [&server](const std::string& name) {
                server.post([&server, name]() {
                    IpcEvent ev;
                    ev.event = "phase";
                    ev.data["name"] = name;
                    server.broadcast(ev);
                });
            };

            auto on_progress = [&server, &last_broadcast, &last_percent](
                                   const std::string& phase, int percent) {
                using clock = std::chrono::steady_clock;
                auto now = clock::now();
                int elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_broadcast).count();
                int jump = percent - last_percent;
                if (last_percent < 0 || elapsed >= 120 || jump >= 10) {
                    last_broadcast = now;
                    last_percent = percent;
                    server.post([&server, phase, percent]() {
                        IpcEvent ev;
                        ev.event = "progress";
                        ev.data["phase"] = phase;
                        ev.data["percent"] = static_cast<int64_t>(percent);
                        server.broadcast(ev);
                    });
                }
            };

            PipelineResult result = run_postprocessing(
                job.cfg, job.input, on_phase, on_progress, &g_pp_stop);

            // Broadcast job.complete with job_id
            int64_t jid = job.job_id;
            server.post([&server, result, jid]() {
                IpcEvent ev;
                ev.event = "job.complete";
                ev.data["note_path"] = result.note_path.string();
                ev.data["output_dir"] = result.output_dir.string();
                ev.data["job_id"] = jid;
                server.broadcast(ev);
            });
        } catch (const std::exception& e) {
            log_error("daemon: postprocessing failed: %s", e.what());
            broadcast_state(server, e.what());
        }

        // Clear postprocessing flag
        {
            std::lock_guard<std::mutex> lock(g_state_mu);
            g_postprocessing.store(false);
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
        "  --socket PATH     Unix socket path (default: $XDG_RUNTIME_DIR/recmeet/daemon.sock)\n"
        "  --log-level LEVEL Log level: none, error, warn, info (default: info)\n"
        "  --log-dir DIR     Log file directory\n"
        "  -h, --help        Show this help\n"
        "  -v, --version     Show version\n"
    );
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::string socket_path = default_socket_path();
    std::string log_level_str = "info";
    fs::path log_dir;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-h" || arg == "--help")) { print_usage(); return 0; }
        if ((arg == "-v" || arg == "--version")) { printf("recmeet-daemon %s\n", RECMEET_VERSION); return 0; }
        if (arg == "--socket" && i + 1 < argc) { socket_path = argv[++i]; continue; }
        if (arg == "--log-level" && i + 1 < argc) { log_level_str = argv[++i]; continue; }
        if (arg == "--log-dir" && i + 1 < argc) { log_dir = argv[++i]; continue; }
        fprintf(stderr, "Unknown option: %s\n", arg.c_str());
        return 1;
    }

    // PID file with flock() to prevent duplicate daemons
    std::string pid_path = socket_path + ".pid";
    std::string pid_dir = pid_path.substr(0, pid_path.rfind('/'));
    if (!pid_dir.empty()) mkdir(pid_dir.c_str(), 0700);
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

    // Initialize logging
    auto log_level = parse_log_level(log_level_str);
    log_init(log_level, log_dir);
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

        // Set reprocess flag on poll thread and broadcast
        g_is_reprocess = is_reprocess;
        broadcast_state_inline(server);

        g_rec_stop.reset();

        // Join any previous recording worker
        if (g_rec_worker.joinable()) g_rec_worker.join();

        g_rec_worker = std::thread([&server, cfg, is_reprocess, job_id]() {
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

                // Enqueue postprocessing job
                PostprocessJob job;
                job.job_id = job_id;
                job.input = std::move(input);
                job.cfg = cfg;

                {
                    std::lock_guard<std::mutex> qlock(g_queue_mu);
                    g_job_queue.push(std::move(job));
                }

                // Atomic handoff: set postprocessing BEFORE clearing recording
                {
                    std::lock_guard<std::mutex> lock(g_state_mu);
                    g_postprocessing.store(true);
                    g_recording.store(false);
                }

                // Clear reprocess flag and broadcast new state
                server.post([&server]() {
                    g_is_reprocess = false;
                });
                broadcast_state(server);

                // Wake the pp worker
                g_queue_cv.notify_one();

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
        } else {
            // "all" — stop everything
            if (rec) g_rec_stop.request();
            if (pp) g_pp_stop.request();
        }

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

    server.run();

    // Cleanup — shut down all workers
    log_info("daemon: shutting down");
    g_rec_stop.request();
    g_pp_stop.request();

    // Shut down the pp worker queue
    {
        std::lock_guard<std::mutex> lock(g_queue_mu);
        g_queue_shutdown = true;
    }
    g_queue_cv.notify_one();

    if (g_rec_worker.joinable()) g_rec_worker.join();
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
