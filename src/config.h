#pragma once

#include "util.h"
#include "obsidian.h"

#include <string>
#include <map>
#include <vector>

namespace recmeet {

struct Config {
    // Audio
    std::string device_pattern = DEFAULT_DEVICE_PATTERN;
    std::string mic_source;     // empty = auto-detect
    std::string monitor_source; // empty = auto-detect
    bool mic_only = false;

    // Transcription
    std::string whisper_model = "base";

    // Summarization
    std::string api_url = "https://api.x.ai/v1/chat/completions";
    std::string api_key;
    std::string api_model = "grok-3";
    bool no_summary = false;

    // Local LLM
    std::string llm_model; // path or name, empty = use HTTP API

    // Output
    fs::path output_dir = "./meetings";

    // Obsidian
    ObsidianConfig obsidian;
    bool obsidian_enabled = false;

    // Context
    fs::path context_file;
};

/// Load config from ~/.config/recmeet/config.yaml (creates default if missing).
Config load_config();

/// Save config to ~/.config/recmeet/config.yaml.
void save_config(const Config& cfg);

} // namespace recmeet
