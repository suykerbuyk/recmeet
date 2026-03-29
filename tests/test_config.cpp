// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "config.h"

#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

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
    cfg.keep_sources = true;
    cfg.whisper_model = "small";
    cfg.language = "en";
    cfg.vocabulary = "John Suykerbuyk, PipeWire";
    cfg.provider = "openai";
    cfg.api_url = "https://api.example.com/v1/chat";
    cfg.api_model = "gpt-4";
    cfg.no_summary = true;
    cfg.llm_model = "/path/to/model.gguf";
    cfg.llm_mmap = true;
    cfg.cluster_threshold = 0.8f;
    cfg.threads = 12;
    cfg.log_level_str = "info";
    cfg.log_dir = "/tmp/recmeet-test-logs";
    cfg.output_dir = "/tmp/meetings";
    cfg.note.domain = "engineering";
    cfg.note_dir = "/home/user/obsidian/Meetings";

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
    CHECK(content.find("keep_sources: true") != std::string::npos);
    CHECK(content.find("model: small") != std::string::npos);
    CHECK(content.find("language: en") != std::string::npos);
    CHECK(content.find("vocabulary: \"John Suykerbuyk, PipeWire\"") != std::string::npos);
    CHECK(content.find("provider: openai") != std::string::npos);
    CHECK(content.find("api_url: \"https://api.example.com/v1/chat\"") != std::string::npos);
    CHECK(content.find("model: gpt-4") != std::string::npos);
    CHECK(content.find("disabled: true") != std::string::npos);
    CHECK(content.find("llm_model: \"/path/to/model.gguf\"") != std::string::npos);
    CHECK(content.find("llm_mmap: true") != std::string::npos);
    CHECK(content.find("cluster_threshold: 0.8") != std::string::npos);
    CHECK(content.find("threads: 12") != std::string::npos);
    CHECK(content.find("level: info") != std::string::npos);
    CHECK(content.find("directory: \"/tmp/recmeet-test-logs\"") != std::string::npos);
    CHECK(content.find("directory: \"/tmp/meetings\"") != std::string::npos);
    CHECK(content.find("domain: engineering") != std::string::npos);
    CHECK(content.find("directory: \"/home/user/obsidian/Meetings\"") != std::string::npos);

    // Load it back and verify
    Config loaded = load_config(path);
    CHECK(loaded.device_pattern == "test-device|pattern");
    CHECK(loaded.mic_source == "alsa_input.test");
    CHECK(loaded.monitor_source == "alsa_output.test.monitor");
    CHECK(loaded.mic_only == true);
    CHECK(loaded.keep_sources == true);
    CHECK(loaded.whisper_model == "small");
    CHECK(loaded.language == "en");
    CHECK(loaded.vocabulary == "John Suykerbuyk, PipeWire");
    CHECK(loaded.provider == "openai");
    CHECK(loaded.api_url == "https://api.example.com/v1/chat");
    CHECK(loaded.api_model == "gpt-4");
    CHECK(loaded.no_summary == true);
    CHECK(loaded.llm_model == "/path/to/model.gguf");
    CHECK(loaded.llm_mmap == true);
    CHECK(loaded.cluster_threshold == 0.8f);
    CHECK(loaded.threads == 12);
    CHECK(loaded.log_level_str == "info");
    CHECK(loaded.log_dir == "/tmp/recmeet-test-logs");
    CHECK(loaded.output_dir == "/tmp/meetings");
    CHECK(loaded.note.domain == "engineering");
    CHECK(loaded.note_dir == "/home/user/obsidian/Meetings");

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
    CHECK(cfg.keep_sources == false);
    CHECK(cfg.no_summary == false);
    CHECK(cfg.llm_mmap == false);
    CHECK(cfg.diarize == true);
    CHECK(cfg.num_speakers == 0);
    CHECK(cfg.cluster_threshold == 1.18f);
    CHECK(cfg.threads == 0);
    CHECK(cfg.vad == true);
    CHECK(cfg.vad_threshold == 0.5f);
    CHECK(cfg.vad_min_silence == 0.5f);
    CHECK(cfg.vad_min_speech == 0.25f);
    CHECK(cfg.vad_max_speech == 30.0f);
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

