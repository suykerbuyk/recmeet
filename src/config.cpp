// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "config.h"
#include "log.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

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

std::string resolve_api_key(const ProviderInfo& provider,
                            const std::map<std::string, std::string>& api_keys,
                            const std::string& legacy_key) {
    if (const char* val = std::getenv(provider.env_var))
        return val;
    auto it = api_keys.find(provider.name);
    if (it != api_keys.end() && !it->second.empty())
        return it->second;
    return legacy_key;
}

Config load_config(const fs::path& config_path) {
    Config cfg;
    fs::path path = config_path.empty() ? config_dir() / "config.yaml" : config_path;

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
    cfg.keep_sources = get_bool(entries, "audio", "keep_sources", false);

    // Transcription section
    cfg.whisper_model = get_val(entries, "transcription", "model", cfg.whisper_model);
    cfg.language = get_val(entries, "transcription", "language", "");
    cfg.vocabulary = get_val(entries, "transcription", "vocabulary", "");

    // Summary section
    cfg.provider = get_val(entries, "summary", "provider", cfg.provider);
    cfg.api_url = get_val(entries, "summary", "api_url", cfg.api_url);
    std::string file_key = get_val(entries, "summary", "api_key", "");
    if (!file_key.empty())
        cfg.api_key = file_key;
    cfg.api_model = get_val(entries, "summary", "model", cfg.api_model);
    cfg.no_summary = get_bool(entries, "summary", "disabled", false);
    cfg.llm_model = get_val(entries, "summary", "llm_model", "");
    cfg.llm_mmap = get_bool(entries, "summary", "llm_mmap", false);

    // Per-provider API keys
    for (size_t i = 0; i < NUM_PROVIDERS; ++i) {
        std::string k = get_val(entries, "api_keys", PROVIDERS[i].name, "");
        if (!k.empty())
            cfg.api_keys[PROVIDERS[i].name] = k;
    }

    // Diarization section
    cfg.diarize = get_bool(entries, "diarization", "enabled", true);
    std::string ns = get_val(entries, "diarization", "num_speakers", "0");
    cfg.num_speakers = std::atoi(ns.c_str());
    std::string ct = get_val(entries, "diarization", "cluster_threshold", "");
    if (!ct.empty()) cfg.cluster_threshold = std::atof(ct.c_str());

    // Chunked diarization parameters (T2.1/T2.2 — same [diarization] section,
    // single source of truth per M-3'). Validate `chunk_minutes * 60 >
    // chunk_overlap_sec + 60` per M-5'; on violation, log a warning and fall
    // back to defaults rather than crashing the daemon at startup.
    {
        float cm = cfg.chunk_minutes;
        float co = cfg.chunk_overlap_sec;
        float st_thr = cfg.stitch_threshold;
        std::string cm_s = get_val(entries, "diarization", "chunk_minutes", "");
        std::string co_s = get_val(entries, "diarization", "chunk_overlap_sec", "");
        std::string st_s = get_val(entries, "diarization", "stitch_threshold", "");
        if (!cm_s.empty()) cm = static_cast<float>(std::atof(cm_s.c_str()));
        if (!co_s.empty()) co = static_cast<float>(std::atof(co_s.c_str()));
        if (!st_s.empty()) st_thr = static_cast<float>(std::atof(st_s.c_str()));

        bool any_loaded = !cm_s.empty() || !co_s.empty();
        if (any_loaded && cm * 60.0f <= co + 60.0f) {
            log_warn("config: invalid chunked-diarize values "
                     "(chunk_minutes=%.3f, chunk_overlap_sec=%.3f); "
                     "chunk_minutes*60 must exceed chunk_overlap_sec+60. "
                     "Falling back to defaults (15.0, 30.0).",
                     cm, co);
            // Keep struct defaults (already in cfg.chunk_minutes /
            // cfg.chunk_overlap_sec). Stitch threshold is independent of the
            // spacing invariant, so honour the loaded value if present.
            if (!st_s.empty()) cfg.stitch_threshold = st_thr;
        } else {
            cfg.chunk_minutes = cm;
            cfg.chunk_overlap_sec = co;
            cfg.stitch_threshold = st_thr;
        }
    }

    // Speaker identification section
    cfg.speaker_id = get_bool(entries, "speaker_id", "enabled", true);
    std::string st = get_val(entries, "speaker_id", "threshold", "");
    if (!st.empty()) cfg.speaker_threshold = std::atof(st.c_str());
    std::string sdb = get_val(entries, "speaker_id", "database", "");
    if (!sdb.empty()) cfg.speaker_db = sdb;

    // VAD section
    cfg.vad = get_bool(entries, "vad", "enabled", true);
    std::string vt = get_val(entries, "vad", "threshold", "");
    if (!vt.empty()) cfg.vad_threshold = std::atof(vt.c_str());
    std::string vms = get_val(entries, "vad", "min_silence", "");
    if (!vms.empty()) cfg.vad_min_silence = std::atof(vms.c_str());
    std::string vmsp = get_val(entries, "vad", "min_speech", "");
    if (!vmsp.empty()) cfg.vad_min_speech = std::atof(vmsp.c_str());
    std::string vmxs = get_val(entries, "vad", "max_speech", "");
    if (!vmxs.empty()) cfg.vad_max_speech = std::atof(vmxs.c_str());

    // Live captions section (Phase 4 — global preference; tray reads this
    // for its checkbox initial state). Both keys are optional; a missing
    // file or section keeps the struct defaults (enabled=false,
    // model=""). The model-name default is resolved at use time so the
    // Phase-0.2 lock stays in one place.
    cfg.captions_enabled = get_bool(entries, "captions", "enabled", false);
    cfg.caption_model = get_val(entries, "captions", "model", "");
    cfg.caption_normalize_display =
        get_bool(entries, "captions", "normalize_display", true);

    // General section
    std::string threads_str = get_val(entries, "general", "threads", "0");
    cfg.threads = std::atoi(threads_str.c_str());

    // Logging section
    cfg.log_level_str = get_val(entries, "logging", "level", "error");
    std::string log_dir_val = get_val(entries, "logging", "directory", "");
    if (!log_dir_val.empty()) cfg.log_dir = log_dir_val;
    std::string retention_val = get_val(entries, "logging", "retention_hours", "4");
    if (!retention_val.empty()) cfg.log_retention_hours = std::stoi(retention_val);

    // Env var override for log level (between config file and CLI)
    if (const char* env_level = std::getenv("RECMEET_LOG_LEVEL"))
        cfg.log_level_str = env_level;

    // Output section
    std::string out = get_val(entries, "output", "directory", "");
    if (!out.empty()) cfg.output_dir = out;

    // Notes section (read "notes" first, fall back to legacy "obsidian")
    std::string note_domain = get_val(entries, "notes", "domain", "");
    if (note_domain.empty())
        note_domain = get_val(entries, "obsidian", "domain", cfg.note.domain);
    cfg.note.domain = note_domain;

    std::string note_dir_val = get_val(entries, "notes", "directory", "");
    if (!note_dir_val.empty()) cfg.note_dir = note_dir_val;

    // Web server section
    std::string port_str = get_val(entries, "web", "port", "8384");
    cfg.web_port = std::atoi(port_str.c_str());
    cfg.web_bind = get_val(entries, "web", "bind", "127.0.0.1");

    // IPC framing limits (Phase A.2). Both knobs default to the struct
    // defaults; a zero / negative override falls back rather than
    // disabling the cap entirely (defense-in-depth — operator typo
    // shouldn't open the door to a slowloris).
    {
        std::string mmb = get_val(entries, "ipc", "max_message_bytes", "");
        if (!mmb.empty()) {
            long long v = std::atoll(mmb.c_str());
            if (v > 0) cfg.max_message_bytes = static_cast<size_t>(v);
            else
                log_warn("config: invalid [ipc] max_message_bytes=%s; "
                         "keeping default %zu", mmb.c_str(),
                         cfg.max_message_bytes);
        }
    }
    {
        std::string mub = get_val(entries, "server", "max_upload_bytes", "");
        if (!mub.empty()) {
            long long v = std::atoll(mub.c_str());
            if (v > 0) cfg.max_upload_bytes = static_cast<size_t>(v);
            else
                log_warn("config: invalid [server] max_upload_bytes=%s; "
                         "keeping default %zu", mub.c_str(),
                         cfg.max_upload_bytes);
        }
    }
    {
        // Phase A.3 — `[ipc] max_clients`. Same warn-and-fallback shape as
        // max_message_bytes above: a zero/negative override is a typo, not
        // an opt-out, so we keep the struct default rather than disabling
        // the cap.
        std::string mc = get_val(entries, "ipc", "max_clients", "");
        if (!mc.empty()) {
            long long v = std::atoll(mc.c_str());
            if (v > 0) cfg.max_clients = static_cast<size_t>(v);
            else
                log_warn("config: invalid [ipc] max_clients=%s; "
                         "keeping default %zu", mc.c_str(),
                         cfg.max_clients);
        }
    }
    {
        // Phase C.7 — `[server] slot.*` JobQueue capacities and
        // `allow_client_downloads`. Same warn-and-fallback shape as the
        // [ipc] knobs above: a zero/negative slot override is a typo, not
        // an opt-out, so the struct default of 1 is kept.
        auto parse_slot = [&](const char* key, int& dst) {
            std::string v = get_val(entries, "server", key, "");
            if (v.empty()) return;
            long long n = std::atoll(v.c_str());
            if (n > 0) dst = static_cast<int>(n);
            else
                log_warn("config: invalid [server] slot.%s=%s; keeping "
                         "default %d", key, v.c_str(), dst);
        };
        parse_slot("slot.postprocess", cfg.slot_postprocess);
        parse_slot("slot.streaming", cfg.slot_streaming);
        parse_slot("slot.model_download", cfg.slot_model_download);

        std::string acd = get_val(entries, "server", "allow_client_downloads", "");
        if (!acd.empty()) {
            cfg.allow_client_downloads =
                (acd == "true" || acd == "1" || acd == "yes" || acd == "on");
        }

        // Phase C.8 — `[server] diarization_cache_ttl_secs`. Same warn-and-
        // fallback shape as the [ipc] knobs above: a negative override is
        // a typo, not an opt-out (use 0 explicitly for "never expire").
        std::string dct = get_val(entries, "server",
                                  "diarization_cache_ttl_secs", "");
        if (!dct.empty()) {
            long long v = std::atoll(dct.c_str());
            if (v >= 0) cfg.diarization_cache_ttl_secs = static_cast<int64_t>(v);
            else
                log_warn("config: invalid [server] diarization_cache_ttl_secs=%s; "
                         "keeping default %lld", dct.c_str(),
                         (long long)cfg.diarization_cache_ttl_secs);
        }
    }

    return cfg;
}

