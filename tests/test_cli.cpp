// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "cli.h"

#include <cstdlib>
#include <getopt.h>

using namespace recmeet;

// Helper to build argv and call parse_cli.
// getopt state must be reset between calls.
// Redirects XDG_CONFIG_HOME to an empty temp dir so load_config()
// returns pure defaults instead of reading the user's real config.
static CliResult run_cli(std::initializer_list<const char*> args) {
    // Build argv (must be non-const char* for getopt)
    std::vector<char*> argv;
    for (const char* a : args)
        argv.push_back(const_cast<char*>(a));
    argv.push_back(nullptr);

    optind = 0; // Reset getopt state

    // Isolate from host config
    const char* old_xdg = std::getenv("XDG_CONFIG_HOME");
    setenv("XDG_CONFIG_HOME", "/tmp/recmeet_test_cli_no_config", 1);

    auto result = parse_cli(static_cast<int>(argv.size() - 1), argv.data());

    // Restore
    if (old_xdg)
        setenv("XDG_CONFIG_HOME", old_xdg, 1);
    else
        unsetenv("XDG_CONFIG_HOME");

    return result;
}

TEST_CASE("parse_cli: defaults from config when no flags", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK_FALSE(cli.list_sources);
    CHECK_FALSE(cli.show_help);
    CHECK_FALSE(cli.show_version);
    // Defaults come from load_config(), which reads the real config file.
    // Just verify the struct is populated.
    CHECK_FALSE(cli.cfg.whisper_model.empty());
}

TEST_CASE("parse_cli: --mic-only sets mic_only", "[cli]") {
    auto cli = run_cli({"recmeet", "--mic-only"});
    CHECK(cli.cfg.mic_only == true);
}

TEST_CASE("parse_cli: --model sets whisper_model", "[cli]") {
    auto cli = run_cli({"recmeet", "--model", "tiny"});
    CHECK(cli.cfg.whisper_model == "tiny");
}

TEST_CASE("parse_cli: --no-summary sets no_summary", "[cli]") {
    auto cli = run_cli({"recmeet", "--no-summary"});
    CHECK(cli.cfg.no_summary == true);
}

TEST_CASE("parse_cli: --source sets mic_source", "[cli]") {
    auto cli = run_cli({"recmeet", "--source", "alsa_input.test"});
    CHECK(cli.cfg.mic_source == "alsa_input.test");
}

TEST_CASE("parse_cli: --monitor sets monitor_source", "[cli]") {
    auto cli = run_cli({"recmeet", "--monitor", "alsa_output.test.monitor"});
    CHECK(cli.cfg.monitor_source == "alsa_output.test.monitor");
}

TEST_CASE("parse_cli: --output-dir sets output_dir", "[cli]") {
    auto cli = run_cli({"recmeet", "--output-dir", "/tmp/test_output"});
    CHECK(cli.cfg.output_dir == "/tmp/test_output");
    CHECK(cli.cfg.output_dir_explicit == true);
}

TEST_CASE("parse_cli: default output_dir_explicit is false", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.output_dir_explicit == false);
}

TEST_CASE("parse_cli: --api-key sets api_key", "[cli]") {
    auto cli = run_cli({"recmeet", "--api-key", "sk-test-key-123"});
    CHECK(cli.cfg.api_key == "sk-test-key-123");
}

TEST_CASE("parse_cli: --list-sources sets flag", "[cli]") {
    auto cli = run_cli({"recmeet", "--list-sources"});
    CHECK(cli.list_sources == true);
}

TEST_CASE("parse_cli: --llm-model sets llm_model", "[cli]") {
    auto cli = run_cli({"recmeet", "--llm-model", "/path/to/model.gguf"});
    CHECK(cli.cfg.llm_model == "/path/to/model.gguf");
}

TEST_CASE("parse_cli: --mmap enables llm_mmap", "[cli]") {
    auto cli = run_cli({"recmeet", "--mmap"});
    CHECK(cli.cfg.llm_mmap == true);
}

