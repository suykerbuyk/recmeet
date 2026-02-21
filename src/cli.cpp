#include "cli.h"

#include <getopt.h>

namespace recmeet {

CliResult parse_cli(int argc, char* argv[]) {
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
        {"language",       required_argument, nullptr, 'g'},
        {"list-sources",   no_argument,       nullptr, 'l'},
        {"help",           no_argument,       nullptr, 'h'},
        {"version",        no_argument,       nullptr, 'v'},
        {nullptr, 0, nullptr, 0},
    };

    CliResult result;
    result.cfg = load_config();

    int opt;
    while ((opt = getopt_long(argc, argv, "hv", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 's': result.cfg.mic_source = optarg; break;
            case 'm': result.cfg.monitor_source = optarg; break;
            case 'M': result.cfg.mic_only = true; break;
            case 'W': result.cfg.whisper_model = optarg; break;
            case 'o': result.cfg.output_dir = optarg; break;
            case 'k': result.cfg.api_key = optarg; break;
            case 'u': result.cfg.api_url = optarg; break;
            case 'A': result.cfg.api_model = optarg; break;
            case 'N': result.cfg.no_summary = true; break;
            case 'd': result.cfg.device_pattern = optarg; break;
            case 'c': result.cfg.context_file = optarg; break;
            case 'O':
                result.cfg.obsidian.vault_path = optarg;
                result.cfg.obsidian_enabled = true;
                break;
            case 'L': result.cfg.llm_model = optarg; break;
            case 'g': result.cfg.language = optarg; break;
            case 'l': result.list_sources = true; break;
            case 'v': result.show_version = true; return result;
            case 'h': result.show_help = true; return result;
            default:  result.show_help = true; return result;
        }
    }

    return result;
}

} // namespace recmeet
