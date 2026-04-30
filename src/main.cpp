// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "audio_file.h"
#include "cli.h"
#include "config.h"
#include "config_json.h"
#include "device_enum.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "log.h"
#include "model_manager.h"
#include "ndjson_parse.h"
#include "notify.h"
#include "pipeline.h"
#include "speaker_id.h"
#include "util.h"
#include "version.h"

#include <whisper.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
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
        "  --vocab WORDS        Comma-separated vocabulary hints for transcription (names, terms)\n"
        "  --list-vocab         List persistent vocabulary words and exit\n"
        "  --add-vocab WORD     Add a word to persistent vocabulary and exit\n"
        "  --remove-vocab WORD  Remove a word from persistent vocabulary and exit\n"
        "  --reset-vocab        Clear all persistent vocabulary words and exit\n"
        "  --output-dir DIR     Base directory for outputs (default: ./meetings)\n"
        "  --note-dir DIR       Directory for meeting notes (default: same as audio)\n"
        "  --provider NAME      API provider: xai, openai, anthropic (default: xai)\n"
        "  --api-key KEY        API key (default: from provider env var or config)\n"
        "  --api-url URL        API endpoint override (default: derived from provider)\n"
        "  --api-model NAME     API model name (default: provider's default model)\n"
        "  --no-summary         Skip summarization (record + transcribe only)\n"
        "  --device-pattern RE  Regex for device auto-detection\n"
        "  --context-file PATH  Pre-meeting notes to include in summary prompt\n"
        "  --context-text TEXT  Inline pre-meeting context for summary\n"
        "  --llm-model PATH     Local GGUF model for summarization (instead of API)\n"
        "  --mmap               Use mmap for LLM model loading (faster load, may cause swap)\n"
        "  --no-mmap            Disable mmap for LLM model loading (default, avoids swap thrashing)\n"
        "  --no-diarize         Disable speaker diarization\n"
        "  --num-speakers N     Number of speakers (0 = auto-detect, default: 0)\n"
        "  --cluster-threshold F  Clustering distance threshold (default: 1.18, higher = fewer speakers)\n"
        "  --no-vad             Disable VAD segmentation (transcribe full audio)\n"
        "  --vad-threshold F    VAD speech detection threshold (default: 0.5)\n"
        "  --threads N          Number of CPU threads for inference (0 = auto-detect, default: 0)\n"
        "  --reprocess PATH     Reprocess audio file or directory containing audio\n"
        "  --log-level LEVEL    Log level: none, error, warn, info (default: none)\n"
        "  --log-dir DIR        Log file directory (default: ~/.local/share/recmeet/logs/)\n"
        "  --list-sources       List available audio sources and exit\n"
        "  --download-models    Download required models and exit\n"
        "  --update-models      Re-download all cached models and exit\n"
        "  --no-speaker-id      Disable speaker identification\n"
        "  --speaker-threshold F  Speaker identification similarity threshold (default: 0.6)\n"
        "  --speaker-db DIR     Speaker database directory (default: ~/.local/share/recmeet/speakers/)\n"
        "  --enroll NAME        Enroll a speaker from an existing recording\n"
        "  --from DIR           Meeting directory for enrollment (use with --enroll)\n"
        "  --speaker N          Speaker number to enroll (1-based; omit for interactive)\n"
        "  --speakers           List enrolled speakers and exit\n"
        "  --remove-speaker NAME  Remove an enrolled speaker and exit\n"
        "  --reset-speakers     Remove all enrolled speakers and exit\n"
        "  --identify DIR       Identify speakers in a recording (dry-run) and exit\n"
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

static int client_status(const std::string& addr = "") {
    IpcClient client(addr);
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
    int64_t queue_depth = json_val_as_int(resp.result["queue_depth"]);
    if (queue_depth > 0)
        printf("Queue: %lld job(s) pending\n", (long long)queue_depth);
    return 0;
}

