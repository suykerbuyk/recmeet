#include <catch2/catch_test_macros.hpp>
#include "config.h"

#include <cstdlib>
#include <fstream>

using namespace recmeet;

static fs::path tmp_dir() {
    fs::path dir = fs::temp_directory_path() / "recmeet_test_config";
    fs::create_directories(dir);
    return dir;
}

TEST_CASE("save_config + load_config round-trip", "[config]") {
    auto dir = tmp_dir();
    fs::path config_path = dir / "config.yaml";

    // We can't easily override config_dir() since it reads XDG env vars,
    // but we can test save_config by saving and reading back the file.
    Config cfg;
    cfg.device_pattern = "test-device|pattern";
    cfg.mic_source = "alsa_input.test";
    cfg.monitor_source = "alsa_output.test.monitor";
    cfg.mic_only = true;
    cfg.whisper_model = "small";
    cfg.language = "en";
    cfg.provider = "openai";
    cfg.api_url = "https://api.example.com/v1/chat";
    cfg.api_model = "gpt-4";
    cfg.no_summary = true;
    cfg.llm_model = "/path/to/model.gguf";
    cfg.threads = 12;
    cfg.output_dir = "/tmp/meetings";
    cfg.obsidian_enabled = true;
    cfg.obsidian.vault_path = "/home/user/obsidian/vault";
    cfg.obsidian.subfolder = "Notes/%Y/";
    cfg.obsidian.domain = "engineering";

    // Override config dir to our temp directory
    // Since save_config uses config_dir(), we'll test the file content directly
    save_config(cfg);

    // Read the actual saved file from the real config dir
    fs::path actual_path = config_dir() / "config.yaml";
    REQUIRE(fs::exists(actual_path));

    std::ifstream in(actual_path);
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string content = buf.str();

    CHECK(content.find("device_pattern: \"test-device|pattern\"") != std::string::npos);
    CHECK(content.find("mic_source: \"alsa_input.test\"") != std::string::npos);
    CHECK(content.find("monitor_source: \"alsa_output.test.monitor\"") != std::string::npos);
    CHECK(content.find("mic_only: true") != std::string::npos);
    CHECK(content.find("model: small") != std::string::npos);
    CHECK(content.find("language: en") != std::string::npos);
    CHECK(content.find("provider: openai") != std::string::npos);
    CHECK(content.find("api_url: \"https://api.example.com/v1/chat\"") != std::string::npos);
    CHECK(content.find("model: gpt-4") != std::string::npos);
    CHECK(content.find("disabled: true") != std::string::npos);
    CHECK(content.find("llm_model: \"/path/to/model.gguf\"") != std::string::npos);
    CHECK(content.find("threads: 12") != std::string::npos);
    CHECK(content.find("directory: \"/tmp/meetings\"") != std::string::npos);
    CHECK(content.find("vault: \"/home/user/obsidian/vault\"") != std::string::npos);
    CHECK(content.find("subfolder: \"Notes/%Y/\"") != std::string::npos);
    CHECK(content.find("domain: engineering") != std::string::npos);

    // Now load it back and verify
    Config loaded = load_config();
    CHECK(loaded.device_pattern == "test-device|pattern");
    CHECK(loaded.mic_source == "alsa_input.test");
    CHECK(loaded.monitor_source == "alsa_output.test.monitor");
    CHECK(loaded.mic_only == true);
    CHECK(loaded.whisper_model == "small");
    CHECK(loaded.language == "en");
    CHECK(loaded.provider == "openai");
    CHECK(loaded.api_url == "https://api.example.com/v1/chat");
    CHECK(loaded.api_model == "gpt-4");
    CHECK(loaded.no_summary == true);
    CHECK(loaded.llm_model == "/path/to/model.gguf");
    CHECK(loaded.threads == 12);
    CHECK(loaded.output_dir == "/tmp/meetings");
    CHECK(loaded.obsidian_enabled == true);
    CHECK(loaded.obsidian.vault_path == "/home/user/obsidian/vault");
    CHECK(loaded.obsidian.subfolder == "Notes/%Y/");
    CHECK(loaded.obsidian.domain == "engineering");

    // Clean up: restore a default config
    Config defaults;
    save_config(defaults);

    fs::remove_all(dir);
}

