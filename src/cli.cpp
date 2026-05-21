// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "cli.h"

#include <cstdio>
#include <cstdlib>
#include <getopt.h>

namespace recmeet {

bool caption_language_supported(const std::string& language) {
    // The streaming Zipformer we ship in V1 is English-only. Treat empty
    // (auto-detect) as English-compatible — the language flag is the user's
    // explicit choice; auto-detect is the default that lets the daemon
    // proceed. ISO 639-1 codes are 2 letters; we lower-case for tolerance.
    if (language.empty()) return true;
    std::string lc;
    lc.reserve(language.size());
    for (char c : language) lc.push_back((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return lc == "en";
}

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
        {"mmap",           no_argument,       nullptr, 1017},
        {"no-mmap",        no_argument,       nullptr, 1018},
        {"vocab",          required_argument, nullptr, 1019},
        {"list-vocab",     no_argument,       nullptr, 1020},
        {"add-vocab",      required_argument, nullptr, 1021},
        {"remove-vocab",   required_argument, nullptr, 1022},
        {"context-text",   required_argument, nullptr, 1024},
        {"reset-vocab",    no_argument,       nullptr, 1023},
        {"log-retention",  required_argument, nullptr, 1027},
        {"daemon-addr",    required_argument, nullptr, 1028},
        {"progress-json",  no_argument,       nullptr, 1025},
        {"config-json",    required_argument, nullptr, 1026},
        {"diarize-chunk-minutes",     required_argument, nullptr, 1029},
        {"diarize-chunk-overlap-sec", required_argument, nullptr, 1030},
        {"diarize-stitch-threshold",  required_argument, nullptr, 1031},
        {"debug-dump-centroids",      required_argument, nullptr, 1038},
        {"max-auto-speakers",         required_argument, nullptr, 1039},
        {"collapse-threshold",        required_argument, nullptr, 1040},
        {"min-cluster-duration",      required_argument, nullptr, 1041},
        {"reprocess-batch", required_argument, nullptr, 1032},
        {"dry-run",         no_argument,       nullptr, 1033},
        {"caption-model",      required_argument, nullptr, 1034},
        {"list-caption-models", no_argument,      nullptr, 1035},
        {"no-captions",        no_argument,       nullptr, 1036},
        {"show-captions",      no_argument,       nullptr, 1037},
        {"help",           no_argument,       nullptr, 'h'},
        {"version",        no_argument,       nullptr, 'v'},
        {nullptr, 0, nullptr, 0},
    };

    CliResult result;
    result.cfg = load_legacy_config_as_job_config();

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
            case 1017: result.cfg.llm_mmap = true; break;
            case 1018: result.cfg.llm_mmap = false; break;
            case 1019: result.cfg.vocabulary = optarg; break;
            case 1020: result.list_vocab = true; break;
            case 1021: result.add_vocab = optarg; break;
            case 1022: result.remove_vocab = optarg; break;
            case 1023: result.reset_vocab = true; break;
            case 1024: result.cfg.context_inline = optarg; break;
            case 1025: result.progress_json = true; break;
            case 1026: result.config_json_path = optarg; break;
            case 1027: result.cfg.log_retention_hours = std::atoi(optarg); break;
            case 1028: result.daemon_addr = optarg;
                       result.daemon_mode = DaemonMode::Force; break;
            case 1029: result.cfg.chunk_minutes = static_cast<float>(std::atof(optarg)); break;
            case 1030: result.cfg.chunk_overlap_sec = static_cast<float>(std::atof(optarg)); break;
            case 1031: result.cfg.stitch_threshold = static_cast<float>(std::atof(optarg)); break;
            case 1032: result.cfg.reprocess_batch_dir = optarg; break;
            case 1033: result.cfg.reprocess_batch_dry_run = true; break;
            case 1034: result.cfg.caption_model = optarg; break;
            case 1035: result.list_caption_models = true; break;
            case 1036: result.caption_force_off = true; break;
            case 1037: result.caption_force_on = true;
                       result.caption_show_on_stderr = true; break;
            case 1038: result.cfg.debug_dump_centroids_path = optarg; break;
            case 1039: result.cfg.max_auto_speakers = std::atoi(optarg); break;
            case 1040: result.cfg.collapse_threshold = static_cast<float>(std::atof(optarg)); break;
            case 1041: result.cfg.min_cluster_duration_sec = static_cast<float>(std::atof(optarg)); break;
            case 'v': result.show_version = true; return result;
            case 'h': result.show_help = true; return result;
            default:  result.show_help = true; return result;
        }
    }

    // M-5' validation: chunk_minutes * 60 must exceed chunk_overlap_sec + 60
    // (positive spacing with ≥ 60 s minimum core size). Mirrors the same check
    // inside `diarize_chunked` so misconfiguration fails fast at the surface
    // where the user can fix it, not deep in the pipeline. Caller (main.cpp)
    // surfaces the error to stderr and exits with code 2 (CLI usage error).
    {
        float cm = result.cfg.chunk_minutes;
        float co = result.cfg.chunk_overlap_sec;
        if (cm * 60.0f <= co + 60.0f) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "--diarize-chunk-minutes (%.3f) * 60 must exceed "
                "--diarize-chunk-overlap-sec (%.3f) + 60 "
                "(positive chunk spacing with at least 60 s of core)",
                cm, co);
            result.parse_error = buf;
        }
    }

    // Mutual exclusion: --reprocess (single dir) and --reprocess-batch (parent
    // dir) target different code paths and cannot be combined. Reject early
    // with a clear message so the operator picks one. Mirrors the surfacing
    // convention of the chunk-validation block above; main.cpp prints
    // result.parse_error to stderr and exits 2.
    if (!result.cfg.reprocess_dir.empty()
        && !result.cfg.reprocess_batch_dir.empty()
        && result.parse_error.empty()) {
        result.parse_error =
            "--reprocess and --reprocess-batch are mutually exclusive "
            "(single-meeting reprocess vs. parent-dir batch reprocess); "
            "pass exactly one";
    }

    // ----------------------------------------------------------------
    // Phase 4 — caption flag precedence + language guard.
    //
    // Precedence (highest → lowest, applied here so the resolved value
    // lands in result.cfg.captions_enabled BEFORE the request is built):
    //   1. --no-captions   (force off)              wins everything.
    //   2. --show-captions (force on)               wins config, loses
    //                                               to language guard.
    //   3. config (captions_enabled in YAML)        baseline.
    //
    // Mutual exclusion: --no-captions + --show-captions is a usage error.
    // The language guard is CLI-side (not daemon-side) so the daemon
    // stays stateless w.r.t. UI-level decisions.
    // ----------------------------------------------------------------
    if (result.caption_force_on && result.caption_force_off
        && result.parse_error.empty()) {
        result.parse_error =
            "--no-captions and --show-captions are mutually exclusive "
            "(force-disable vs. force-enable); pass exactly one";
    }

    if (result.caption_force_off) {
        result.cfg.captions_enabled = false;
    } else if (result.caption_force_on) {
        result.cfg.captions_enabled = true;
    }
    // (no flag → keep config-loaded value)

    // Language guard. V1 ships an English-only streaming Zipformer; if the
    // operator forced a non-English language we override captions to off
    // and emit a warning on the result. main.cpp prints parse_warning to
    // stderr without exiting (non-fatal — recording proceeds).
    if (result.cfg.captions_enabled
        && !caption_language_supported(result.cfg.language)) {
        result.cfg.captions_enabled = false;
        result.caption_show_on_stderr = false;
        char buf[256];
        if (result.caption_force_on) {
            std::snprintf(buf, sizeof(buf),
                "live captions: --show-captions ignored — only English "
                "supported in V1 (got --language=%s)",
                result.cfg.language.c_str());
        } else {
            std::snprintf(buf, sizeof(buf),
                "live captions: only English supported in V1; final "
                "transcript will use whisper as configured "
                "(got --language=%s)",
                result.cfg.language.c_str());
        }
        result.parse_warning = buf;
    }

    return result;
}

} // namespace recmeet
