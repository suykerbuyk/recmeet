#pragma once

#include "util.h"
#include "obsidian.h"

#include <string>
#include <map>
#include <vector>

namespace recmeet {

struct ProviderInfo {
    const char* name;          // "xai", "openai", "anthropic"
    const char* display;       // "xAI (Grok)", "OpenAI", "Anthropic"
    const char* base_url;      // "https://api.x.ai/v1"
    const char* env_var;       // "XAI_API_KEY"
    const char* default_model; // "grok-3"
};

extern const ProviderInfo PROVIDERS[];
extern const size_t NUM_PROVIDERS;

const ProviderInfo* find_provider(const std::string& name);
std::string resolve_api_key(const ProviderInfo& provider, const std::string& fallback_key);

struct Config {
    // Audio
    std::string device_pattern = DEFAULT_DEVICE_PATTERN;
    std::string mic_source;     // empty = auto-detect
    std::string monitor_source; // empty = auto-detect
    bool mic_only = false;

    // Transcription
    std::string whisper_model = "base";
    std::string language; // empty = auto-detect, otherwise ISO 639-1 code (e.g. "en")

    // Summarization
    std::string provider = "xai";
    std::string api_url; // empty = derived from provider
    std::string api_key;
    std::string api_model = "grok-3";
    bool no_summary = false;

    // Local LLM
    std::string llm_model; // path or name, empty = use HTTP API

    // Diarization (on by default when built with RECMEET_USE_SHERPA)
    bool diarize = true;
    int num_speakers = 0;  // 0 = auto-detect

    // Performance
    int threads = 0;  // 0 = auto-detect (hardware_concurrency - 1)

    // Output
    fs::path output_dir = "./meetings";

    // Obsidian
    ObsidianConfig obsidian;
    bool obsidian_enabled = false;

    // Context
    fs::path context_file;

    // Reprocess
    fs::path reprocess_dir;
};

/// Load config from ~/.config/recmeet/config.yaml (creates default if missing).
Config load_config();

/// Save config to ~/.config/recmeet/config.yaml.
void save_config(const Config& cfg);

} // namespace recmeet
