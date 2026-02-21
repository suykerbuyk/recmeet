#include "config.h"
#include "device_enum.h"
#include "notify.h"
#include "pipeline.h"
#include "util.h"
#include "version.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>

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
        "  --output-dir DIR     Base directory for outputs (default: ./meetings)\n"
        "  --api-key KEY        xAI/OpenAI API key (default: from env/config)\n"
        "  --api-url URL        API endpoint (default: https://api.x.ai/v1/chat/completions)\n"
        "  --api-model NAME     API model name (default: grok-3)\n"
        "  --no-summary         Skip summarization (record + transcribe only)\n"
        "  --device-pattern RE  Regex for device auto-detection\n"
        "  --context-file PATH  Pre-meeting notes to include in summary prompt\n"
        "  --obsidian-vault DIR Obsidian vault path for note output\n"
        "  --llm-model PATH     Local GGUF model for summarization (instead of API)\n"
        "  --list-sources       List available audio sources and exit\n"
        "  -h, --help           Show this help\n"
        "  -v, --version        Show version\n"
    );
}

static void print_version() {
    printf("recmeet %s\n", RECMEET_VERSION);
}

int main(int argc, char* argv[]) {
    static const struct option long_opts[] = {
        {"source",         required_argument, nullptr, 's'},
        {"monitor",        required_argument, nullptr, 'm'},
        {"mic-only",       no_argument,       nullptr, 'M'},
        {"model",          required_argument, nullptr, 'W'},
        {"output-dir",     required_argument, nullptr, 'o'},
        {"api-key",        required_argument, nullptr, 'k'},
        {"api-url",        required_argument, nullptr, 'u'},
        {"api-model",      required_argument, nullptr, 'A'},
        {"no-summary",     no_argument,       nullptr, 'N'},
        {"device-pattern", required_argument, nullptr, 'd'},
        {"context-file",   required_argument, nullptr, 'c'},
        {"obsidian-vault", required_argument, nullptr, 'O'},
        {"llm-model",      required_argument, nullptr, 'L'},
        {"list-sources",   no_argument,       nullptr, 'l'},
        {"help",           no_argument,       nullptr, 'h'},
        {"version",        no_argument,       nullptr, 'v'},
        {nullptr, 0, nullptr, 0},
    };

    // Load config file as defaults
    Config cfg = load_config();
    bool list_sources = false;

    int opt;
    while ((opt = getopt_long(argc, argv, "hv", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 's': cfg.mic_source = optarg; break;
            case 'm': cfg.monitor_source = optarg; break;
            case 'M': cfg.mic_only = true; break;
            case 'W': cfg.whisper_model = optarg; break;
            case 'o': cfg.output_dir = optarg; break;
            case 'k': cfg.api_key = optarg; break;
            case 'u': cfg.api_url = optarg; break;
            case 'A': cfg.api_model = optarg; break;
            case 'N': cfg.no_summary = true; break;
            case 'd': cfg.device_pattern = optarg; break;
            case 'c': cfg.context_file = optarg; break;
            case 'O':
                cfg.obsidian.vault_path = optarg;
                cfg.obsidian_enabled = true;
                break;
            case 'L': cfg.llm_model = optarg; break;
            case 'l': list_sources = true; break;
            case 'v': print_version(); return 0;
            case 'h': print_usage(); return 0;
            default:  print_usage(); return 1;
        }
    }

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

    // Validate: need API key for summary unless disabled or using local LLM
    if (!cfg.no_summary && cfg.api_key.empty() && cfg.llm_model.empty()) {
        fprintf(stderr, "Warning: No API key and no local LLM model. Summary will be skipped.\n");
        fprintf(stderr, "Set XAI_API_KEY in environment, config, or use --api-key / --llm-model.\n");
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
