#include "cli.h"
#include "device_enum.h"
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
