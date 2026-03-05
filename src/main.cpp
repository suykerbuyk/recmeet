// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "cli.h"
#include "config.h"
#include "config_json.h"
#include "device_enum.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "log.h"
#include "model_manager.h"
#include "notify.h"
#include "pipeline.h"
#include "util.h"
#include "version.h"

#include <whisper.h>

#include <csignal>
#include <cstring>
#include <cstdio>
#include <unistd.h>

using namespace recmeet;

static StopToken g_stop;

static void signal_handler(int) {
    g_stop.request();
}

// Whisper log callback for interactive CLI use.
// On a TTY, INFO-level messages (model loading, "auto-detected language", etc.)
// overwrite in place via \r so they don't scroll the terminal.  WARN/ERROR
// messages print normally so they remain visible.
static bool g_whisper_overwrote = false;

static void whisper_cli_log(enum ggml_log_level level, const char* text, void*) {
    if (!text || !*text) return;
    static const bool is_tty = isatty(STDERR_FILENO);

    if (is_tty && (level <= GGML_LOG_LEVEL_INFO || level == GGML_LOG_LEVEL_CONT)) {
        size_t len = std::strlen(text);
        while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'))
            --len;
        if (len > 0) {
            fprintf(stderr, "\r\033[K%.*s", (int)len, text);
            g_whisper_overwrote = true;
        }
    } else {
        if (g_whisper_overwrote) {
            fprintf(stderr, "\n");
            g_whisper_overwrote = false;
        }
        fprintf(stderr, "%s", text);
    }
}

// Clear any pending in-place whisper line before printing other output.
static void whisper_flush_line() {
    if (g_whisper_overwrote) {
        fprintf(stderr, "\r\033[K");
        g_whisper_overwrote = false;
    }
}

static void print_usage() {
    fprintf(stderr,
        "Usage: recmeet [OPTIONS]\n"
        "\n"
        "Record, transcribe, and summarize meetings.\n"
        "\n"
        "Options:\n"
        "  --source NAME        PipeWire/PulseAudio mic source (auto-detect if omitted)\n"
        "  --monitor NAME       Monitor/speaker source (auto-detect if omitted)\n"
        "  --mic-only           Record mic only (skip monitor capture)\n"
        "  --keep-sources       Keep separate mic.wav and monitor.wav after mixing\n"
        "  --model NAME         Whisper model: tiny/base/small/medium/large-v3 (default: base)\n"
        "  --language CODE      Force whisper language (e.g. en, de, ja; default: auto-detect)\n"
        "  --output-dir DIR     Base directory for outputs (default: ./meetings)\n"
        "  --note-dir DIR       Directory for meeting notes (default: same as audio)\n"
        "  --provider NAME      API provider: xai, openai, anthropic (default: xai)\n"
        "  --api-key KEY        API key (default: from provider env var or config)\n"
        "  --api-url URL        API endpoint override (default: derived from provider)\n"
        "  --api-model NAME     API model name (default: provider's default model)\n"
        "  --no-summary         Skip summarization (record + transcribe only)\n"
        "  --device-pattern RE  Regex for device auto-detection\n"
        "  --context-file PATH  Pre-meeting notes to include in summary prompt\n"
        "  --llm-model PATH     Local GGUF model for summarization (instead of API)\n"
        "  --no-diarize         Disable speaker diarization\n"
        "  --num-speakers N     Number of speakers (0 = auto-detect, default: 0)\n"
        "  --cluster-threshold F  Clustering distance threshold (default: 1.18, higher = fewer speakers)\n"
        "  --no-vad             Disable VAD segmentation (transcribe full audio)\n"
        "  --vad-threshold F    VAD speech detection threshold (default: 0.5)\n"
        "  --threads N          Number of CPU threads for inference (0 = auto-detect, default: 0)\n"
        "  --reprocess DIR      Reprocess existing recording from audio.wav\n"
        "  --log-level LEVEL    Log level: none, error, warn, info (default: none)\n"
        "  --log-dir DIR        Log file directory (default: ~/.local/share/recmeet/logs/)\n"
        "  --list-sources       List available audio sources and exit\n"
        "  --download-models    Download required models and exit\n"
        "  --update-models      Re-download all cached models and exit\n"
        "  --daemon             Force client mode (require running daemon)\n"
        "  --no-daemon          Force standalone mode (skip daemon detection)\n"
        "  --status             Query daemon status and exit\n"
        "  --stop               Stop daemon recording and exit\n"
        "  -h, --help           Show this help\n"
        "  -v, --version        Show version\n"
    );
}