TEST_CASE("parse_cli: --no-mmap disables llm_mmap", "[cli]") {
    auto cli = run_cli({"recmeet", "--no-mmap"});
    CHECK(cli.cfg.llm_mmap == false);
}

TEST_CASE("parse_cli: llm_mmap defaults to false", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.llm_mmap == false);
}

TEST_CASE("parse_cli: --context-file sets context_file", "[cli]") {
    auto cli = run_cli({"recmeet", "--context-file", "/tmp/notes.md"});
    CHECK(cli.cfg.context_file == "/tmp/notes.md");
}

TEST_CASE("parse_cli: --language sets language", "[cli]") {
    auto cli = run_cli({"recmeet", "--language", "en"});
    CHECK(cli.cfg.language == "en");
}

TEST_CASE("parse_cli: default language is empty (auto-detect)", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.language.empty());
}

TEST_CASE("parse_cli: --reprocess sets reprocess_dir", "[cli]") {
    auto cli = run_cli({"recmeet", "--reprocess", "/some/dir"});
    CHECK(cli.cfg.reprocess_dir == "/some/dir");
}

TEST_CASE("parse_cli: default reprocess_dir is empty", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.reprocess_dir.empty());
}

TEST_CASE("parse_cli: --provider sets provider", "[cli]") {
    auto cli = run_cli({"recmeet", "--provider", "openai"});
    CHECK(cli.cfg.provider == "openai");
}

TEST_CASE("parse_cli: default provider is xai", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.provider == "xai");
}

TEST_CASE("parse_cli: --no-diarize disables diarize", "[cli]") {
    auto cli = run_cli({"recmeet", "--no-diarize"});
    CHECK(cli.cfg.diarize == false);
}

TEST_CASE("parse_cli: default diarize is true", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.diarize == true);
}

TEST_CASE("parse_cli: --num-speakers sets num_speakers", "[cli]") {
    auto cli = run_cli({"recmeet", "--num-speakers", "3"});
    CHECK(cli.cfg.num_speakers == 3);
}

TEST_CASE("parse_cli: default num_speakers is 0", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.num_speakers == 0);
}

TEST_CASE("parse_cli: --cluster-threshold sets cluster_threshold", "[cli]") {
    auto cli = run_cli({"recmeet", "--cluster-threshold", "0.7"});
    CHECK(cli.cfg.cluster_threshold == 0.7f);
}

TEST_CASE("parse_cli: default cluster_threshold is 1.18", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.cluster_threshold == 1.18f);
}

TEST_CASE("parse_cli: --threads sets threads", "[cli]") {
    auto cli = run_cli({"recmeet", "--threads", "8"});
    CHECK(cli.cfg.threads == 8);
}

TEST_CASE("parse_cli: default threads is 0", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.threads == 0);
}

TEST_CASE("parse_cli: --keep-sources sets keep_sources", "[cli]") {
    auto cli = run_cli({"recmeet", "--keep-sources"});
    CHECK(cli.cfg.keep_sources == true);
}

TEST_CASE("parse_cli: default keep_sources is false", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.keep_sources == false);
}

TEST_CASE("parse_cli: --no-vad disables vad", "[cli]") {
    auto cli = run_cli({"recmeet", "--no-vad"});
    CHECK(cli.cfg.vad == false);
}

TEST_CASE("parse_cli: default vad is true", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.vad == true);
}

TEST_CASE("parse_cli: --vad-threshold sets vad_threshold", "[cli]") {
    auto cli = run_cli({"recmeet", "--vad-threshold", "0.3"});
    CHECK(cli.cfg.vad_threshold == 0.3f);
}

TEST_CASE("parse_cli: default vad_threshold is 0.5", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.vad_threshold == 0.5f);
}

TEST_CASE("parse_cli: --note-dir sets note_dir", "[cli]") {
    auto cli = run_cli({"recmeet", "--note-dir", "/tmp/obsidian/meetings"});
    CHECK(cli.cfg.note_dir == "/tmp/obsidian/meetings");
}