void save_config(const Config& cfg, const fs::path& config_path) {
    fs::path path;
    if (config_path.empty()) {
        fs::path dir = config_dir();
        fs::create_directories(dir);
        path = dir / "config.yaml";
    } else {
        fs::create_directories(config_path.parent_path());
        path = config_path;
    }

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
    if (cfg.keep_sources)
        out << "  keep_sources: true\n";

    out << "\ntranscription:\n"
        << "  model: " << cfg.whisper_model << "\n";
    if (!cfg.language.empty())
        out << "  language: " << cfg.language << "\n";
    if (!cfg.vocabulary.empty())
        out << "  vocabulary: \"" << cfg.vocabulary << "\"\n";

    out << "\nsummary:\n"
        << "  provider: " << cfg.provider << "\n";
    if (!cfg.api_url.empty())
        out << "  api_url: \"" << cfg.api_url << "\"\n";
    out << "  model: " << cfg.api_model << "\n";
    if (cfg.no_summary)
        out << "  disabled: true\n";
    if (!cfg.llm_model.empty())
        out << "  llm_model: \"" << cfg.llm_model << "\"\n";
    if (cfg.llm_mmap)
        out << "  llm_mmap: true\n";

    // Per-provider API keys (never write legacy api_key)
    {
        bool has_keys = false;
        for (const auto& [name, key] : cfg.api_keys) {
            if (!key.empty()) { has_keys = true; break; }
        }
        if (has_keys) {
            out << "\napi_keys:\n";
            for (const auto& [name, key] : cfg.api_keys) {
                if (!key.empty())
                    out << "  " << name << ": \"" << key << "\"\n";
            }
        }
    }

    if (!cfg.diarize || cfg.num_speakers > 0 || cfg.cluster_threshold != 1.18f ||
        cfg.chunk_minutes != 15.0f || cfg.chunk_overlap_sec != 30.0f ||
        cfg.stitch_threshold != 0.6f) {
        out << "\ndiarization:\n";
        if (!cfg.diarize)
            out << "  enabled: false\n";
        if (cfg.num_speakers > 0)
            out << "  num_speakers: " << cfg.num_speakers << "\n";
        if (cfg.cluster_threshold != 1.18f)
            out << "  cluster_threshold: " << cfg.cluster_threshold << "\n";
        if (cfg.chunk_minutes != 15.0f)
            out << "  chunk_minutes: " << cfg.chunk_minutes << "\n";
        if (cfg.chunk_overlap_sec != 30.0f)
            out << "  chunk_overlap_sec: " << cfg.chunk_overlap_sec << "\n";
        if (cfg.stitch_threshold != 0.6f)
            out << "  stitch_threshold: " << cfg.stitch_threshold << "\n";
    }

    if (!cfg.speaker_id || cfg.speaker_threshold != 0.6f || !cfg.speaker_db.empty()) {
        out << "\nspeaker_id:\n";
        if (!cfg.speaker_id)
            out << "  enabled: false\n";
        if (cfg.speaker_threshold != 0.6f)
            out << "  threshold: " << cfg.speaker_threshold << "\n";
        if (!cfg.speaker_db.empty())
            out << "  database: \"" << cfg.speaker_db.string() << "\"\n";
    }

    if (!cfg.vad || cfg.vad_threshold != 0.5f || cfg.vad_min_silence != 0.5f ||
        cfg.vad_min_speech != 0.25f || cfg.vad_max_speech != 30.0f) {
        out << "\nvad:\n";
        if (!cfg.vad)
            out << "  enabled: false\n";
        if (cfg.vad_threshold != 0.5f)
            out << "  threshold: " << cfg.vad_threshold << "\n";
        if (cfg.vad_min_silence != 0.5f)
            out << "  min_silence: " << cfg.vad_min_silence << "\n";
        if (cfg.vad_min_speech != 0.25f)
            out << "  min_speech: " << cfg.vad_min_speech << "\n";
        if (cfg.vad_max_speech != 30.0f)
            out << "  max_speech: " << cfg.vad_max_speech << "\n";
    }

    // Live captions (Phase 4 + 5.5) — only emitted when non-default to keep
    // the generated YAML compact for the common case where captions are off.
    // `normalize_display` (Phase 5.5) is a render-side knob (default true);
    // emit only when the user has explicitly turned it off so the YAML
    // round-trip preserves the negation.
    if (cfg.captions_enabled || !cfg.caption_model.empty()
        || !cfg.caption_normalize_display) {
        out << "\ncaptions:\n";
        if (cfg.captions_enabled)
            out << "  enabled: true\n";
        if (!cfg.caption_model.empty())
            out << "  model: " << cfg.caption_model << "\n";
        if (!cfg.caption_normalize_display)
            out << "  normalize_display: false\n";
    }

    out << "\noutput:\n"
        << "  directory: \"" << cfg.output_dir.string() << "\"\n";

    if (cfg.threads > 0) {
        out << "\ngeneral:\n"
            << "  threads: " << cfg.threads << "\n";
    }

    if (cfg.log_level_str != "error" || !cfg.log_dir.empty() || cfg.log_retention_hours != 4) {
        out << "\nlogging:\n"
            << "  level: " << cfg.log_level_str << "\n";
        if (!cfg.log_dir.empty())
            out << "  directory: \"" << cfg.log_dir.string() << "\"\n";
        if (cfg.log_retention_hours != 4)
            out << "  retention_hours: " << cfg.log_retention_hours << "\n";
    }

    out << "\nnotes:\n"
        << "  domain: " << cfg.note.domain << "\n";
    if (!cfg.note_dir.empty())
        out << "  directory: \"" << cfg.note_dir.string() << "\"\n";

    if (cfg.web_port != 8384 || cfg.web_bind != "127.0.0.1") {
        out << "\nweb:\n";
        if (cfg.web_port != 8384)
            out << "  port: " << cfg.web_port << "\n";
        if (cfg.web_bind != "127.0.0.1")
            out << "  bind: \"" << cfg.web_bind << "\"\n";
    }

    // IPC framing limits (Phase A.2 / A.3). Only emit when non-default to
    // keep the generated YAML compact. `max_message_bytes` and
    // `max_clients` share the `[ipc]` section; emit the section header
    // once when either knob is overridden.
    bool emit_ipc_section = (cfg.max_message_bytes != 8ull * 1024 * 1024)
                         || (cfg.max_clients       != 16);
    if (emit_ipc_section) {
        out << "\nipc:\n";
        if (cfg.max_message_bytes != 8ull * 1024 * 1024)
            out << "  max_message_bytes: " << cfg.max_message_bytes << "\n";
        if (cfg.max_clients != 16)
            out << "  max_clients: " << cfg.max_clients << "\n";
    }
    {
        // Phase C.7 — emit a [server] section when any server-scoped knob
        // diverges from its struct default. max_upload_bytes plus the
        // C.7 JobQueue slot capacities and allow_client_downloads.
        bool emit_server_section =
            cfg.max_upload_bytes != 4ull * 1024 * 1024 * 1024 ||
            cfg.slot_postprocess != 1 || cfg.slot_streaming != 1 ||
            cfg.slot_model_download != 1 || !cfg.allow_client_downloads;
        if (emit_server_section) {
            out << "\nserver:\n";
            if (cfg.max_upload_bytes != 4ull * 1024 * 1024 * 1024)
                out << "  max_upload_bytes: " << cfg.max_upload_bytes << "\n";
            if (cfg.slot_postprocess != 1)
                out << "  slot.postprocess: " << cfg.slot_postprocess << "\n";
            if (cfg.slot_streaming != 1)
                out << "  slot.streaming: " << cfg.slot_streaming << "\n";
            if (cfg.slot_model_download != 1)
                out << "  slot.model_download: " << cfg.slot_model_download << "\n";
            if (!cfg.allow_client_downloads)
                out << "  allow_client_downloads: false\n";
        }
    }

    out.close();
    chmod(path.c_str(), 0600);
}

} // namespace recmeet