static void print_version() {
    printf("recmeet %s\n", RECMEET_VERSION);
}

// ---------------------------------------------------------------------------
// Client mode — talk to the daemon via IPC
// ---------------------------------------------------------------------------

static int client_status() {
    IpcClient client;
    if (!client.connect()) {
        printf("Daemon: not running\n");
        return 1;
    }
    IpcResponse resp;
    IpcError err;
    if (!client.call("status.get", resp, err)) {
        fprintf(stderr, "Error: %s\n", err.message.c_str());
        return 1;
    }
    printf("Daemon: running\nState: %s\n", json_val_as_string(resp.result["state"], "unknown").c_str());
    return 0;
}

static int client_stop() {
    IpcClient client;
    if (!client.connect()) {
        fprintf(stderr, "Daemon not running.\n");
        return 1;
    }
    IpcResponse resp;
    IpcError err;
    if (!client.call("record.stop", resp, err)) {
        fprintf(stderr, "Error: %s\n", err.message.c_str());
        return 1;
    }
    printf("Stop signal sent.\n");
    return 0;
}

static int client_record(const Config& cfg) {
    IpcClient client;
    if (!client.connect()) {
        fprintf(stderr, "Error: daemon not running. Start with: recmeet-daemon\n");
        return 1;
    }

    // Build params from config overrides
    JsonMap params = config_to_map(cfg);

    client.set_event_callback([](const IpcEvent& ev) {
        if (ev.event == "phase") {
            std::string name = json_val_as_string(ev.data.at("name"));
            fprintf(stderr, "Phase: %s\n", name.c_str());
        } else if (ev.event == "state.changed") {
            std::string state = json_val_as_string(ev.data.at("state"));
            auto err_it = ev.data.find("error");
            if (err_it != ev.data.end()) {
                std::string error = json_val_as_string(err_it->second);
                if (!error.empty())
                    fprintf(stderr, "Error: %s\n", error.c_str());
            }
        } else if (ev.event == "job.complete") {
            std::string note = json_val_as_string(ev.data.at("note_path"));
            std::string dir = json_val_as_string(ev.data.at("output_dir"));
            if (!note.empty())
                printf("Note: %s\n", note.c_str());
            printf("Output: %s\n", dir.c_str());
        }
    });

    IpcResponse resp;
    IpcError err;
    if (!client.call("record.start", params, resp, err)) {
        fprintf(stderr, "Error: %s\n", err.message.c_str());
        return 1;
    }

    fprintf(stderr, "Recording started. Press Ctrl+C to stop.\n");

    // Install signal handler to send stop on Ctrl+C
    static IpcClient* g_client = &client;
    struct sigaction sa{};
    sa.sa_handler = [](int) {
        if (g_client && g_client->connected()) {
            IpcResponse r; IpcError e;
            g_client->call("record.stop", r, e, 5000);
        }
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Wait for job completion or disconnect
    client.read_events("job.complete");

    g_client = nullptr;
    return 0;
}

// ---------------------------------------------------------------------------
// Standalone mode — run the pipeline directly (original behavior)
// ---------------------------------------------------------------------------

static int standalone_main(CliResult& cli);

int main(int argc, char* argv[]) {
    auto cli = recmeet::parse_cli(argc, argv);
    if (cli.show_version) { print_version(); return 0; }
    if (cli.show_help) { print_usage(); return 0; }

    // Status/stop commands always use client mode
    if (cli.show_status) return client_status();
    if (cli.send_stop) return client_stop();

    // Model management commands — always standalone, no daemon needed
    if (cli.download_models) {
        Config cfg = cli.cfg;
        fprintf(stderr, "Downloading models...\n");
        bool ok = true;
        try {
            fprintf(stderr, "  Whisper model '%s'... ", cfg.whisper_model.c_str());
            ensure_whisper_model(cfg.whisper_model);
            fprintf(stderr, "ready\n");
        } catch (const std::exception& e) {
            fprintf(stderr, "FAILED: %s\n", e.what());
            ok = false;
        }
#if RECMEET_USE_SHERPA
        if (cfg.diarize) {
            try {
                fprintf(stderr, "  Diarization models... ");
                ensure_sherpa_models();
                fprintf(stderr, "ready\n");
            } catch (const std::exception& e) {
                fprintf(stderr, "FAILED: %s\n", e.what());
            }
        }
        if (cfg.vad) {
            try {
                fprintf(stderr, "  VAD model... ");
                ensure_vad_model();
                fprintf(stderr, "ready\n");
            } catch (const std::exception& e) {
                fprintf(stderr, "FAILED: %s\n", e.what());
            }
        }
#endif
        fprintf(stderr, ok ? "Done.\n" : "Done (with errors).\n");
        return ok ? 0 : 1;
    }

    if (cli.update_models) {
        fprintf(stderr, "Updating cached models...\n");
        auto models = list_cached_models();
        bool any_error = false;
        bool sherpa_updated = false;

        for (const auto& m : models) {
            if (!m.cached) continue;

            fprintf(stderr, "  %s/%s... ", m.category.c_str(), m.name.c_str());
            try {
                if (m.category == "whisper") {
                    download_whisper_model(m.name);
                }
#if RECMEET_USE_SHERPA
                else if (m.category == "sherpa" && !sherpa_updated) {
                    download_sherpa_models();
                    sherpa_updated = true;
                } else if (m.category == "sherpa") {
                    fprintf(stderr, "(already updated)\n");
                    continue;
                } else if (m.category == "vad") {
                    download_vad_model();
                }
#endif
                fprintf(stderr, "updated\n");
            } catch (const std::exception& e) {
                fprintf(stderr, "FAILED: %s\n", e.what());
                any_error = true;
            }
        }

        bool any_cached = false;
        for (const auto& m : models) { if (m.cached) { any_cached = true; break; } }
        if (!any_cached)
            fprintf(stderr, "  No cached models found.\n");

        fprintf(stderr, any_error ? "Done (with errors).\n" : "Done.\n");
        return any_error ? 1 : 0;
    }

    Config cfg = cli.cfg;
    bool list_sources = cli.list_sources;

    // Initialize logging
    auto log_level = parse_log_level(cfg.log_level_str);
    log_init(log_level, cfg.log_dir);

    // Determine whether to use daemon
    bool use_daemon = false;
    if (cli.daemon_mode == DaemonMode::Force) {
        use_daemon = true;
    } else if (cli.daemon_mode == DaemonMode::Disable) {
        use_daemon = false;
    } else {
        // Auto: use daemon if it's running
        use_daemon = daemon_running();
    }

    if (use_daemon && !list_sources) {
        log_info("Using daemon mode");
        int rc = client_record(cfg);
        log_shutdown();
        return rc;
    }

    // Fall through to standalone mode
    return standalone_main(cli);
}

static int standalone_main(CliResult& cli) {
    Config cfg = cli.cfg;

    // List sources mode
    if (cli.list_sources) {
        try {
            auto sources = recmeet::list_sources();
            printf("Available audio sources:\n");
            for (const auto& s : sources) {
                printf("  %-50s  %s%s\n", s.name.c_str(), s.description.c_str(),
                       s.is_monitor ? " [monitor]" : "");
            }
        } catch (const std::exception& e) {
            log_error("list_sources failed: %s", e.what());
            fprintf(stderr, "Error: %s\n", e.what());
            log_shutdown();
            return 1;
        }
        log_shutdown();
        return 0;
    }

    // Resolve API key from provider-specific env var
    if (cfg.llm_model.empty()) {
        const auto* prov = find_provider(cfg.provider);
        if (prov) {
            std::string key = resolve_api_key(*prov, cfg.api_key);
            if (!key.empty())
                cfg.api_key = key;
        }
    }

    // Validate: need API key for summary unless disabled or using local LLM
    if (!cfg.no_summary && cfg.api_key.empty() && cfg.llm_model.empty()) {
        const auto* prov = find_provider(cfg.provider);
        const char* env_var = prov ? prov->env_var : "XAI_API_KEY";
        fprintf(stderr, "Warning: No API key and no local LLM model. Summary will be skipped.\n");
        fprintf(stderr, "Set %s in environment, config, or use --api-key / --llm-model.\n", env_var);
        fprintf(stderr, "Use --no-summary to suppress this warning.\n\n");
        cfg.no_summary = true;
    }

    // Install signal handlers
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    notify_init();

    // Pre-check: ensure whisper model is available before recording/reprocessing
    {
        try {
            if (!is_whisper_model_cached(cfg.whisper_model)) {
                fprintf(stderr, "Whisper model '%s' not found locally.\n", cfg.whisper_model.c_str());
                fprintf(stderr, "Download now? [Y/n] ");
                int ch = getchar();
                if (ch == 'n' || ch == 'N') {
                    fprintf(stderr, "Aborted. Use --model to select a different model.\n");
                    log_shutdown();
                    notify_cleanup();
                    return 1;
                }
                fprintf(stderr, "Downloading...\n");
                ensure_whisper_model(cfg.whisper_model);
                fprintf(stderr, "Model ready.\n\n");
            }
        } catch (const RecmeetError& e) {
            fprintf(stderr, "Error: %s\n", e.what());
            log_shutdown();
            notify_cleanup();
            return 1;
        }
    }

    // Pre-check: validate LLM model path if local summarization is configured
    if (!cfg.no_summary && !cfg.llm_model.empty()) {
        try {
            ensure_llama_model(cfg.llm_model);
        } catch (const RecmeetError& e) {
            fprintf(stderr, "Error: %s\n", e.what());
            log_shutdown();
            notify_cleanup();
            return 1;
        }
    }

    // Pre-check: diarization
    if (cfg.diarize) {
#if RECMEET_USE_SHERPA
        if (!is_sherpa_model_cached()) {
            fprintf(stderr, "Speaker diarization models not found locally.\n");
            fprintf(stderr, "Download now? (~40 MB) [Y/n] ");
            int ch = getchar();
            if (ch == 'n' || ch == 'N') {
                fprintf(stderr, "Diarization disabled.\n");
                cfg.diarize = false;
            } else {
                try {
                    ensure_sherpa_models();
                    fprintf(stderr, "Diarization models ready.\n\n");
                } catch (const RecmeetError& e) {
                    fprintf(stderr, "Error downloading models: %s\n", e.what());
                    fprintf(stderr, "Diarization disabled.\n");
                    cfg.diarize = false;
                }
            }
        }
#else
        fprintf(stderr, "Warning: Diarization requires sherpa-onnx support (not compiled in).\n");
        fprintf(stderr, "Rebuild with: cmake -DRECMEET_USE_SHERPA=ON, or use --no-diarize to suppress.\n");
        cfg.diarize = false;
#endif
    }

    // Pre-check: VAD model
    if (cfg.vad) {
#if RECMEET_USE_SHERPA
        if (!is_vad_model_cached()) {
            fprintf(stderr, "VAD model (Silero) not found locally.\n");
            fprintf(stderr, "Download now? (~2 MB) [Y/n] ");
            int ch = getchar();
            if (ch == 'n' || ch == 'N') {
                fprintf(stderr, "VAD disabled.\n");
                cfg.vad = false;
            } else {
                try {
                    ensure_vad_model();
                    fprintf(stderr, "VAD model ready.\n\n");
                } catch (const RecmeetError& e) {
                    fprintf(stderr, "Error downloading VAD model: %s\n", e.what());
                    fprintf(stderr, "VAD disabled.\n");
                    cfg.vad = false;
                }
            }
        }
#else
        fprintf(stderr, "Warning: VAD requires sherpa-onnx support (not compiled in).\n");
        fprintf(stderr, "Rebuild with: cmake -DRECMEET_USE_SHERPA=ON, or use --no-vad to suppress.\n");
        cfg.vad = false;
#endif
    }

    if (cfg.reprocess_dir.empty())
        fprintf(stderr, "Press Ctrl+C to stop recording.\n\n");

    // On a TTY, whisper INFO messages overwrite in place instead of scrolling
    whisper_log_set(whisper_cli_log, nullptr);

    try {
        auto result = run_pipeline(cfg, g_stop);
        whisper_flush_line();
        (void)result;
    } catch (const RecmeetError& e) {
        whisper_flush_line();
        fprintf(stderr, "Error: %s\n", e.what());
        log_shutdown();
        notify_cleanup();
        return 1;
    } catch (const std::exception& e) {
        whisper_flush_line();
        fprintf(stderr, "Unexpected error: %s\n", e.what());
        log_shutdown();
        notify_cleanup();
        return 1;
    }

    log_shutdown();
    notify_cleanup();
    return 0;
}