TEST_CASE("load_config: returns defaults when no file exists", "[config]") {
    // Temporarily rename config if it exists
    fs::path cfg_path = config_dir() / "config.yaml";
    fs::path backup = config_dir() / "config.yaml.bak";
    bool had_config = fs::exists(cfg_path);
    if (had_config)
        fs::rename(cfg_path, backup);

    Config cfg = load_config();
    CHECK(cfg.whisper_model == "base");
    CHECK(cfg.provider == "xai");
    CHECK(cfg.api_url.empty());
    CHECK(cfg.api_model == "grok-3");
    CHECK(cfg.mic_only == false);
    CHECK(cfg.no_summary == false);
    CHECK(cfg.diarize == true);
    CHECK(cfg.num_speakers == 0);
    CHECK(cfg.threads == 0);
    CHECK(cfg.obsidian_enabled == false);

    // Restore
    if (had_config)
        fs::rename(backup, cfg_path);
}

TEST_CASE("load_config: handles malformed YAML gracefully", "[config]") {
    fs::path cfg_path = config_dir() / "config.yaml";
    fs::path backup = config_dir() / "config.yaml.bak";
    bool had_config = fs::exists(cfg_path);
    if (had_config)
        fs::rename(cfg_path, backup);

    // Write garbled content
    fs::create_directories(config_dir());
    {
        std::ofstream out(cfg_path);
        out << "{{{{not yaml at all!!!:::\n\x01\x02\x03\n";
    }

    // Should not crash — returns some config (likely defaults)
    Config cfg = load_config();
    CHECK_FALSE(cfg.whisper_model.empty()); // Still has a default

    // Restore
    if (had_config)
        fs::rename(backup, cfg_path);
    else
        fs::remove(cfg_path);
}

TEST_CASE("load_config: partial config fills only specified fields", "[config]") {
    fs::path cfg_path = config_dir() / "config.yaml";
    fs::path backup = config_dir() / "config.yaml.bak";
    bool had_config = fs::exists(cfg_path);
    if (had_config)
        fs::rename(cfg_path, backup);

    // Write config with only whisper model specified
    fs::create_directories(config_dir());
    {
        std::ofstream out(cfg_path);
        out << "transcription:\n  model: tiny\n";
    }

    Config cfg = load_config();
    CHECK(cfg.whisper_model == "tiny");
    // Others should be defaults
    CHECK(cfg.provider == "xai");
    CHECK(cfg.api_url.empty());
    CHECK(cfg.api_model == "grok-3");
    CHECK(cfg.mic_only == false);
    CHECK(cfg.obsidian_enabled == false);

    // Restore
    if (had_config)
        fs::rename(backup, cfg_path);
    else
        fs::remove(cfg_path);
}

TEST_CASE("load_config: reads XAI_API_KEY from environment", "[config]") {
    fs::path cfg_path = config_dir() / "config.yaml";
    fs::path backup = config_dir() / "config.yaml.bak";
    bool had_config = fs::exists(cfg_path);
    if (had_config)
        fs::rename(cfg_path, backup);

    // Remove config file so only env var matters
    if (fs::exists(cfg_path))
        fs::remove(cfg_path);

    // Save and set env var
    const char* old_key = std::getenv("XAI_API_KEY");
    std::string saved_key = old_key ? old_key : "";
    setenv("XAI_API_KEY", "test-api-key-12345", 1);

    Config cfg = load_config();
    CHECK(cfg.api_key == "test-api-key-12345");

    // Restore env
    if (saved_key.empty())
        unsetenv("XAI_API_KEY");
    else
        setenv("XAI_API_KEY", saved_key.c_str(), 1);

    // Restore config
    if (had_config)
        fs::rename(backup, cfg_path);
}
