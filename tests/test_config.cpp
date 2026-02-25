// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "config.h"

#include <cstdlib>
#include <fstream>

using namespace recmeet;

static fs::path tmp_config() {
    fs::path dir = fs::temp_directory_path() / "recmeet_test_config";
    fs::create_directories(dir);
    return dir / "config.yaml";
}

TEST_CASE("save_config + load_config round-trip", "[config]") {
    fs::path path = tmp_config();

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
    cfg.cluster_threshold = 0.8f;
    cfg.threads = 12;
    cfg.log_level_str = "info";
    cfg.log_dir = "/tmp/recmeet-test-logs";
    cfg.output_dir = "/tmp/meetings";
    cfg.note.domain = "engineering";

    save_config(cfg, path);
    REQUIRE(fs::exists(path));

    std::ifstream in(path);
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
    CHECK(content.find("cluster_threshold: 0.8") != std::string::npos);
    CHECK(content.find("threads: 12") != std::string::npos);
    CHECK(content.find("level: info") != std::string::npos);
    CHECK(content.find("directory: \"/tmp/recmeet-test-logs\"") != std::string::npos);
    CHECK(content.find("directory: \"/tmp/meetings\"") != std::string::npos);
    CHECK(content.find("domain: engineering") != std::string::npos);

    // Load it back and verify
    Config loaded = load_config(path);
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
    CHECK(loaded.cluster_threshold == 0.8f);
    CHECK(loaded.threads == 12);
    CHECK(loaded.log_level_str == "info");
    CHECK(loaded.log_dir == "/tmp/recmeet-test-logs");
    CHECK(loaded.output_dir == "/tmp/meetings");
    CHECK(loaded.note.domain == "engineering");

    fs::remove_all(path.parent_path());
}

TEST_CASE("load_config: returns defaults when no file exists", "[config]") {
    fs::path path = fs::temp_directory_path() / "recmeet_test_config_noexist" / "config.yaml";
    fs::remove_all(path.parent_path());

    Config cfg = load_config(path);
    CHECK(cfg.whisper_model == "base");
    CHECK(cfg.provider == "xai");
    CHECK(cfg.api_url.empty());
    CHECK(cfg.api_model == "grok-3");
    CHECK(cfg.mic_only == false);
    CHECK(cfg.no_summary == false);
    CHECK(cfg.diarize == true);
    CHECK(cfg.num_speakers == 0);
    CHECK(cfg.cluster_threshold == 1.18f);
    CHECK(cfg.threads == 0);
}

TEST_CASE("load_config: handles malformed YAML gracefully", "[config]") {
    fs::path path = tmp_config();

    {
        std::ofstream out(path);
        out << "{{{{not yaml at all!!!:::\n\x01\x02\x03\n";
    }

    // Should not crash — returns some config (likely defaults)
    Config cfg = load_config(path);
    CHECK_FALSE(cfg.whisper_model.empty()); // Still has a default

    fs::remove_all(path.parent_path());
}

TEST_CASE("load_config: partial config fills only specified fields", "[config]") {
    fs::path path = tmp_config();

    {
        std::ofstream out(path);
        out << "transcription:\n  model: tiny\n";
    }

    Config cfg = load_config(path);
    CHECK(cfg.whisper_model == "tiny");
    // Others should be defaults
    CHECK(cfg.provider == "xai");
    CHECK(cfg.api_url.empty());
    CHECK(cfg.api_model == "grok-3");
    CHECK(cfg.mic_only == false);

    fs::remove_all(path.parent_path());
}

TEST_CASE("load_config: reads XAI_API_KEY from environment", "[config]") {
    fs::path path = fs::temp_directory_path() / "recmeet_test_config_env" / "config.yaml";
    fs::remove_all(path.parent_path());

    // Save and set env var
    const char* old_key = std::getenv("XAI_API_KEY");
    std::string saved_key = old_key ? old_key : "";
    setenv("XAI_API_KEY", "test-api-key-12345", 1);

    Config cfg = load_config(path);
    CHECK(cfg.api_key == "test-api-key-12345");

    // Restore env
    if (saved_key.empty())
        unsetenv("XAI_API_KEY");
    else
        setenv("XAI_API_KEY", saved_key.c_str(), 1);
}
