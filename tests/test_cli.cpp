#include <catch2/catch_test_macros.hpp>
#include "cli.h"

#include <getopt.h>

using namespace recmeet;

// Helper to build argv and call parse_cli.
// getopt state must be reset between calls.
static CliResult run_cli(std::initializer_list<const char*> args) {
    // Build argv (must be non-const char* for getopt)
    std::vector<char*> argv;
    for (const char* a : args)
        argv.push_back(const_cast<char*>(a));
    argv.push_back(nullptr);

    optind = 0; // Reset getopt state

    return parse_cli(static_cast<int>(argv.size() - 1), argv.data());
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
}

TEST_CASE("parse_cli: --api-key sets api_key", "[cli]") {
    auto cli = run_cli({"recmeet", "--api-key", "sk-test-key-123"});
    CHECK(cli.cfg.api_key == "sk-test-key-123");
}

TEST_CASE("parse_cli: --obsidian-vault sets obsidian config", "[cli]") {
    auto cli = run_cli({"recmeet", "--obsidian-vault", "/home/user/vault"});
    CHECK(cli.cfg.obsidian.vault_path == "/home/user/vault");
    CHECK(cli.cfg.obsidian_enabled == true);
}

TEST_CASE("parse_cli: --list-sources sets flag", "[cli]") {
    auto cli = run_cli({"recmeet", "--list-sources"});
    CHECK(cli.list_sources == true);
}

TEST_CASE("parse_cli: --llm-model sets llm_model", "[cli]") {
    auto cli = run_cli({"recmeet", "--llm-model", "/path/to/model.gguf"});
    CHECK(cli.cfg.llm_model == "/path/to/model.gguf");
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

TEST_CASE("parse_cli: --threads sets threads", "[cli]") {
    auto cli = run_cli({"recmeet", "--threads", "8"});
    CHECK(cli.cfg.threads == 8);
}

TEST_CASE("parse_cli: default threads is 0", "[cli]") {
    auto cli = run_cli({"recmeet"});
    CHECK(cli.cfg.threads == 0);
}