TEST_CASE("parse_cli: default note_dir is empty", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.note_dir.empty());
}

TEST_CASE("parse_cli: --reset-speakers sets flag", "[cli]") {
    auto cli = run_cli({"recmeet", "--reset-speakers"});
    CHECK(cli.reset_speakers == true);
}

TEST_CASE("parse_cli: default reset_speakers is false", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.reset_speakers == false);
}

TEST_CASE("parse_cli: --log-level sets log level", "[cli]") {
    auto cli = run_cli({"recmeet", "--log-level", "info"});
    CHECK(cli.cfg.log_level_str == "info");
}

TEST_CASE("parse_cli: default log_level_str is error", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.log_level_str == "error");
}

TEST_CASE("parse_cli: --vocab sets vocabulary", "[cli]") {
    auto cli = run_cli({"recmeet", "--vocab", "John Suykerbuyk, PipeWire"});
    CHECK(cli.cfg.vocabulary == "John Suykerbuyk, PipeWire");
}

TEST_CASE("parse_cli: default vocabulary is empty", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.vocabulary.empty());
}

TEST_CASE("parse_cli: --list-vocab sets flag", "[cli]") {
    auto cli = run_cli({"recmeet", "--list-vocab"});
    CHECK(cli.list_vocab);
}

TEST_CASE("parse_cli: --add-vocab sets value", "[cli]") {
    auto cli = run_cli({"recmeet", "--add-vocab", "Kubernetes"});
    CHECK(cli.add_vocab == "Kubernetes");
}

TEST_CASE("parse_cli: --remove-vocab sets value", "[cli]") {
    auto cli = run_cli({"recmeet", "--remove-vocab", "PipeWire"});
    CHECK(cli.remove_vocab == "PipeWire");
}

TEST_CASE("parse_cli: --reset-vocab sets flag", "[cli]") {
    auto cli = run_cli({"recmeet", "--reset-vocab"});
    CHECK(cli.reset_vocab);
}

TEST_CASE("parse_cli: --context-text sets context_inline", "[cli]") {
    auto cli = run_cli({"recmeet", "--context-text", "Subject: Standup\nParticipants: Alice, Bob"});
    CHECK(cli.cfg.context_inline == "Subject: Standup\nParticipants: Alice, Bob");
}

TEST_CASE("parse_cli: default context_inline is empty", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.context_inline.empty());
}

TEST_CASE("parse_cli: --progress-json sets flag", "[cli]") {
    auto cli = run_cli({"recmeet", "--progress-json"});
    CHECK(cli.progress_json == true);
}

TEST_CASE("parse_cli: default progress_json is false", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.progress_json == false);
}

TEST_CASE("parse_cli: --config-json sets path", "[cli]") {
    auto cli = run_cli({"recmeet", "--config-json", "/tmp/pp-config.json"});
    CHECK(cli.config_json_path == "/tmp/pp-config.json");
}

TEST_CASE("parse_cli: default config_json_path is empty", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.config_json_path.empty());
}

TEST_CASE("parse_cli: subprocess mode flags combine", "[cli]") {
    auto cli = run_cli({"recmeet", "--reprocess", "/tmp/dir",
                         "--config-json", "/tmp/cfg.json",
                         "--progress-json", "--no-daemon"});
    CHECK(cli.progress_json == true);
    CHECK(cli.config_json_path == "/tmp/cfg.json");
    CHECK(cli.cfg.reprocess_dir == "/tmp/dir");
    CHECK(cli.daemon_mode == DaemonMode::Disable);
}

TEST_CASE("parse_cli: --context-text and --context-file can coexist", "[cli]") {
    auto cli = run_cli({"recmeet", "--context-text", "inline notes",
                         "--context-file", "/tmp/agenda.txt"});
    CHECK(cli.cfg.context_inline == "inline notes");
    CHECK(cli.cfg.context_file == "/tmp/agenda.txt");
}
