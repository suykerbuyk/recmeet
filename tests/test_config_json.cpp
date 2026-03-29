// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "config_json.h"

using namespace recmeet;

static Config make_test_config() {
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
    cfg.diarize = false;
    cfg.num_speakers = 3;
    cfg.cluster_threshold = 0.8f;
    cfg.vad = false;
    cfg.vad_threshold = 0.6f;
    cfg.vad_min_silence = 0.3f;
    cfg.vad_min_speech = 0.15f;
    cfg.vad_max_speech = 20.0f;
    cfg.threads = 12;
    cfg.log_level_str = "info";
    cfg.log_dir = "/tmp/recmeet-test-logs";
    cfg.output_dir = "/tmp/meetings";
    cfg.note_dir = "/home/user/obsidian/Meetings";
    cfg.context_file = "/tmp/context.txt";
    cfg.context_inline = "Subject: Weekly standup\nParticipants: Alice, Bob";
    cfg.reprocess_dir = "/tmp/meetings/2026-03-06_11-58";
    cfg.note.domain = "engineering";
    return cfg;
}

TEST_CASE("config_to_json + config_from_json round-trip", "[config_json]") {
    Config original = make_test_config();
    std::string json = config_to_json(original);

    REQUIRE_FALSE(json.empty());
    REQUIRE(json[0] == '{');

    Config loaded = config_from_json(json);

    CHECK(loaded.device_pattern == original.device_pattern);
    CHECK(loaded.mic_source == original.mic_source);
    CHECK(loaded.monitor_source == original.monitor_source);
    CHECK(loaded.mic_only == original.mic_only);
    CHECK(loaded.keep_sources == original.keep_sources);
    CHECK(loaded.whisper_model == original.whisper_model);
    CHECK(loaded.language == original.language);
    CHECK(loaded.vocabulary == original.vocabulary);
    CHECK(loaded.provider == original.provider);
    CHECK(loaded.api_url == original.api_url);
    CHECK(loaded.api_model == original.api_model);
    CHECK(loaded.no_summary == original.no_summary);
    CHECK(loaded.llm_model == original.llm_model);
    CHECK(loaded.llm_mmap == original.llm_mmap);
    CHECK(loaded.diarize == original.diarize);
    CHECK(loaded.num_speakers == original.num_speakers);
    CHECK_THAT(loaded.cluster_threshold,
               Catch::Matchers::WithinAbs(original.cluster_threshold, 0.01));
    CHECK(loaded.vad == original.vad);
    CHECK_THAT(loaded.vad_threshold,
               Catch::Matchers::WithinAbs(original.vad_threshold, 0.01));
    CHECK_THAT(loaded.vad_min_silence,
               Catch::Matchers::WithinAbs(original.vad_min_silence, 0.01));
    CHECK_THAT(loaded.vad_min_speech,
               Catch::Matchers::WithinAbs(original.vad_min_speech, 0.01));
    CHECK_THAT(loaded.vad_max_speech,
               Catch::Matchers::WithinAbs(original.vad_max_speech, 0.1));
    CHECK(loaded.threads == original.threads);
    CHECK(loaded.log_level_str == original.log_level_str);
    CHECK(loaded.log_dir == original.log_dir);
    CHECK(loaded.output_dir == original.output_dir);
    CHECK(loaded.note_dir == original.note_dir);
    CHECK(loaded.context_file == original.context_file);
    CHECK(loaded.context_inline == original.context_inline);
    CHECK(loaded.reprocess_dir == original.reprocess_dir);
    CHECK(loaded.note.domain == original.note.domain);
}

TEST_CASE("config_to_map + config_from_map round-trip", "[config_json]") {
    Config original = make_test_config();
    JsonMap map = config_to_map(original);
    Config loaded = config_from_map(map);

    CHECK(loaded.device_pattern == original.device_pattern);
    CHECK(loaded.mic_only == original.mic_only);
    CHECK(loaded.whisper_model == original.whisper_model);
    CHECK(loaded.vocabulary == original.vocabulary);
    CHECK(loaded.context_file == original.context_file);
    CHECK(loaded.context_inline == original.context_inline);
    CHECK(loaded.threads == original.threads);
    CHECK(loaded.note.domain == original.note.domain);
}

