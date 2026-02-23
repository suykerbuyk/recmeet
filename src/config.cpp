// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace recmeet {

namespace {

// Simple YAML parser — handles flat key: value and one level of nesting.
// Good enough for our config file; no need for a full YAML library.

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string unquote(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                           (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

struct YamlEntry {
    std::string key;
    std::string value;
    int indent;
};

std::vector<YamlEntry> parse_yaml(const std::string& text) {
    std::vector<YamlEntry> entries;
    std::istringstream stream(text);
    std::string line;

    while (std::getline(stream, line)) {
        // Skip comments and empty lines
        auto trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        // Count leading spaces
        int indent = 0;
        while (indent < (int)line.size() && line[indent] == ' ') indent++;

        auto colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trim(trimmed.substr(0, colon));
        std::string val = trim(trimmed.substr(colon + 1));
        entries.push_back({key, unquote(val), indent});
    }
    return entries;
}

std::string get_val(const std::vector<YamlEntry>& entries,
                    const std::string& section, const std::string& key,
                    const std::string& def = "") {
    bool in_section = section.empty();
    for (const auto& e : entries) {
        if (!section.empty()) {
            if (e.indent == 0 && e.key == section && e.value.empty())
                in_section = true;
            else if (e.indent == 0 && e.key != section)
                in_section = false;
        }
        if (in_section && e.key == key)
            return e.value;
    }
    return def;
}

bool get_bool(const std::vector<YamlEntry>& entries,
              const std::string& section, const std::string& key,
              bool def = false) {
    std::string val = get_val(entries, section, key, def ? "true" : "false");
    return val == "true" || val == "yes" || val == "1";
}

} // anonymous namespace

const ProviderInfo PROVIDERS[] = {
    {"xai",       "xAI (Grok)", "https://api.x.ai/v1",       "XAI_API_KEY",       "grok-3"},
    {"openai",    "OpenAI",     "https://api.openai.com/v1",  "OPENAI_API_KEY",    "gpt-4o"},
    {"anthropic", "Anthropic",  "https://api.anthropic.com/v1", "ANTHROPIC_API_KEY", "claude-sonnet-4-6"},
};

const size_t NUM_PROVIDERS = sizeof(PROVIDERS) / sizeof(PROVIDERS[0]);

const ProviderInfo* find_provider(const std::string& name) {
    for (size_t i = 0; i < NUM_PROVIDERS; ++i)
        if (name == PROVIDERS[i].name)
            return &PROVIDERS[i];
    return nullptr;
}

std::string resolve_api_key(const ProviderInfo& provider, const std::string& fallback_key) {
    if (const char* val = std::getenv(provider.env_var))
        return val;
    return fallback_key;
}

Config load_config() {
    Config cfg;
    fs::path path = config_dir() / "config.yaml";

    // Check for API key in environment (try provider-specific, then XAI_API_KEY for compat)
    for (size_t i = 0; i < NUM_PROVIDERS; ++i) {
        if (const char* key = std::getenv(PROVIDERS[i].env_var)) {
            cfg.api_key = key;
            break;
        }
    }

    if (!fs::exists(path))
        return cfg;

    std::ifstream in(path);
    if (!in) return cfg;

    std::ostringstream buf;
    buf << in.rdbuf();
    auto entries = parse_yaml(buf.str());

    // Audio section
    cfg.device_pattern = get_val(entries, "audio", "device_pattern", cfg.device_pattern);
    cfg.mic_source = get_val(entries, "audio", "mic_source", "");
    cfg.monitor_source = get_val(entries, "audio", "monitor_source", "");
    cfg.mic_only = get_bool(entries, "audio", "mic_only", false);

    // Transcription section
    cfg.whisper_model = get_val(entries, "transcription", "model", cfg.whisper_model);
    cfg.language = get_val(entries, "transcription", "language", "");

    // Summary section
    cfg.provider = get_val(entries, "summary", "provider", cfg.provider);
    cfg.api_url = get_val(entries, "summary", "api_url", cfg.api_url);
    std::string file_key = get_val(entries, "summary", "api_key", "");
    if (!file_key.empty())
        cfg.api_key = file_key;
    cfg.api_model = get_val(entries, "summary", "model", cfg.api_model);
    cfg.no_summary = get_bool(entries, "summary", "disabled", false);
    cfg.llm_model = get_val(entries, "summary", "llm_model", "");

    // Diarization section
    cfg.diarize = get_bool(entries, "diarization", "enabled", true);
    std::string ns = get_val(entries, "diarization", "num_speakers", "0");
    cfg.num_speakers = std::atoi(ns.c_str());
    std::string ct = get_val(entries, "diarization", "cluster_threshold", "");
    if (!ct.empty()) cfg.cluster_threshold = std::atof(ct.c_str());

    // General section
    std::string threads_str = get_val(entries, "general", "threads", "0");
    cfg.threads = std::atoi(threads_str.c_str());

    // Output section
    std::string out = get_val(entries, "output", "directory", "");
    if (!out.empty()) cfg.output_dir = out;

    // Obsidian section
    std::string vault = get_val(entries, "obsidian", "vault", "");
    if (!vault.empty()) {
        cfg.obsidian_enabled = true;
        cfg.obsidian.vault_path = vault;
    }
    cfg.obsidian.subfolder = get_val(entries, "obsidian", "subfolder", cfg.obsidian.subfolder);
    cfg.obsidian.domain = get_val(entries, "obsidian", "domain", cfg.obsidian.domain);

    return cfg;
}

void save_config(const Config& cfg) {
    fs::path dir = config_dir();
    fs::create_directories(dir);
    fs::path path = dir / "config.yaml";

    std::ofstream out(path);
    if (!out)
        throw RecmeetError("Cannot write config: " + path.string());

    out << "# recmeet configuration\n\n"
        << "audio:\n"
        << "  device_pattern: \"" << cfg.device_pattern << "\"\n";
    if (!cfg.mic_source.empty())
        out << "  mic_source: \"" << cfg.mic_source << "\"\n";
    if (!cfg.monitor_source.empty())
        out << "  monitor_source: \"" << cfg.monitor_source << "\"\n";
    if (cfg.mic_only)
        out << "  mic_only: true\n";

    out << "\ntranscription:\n"
        << "  model: " << cfg.whisper_model << "\n";
    if (!cfg.language.empty())
        out << "  language: " << cfg.language << "\n";

    out << "\nsummary:\n"
        << "  provider: " << cfg.provider << "\n";
    if (!cfg.api_url.empty())
        out << "  api_url: \"" << cfg.api_url << "\"\n";
    out << "  model: " << cfg.api_model << "\n";
    if (cfg.no_summary)
        out << "  disabled: true\n";
    if (!cfg.llm_model.empty())
        out << "  llm_model: \"" << cfg.llm_model << "\"\n";

    if (!cfg.diarize || cfg.num_speakers > 0 || cfg.cluster_threshold != 1.18f) {
        out << "\ndiarization:\n";
        if (!cfg.diarize)
            out << "  enabled: false\n";
        if (cfg.num_speakers > 0)
            out << "  num_speakers: " << cfg.num_speakers << "\n";
        if (cfg.cluster_threshold != 1.18f)
            out << "  cluster_threshold: " << cfg.cluster_threshold << "\n";
    }

    out << "\noutput:\n"
        << "  directory: \"" << cfg.output_dir.string() << "\"\n";

    if (cfg.threads > 0) {
        out << "\ngeneral:\n"
            << "  threads: " << cfg.threads << "\n";
    }

    if (cfg.obsidian_enabled) {
        out << "\nobsidian:\n"
            << "  vault: \"" << cfg.obsidian.vault_path.string() << "\"\n"
            << "  subfolder: \"" << cfg.obsidian.subfolder << "\"\n"
            << "  domain: " << cfg.obsidian.domain << "\n";
    }
}

} // namespace recmeet
