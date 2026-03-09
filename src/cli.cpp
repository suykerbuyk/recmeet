// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "cli.h"

#include <cstdlib>
#include <getopt.h>

namespace recmeet {

CliResult parse_cli(int argc, char* argv[]) {
    static const struct option long_opts[] = {
        {"source",         required_argument, nullptr, 's'},
        {"monitor",        required_argument, nullptr, 'm'},
        {"mic-only",       no_argument,       nullptr, 'M'},
        {"keep-sources",   no_argument,       nullptr, 'K'},
        {"model",          required_argument, nullptr, 'W'},
        {"output-dir",     required_argument, nullptr, 'o'},
        {"api-key",        required_argument, nullptr, 'k'},
        {"api-url",        required_argument, nullptr, 'u'},
        {"api-model",      required_argument, nullptr, 'A'},
        {"no-summary",     no_argument,       nullptr, 'N'},
        {"device-pattern", required_argument, nullptr, 'd'},
        {"context-file",   required_argument, nullptr, 'c'},
        {"provider",       required_argument, nullptr, 'P'},
        {"llm-model",      required_argument, nullptr, 'L'},
        {"language",       required_argument, nullptr, 'g'},
        {"reprocess",      required_argument, nullptr, 'R'},
        {"no-diarize",     no_argument,       nullptr, 'D'},
        {"num-speakers",   required_argument, nullptr, 'S'},
        {"cluster-threshold", required_argument, nullptr, 'C'},
        {"no-vad",         no_argument,       nullptr, 'V'},
        {"vad-threshold",  required_argument, nullptr, 'B'},
        {"threads",        required_argument, nullptr, 'T'},
        {"log-level",      required_argument, nullptr, 'E'},
        {"note-dir",       required_argument, nullptr, 'n'},
        {"log-dir",        required_argument, nullptr, 'F'},
        {"list-sources",   no_argument,       nullptr, 'l'},
        {"daemon",         no_argument,       nullptr, 1001},
        {"no-daemon",      no_argument,       nullptr, 1002},
        {"status",         no_argument,       nullptr, 1003},
        {"stop",           no_argument,       nullptr, 1004},
        {"download-models", no_argument,      nullptr, 1005},
        {"update-models",  no_argument,       nullptr, 1006},
        {"enroll",         required_argument, nullptr, 1007},
        {"from",           required_argument, nullptr, 1008},
        {"speaker",        required_argument, nullptr, 1009},
        {"speakers",       no_argument,       nullptr, 1010},
        {"remove-speaker", required_argument, nullptr, 1011},
        {"identify",       required_argument, nullptr, 1012},
        {"no-speaker-id",  no_argument,       nullptr, 1013},
        {"speaker-threshold", required_argument, nullptr, 1014},
        {"speaker-db",     required_argument, nullptr, 1015},
        {"reset-speakers", no_argument,       nullptr, 1016},
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
            case 'K': result.cfg.keep_sources = true; break;
            case 'W': result.cfg.whisper_model = optarg; break;
            case 'o': result.cfg.output_dir = optarg; result.cfg.output_dir_explicit = true; break;
            case 'k': result.cfg.api_key = optarg; break;
            case 'u': result.cfg.api_url = optarg; break;
            case 'A': result.cfg.api_model = optarg; break;
            case 'N': result.cfg.no_summary = true; break;
            case 'd': result.cfg.device_pattern = optarg; break;
            case 'c': result.cfg.context_file = optarg; break;
            case 'P': result.cfg.provider = optarg; break;
            case 'L': result.cfg.llm_model = optarg; break;
            case 'g': result.cfg.language = optarg; break;
            case 'R': result.cfg.reprocess_dir = optarg; break;
            case 'D': result.cfg.diarize = false; break;
            case 'S': result.cfg.num_speakers = std::atoi(optarg); break;
            case 'C': result.cfg.cluster_threshold = std::atof(optarg); break;
            case 'V': result.cfg.vad = false; break;
            case 'B': result.cfg.vad_threshold = std::atof(optarg); break;
            case 'T': result.cfg.threads = std::atoi(optarg); break;
            case 'E': result.cfg.log_level_str = optarg; break;
            case 'n': result.cfg.note_dir = optarg; break;
            case 'F': result.cfg.log_dir = optarg; break;
            case 'l': result.list_sources = true; break;
            case 1001: result.daemon_mode = DaemonMode::Force; break;
            case 1002: result.daemon_mode = DaemonMode::Disable; break;
            case 1003: result.show_status = true; break;
            case 1004: result.send_stop = true; break;
            case 1005: result.download_models = true; break;
            case 1006: result.update_models = true; break;
            case 1007: result.enroll_name = optarg; break;
            case 1008: result.enroll_from = optarg; break;
            case 1009: result.enroll_speaker = std::atoi(optarg); break;
            case 1010: result.list_speakers = true; break;
            case 1011: result.remove_speaker = optarg; break;
            case 1012: result.identify_dir = optarg; break;
            case 1013: result.cfg.speaker_id = false; break;
            case 1014: result.cfg.speaker_threshold = std::atof(optarg); break;
            case 1015: result.cfg.speaker_db = optarg; break;
            case 1016: result.reset_speakers = true; break;
            case 'v': result.show_version = true; return result;
            case 'h': result.show_help = true; return result;
            default:  result.show_help = true; return result;
        }
    }

    return result;
}

} // namespace recmeet
