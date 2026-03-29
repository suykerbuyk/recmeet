// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "config_json.h"

namespace recmeet {

// ---------------------------------------------------------------------------
// Config → JsonMap
// ---------------------------------------------------------------------------

JsonMap config_to_map(const Config& cfg) {
    JsonMap m;

    // Audio
    m["device_pattern"]  = cfg.device_pattern;
    m["mic_source"]      = cfg.mic_source;
    m["monitor_source"]  = cfg.monitor_source;
    m["mic_only"]        = cfg.mic_only;
    m["keep_sources"]    = cfg.keep_sources;

    // Transcription
    m["whisper_model"]   = cfg.whisper_model;
    m["language"]        = cfg.language;
    m["vocabulary"]      = cfg.vocabulary;

    // Summarization
    m["provider"]        = cfg.provider;
    m["api_key"]         = cfg.api_key;
    m["api_url"]         = cfg.api_url;
    m["api_model"]       = cfg.api_model;
    m["no_summary"]      = cfg.no_summary;
    m["llm_model"]       = cfg.llm_model;
    m["llm_mmap"]        = cfg.llm_mmap;

    // Diarization
    m["diarize"]             = cfg.diarize;
    m["num_speakers"]        = static_cast<int64_t>(cfg.num_speakers);
    m["cluster_threshold"]   = static_cast<double>(cfg.cluster_threshold);

    // Speaker identification
    m["speaker_id"]          = cfg.speaker_id;
    m["speaker_threshold"]   = static_cast<double>(cfg.speaker_threshold);
    m["speaker_db"]          = cfg.speaker_db.string();

    // VAD
    m["vad"]              = cfg.vad;
    m["vad_threshold"]    = static_cast<double>(cfg.vad_threshold);
    m["vad_min_silence"]  = static_cast<double>(cfg.vad_min_silence);
    m["vad_min_speech"]   = static_cast<double>(cfg.vad_min_speech);
    m["vad_max_speech"]   = static_cast<double>(cfg.vad_max_speech);

    // Performance
    m["threads"]          = static_cast<int64_t>(cfg.threads);

    // Logging
    m["log_level"]        = cfg.log_level_str;
    m["log_dir"]          = cfg.log_dir.string();

    // Output
    m["output_dir"]       = cfg.output_dir.string();
    m["note_dir"]         = cfg.note_dir.string();
    m["reprocess_dir"]    = cfg.reprocess_dir.string();

    // Context
    m["context_file"]     = cfg.context_file.string();
    m["context_inline"]   = cfg.context_inline;

    // Notes
    m["note_domain"]      = cfg.note.domain;

    // Per-provider API keys (flat dot-prefixed to avoid nested JSON)
    for (const auto& [name, key] : cfg.api_keys) {
        if (!key.empty())
            m["api_keys." + name] = key;
    }

    return m;
}

// ---------------------------------------------------------------------------
// Config → JSON string
// ---------------------------------------------------------------------------

std::string config_to_json(const Config& cfg) {
    return serialize_json_map(config_to_map(cfg));
}

// ---------------------------------------------------------------------------
// JsonMap → Config
// ---------------------------------------------------------------------------

Config config_from_map(const JsonMap& m) {
    Config cfg;

    auto str = [&](const std::string& key, std::string& dst) {
        auto it = m.find(key);
        if (it != m.end()) dst = json_val_as_string(it->second, dst);
    };
    auto b = [&](const std::string& key, bool& dst) {
        auto it = m.find(key);
        if (it != m.end()) dst = json_val_as_bool(it->second, dst);
    };
    auto i = [&](const std::string& key, int& dst) {
        auto it = m.find(key);
        if (it != m.end()) dst = static_cast<int>(json_val_as_int(it->second, dst));
    };
    auto f = [&](const std::string& key, float& dst) {
        auto it = m.find(key);
        if (it != m.end()) dst = static_cast<float>(json_val_as_double(it->second, dst));
    };
    auto path = [&](const std::string& key, fs::path& dst) {
        auto it = m.find(key);
        if (it != m.end()) {
            std::string s = json_val_as_string(it->second);
            if (!s.empty()) dst = s;
        }
    };

    str("device_pattern", cfg.device_pattern);
    str("mic_source", cfg.mic_source);
    str("monitor_source", cfg.monitor_source);
    b("mic_only", cfg.mic_only);
    b("keep_sources", cfg.keep_sources);

    str("whisper_model", cfg.whisper_model);
    str("language", cfg.language);
    str("vocabulary", cfg.vocabulary);

    str("provider", cfg.provider);
    str("api_key", cfg.api_key);
    str("api_url", cfg.api_url);
    str("api_model", cfg.api_model);
    b("no_summary", cfg.no_summary);
    str("llm_model", cfg.llm_model);
    b("llm_mmap", cfg.llm_mmap);

    b("diarize", cfg.diarize);
    i("num_speakers", cfg.num_speakers);
    f("cluster_threshold", cfg.cluster_threshold);

    b("speaker_id", cfg.speaker_id);
    f("speaker_threshold", cfg.speaker_threshold);
    path("speaker_db", cfg.speaker_db);

    b("vad", cfg.vad);
    f("vad_threshold", cfg.vad_threshold);
    f("vad_min_silence", cfg.vad_min_silence);
    f("vad_min_speech", cfg.vad_min_speech);
    f("vad_max_speech", cfg.vad_max_speech);

    i("threads", cfg.threads);

    str("log_level", cfg.log_level_str);
    path("log_dir", cfg.log_dir);

    path("output_dir", cfg.output_dir);
    path("note_dir", cfg.note_dir);
    path("reprocess_dir", cfg.reprocess_dir);

    path("context_file", cfg.context_file);
    str("context_inline", cfg.context_inline);

    str("note_domain", cfg.note.domain);

    // Per-provider API keys (dot-prefixed)
    const std::string prefix = "api_keys.";
    for (const auto& [k, v] : m) {
        if (k.substr(0, prefix.size()) == prefix) {
            std::string provider = k.substr(prefix.size());
            std::string val = json_val_as_string(v);
            if (!val.empty())
                cfg.api_keys[provider] = val;
        }
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// JSON string → Config
// ---------------------------------------------------------------------------

Config config_from_json(const std::string& json) {
    IpcMessage msg;
    // Wrap in a dummy message to reuse our parser, or parse directly
    JsonMap map;
    // Use the internal parser — parse_json_object is in the anonymous namespace
    // of ipc_protocol.cpp, so we serialize→parse via IpcResponse round-trip.
    // Instead, just build an IpcResponse wrapper and parse it.
    // Actually, simpler: config_to_json outputs a JsonMap, and we need to
    // parse it back. We can wrap it as {"id":0,"result":...} and use parse_ipc_message.
    std::string wrapped = "{\"id\":0,\"result\":" + json + "}";
    if (parse_ipc_message(wrapped, msg) && msg.type == IpcMessageType::Response)
        return config_from_map(msg.response.result);
    return Config{};
}

} // namespace recmeet