static int client_stop(const std::string& addr = "") {
    IpcClient client(addr);
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

static int client_record(const Config& cfg, const std::string& addr = "") {
    IpcClient client(addr);
    if (!client.connect()) {
        fprintf(stderr, "Error: daemon not running. Start with: recmeet-daemon\n");
        return 1;
    }

    bool is_reprocess = !cfg.reprocess_dir.empty();

    // Build params from config overrides
    JsonMap params = config_to_map(cfg);

    static int last_cli_progress = -1;
    static const bool cli_tty = isatty(STDERR_FILENO);
    static int client_exit_code = 0;
    int64_t my_job_id = 0;  // assigned after record.start response

    client.set_event_callback([&client, &my_job_id](const IpcEvent& ev) {
        if (ev.event == "phase") {
            std::string name = json_val_as_string(ev.data.at("name"));
            if (cli_tty && last_cli_progress >= 0)
                fprintf(stderr, "\n");  // newline after progress line
            last_cli_progress = -1;
            fprintf(stderr, "Phase: %s\n", name.c_str());
        } else if (ev.event == "progress") {
            std::string phase = json_val_as_string(ev.data.at("phase"));
            int percent = static_cast<int>(json_val_as_int(ev.data.at("percent")));
            if (cli_tty) {
                fprintf(stderr, "\r\033[K%s... %d%%",
                        phase.c_str(), percent);
                last_cli_progress = percent;
            } else if (percent - last_cli_progress >= 10 || last_cli_progress < 0) {
                fprintf(stderr, "%s... %d%%\n", phase.c_str(), percent);
                last_cli_progress = percent;
            }
        } else if (ev.event == "state.changed") {
            std::string state = json_val_as_string(ev.data.at("state"));
            auto err_it = ev.data.find("error");
            if (err_it != ev.data.end()) {
                std::string error = json_val_as_string(err_it->second);
                if (!error.empty()) {
                    if (cli_tty && last_cli_progress >= 0)
                        fprintf(stderr, "\n");
                    last_cli_progress = -1;
                    fprintf(stderr, "Error: %s\n", error.c_str());
                    client_exit_code = 1;
                    // Force exit from read_events by closing connection
                    client.close_connection();
                }
            }
        } else if (ev.event == "job.complete") {
            // Filter by our job_id if we have one (ignore completions from other jobs)
            auto jid_it = ev.data.find("job_id");
            if (my_job_id > 0 && jid_it != ev.data.end()
                && json_val_as_int(jid_it->second) != my_job_id)
                return;
            if (cli_tty && last_cli_progress >= 0)
                fprintf(stderr, "\n");
            last_cli_progress = -1;
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

    // Capture job_id for filtering job.complete events
    {
        auto jid_it = resp.result.find("job_id");
        if (jid_it != resp.result.end())
            my_job_id = json_val_as_int(jid_it->second);
    }

    if (is_reprocess)
        fprintf(stderr, "Reprocessing %s\n", cfg.reprocess_dir.c_str());
    else
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

    // Wait for job completion, error, or disconnect
    client.read_events("job.complete");

    g_client = nullptr;
    return client_exit_code;
}

// ---------------------------------------------------------------------------
// Standalone mode — run the pipeline directly (original behavior)
// ---------------------------------------------------------------------------

static int standalone_main(CliResult& cli);

int main(int argc, char* argv[]) {
    auto cli = recmeet::parse_cli(argc, argv);
    if (cli.show_version) { print_version(); return 0; }
    if (cli.show_help) { print_usage(); return 0; }

    // RECMEET_DAEMON_ADDR env var as fallback for --daemon-addr
    if (cli.daemon_addr.empty()) {
        if (const char* env = std::getenv("RECMEET_DAEMON_ADDR")) {
            cli.daemon_addr = env;
            cli.daemon_mode = recmeet::DaemonMode::Force;
        }
    }

    // Status/stop commands always use client mode
    if (cli.show_status) return client_status(cli.daemon_addr);
    if (cli.send_stop) return client_stop(cli.daemon_addr);

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

    // Vocabulary management commands — standalone, no daemon needed
    if (cli.list_vocab) {
        if (cli.cfg.vocabulary.empty()) {
            printf("No vocabulary words configured.\n");
            printf("Enrolled speaker names are included automatically.\n");
        } else {
            std::istringstream ss(cli.cfg.vocabulary);
            std::string token;
            std::vector<std::string> words;
            while (std::getline(ss, token, ',')) {
                auto start = token.find_first_not_of(" \t");
                auto end = token.find_last_not_of(" \t");
                if (start != std::string::npos)
                    words.push_back(token.substr(start, end - start + 1));
            }
            printf("Vocabulary words (%zu):\n", words.size());
            for (const auto& w : words)
                printf("  %s\n", w.c_str());
        }
        return 0;
    }

    if (!cli.add_vocab.empty()) {
        std::string word = cli.add_vocab;
        if (cli.cfg.vocabulary.empty()) {
            cli.cfg.vocabulary = word;
        } else {
            // Check for duplicates
            std::istringstream ss(cli.cfg.vocabulary);
            std::string token;
            bool found = false;
            while (std::getline(ss, token, ',')) {
                auto start = token.find_first_not_of(" \t");
                auto end = token.find_last_not_of(" \t");
                if (start != std::string::npos) {
                    std::string trimmed = token.substr(start, end - start + 1);
                    if (trimmed == word) { found = true; break; }
                }
            }
            if (found) {
                printf("'%s' is already in vocabulary.\n", word.c_str());
                return 0;
            }
            cli.cfg.vocabulary += ", " + word;
        }
        save_config(cli.cfg);
        printf("Added '%s' to vocabulary.\n", word.c_str());
        return 0;
    }

    if (!cli.remove_vocab.empty()) {
        std::string target = cli.remove_vocab;
        std::istringstream ss(cli.cfg.vocabulary);
        std::string token;
        std::vector<std::string> words;
        while (std::getline(ss, token, ',')) {
            auto start = token.find_first_not_of(" \t");
            auto end = token.find_last_not_of(" \t");
            if (start != std::string::npos)
                words.push_back(token.substr(start, end - start + 1));
        }
        auto it = std::find(words.begin(), words.end(), target);
        if (it == words.end()) {
            fprintf(stderr, "'%s' not found in vocabulary.\n", target.c_str());
            return 1;
        }
        words.erase(it);
        cli.cfg.vocabulary.clear();
        for (size_t i = 0; i < words.size(); ++i) {
            if (i > 0) cli.cfg.vocabulary += ", ";
            cli.cfg.vocabulary += words[i];
        }
        save_config(cli.cfg);
        printf("Removed '%s' from vocabulary.\n", target.c_str());
        return 0;
    }

    if (cli.reset_vocab) {
        if (cli.cfg.vocabulary.empty()) {
            printf("Vocabulary is already empty.\n");
        } else {
            cli.cfg.vocabulary.clear();
            save_config(cli.cfg);
            printf("Cleared all vocabulary words.\n");
        }
        return 0;
    }

    // Speaker management commands — standalone, no daemon needed
    if (cli.list_speakers) {
        fs::path db_dir = cli.cfg.speaker_db.empty()
            ? default_speaker_db_dir() : cli.cfg.speaker_db;
        auto names = list_speakers(db_dir);
        if (names.empty()) {
            printf("No enrolled speakers. Use --enroll to add one.\n");
        } else {
            printf("Enrolled speakers (%zu):\n", names.size());
            for (const auto& name : names) {
                auto profiles = load_speaker_db(db_dir);
                for (const auto& p : profiles) {
                    if (p.name == name) {
                        printf("  %-20s  %zu enrollment(s)  updated: %s\n",
                               p.name.c_str(), p.embeddings.size(), p.updated.c_str());
                        break;
                    }
                }
            }
        }
        return 0;
    }

    if (cli.reset_speakers) {
        fs::path db_dir = cli.cfg.speaker_db.empty()
            ? default_speaker_db_dir() : cli.cfg.speaker_db;
        int count = reset_speakers(db_dir);
        printf("Removed %d speaker profile(s).\n", count);
        return 0;
    }

    if (!cli.remove_speaker.empty()) {
        fs::path db_dir = cli.cfg.speaker_db.empty()
            ? default_speaker_db_dir() : cli.cfg.speaker_db;
        if (remove_speaker(db_dir, cli.remove_speaker)) {
            printf("Removed speaker '%s'.\n", cli.remove_speaker.c_str());
        } else {
            fprintf(stderr, "Speaker '%s' not found.\n", cli.remove_speaker.c_str());
            return 1;
        }
        return 0;
    }

#if RECMEET_USE_SHERPA
    if (!cli.enroll_name.empty()) {
        if (cli.enroll_from.empty()) {
            fprintf(stderr, "Error: --enroll requires --from <meeting_dir>\n");
            return 1;
        }
        fs::path meeting_dir = cli.enroll_from;
        fs::path audio_path = find_audio_file(meeting_dir);
        if (audio_path.empty()) {
            fprintf(stderr, "Error: No audio file in %s\n", meeting_dir.c_str());
            return 1;
        }

        auto model_paths = ensure_sherpa_models();
        auto samples = read_wav_float(audio_path);
        if (samples.empty()) {
            fprintf(stderr, "Error: Cannot read audio from %s\n", audio_path.c_str());
            return 1;
        }

        // Run diarization to identify speaker clusters
        fprintf(stderr, "Diarizing %s...\n", audio_path.c_str());
        auto diar = diarize(samples.data(), samples.size(),
                            cli.cfg.num_speakers, 0, cli.cfg.cluster_threshold);

        if (diar.num_speakers == 0) {
            fprintf(stderr, "Error: No speakers found in recording.\n");
            return 1;
        }

        // Determine which speaker to enroll
        int target_speaker = -1;
        if (cli.enroll_speaker > 0) {
            target_speaker = cli.enroll_speaker - 1;  // 1-based → 0-based
            if (target_speaker >= diar.num_speakers) {
                fprintf(stderr, "Error: Speaker %d not found (recording has %d speakers).\n",
                        cli.enroll_speaker, diar.num_speakers);
                return 1;
            }
        } else {
            // Interactive: show speakers with durations and ask
            fprintf(stderr, "\nSpeakers found in recording:\n");
            for (int i = 0; i < diar.num_speakers; ++i) {
                double duration = 0;
                for (const auto& seg : diar.segments)
                    if (seg.speaker == i) duration += seg.end - seg.start;
                fprintf(stderr, "  %d. Speaker_%02d  (%.1fs of speech)\n",
                        i + 1, i + 1, duration);
            }
            fprintf(stderr, "\nWhich speaker is '%s'? [1-%d]: ",
                    cli.enroll_name.c_str(), diar.num_speakers);
            int choice = 0;
            if (scanf("%d", &choice) != 1 || choice < 1 || choice > diar.num_speakers) {
                fprintf(stderr, "Invalid choice.\n");
                return 1;
            }
            target_speaker = choice - 1;
        }

        // Extract embedding
        fprintf(stderr, "Extracting voiceprint for '%s' from speaker %d...\n",
                cli.enroll_name.c_str(), target_speaker + 1);
        auto embedding = extract_speaker_embedding(
            samples.data(), samples.size(), diar, target_speaker,
            model_paths.embedding);

        if (embedding.empty()) {
            fprintf(stderr, "Error: Could not extract embedding (insufficient audio?).\n");
            return 1;
        }

        // Load existing profile or create new one
        fs::path db_dir = cli.cfg.speaker_db.empty()
            ? default_speaker_db_dir() : cli.cfg.speaker_db;
        auto profiles = load_speaker_db(db_dir);

        SpeakerProfile profile;
        for (const auto& p : profiles) {
            if (p.name == cli.enroll_name) {
                profile = p;
                break;
            }
        }

        if (profile.name.empty()) {
            profile.name = cli.enroll_name;
            profile.created = profile.updated;
        }

        profile.embeddings.push_back(std::move(embedding));
        profile.updated = iso_now();
        if (profile.created.empty()) profile.created = profile.updated;

        save_speaker(db_dir, profile);

        printf("Enrolled '%s' (%zu embedding(s) total).\n",
               profile.name.c_str(), profile.embeddings.size());
        return 0;
    }

    if (!cli.identify_dir.empty()) {
        fs::path meeting_dir = cli.identify_dir;
        fs::path audio_path = find_audio_file(meeting_dir);
        if (audio_path.empty()) {
            fprintf(stderr, "Error: No audio file in %s\n", meeting_dir.c_str());
            return 1;
        }

        auto model_paths = ensure_sherpa_models();
        auto samples = read_wav_float(audio_path);
        if (samples.empty()) {
            fprintf(stderr, "Error: Cannot read audio from %s\n", audio_path.c_str());
            return 1;
        }

        fprintf(stderr, "Diarizing...\n");
        auto diar = diarize(samples.data(), samples.size(),
                            cli.cfg.num_speakers, 0, cli.cfg.cluster_threshold);

        fs::path db_dir = cli.cfg.speaker_db.empty()
            ? default_speaker_db_dir() : cli.cfg.speaker_db;
        auto db = load_speaker_db(db_dir);

        if (db.empty()) {
            fprintf(stderr, "No enrolled speakers. Use --enroll to add one.\n");
            return 1;
        }

        fprintf(stderr, "Identifying speakers...\n");
        auto id_result = identify_speakers(
            samples.data(), samples.size(), diar, db,
            model_paths.embedding, cli.cfg.speaker_threshold);

        printf("\nSpeaker identification results:\n");
        for (int i = 0; i < diar.num_speakers; ++i) {
            double duration = 0;
            for (const auto& seg : diar.segments)
                if (seg.speaker == i) duration += seg.end - seg.start;
            auto it = id_result.names.find(i);
            if (it != id_result.names.end())
                printf("  Speaker_%02d → %s  (%.1fs, score: %.3f)\n",
                       i + 1, it->second.c_str(), duration, id_result.scores[i]);
            else
                printf("  Speaker_%02d → (unknown)  (%.1fs)\n", i + 1, duration);
        }
        return 0;
    }
#endif // RECMEET_USE_SHERPA

    Config cfg = cli.cfg;
    bool list_sources = cli.list_sources;

    // Resolve API key early so both daemon and standalone paths have it
    if (cfg.llm_model.empty()) {
        const auto* prov = find_provider(cfg.provider);
        if (prov) {
            std::string key = resolve_api_key(*prov, cfg.api_keys, cfg.api_key);
            if (!key.empty()) {
                cfg.api_key = key;
                cli.cfg.api_key = key;  // standalone_main() copies from cli.cfg
            }
        }
    }

    // Initialize logging (stderr only on interactive TTY, not subprocess pipe)
    auto log_level = parse_log_level(cfg.log_level_str);
    log_init(log_level, cfg.log_dir, cfg.log_retention_hours, isatty(STDERR_FILENO));

    // Early validation: reject unsupported audio formats before daemon dispatch
    if (!cfg.reprocess_dir.empty()) {
        try {
            validate_reprocess_input(cfg.reprocess_dir);
        } catch (const RecmeetError& e) {
            fprintf(stderr, "Error: %s\n", e.what());
            log_shutdown();
            return 1;
        }
    }

    // Determine whether to use daemon
    bool use_daemon = false;
    if (cli.daemon_mode == DaemonMode::Force) {
        use_daemon = true;
    } else if (cli.daemon_mode == DaemonMode::Disable) {
        use_daemon = false;
    } else {
        // Auto: use daemon if it's running
        use_daemon = daemon_running(cli.daemon_addr);
    }

    if (use_daemon && !list_sources) {
        log_info("Using daemon mode");
        int rc = client_record(cfg, cli.daemon_addr);
        log_shutdown();
        return rc;
    }

    // Fall through to standalone mode
    return standalone_main(cli);
}

// ---------------------------------------------------------------------------
// Subprocess postprocessing mode (daemon-internal)
// Writes NDJSON progress to stdout, exit 0=ok, 1=error, 2=cancelled.
// ---------------------------------------------------------------------------

static int subprocess_main(CliResult& cli) {
    // Subprocess must NOT log to file or stderr — output goes through NDJSON
    // stdout and parent captures stderr for relay
    log_shutdown();

    // Load full config from JSON file
    std::string json_content;
    {
        std::ifstream in(cli.config_json_path);
        if (!in) {
            fprintf(stderr, "Cannot open config file: %s\n", cli.config_json_path.c_str());
            return 1;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        json_content = ss.str();
    }

    Config cfg = config_from_json(json_content);

    // Suppress whisper log noise
    whisper_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);

    // Do NOT call notify_init() — avoid duplicate desktop notifications
    // (the daemon handles notifications from IPC events)

    // Install signal handler for graceful cancellation
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Heartbeat thread — writes periodic NDJSON so the daemon knows we're alive
    // and can observe RSS growth. Any real progress/phase event also serves as a
    // heartbeat, so the interval here (10 s) is just a fallback for long-running
    // stages that don't report.
    //
    // Emission path uses write(2) on a stack buffer — NOT fprintf/fflush — to
    // avoid the libc stdio mutex. The iter 110 OOM showed fprintf can block for
    // minutes under heavy malloc-arena lock contention from a deadlocked worker,
    // taking the heartbeat thread (and watchdog visibility) with it.
    //
    // RSS self-limit: if RECMEET_RSS_LIMIT_MB is set and our resident set
    // exceeds it, write a stderr line and _Exit(1). Diagnostic / best-effort
    // only — under deadlock conditions even write(2) may block. The cgroup
    // MemoryMax= in the systemd unit is the real backstop.
    std::atomic<bool> heartbeat_stop{false};
    std::thread heartbeat_thread([&heartbeat_stop]() {
        const long limit_kb = []() {
            const char* s = std::getenv("RECMEET_RSS_LIMIT_MB");
            return s ? std::atol(s) * 1024L : 0L;
        }();
        while (!heartbeat_stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 100 && !heartbeat_stop.load(std::memory_order_relaxed); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (heartbeat_stop.load(std::memory_order_relaxed)) break;

            long rss_kb = read_self_rss_kb();
            write_heartbeat_ndjson(STDOUT_FILENO, rss_kb);

            if (limit_kb > 0 && rss_kb > limit_kb) {
                // Daemon's stderr-capture concatenates this line onto the
                // user-facing error. Exit 1 (generic error) — daemon doesn't
                // distinguish exit codes here.
                write_rss_limit_msg(STDERR_FILENO);
                std::_Exit(1);
            }
        }
    });

    auto stop_heartbeat = [&]() {
        heartbeat_stop.store(true, std::memory_order_relaxed);
        if (heartbeat_thread.joinable()) heartbeat_thread.join();
    };

    auto on_phase = [](const std::string& name) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"name\":\"%s\"}", name.c_str());
        write_ndjson("phase", buf);
    };

    auto on_progress = [](const std::string& phase, int percent) {
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"phase\":\"%s\",\"percent\":%d}", phase.c_str(), percent);
        write_ndjson("progress", buf);
    };

    try {
        auto input = run_recording(cfg, g_stop, on_phase);
        auto result = run_postprocessing(cfg, input, on_phase, on_progress, &g_stop);

        // Emit job.complete
        std::string data = "{\"note_path\":\"";
        // Simple JSON escaping for paths
        for (char c : result.note_path.string()) {
            if (c == '"') data += "\\\"";
            else if (c == '\\') data += "\\\\";
            else data += c;
        }
        data += "\",\"output_dir\":\"";
        for (char c : result.output_dir.string()) {
            if (c == '"') data += "\\\"";
            else if (c == '\\') data += "\\\\";
            else data += c;
        }
        data += "\"}";
        write_ndjson("job.complete", data.c_str());

        // Clean up config file
        std::error_code ec;
        fs::remove(cli.config_json_path, ec);

        stop_heartbeat();
        return 0;
    } catch (const RecmeetError& e) {
        stop_heartbeat();
        std::string msg = e.what();
        if (msg == "Cancelled") {
            std::error_code ec;
            fs::remove(cli.config_json_path, ec);
            return 2;
        }
        fprintf(stderr, "%s\n", e.what());
        std::error_code ec;
        fs::remove(cli.config_json_path, ec);
        return 1;
    } catch (const std::exception& e) {
        stop_heartbeat();
        fprintf(stderr, "%s\n", e.what());
        std::error_code ec;
        fs::remove(cli.config_json_path, ec);
        return 1;
    }
}

static int standalone_main(CliResult& cli) {
    // Subprocess mode: daemon launched us with --progress-json --config-json
    if (cli.progress_json && !cli.config_json_path.empty()) {
        return subprocess_main(cli);
    }

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

    // Validate: need API key for summary unless disabled or using local LLM
    // (API key already resolved in main() before daemon/standalone branch)
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