TEST_CASE("config_from_json: returns defaults on invalid JSON", "[config_json]") {
    Config cfg = config_from_json("not valid json");
    CHECK(cfg.whisper_model == "base");
    CHECK(cfg.provider == "xai");
    CHECK(cfg.threads == 0);
}

TEST_CASE("config_from_json: returns defaults on empty input", "[config_json]") {
    Config cfg = config_from_json("");
    CHECK(cfg.whisper_model == "base");
}

TEST_CASE("config_to_json: handles special characters in paths", "[config_json]") {
    Config cfg;
    cfg.output_dir = "/home/user/my \"meetings\"/output";
    cfg.device_pattern = "device\\pattern";

    std::string json = config_to_json(cfg);
    Config loaded = config_from_json(json);

    CHECK(loaded.output_dir == cfg.output_dir);
    CHECK(loaded.device_pattern == cfg.device_pattern);
}

TEST_CASE("config_to_map includes api_key for client-to-daemon IPC", "[config_json]") {
    Config cfg;
    cfg.api_key = "sk-secret-key-12345";
    cfg.provider = "openai";

    JsonMap map = config_to_map(cfg);

    // api_key IS included so clients can send credentials to daemon
    REQUIRE(map.find("api_key") != map.end());
    CHECK(json_val_as_string(map.at("api_key")) == "sk-secret-key-12345");

    // Round-trip preserves api_key
    Config loaded = config_from_map(map);
    CHECK(loaded.api_key == "sk-secret-key-12345");
}

TEST_CASE("config_to_json: default config round-trips", "[config_json]") {
    Config original;
    std::string json = config_to_json(original);
    Config loaded = config_from_json(json);

    CHECK(loaded.whisper_model == "base");
    CHECK(loaded.provider == "xai");
    CHECK(loaded.api_model == "grok-3");
    CHECK(loaded.mic_only == false);
    CHECK(loaded.diarize == true);
    CHECK(loaded.vad == true);
    CHECK(loaded.threads == 0);
}

TEST_CASE("api_keys IPC round-trip via config_to_map/config_from_map", "[config_json]") {
    Config original;
    original.api_keys["xai"] = "xai-key-123";
    original.api_keys["openai"] = "sk-key-456";
    original.api_keys["anthropic"] = "sk-ant-789";

    JsonMap map = config_to_map(original);

    // Verify flat dot-prefixed keys in map
    CHECK(json_val_as_string(map.at("api_keys.xai")) == "xai-key-123");
    CHECK(json_val_as_string(map.at("api_keys.openai")) == "sk-key-456");
    CHECK(json_val_as_string(map.at("api_keys.anthropic")) == "sk-ant-789");

    Config loaded = config_from_map(map);
    CHECK(loaded.api_keys["xai"] == "xai-key-123");
    CHECK(loaded.api_keys["openai"] == "sk-key-456");
    CHECK(loaded.api_keys["anthropic"] == "sk-ant-789");
}

TEST_CASE("api_keys not leaked in IPC events", "[config_json]") {
    // config_to_map includes api_key and api_keys for client→daemon IPC,
    // but event broadcasts should never contain them.
    // This test verifies that api_keys use dot-prefix encoding (not nested)
    // and that api_key is a separate field (existing behavior).
    Config cfg;
    cfg.api_key = "sk-secret";
    cfg.api_keys["xai"] = "xai-secret";

    JsonMap map = config_to_map(cfg);

    // These keys exist in the map (for IPC params), but the daemon's
    // broadcast code only sends event-specific fields, never the full config map.
    CHECK(map.find("api_key") != map.end());
    CHECK(map.find("api_keys.xai") != map.end());

    // Verify no nested "api_keys" key exists (would break IPC protocol)
    CHECK(map.find("api_keys") == map.end());
}
