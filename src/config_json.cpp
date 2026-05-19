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
    // Phase E.2(a) — client-side summarization style preference. Empty
    // default; round-trips through the daemon→subprocess JSON-config so
    // a future subprocess can read it without re-resolving from session.init.
    m["summary_style"]   = cfg.summary_style;

    // Diarization
    m["diarize"]             = cfg.diarize;
    m["num_speakers"]        = static_cast<int64_t>(cfg.num_speakers);
    m["cluster_threshold"]   = static_cast<double>(cfg.cluster_threshold);
    m["chunk_minutes"]       = static_cast<double>(cfg.chunk_minutes);
    m["chunk_overlap_sec"]   = static_cast<double>(cfg.chunk_overlap_sec);
    m["stitch_threshold"]    = static_cast<double>(cfg.stitch_threshold);

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

    // Live captions (Phase 3 — opt-in per recording via record.start params;
    // Phase 5.5 added `caption_normalize_display`, a client-only knob — the
    // daemon never reads it, but it round-trips through this map so the tray
    // and CLI carry it correctly across config saves).
    m["captions_enabled"]          = cfg.captions_enabled;
    m["caption_model"]             = cfg.caption_model;
    m["caption_normalize_display"] = cfg.caption_normalize_display;
    // Phase E.2(b) — client-side caption-latency preference. Round-trips
    // through the daemon→subprocess JSON-config boundary so the subprocess
    // (and future client-side consumers) see the same value session.init
    // applied to the Config.
    m["caption_latency_ms"]        = static_cast<int64_t>(cfg.caption_latency_ms);

    // Performance
    m["threads"]          = static_cast<int64_t>(cfg.threads);

    // Logging
    m["log_level"]        = cfg.log_level_str;
    m["log_dir"]          = cfg.log_dir.string();
    m["log_retention_hours"] = static_cast<int64_t>(cfg.log_retention_hours);

    // Output
    m["output_dir"]       = cfg.output_dir.string();
    m["note_dir"]         = cfg.note_dir.string();
    m["reprocess_dir"]    = cfg.reprocess_dir.string();
    m["reprocess_batch_dir"]     = cfg.reprocess_batch_dir.string();
    m["reprocess_batch_dry_run"] = cfg.reprocess_batch_dry_run;
    m["batch_mode"]              = cfg.batch_mode;

    // Context
    m["context_file"]     = cfg.context_file.string();
    m["context_inline"]   = cfg.context_inline;

    // Notes
    m["note_domain"]      = cfg.note.domain;
    {
        std::string joined;
        for (const auto& t : cfg.note.tags) {
            if (!joined.empty()) joined += ",";
            joined += t;
        }
        m["note_tags"] = joined;
    }

    // Phase C.8 — enroll mode flags. Round-trip through the daemon →
    // subprocess JSON-config so the subprocess inspects the flag and
    // skips transcribe / summarize / note-write.
    m["enroll_mode"] = cfg.enroll_mode;
    m["enroll_name"] = cfg.enroll_name;

    // Per-provider API keys (flat dot-prefixed to avoid nested JSON)
    for (const auto& [name, key] : cfg.api_keys) {
        if (!key.empty())
            m["api_keys." + name] = key;
    }

    // Phase E.2(c) — client-side server registry. `JsonVal` is a scalar
    // variant (string / int / double / bool / null), so nested-array
    // shapes are not representable. Flatten as `servers.count` plus per-
    // index `servers.<i>.name` / `servers.<i>.address` pairs. v1 only
    // honors index 0 so the typical wire form carries at most 3 keys
    // (count, name, address). config_from_map reconstructs the vector.
    m["servers.count"] = static_cast<int64_t>(cfg.servers.size());
    for (size_t i = 0; i < cfg.servers.size(); ++i) {
        std::string prefix = "servers." + std::to_string(i) + ".";
        m[prefix + "name"]    = cfg.servers[i].name;
        m[prefix + "address"] = cfg.servers[i].address;
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
    str("summary_style", cfg.summary_style);

    b("diarize", cfg.diarize);
    i("num_speakers", cfg.num_speakers);
    f("cluster_threshold", cfg.cluster_threshold);
    f("chunk_minutes", cfg.chunk_minutes);
    f("chunk_overlap_sec", cfg.chunk_overlap_sec);
    f("stitch_threshold", cfg.stitch_threshold);

    b("speaker_id", cfg.speaker_id);
    f("speaker_threshold", cfg.speaker_threshold);
    path("speaker_db", cfg.speaker_db);

    b("vad", cfg.vad);
    f("vad_threshold", cfg.vad_threshold);
    f("vad_min_silence", cfg.vad_min_silence);
    f("vad_min_speech", cfg.vad_min_speech);
    f("vad_max_speech", cfg.vad_max_speech);

    b("captions_enabled", cfg.captions_enabled);
    str("caption_model", cfg.caption_model);
    b("caption_normalize_display", cfg.caption_normalize_display);
    i("caption_latency_ms", cfg.caption_latency_ms);

    i("threads", cfg.threads);

    str("log_level", cfg.log_level_str);
    path("log_dir", cfg.log_dir);
    i("log_retention_hours", cfg.log_retention_hours);

    path("output_dir", cfg.output_dir);
    path("note_dir", cfg.note_dir);
    path("reprocess_dir", cfg.reprocess_dir);
    path("reprocess_batch_dir", cfg.reprocess_batch_dir);
    b("reprocess_batch_dry_run", cfg.reprocess_batch_dry_run);
    b("batch_mode", cfg.batch_mode);

    path("context_file", cfg.context_file);
    str("context_inline", cfg.context_inline);

    str("note_domain", cfg.note.domain);
    {
        auto it = m.find("note_tags");
        if (it != m.end()) {
            std::string s = json_val_as_string(it->second);
            cfg.note.tags.clear();
            size_t pos = 0;
            while (pos < s.size()) {
                auto next = s.find(',', pos);
                if (next == std::string::npos) next = s.size();
                auto tag = s.substr(pos, next - pos);
                if (!tag.empty()) cfg.note.tags.push_back(tag);
                pos = next + 1;
            }
        }
    }

    b("enroll_mode", cfg.enroll_mode);
    str("enroll_name", cfg.enroll_name);

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

    // Phase E.2(c) — reconstruct the servers vector from the flattened
    // `servers.count` + `servers.<i>.name` / `servers.<i>.address` keys.
    // Missing `servers.count` (e.g. older daemon writing the JSON without
    // the new field) → empty vector, no diff from struct default.
    {
        auto cit = m.find("servers.count");
        if (cit != m.end()) {
            int64_t n = json_val_as_int(cit->second, 0);
            if (n > 0) {
                cfg.servers.clear();
                cfg.servers.reserve(static_cast<size_t>(n));
                for (int64_t i = 0; i < n; ++i) {
                    ServerEntry entry;
                    std::string ip = "servers." + std::to_string(i) + ".";
                    auto nit = m.find(ip + "name");
                    auto ait = m.find(ip + "address");
                    if (nit != m.end())
                        entry.name = json_val_as_string(nit->second);
                    if (ait != m.end())
                        entry.address = json_val_as_string(ait->second);
                    cfg.servers.push_back(std::move(entry));
                }
            }
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

// ---------------------------------------------------------------------------
// Phase E.2 Wave 2.2a — ServerConfig / ClientConfig <-> JsonMap
// ---------------------------------------------------------------------------

JsonMap config_to_map(const ServerConfig& cfg) {
    JsonMap m;
    m["whisper_model"]            = cfg.whisper_model;
    m["llm_model"]                = cfg.llm_model;
    m["llm_mmap"]                 = cfg.llm_mmap;
    m["captions_enabled"]         = cfg.captions_enabled;
    m["caption_model"]            = cfg.caption_model;

    m["provider"]                 = cfg.provider;
    m["api_url"]                  = cfg.api_url;
    m["api_key"]                  = cfg.api_key;
    m["api_model"]                = cfg.api_model;
    for (const auto& [name, key] : cfg.api_keys) {
        if (!key.empty()) m["api_keys." + name] = key;
    }

    m["diarize"]                  = cfg.diarize;
    m["num_speakers"]             = static_cast<int64_t>(cfg.num_speakers);
    m["cluster_threshold"]        = static_cast<double>(cfg.cluster_threshold);
    m["chunk_minutes"]            = static_cast<double>(cfg.chunk_minutes);
    m["chunk_overlap_sec"]        = static_cast<double>(cfg.chunk_overlap_sec);
    m["stitch_threshold"]         = static_cast<double>(cfg.stitch_threshold);

    m["speaker_id"]               = cfg.speaker_id;
    m["speaker_threshold"]        = static_cast<double>(cfg.speaker_threshold);
    m["speaker_db"]               = cfg.speaker_db.string();

    m["vad"]                      = cfg.vad;
    m["vad_threshold"]            = static_cast<double>(cfg.vad_threshold);
    m["vad_min_silence"]          = static_cast<double>(cfg.vad_min_silence);
    m["vad_min_speech"]           = static_cast<double>(cfg.vad_min_speech);
    m["vad_max_speech"]           = static_cast<double>(cfg.vad_max_speech);

    m["threads"]                  = static_cast<int64_t>(cfg.threads);

    m["log_level"]                = cfg.log_level_str;
    m["log_dir"]                  = cfg.log_dir.string();
    m["log_retention_hours"]      = static_cast<int64_t>(cfg.log_retention_hours);

    m["web_port"]                 = static_cast<int64_t>(cfg.web_port);
    m["web_bind"]                 = cfg.web_bind;

    m["max_message_bytes"]        = static_cast<int64_t>(cfg.max_message_bytes);
    m["max_upload_bytes"]         = static_cast<int64_t>(cfg.max_upload_bytes);
    m["max_clients"]              = static_cast<int64_t>(cfg.max_clients);

    m["slot_postprocess"]         = static_cast<int64_t>(cfg.slot_postprocess);
    m["slot_streaming"]           = static_cast<int64_t>(cfg.slot_streaming);
    m["slot_model_download"]      = static_cast<int64_t>(cfg.slot_model_download);

    m["allow_client_downloads"]   = cfg.allow_client_downloads;

    m["retain_terminal_hours"]    = static_cast<int64_t>(cfg.retain_terminal_hours);
    m["diarization_cache_ttl_secs"] = static_cast<int64_t>(cfg.diarization_cache_ttl_secs);

    return m;
}

JsonMap config_to_map(const ClientConfig& cfg) {
    JsonMap m;
    m["device_pattern"]            = cfg.device_pattern;
    m["mic_source"]                = cfg.mic_source;
    m["monitor_source"]            = cfg.monitor_source;
    m["mic_only"]                  = cfg.mic_only;
    m["keep_sources"]              = cfg.keep_sources;

    m["language"]                  = cfg.language;
    m["vocabulary"]                = cfg.vocabulary;

    m["summary_style"]             = cfg.summary_style;
    m["no_summary"]                = cfg.no_summary;

    m["provider"]                  = cfg.provider;
    m["api_url"]                   = cfg.api_url;
    m["api_key"]                   = cfg.api_key;
    m["api_model"]                 = cfg.api_model;
    for (const auto& [name, key] : cfg.api_keys) {
        if (!key.empty()) m["api_keys." + name] = key;
    }

    m["llm_model"]                 = cfg.llm_model;
    m["llm_mmap"]                  = cfg.llm_mmap;

    m["caption_latency_ms"]        = static_cast<int64_t>(cfg.caption_latency_ms);
    m["caption_normalize_display"] = cfg.caption_normalize_display;

    m["output_dir"]                = cfg.output_dir.string();
    m["output_dir_explicit"]       = cfg.output_dir_explicit;
    m["note_dir"]                  = cfg.note_dir.string();

    m["note_domain"]               = cfg.note.domain;
    {
        std::string joined;
        for (const auto& t : cfg.note.tags) {
            if (!joined.empty()) joined += ",";
            joined += t;
        }
        m["note_tags"] = joined;
    }

    m["context_file"]              = cfg.context_file.string();
    m["context_inline"]            = cfg.context_inline;

    m["reprocess_dir"]             = cfg.reprocess_dir.string();
    m["reprocess_batch_dir"]       = cfg.reprocess_batch_dir.string();
    m["reprocess_batch_dry_run"]   = cfg.reprocess_batch_dry_run;
    m["batch_mode"]                = cfg.batch_mode;

    m["staging_max_bytes"]         = static_cast<int64_t>(cfg.staging_max_bytes);

    m["servers.count"]             = static_cast<int64_t>(cfg.servers.size());
    for (size_t i = 0; i < cfg.servers.size(); ++i) {
        std::string prefix = "servers." + std::to_string(i) + ".";
        m[prefix + "name"]    = cfg.servers[i].name;
        m[prefix + "address"] = cfg.servers[i].address;
    }

    m["enroll_mode"]               = cfg.enroll_mode;
    m["enroll_name"]               = cfg.enroll_name;

    return m;
}

ServerConfig server_config_from_map(const JsonMap& m) {
    ServerConfig cfg;
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
    auto i64 = [&](const std::string& key, int64_t& dst) {
        auto it = m.find(key);
        if (it != m.end()) dst = json_val_as_int(it->second, dst);
    };
    auto sz = [&](const std::string& key, size_t& dst) {
        auto it = m.find(key);
        if (it != m.end()) dst = static_cast<size_t>(
            json_val_as_int(it->second, static_cast<int64_t>(dst)));
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

    str("whisper_model", cfg.whisper_model);
    str("llm_model", cfg.llm_model);
    b("llm_mmap", cfg.llm_mmap);
    b("captions_enabled", cfg.captions_enabled);
    str("caption_model", cfg.caption_model);

    str("provider", cfg.provider);
    str("api_url", cfg.api_url);
    str("api_key", cfg.api_key);
    str("api_model", cfg.api_model);

    const std::string prefix = "api_keys.";
    for (const auto& [k, v] : m) {
        if (k.substr(0, prefix.size()) == prefix) {
            std::string provider = k.substr(prefix.size());
            std::string val = json_val_as_string(v);
            if (!val.empty()) cfg.api_keys[provider] = val;
        }
    }

    b("diarize", cfg.diarize);
    i("num_speakers", cfg.num_speakers);
    f("cluster_threshold", cfg.cluster_threshold);
    f("chunk_minutes", cfg.chunk_minutes);
    f("chunk_overlap_sec", cfg.chunk_overlap_sec);
    f("stitch_threshold", cfg.stitch_threshold);

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
    i("log_retention_hours", cfg.log_retention_hours);

    i("web_port", cfg.web_port);
    str("web_bind", cfg.web_bind);

    sz("max_message_bytes", cfg.max_message_bytes);
    sz("max_upload_bytes", cfg.max_upload_bytes);
    sz("max_clients", cfg.max_clients);

    i("slot_postprocess", cfg.slot_postprocess);
    i("slot_streaming", cfg.slot_streaming);
    i("slot_model_download", cfg.slot_model_download);

    b("allow_client_downloads", cfg.allow_client_downloads);

    i("retain_terminal_hours", cfg.retain_terminal_hours);
    i64("diarization_cache_ttl_secs", cfg.diarization_cache_ttl_secs);

    return cfg;
}

ClientConfig client_config_from_map(const JsonMap& m) {
    ClientConfig cfg;
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
    auto sz = [&](const std::string& key, size_t& dst) {
        auto it = m.find(key);
        if (it != m.end()) dst = static_cast<size_t>(
            json_val_as_int(it->second, static_cast<int64_t>(dst)));
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

    str("language", cfg.language);
    str("vocabulary", cfg.vocabulary);

    str("summary_style", cfg.summary_style);
    b("no_summary", cfg.no_summary);

    str("provider", cfg.provider);
    str("api_url", cfg.api_url);
    str("api_key", cfg.api_key);
    str("api_model", cfg.api_model);

    const std::string prefix = "api_keys.";
    for (const auto& [k, v] : m) {
        if (k.substr(0, prefix.size()) == prefix) {
            std::string provider = k.substr(prefix.size());
            std::string val = json_val_as_string(v);
            if (!val.empty()) cfg.api_keys[provider] = val;
        }
    }

    str("llm_model", cfg.llm_model);
    b("llm_mmap", cfg.llm_mmap);

    i("caption_latency_ms", cfg.caption_latency_ms);
    b("caption_normalize_display", cfg.caption_normalize_display);

    path("output_dir", cfg.output_dir);
    b("output_dir_explicit", cfg.output_dir_explicit);
    path("note_dir", cfg.note_dir);

    str("note_domain", cfg.note.domain);
    {
        auto it = m.find("note_tags");
        if (it != m.end()) {
            std::string s = json_val_as_string(it->second);
            cfg.note.tags.clear();
            size_t pos = 0;
            while (pos < s.size()) {
                auto next = s.find(',', pos);
                if (next == std::string::npos) next = s.size();
                auto tag = s.substr(pos, next - pos);
                if (!tag.empty()) cfg.note.tags.push_back(tag);
                pos = next + 1;
            }
        }
    }

    path("context_file", cfg.context_file);
    str("context_inline", cfg.context_inline);

    path("reprocess_dir", cfg.reprocess_dir);
    path("reprocess_batch_dir", cfg.reprocess_batch_dir);
    b("reprocess_batch_dry_run", cfg.reprocess_batch_dry_run);
    b("batch_mode", cfg.batch_mode);

    sz("staging_max_bytes", cfg.staging_max_bytes);

    {
        auto cit = m.find("servers.count");
        if (cit != m.end()) {
            int64_t n = json_val_as_int(cit->second, 0);
            if (n > 0) {
                cfg.servers.clear();
                cfg.servers.reserve(static_cast<size_t>(n));
                for (int64_t idx = 0; idx < n; ++idx) {
                    ServerEntry entry;
                    std::string ip = "servers." + std::to_string(idx) + ".";
                    auto nit = m.find(ip + "name");
                    auto ait = m.find(ip + "address");
                    if (nit != m.end()) entry.name = json_val_as_string(nit->second);
                    if (ait != m.end()) entry.address = json_val_as_string(ait->second);
                    cfg.servers.push_back(std::move(entry));
                }
            }
        }
    }

    b("enroll_mode", cfg.enroll_mode);
    str("enroll_name", cfg.enroll_name);

    return cfg;
}

} // namespace recmeet
