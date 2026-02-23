// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "cli.h"
#include "config.h"
#include "diarize.h"
#include "device_enum.h"
#include "model_manager.h"
#include "notify.h"
#include "pipeline.h"
#include "util.h"
#include "version.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>

using namespace recmeet;

static StopToken g_stop;

static void signal_handler(int) {
    g_stop.request();
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
        "  --model NAME         Whisper model: tiny/base/small/medium/large-v3 (default: base)\n"
        "  --language CODE      Force whisper language (e.g. en, de, ja; default: auto-detect)\n"
        "  --output-dir DIR     Base directory for outputs (default: ./meetings)\n"
        "  --provider NAME      API provider: xai, openai, anthropic (default: xai)\n"
        "  --api-key KEY        API key (default: from provider env var or config)\n"
        "  --api-url URL        API endpoint override (default: derived from provider)\n"
        "  --api-model NAME     API model name (default: provider's default model)\n"
        "  --no-summary         Skip summarization (record + transcribe only)\n"
        "  --device-pattern RE  Regex for device auto-detection\n"
        "  --context-file PATH  Pre-meeting notes to include in summary prompt\n"
        "  --obsidian-vault DIR Obsidian vault path for note output\n"
        "  --llm-model PATH     Local GGUF model for summarization (instead of API)\n"
        "  --no-diarize         Disable speaker diarization\n"
        "  --num-speakers N     Number of speakers (0 = auto-detect, default: 0)\n"
        "  --cluster-threshold F  Clustering distance threshold (default: 1.18, higher = fewer speakers)\n"
        "  --threads N          Number of CPU threads for inference (0 = auto-detect, default: 0)\n"
        "  --reprocess DIR      Reprocess existing recording from audio.wav\n"
        "  --list-sources       List available audio sources and exit\n"
        "  -h, --help           Show this help\n"
        "  -v, --version        Show version\n"
    );
}

static void print_version() {
    printf("recmeet %s\n", RECMEET_VERSION);
}

int main(int argc, char* argv[]) {
    auto cli = recmeet::parse_cli(argc, argv);
    if (cli.show_version) { print_version(); return 0; }
    if (cli.show_help) { print_usage(); return 0; }
    Config cfg = cli.cfg;
    bool list_sources = cli.list_sources;

    // List sources mode
    if (list_sources) {
        try {
            auto sources = recmeet::list_sources();
            printf("Available audio sources:\n");
            for (const auto& s : sources) {
                printf("  %-50s  %s%s\n", s.name.c_str(), s.description.c_str(),
                       s.is_monitor ? " [monitor]" : "");
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "Error: %s\n", e.what());
            return 1;
        }
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
                    notify_cleanup();
                    return 1;
                }
                fprintf(stderr, "Downloading...\n");
                ensure_whisper_model(cfg.whisper_model);
                fprintf(stderr, "Model ready.\n\n");
            }
        } catch (const RecmeetError& e) {
            fprintf(stderr, "Error: %s\n", e.what());
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

    if (cfg.reprocess_dir.empty())
        fprintf(stderr, "Press Ctrl+C to stop recording.\n\n");

    try {
        auto result = run_pipeline(cfg, g_stop);
        (void)result;
    } catch (const RecmeetError& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        notify_cleanup();
        return 1;
    } catch (const std::exception& e) {
        fprintf(stderr, "Unexpected error: %s\n", e.what());
        notify_cleanup();
        return 1;
    }

    notify_cleanup();
    return 0;
}