TEST_CASE("save_config never writes legacy api_key to YAML", "[config]") {
    fs::path dir = fs::temp_directory_path() / "recmeet_test_save_apikey";
    fs::remove_all(dir);
    fs::path path = dir / "config.yaml";

    Config cfg;
    cfg.api_key = "sk-super-secret-key-99999";
    cfg.provider = "openai";
    save_config(cfg, path);

    // Read the file and verify legacy api_key is NOT present
    std::ifstream in(path);
    REQUIRE(in.good());
    std::string contents((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    CHECK(contents.find("sk-super-secret-key-99999") == std::string::npos);
    // "api_key:" should not appear (only "api_keys:" is allowed)
    CHECK(contents.find("  api_key:") == std::string::npos);

    fs::remove_all(dir);
}

TEST_CASE("api_keys round-trip via save_config + load_config", "[config]") {
    fs::path dir = fs::temp_directory_path() / "recmeet_test_api_keys_rt";
    fs::remove_all(dir);
    fs::path path = dir / "config.yaml";

    Config cfg;
    cfg.api_keys["xai"] = "xai-test-key-123";
    cfg.api_keys["openai"] = "sk-test-key-456";
    cfg.api_keys["anthropic"] = "sk-ant-test-key-789";
    save_config(cfg, path);

    Config loaded = load_config(path);
    CHECK(loaded.api_keys["xai"] == "xai-test-key-123");
    CHECK(loaded.api_keys["openai"] == "sk-test-key-456");
    CHECK(loaded.api_keys["anthropic"] == "sk-ant-test-key-789");

    fs::remove_all(dir);
}

TEST_CASE("api_keys section format in YAML", "[config]") {
    fs::path dir = fs::temp_directory_path() / "recmeet_test_api_keys_fmt";
    fs::remove_all(dir);
    fs::path path = dir / "config.yaml";

    Config cfg;
    cfg.api_keys["xai"] = "xai-key";
    cfg.api_keys["openai"] = "sk-key";
    save_config(cfg, path);

    std::ifstream in(path);
    std::string contents((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());

    CHECK(contents.find("api_keys:\n") != std::string::npos);
    CHECK(contents.find("  openai: \"sk-key\"") != std::string::npos);
    CHECK(contents.find("  xai: \"xai-key\"") != std::string::npos);

    fs::remove_all(dir);
}

TEST_CASE("save_config sets file permissions to 0600", "[config]") {
    fs::path dir = fs::temp_directory_path() / "recmeet_test_perms";
    fs::remove_all(dir);
    fs::path path = dir / "config.yaml";

    Config cfg;
    cfg.api_keys["xai"] = "xai-secret";
    save_config(cfg, path);

    struct stat st;
    REQUIRE(stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    fs::remove_all(dir);
}

TEST_CASE("resolve_api_key priority: env > api_keys > legacy > empty", "[config]") {
    const auto* prov = find_provider("xai");
    REQUIRE(prov != nullptr);

    // Save and clear env
    const char* old = std::getenv("XAI_API_KEY");
    std::string saved = old ? old : "";
    unsetenv("XAI_API_KEY");

    // Empty everything → empty
    {
        std::map<std::string, std::string> keys;
        CHECK(resolve_api_key(*prov, keys, "").empty());
    }

    // Legacy only
    {
        std::map<std::string, std::string> keys;
        CHECK(resolve_api_key(*prov, keys, "legacy-key") == "legacy-key");
    }

    // api_keys beats legacy
    {
        std::map<std::string, std::string> keys = {{"xai", "map-key"}};
        CHECK(resolve_api_key(*prov, keys, "legacy-key") == "map-key");
    }

    // env beats api_keys
    {
        setenv("XAI_API_KEY", "env-key", 1);
        std::map<std::string, std::string> keys = {{"xai", "map-key"}};
        CHECK(resolve_api_key(*prov, keys, "legacy-key") == "env-key");
        unsetenv("XAI_API_KEY");
    }

    // Restore env
    if (!saved.empty())
        setenv("XAI_API_KEY", saved.c_str(), 1);
}

TEST_CASE("backward compat: legacy api_key in config used as fallback", "[config]") {
    fs::path dir = fs::temp_directory_path() / "recmeet_test_legacy_compat";
    fs::remove_all(dir);
    fs::path path = dir / "config.yaml";

    {
        fs::create_directories(dir);
        std::ofstream out(path);
        out << "summary:\n  provider: openai\n  api_key: \"sk-legacy-fallback\"\n";
    }

    Config cfg = load_config(path);
    CHECK(cfg.api_key == "sk-legacy-fallback");
    CHECK(cfg.api_keys.empty());  // No api_keys section → empty map

    fs::remove_all(dir);
}
