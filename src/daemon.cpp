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
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace recmeet;

// ---------------------------------------------------------------------------
// Daemon state
// ---------------------------------------------------------------------------

enum class DaemonState { Idle, Recording, Reprocessing, Postprocessing, Downloading };

static std::atomic<DaemonState> g_state{DaemonState::Idle};
static Config g_config;
static StopToken g_stop;
static std::mutex g_config_mu;

// Active recording/postprocessing thread
static std::thread g_worker;

// Global server pointer for signal handler
static IpcServer* g_server = nullptr;

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
    // SIGINT/SIGTERM → stop recording if active, then exit
    g_stop.request();
    if (g_server) g_server->stop();
}

// Suppress whisper log output in daemon mode
static void whisper_null_log(enum ggml_log_level, const char*, void*) {}

// ---------------------------------------------------------------------------
// Daemon state name
// ---------------------------------------------------------------------------

static const char* state_name(DaemonState s) {
    switch (s) {
        case DaemonState::Idle:            return "idle";
        case DaemonState::Recording:       return "recording";
        case DaemonState::Reprocessing:    return "reprocessing";
        case DaemonState::Postprocessing:  return "postprocessing";
        case DaemonState::Downloading:     return "downloading";
    }
    return "unknown";
}

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
        resp.result["state"] = std::string(state_name(g_state.load()));
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
        DaemonState expected = DaemonState::Idle;
        bool is_reprocess = false;

        // Check if this is a reprocess request (peek at params before full config merge)
        {
            auto it = req.params.find("reprocess_dir");
            if (it != req.params.end()) {
                std::string val = json_val_as_string(it->second);
                is_reprocess = !val.empty();
            }
        }

        DaemonState initial = is_reprocess ? DaemonState::Reprocessing : DaemonState::Recording;
        if (!g_state.compare_exchange_strong(expected, initial)) {
            err.code = static_cast<int>(IpcErrorCode::Busy);
            err.message = std::string("Daemon is busy (") + state_name(expected) + ")";
            return false;
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
            g_state.store(DaemonState::Idle);
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "Model setup failed";
            return false;
        }

        // Broadcast state change so all clients (including other trays) learn about it
        {
            IpcEvent ev;
            ev.event = "state.changed";
            ev.data["state"] = std::string(state_name(initial));
            server.broadcast(ev);
        }

        g_stop.reset();

        // Join any previous worker
        if (g_worker.joinable()) g_worker.join();

        g_worker = std::thread([&server, cfg]() {
            auto on_phase = [&server](const std::string& phase) {
                server.post([&server, phase]() {
                    IpcEvent ev;
                    ev.event = "phase";
                    ev.data["name"] = phase;
                    server.broadcast(ev);
                });
            };

            try {
                auto input = run_recording(cfg, g_stop, on_phase);

                // Reset stop token so postprocessing isn't immediately cancelled
                g_stop.reset();

                server.post([&server]() {
                    g_state.store(DaemonState::Postprocessing);
                    IpcEvent ev;
                    ev.event = "state.changed";
                    ev.data["state"] = std::string("postprocessing");
                    server.broadcast(ev);
                });

                auto on_progress = [&server](const std::string& phase, int percent) {
                    using clock = std::chrono::steady_clock;
                    static auto last_broadcast = clock::time_point{};
                    static int last_percent = -1;

                    auto now = clock::now();
                    int elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(
                        now - last_broadcast).count();
                    int jump = percent - last_percent;

                    // Throttle: broadcast every 120s or on ≥10% jump
                    if (last_percent < 0 || elapsed_sec >= 120 || jump >= 10) {
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

                auto result = run_postprocessing(cfg, input, on_phase, on_progress, &g_stop);

                server.post([&server, result]() {
                    g_state.store(DaemonState::Idle);
                    IpcEvent ev;
                    ev.event = "job.complete";
                    ev.data["note_path"] = result.note_path.string();
                    ev.data["output_dir"] = result.output_dir.string();
                    server.broadcast(ev);

                    IpcEvent state_ev;
                    state_ev.event = "state.changed";
                    state_ev.data["state"] = std::string("idle");
                    server.broadcast(state_ev);
                });
            } catch (const std::exception& e) {
                fprintf(stderr, "Pipeline error: %s\n", e.what());
                log_error("daemon: pipeline error: %s", e.what());
                server.post([&server, what = std::string(e.what())]() {
                    g_state.store(DaemonState::Idle);
                    IpcEvent ev;
                    ev.event = "state.changed";
                    ev.data["state"] = std::string("idle");
                    ev.data["error"] = what;
                    server.broadcast(ev);
                });
            }
        });

        resp.result["ok"] = true;
        return true;
    });

    server.on("record.stop", [](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
        DaemonState cur = g_state.load();
        if (cur != DaemonState::Recording && cur != DaemonState::Reprocessing
            && cur != DaemonState::Postprocessing) {
            err.code = static_cast<int>(IpcErrorCode::NotRecording);
            err.message = "Not recording";
            return false;
        }
        g_stop.request();
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
        DaemonState expected = DaemonState::Idle;
        if (!g_state.compare_exchange_strong(expected, DaemonState::Downloading)) {
            err.code = static_cast<int>(IpcErrorCode::Busy);
            err.message = std::string("Daemon is busy (") + state_name(expected) + ")";
            return false;
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

        if (g_worker.joinable()) g_worker.join();

        g_worker = std::thread([&server, whisper_model, want_sherpa, want_vad]() {
            auto broadcast_dl = [&server](const std::string& model, const std::string& status) {
                server.post([&server, model, status]() {
                    IpcEvent ev;
                    ev.event = "model.downloading";
                    ev.data["model"] = model;
                    ev.data["status"] = status;
                    server.broadcast(ev);
                });
            };

            server.post([&server]() {
                IpcEvent ev;
                ev.event = "state.changed";
                ev.data["state"] = std::string("downloading");
                server.broadcast(ev);
            });

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

            server.post([&server]() {
                g_state.store(DaemonState::Idle);
                IpcEvent ev;
                ev.event = "state.changed";
                ev.data["state"] = std::string("idle");
                server.broadcast(ev);
            });
        });

        resp.result["ok"] = true;
        return true;
    });

    server.on("models.update", [&server](const IpcRequest&, IpcResponse& resp, IpcError& err) {
        DaemonState expected = DaemonState::Idle;
        if (!g_state.compare_exchange_strong(expected, DaemonState::Downloading)) {
            err.code = static_cast<int>(IpcErrorCode::Busy);
            err.message = std::string("Daemon is busy (") + state_name(expected) + ")";
            return false;
        }

        if (g_worker.joinable()) g_worker.join();

        g_worker = std::thread([&server]() {
            auto broadcast_dl = [&server](const std::string& model, const std::string& status) {
                server.post([&server, model, status]() {
                    IpcEvent ev;
                    ev.event = "model.downloading";
                    ev.data["model"] = model;
                    ev.data["status"] = status;
                    server.broadcast(ev);
                });
            };

            server.post([&server]() {
                IpcEvent ev;
                ev.event = "state.changed";
                ev.data["state"] = std::string("downloading");
                server.broadcast(ev);
            });

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

            server.post([&server]() {
                g_state.store(DaemonState::Idle);
                IpcEvent ev;
                ev.event = "state.changed";
                ev.data["state"] = std::string("idle");
                server.broadcast(ev);
            });
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

    // Cleanup
    log_info("daemon: shutting down");
    g_stop.request();
    if (g_worker.joinable()) g_worker.join();
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
