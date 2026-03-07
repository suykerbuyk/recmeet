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
    cfg.provider = "openai";
    cfg.api_url = "https://api.example.com/v1/chat";
    cfg.api_model = "gpt-4";
    cfg.no_summary = true;
    cfg.llm_model = "/path/to/model.gguf";
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
    CHECK(loaded.provider == original.provider);
    CHECK(loaded.api_url == original.api_url);
    CHECK(loaded.api_model == original.api_model);
    CHECK(loaded.no_summary == original.no_summary);
    CHECK(loaded.llm_model == original.llm_model);
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
