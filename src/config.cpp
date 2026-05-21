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

// Phase E.2(c) — parse a `[<section>] <list_key>:` YAML list of
// `{name, address}` maps. The flat parser above strips list dashes the
// wrong way (turning `- name: default` into key=`- name`); rather than
// extend the flat parser with a list-aware state machine, scan the raw
// text once for the matching `<section>:` → `<list_key>:` block and
// collect entries until the indent drops back to the section's level.
//
// This is intentionally narrow: only the single section→list_key pair
// the caller passes is parsed, the indent must be a strict descent, and
// only `name`/`address` keys are recognized per entry. Anything else is
// ignored (forward-compat with future schema growth). Empty list when no
// match is found.
std::vector<ServerEntry> parse_yaml_servers(const std::string& text,
                                            const std::string& section,
                                            const std::string& list_key) {
    std::vector<ServerEntry> out;
    std::istringstream stream(text);
    std::string line;

    bool in_section = false;
    bool in_list = false;
    int section_indent = -1;
    int list_indent = -1;
    ServerEntry cur;
    bool cur_dirty = false;

    auto flush = [&]() {
        if (cur_dirty && (!cur.name.empty() || !cur.address.empty())) {
            out.push_back(cur);
        }
        cur = ServerEntry{};
        cur_dirty = false;
    };

    while (std::getline(stream, line)) {
        auto trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        int indent = 0;
        while (indent < (int)line.size() && line[indent] == ' ') indent++;

        // Track top-level section entry/exit.
        if (indent == 0) {
            // Leaving any section we were in.
            flush();
            in_list = false;
            in_section = false;
            section_indent = -1;
            list_indent = -1;
            auto colon = trimmed.find(':');
            if (colon != std::string::npos) {
                std::string k = trim(trimmed.substr(0, colon));
                if (k == section) {
                    in_section = true;
                    section_indent = indent;
                }
            }
            continue;
        }

        if (!in_section) continue;

        // Inside the section. Look for the list_key header or a peer key
        // at the same indent level that would terminate the list.
        if (!in_list) {
            auto colon = trimmed.find(':');
            if (colon == std::string::npos) continue;
            std::string k = trim(trimmed.substr(0, colon));
            std::string v = trim(trimmed.substr(colon + 1));
            if (k == list_key && v.empty()) {
                in_list = true;
                list_indent = indent;
            }
            continue;
        }

        // In the list. The list ends when the indent drops back to the
        // section header indent or below (handled above for indent==0)
        // OR drops back to the list_key's indent with a non-dash key.
        if (indent <= list_indent && trimmed[0] != '-') {
            // Sibling key of `servers:` — list is done.
            flush();
            in_list = false;
            continue;
        }

        // List-item start: `- name: default` or `- address: ...`.
        if (trimmed[0] == '-') {
            // Begin a new entry. Flush the previous one if it had content.
            flush();
            // Strip the leading `- ` (or `-` plus optional spaces).
            std::string rest = trimmed.substr(1);
            // skip leading whitespace
            size_t p = rest.find_first_not_of(" \t");
            if (p == std::string::npos) {
                // bare dash on its own — start an empty entry; next line
                // continues it.
                cur_dirty = true;
                continue;
            }
            rest = rest.substr(p);
            auto colon = rest.find(':');
            if (colon == std::string::npos) continue;
            std::string k = trim(rest.substr(0, colon));
            std::string v = unquote(trim(rest.substr(colon + 1)));
            if (k == "name") cur.name = v;
            else if (k == "address") cur.address = v;
            cur_dirty = true;
            continue;
        }

        // Continuation line for the current entry: `  address: ...` at
        // an indent strictly greater than the list_key.
        auto colon = trimmed.find(':');
        if (colon == std::string::npos) continue;
        std::string k = trim(trimmed.substr(0, colon));
        std::string v = unquote(trim(trimmed.substr(colon + 1)));
        if (k == "name") cur.name = v;
        else if (k == "address") cur.address = v;
        cur_dirty = true;
    }

    flush();
    return out;
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

JobConfig load_legacy_config_as_job_config(const fs::path& config_path) {
    JobConfig cfg;
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
    std::string raw_text = buf.str();
    auto entries = parse_yaml(raw_text);

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
    // Phase E.2(a) — client-side preferred summarization style. Empty
    // default; daemon resolves the effective style from session.init.
    cfg.summary_style = get_val(entries, "summary", "style", "");

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
    // Phase B (diarize-overcount): max_auto_speakers cap + collapse_threshold
    // floor. Both live in the same [diarization] section.
    std::string mas = get_val(entries, "diarization", "max_auto_speakers", "");
    if (!mas.empty()) cfg.max_auto_speakers = std::atoi(mas.c_str());
    std::string colt = get_val(entries, "diarization", "collapse_threshold", "");
    if (!colt.empty()) cfg.collapse_threshold = std::atof(colt.c_str());

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
        //
        // C.13 (M-1): explicit `retain_terminal_hours` overrides this legacy
        // field — see the derive block below. The parse stays so legacy
        // configs that set only the old key still work, and so a config
        // that sets BOTH still has a meaningful "the unified knob won"
        // log line at the override site.
        std::string dct = get_val(entries, "server",
                                  "diarization_cache_ttl_secs", "");
        bool legacy_dct_set = !dct.empty();
        if (legacy_dct_set) {
            long long v = std::atoll(dct.c_str());
            if (v >= 0) cfg.diarization_cache_ttl_secs = static_cast<int64_t>(v);
            else {
                log_warn("config: invalid [server] diarization_cache_ttl_secs=%s; "
                         "keeping default %lld", dct.c_str(),
                         (long long)cfg.diarization_cache_ttl_secs);
                legacy_dct_set = false;  // invalid input: defer to unified knob default
            }
        }

        // Phase C.13 (M-1) — consolidated `[server] retain_terminal_hours`.
        // Default 24 h. When explicitly set in YAML it overrides the legacy
        // `diarization_cache_ttl_secs` value AND drives the SessionManager
        // TTL (read by daemon main() as `retain_terminal_hours * 3600`).
        // Precedence:
        //   1. retain_terminal_hours present + valid → unified knob wins.
        //   2. retain_terminal_hours absent, legacy dct set → legacy + derive
        //      retain_terminal_hours from it for the SessionManager TTL.
        //   3. Both absent → default 24 h.
        std::string rth = get_val(entries, "server",
                                  "retain_terminal_hours", "");
        if (!rth.empty()) {
            long long v = std::atoll(rth.c_str());
            if (v >= 0) {
                cfg.retain_terminal_hours = static_cast<int>(v);
                // Override the legacy field — the unified knob is now
                // authoritative. Log only when the operator actually set
                // both, so a "default + legacy" combo stays silent.
                int64_t derived = static_cast<int64_t>(v) * 3600;
                if (legacy_dct_set && cfg.diarization_cache_ttl_secs != derived) {
                    log_warn("config: [server] retain_terminal_hours=%lld overrides "
                             "[server] diarization_cache_ttl_secs=%lld "
                             "(unified knob wins per C.13 M-1)",
                             (long long)v,
                             (long long)cfg.diarization_cache_ttl_secs);
                }
                cfg.diarization_cache_ttl_secs = derived;
            } else {
                log_warn("config: invalid [server] retain_terminal_hours=%s; "
                         "keeping default %d", rth.c_str(),
                         cfg.retain_terminal_hours);
            }
        } else if (legacy_dct_set) {
            // Legacy-only config: derive retain_terminal_hours from the
            // legacy field so the SessionManager TTL stays coupled.
            // Integer division — round to the nearest hour; sub-hour TTLs
            // are not an operator-facing scenario.
            cfg.retain_terminal_hours = static_cast<int>(
                cfg.diarization_cache_ttl_secs / 3600);
            if (cfg.retain_terminal_hours <= 0)
                cfg.retain_terminal_hours = 24;
        }
    }

    // Phase D.6 — `[client] staging_max_bytes`. Same warn-and-fallback
    // shape as the [ipc] / [server] knobs above. A zero/negative override
    // is treated as a typo and falls back to the 500 GiB default rather
    // than disabling the cap (an unbounded staging dir would lose
    // operator data through the eviction sweep — conservative default).
    {
        std::string smb = get_val(entries, "client", "staging_max_bytes", "");
        if (!smb.empty()) {
            long long v = std::atoll(smb.c_str());
            if (v > 0) cfg.staging_max_bytes = static_cast<size_t>(v);
            else
                log_warn("config: invalid [client] staging_max_bytes=%s; "
                         "keeping default %zu", smb.c_str(),
                         cfg.staging_max_bytes);
        }
    }

    // Phase E.2(b) — `[client] caption_latency_ms`. Range [200, 2000]
    // matches the session.init / session.update_prefs handler guards
    // (see ipc_server.cpp). Out-of-range values warn-and-fall-back to the
    // struct default rather than silent-clamping: an operator typo should
    // surface in logs, not get massaged into silence.
    {
        std::string clm = get_val(entries, "client", "caption_latency_ms", "");
        if (!clm.empty()) {
            long long v = std::atoll(clm.c_str());
            if (v >= 200 && v <= 2000) {
                cfg.caption_latency_ms = static_cast<int>(v);
            } else {
                log_warn("config: invalid [client] caption_latency_ms=%s "
                         "(must be in [200, 2000]); keeping default %d",
                         clm.c_str(), cfg.caption_latency_ms);
            }
        }
    }

    // Phase E.2(c) — `[client] servers:` list. Uses the dedicated list
    // parser since the flat-key parser cannot represent `{name, address}`
    // map entries. v1 honors `servers[0]` only; the plural shape is
    // preserved-not-precluded for the multi-server hook.
    cfg.servers = parse_yaml_servers(raw_text, "client", "servers");

    return cfg;
}

void save_legacy_config_as_job_config(const JobConfig& cfg, const fs::path& config_path) {
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
    // Phase E.2(a) — emit `style:` only when non-empty so the generated
    // YAML stays compact for the common case where the client has no
    // preference and the daemon resolves the default.
    if (!cfg.summary_style.empty())
        out << "  style: " << cfg.summary_style << "\n";

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
        cfg.stitch_threshold != 0.6f ||
        cfg.max_auto_speakers != 8 || cfg.collapse_threshold != 0.55f) {
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
        if (cfg.max_auto_speakers != 8)
            out << "  max_auto_speakers: " << cfg.max_auto_speakers << "\n";
        if (cfg.collapse_threshold != 0.55f)
            out << "  collapse_threshold: " << cfg.collapse_threshold << "\n";
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

    // Phase D.6 — `[client]` section. New YAML namespace introduced for the
    // first client-side scoped knob (`staging_max_bytes`). Phase E.2 added
    // `caption_latency_ms` and a `servers:` list to this section. Emit the
    // section header once when ANY client-scoped knob diverges from its
    // default, then emit only the diverging knobs to keep the YAML compact.
    // Phase E.2's Wave 2.2 moves this section into a separate client.yaml
    // file.
    {
        constexpr size_t default_staging_max_bytes =
            static_cast<size_t>(500) * 1024 * 1024 * 1024;
        bool emit_client =
            cfg.staging_max_bytes != default_staging_max_bytes
            || cfg.caption_latency_ms != 500
            || !cfg.servers.empty();
        if (emit_client) {
            out << "\nclient:\n";
            if (cfg.staging_max_bytes != default_staging_max_bytes)
                out << "  staging_max_bytes: " << cfg.staging_max_bytes << "\n";
            if (cfg.caption_latency_ms != 500)
                out << "  caption_latency_ms: " << cfg.caption_latency_ms << "\n";
            // Phase E.2(c) — emit the servers list as a YAML list of
            // `{name, address}` maps. Empty vector emits no block at all.
            if (!cfg.servers.empty()) {
                out << "  servers:\n";
                for (const auto& srv : cfg.servers) {
                    out << "    - name: " << srv.name << "\n"
                        << "      address: \"" << srv.address << "\"\n";
                }
            }
        }
    }

    out.close();
    chmod(path.c_str(), 0600);
}

// ---------------------------------------------------------------------------
// Phase E.2 Wave 2.2a — ServerConfig / ClientConfig load+save+migrate
// ---------------------------------------------------------------------------
//
// The new split-file APIs live alongside the monolithic load_config /
// save_config so the existing 193 consumers compile unchanged. Wave 2.2b
// retypes consumers and removes the monolithic surface.

ServerConfig to_server_config(const JobConfig& cfg) {
    ServerConfig s;
    s.whisper_model              = cfg.whisper_model;
    s.llm_model                  = cfg.llm_model;
    s.llm_mmap                   = cfg.llm_mmap;
    s.captions_enabled           = cfg.captions_enabled;
    s.caption_model              = cfg.caption_model;
    s.provider                   = cfg.provider;
    s.api_url                    = cfg.api_url;
    s.api_key                    = cfg.api_key;
    s.api_model                  = cfg.api_model;
    s.api_keys                   = cfg.api_keys;
    s.diarize                    = cfg.diarize;
    s.num_speakers               = cfg.num_speakers;
    s.cluster_threshold          = cfg.cluster_threshold;
    s.chunk_minutes              = cfg.chunk_minutes;
    s.chunk_overlap_sec          = cfg.chunk_overlap_sec;
    s.stitch_threshold           = cfg.stitch_threshold;
    s.speaker_id                 = cfg.speaker_id;
    s.speaker_threshold          = cfg.speaker_threshold;
    s.speaker_db                 = cfg.speaker_db;
    s.vad                        = cfg.vad;
    s.vad_threshold              = cfg.vad_threshold;
    s.vad_min_silence            = cfg.vad_min_silence;
    s.vad_min_speech             = cfg.vad_min_speech;
    s.vad_max_speech             = cfg.vad_max_speech;
    s.threads                    = cfg.threads;
    s.log_level_str              = cfg.log_level_str;
    s.log_dir                    = cfg.log_dir;
    s.log_retention_hours        = cfg.log_retention_hours;
    s.web_port                   = cfg.web_port;
    s.web_bind                   = cfg.web_bind;
    s.max_message_bytes          = cfg.max_message_bytes;
    s.max_upload_bytes           = cfg.max_upload_bytes;
    s.max_clients                = cfg.max_clients;
    s.slot_postprocess           = cfg.slot_postprocess;
    s.slot_streaming             = cfg.slot_streaming;
    s.slot_model_download        = cfg.slot_model_download;
    s.allow_client_downloads     = cfg.allow_client_downloads;
    s.retain_terminal_hours      = cfg.retain_terminal_hours;
    s.diarization_cache_ttl_secs = cfg.diarization_cache_ttl_secs;
    // iter-172 plan addendum — lift the legacy `output_dir` (a JobConfig /
    // pre-split monolith field) into ServerConfig's new server-side slot.
    // The client's ClientConfig.output_dir continues to carry the client's
    // at-rest view; the daemon now consults `meetings_root` for "where do
    // meetings live on this host".
    s.meetings_root              = cfg.output_dir;
    return s;
}

ClientConfig to_client_config(const JobConfig& cfg) {
    ClientConfig c;
    c.device_pattern             = cfg.device_pattern;
    c.mic_source                 = cfg.mic_source;
    c.monitor_source             = cfg.monitor_source;
    c.mic_only                   = cfg.mic_only;
    c.keep_sources               = cfg.keep_sources;
    c.language                   = cfg.language;
    c.vocabulary                 = cfg.vocabulary;
    c.summary_style              = cfg.summary_style;
    c.no_summary                 = cfg.no_summary;
    c.provider                   = cfg.provider;
    c.api_url                    = cfg.api_url;
    c.api_key                    = cfg.api_key;
    c.api_model                  = cfg.api_model;
    c.api_keys                   = cfg.api_keys;
    c.llm_model                  = cfg.llm_model;
    c.llm_mmap                   = cfg.llm_mmap;
    c.caption_latency_ms         = cfg.caption_latency_ms;
    c.caption_normalize_display  = cfg.caption_normalize_display;
    c.output_dir                 = cfg.output_dir;
    c.output_dir_explicit        = cfg.output_dir_explicit;
    c.note_dir                   = cfg.note_dir;
    c.note                       = cfg.note;
    c.staging_max_bytes          = cfg.staging_max_bytes;
    c.servers                    = cfg.servers;
    // Per-job dynamic fields (context_file, context_inline, reprocess_dir,
    // reprocess_batch_dir, reprocess_batch_dry_run, batch_mode, enroll_mode,
    // enroll_name) are intentionally dropped on convert — they live on
    // JobConfig only (E.2(d.1), Wave 2.2b) and transit via session.init /
    // PostprocessInput, never via the at-rest ClientConfig.
    return c;
}

ServerConfig load_server_config(const fs::path& config_path) {
    ServerConfig cfg;
    fs::path path = config_path.empty() ? config_dir() / "daemon.yaml" : config_path;

    // DUAL fallback: env-provided API key seeds api_key the same way
    // the monolithic load_config does.
    for (size_t i = 0; i < NUM_PROVIDERS; ++i) {
        if (const char* key = std::getenv(PROVIDERS[i].env_var)) {
            cfg.api_key = key;
            break;
        }
    }

    if (!fs::exists(path)) return cfg;

    std::ifstream in(path);
    if (!in) return cfg;
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string raw = buf.str();
    auto entries = parse_yaml(raw);

    // Transcription — server picks the model.
    cfg.whisper_model = get_val(entries, "transcription", "model", cfg.whisper_model);

    // Summary — DUAL fallback subset on the daemon side.
    cfg.provider  = get_val(entries, "summary", "provider", cfg.provider);
    cfg.api_url   = get_val(entries, "summary", "api_url", cfg.api_url);
    std::string fk = get_val(entries, "summary", "api_key", "");
    if (!fk.empty()) cfg.api_key = fk;
    cfg.api_model = get_val(entries, "summary", "model", cfg.api_model);
    cfg.llm_model = get_val(entries, "summary", "llm_model", "");
    cfg.llm_mmap  = get_bool(entries, "summary", "llm_mmap", false);

    // Per-provider API keys.
    for (size_t i = 0; i < NUM_PROVIDERS; ++i) {
        std::string k = get_val(entries, "api_keys", PROVIDERS[i].name, "");
        if (!k.empty()) cfg.api_keys[PROVIDERS[i].name] = k;
    }

    // Diarization.
    cfg.diarize = get_bool(entries, "diarization", "enabled", true);
    std::string ns = get_val(entries, "diarization", "num_speakers", "0");
    cfg.num_speakers = std::atoi(ns.c_str());
    std::string ct = get_val(entries, "diarization", "cluster_threshold", "");
    if (!ct.empty()) cfg.cluster_threshold = std::atof(ct.c_str());
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
                     "Falling back to defaults (15.0, 30.0).", cm, co);
            if (!st_s.empty()) cfg.stitch_threshold = st_thr;
        } else {
            cfg.chunk_minutes = cm;
            cfg.chunk_overlap_sec = co;
            cfg.stitch_threshold = st_thr;
        }
    }

    // Speaker identification.
    cfg.speaker_id = get_bool(entries, "speaker_id", "enabled", true);
    std::string st = get_val(entries, "speaker_id", "threshold", "");
    if (!st.empty()) cfg.speaker_threshold = std::atof(st.c_str());
    std::string sdb = get_val(entries, "speaker_id", "database", "");
    if (!sdb.empty()) cfg.speaker_db = sdb;

    // VAD.
    cfg.vad = get_bool(entries, "vad", "enabled", true);
    std::string vt = get_val(entries, "vad", "threshold", "");
    if (!vt.empty()) cfg.vad_threshold = std::atof(vt.c_str());
    std::string vms = get_val(entries, "vad", "min_silence", "");
    if (!vms.empty()) cfg.vad_min_silence = std::atof(vms.c_str());
    std::string vmsp = get_val(entries, "vad", "min_speech", "");
    if (!vmsp.empty()) cfg.vad_min_speech = std::atof(vmsp.c_str());
    std::string vmxs = get_val(entries, "vad", "max_speech", "");
    if (!vmxs.empty()) cfg.vad_max_speech = std::atof(vmxs.c_str());

    // Captions (server side — engine + model only; normalize_display is
    // a client-render knob and stays in client.yaml).
    cfg.captions_enabled = get_bool(entries, "captions", "enabled", false);
    cfg.caption_model = get_val(entries, "captions", "model", "");

    // General.
    std::string threads_str = get_val(entries, "general", "threads", "0");
    cfg.threads = std::atoi(threads_str.c_str());

    // Logging.
    cfg.log_level_str = get_val(entries, "logging", "level", "error");
    std::string log_dir_val = get_val(entries, "logging", "directory", "");
    if (!log_dir_val.empty()) cfg.log_dir = log_dir_val;
    std::string retention_val = get_val(entries, "logging", "retention_hours", "4");
    if (!retention_val.empty()) cfg.log_retention_hours = std::stoi(retention_val);
    if (const char* env_level = std::getenv("RECMEET_LOG_LEVEL"))
        cfg.log_level_str = env_level;

    // Web server.
    std::string port_str = get_val(entries, "web", "port", "8384");
    cfg.web_port = std::atoi(port_str.c_str());
    cfg.web_bind = get_val(entries, "web", "bind", "127.0.0.1");

    // IPC limits.
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

        std::string dct = get_val(entries, "server",
                                  "diarization_cache_ttl_secs", "");
        bool legacy_dct_set = !dct.empty();
        if (legacy_dct_set) {
            long long v = std::atoll(dct.c_str());
            if (v >= 0) cfg.diarization_cache_ttl_secs = static_cast<int64_t>(v);
            else {
                log_warn("config: invalid [server] diarization_cache_ttl_secs=%s; "
                         "keeping default %lld", dct.c_str(),
                         (long long)cfg.diarization_cache_ttl_secs);
                legacy_dct_set = false;
            }
        }

        std::string rth = get_val(entries, "server", "retain_terminal_hours", "");
        if (!rth.empty()) {
            long long v = std::atoll(rth.c_str());
            if (v >= 0) {
                cfg.retain_terminal_hours = static_cast<int>(v);
                int64_t derived = static_cast<int64_t>(v) * 3600;
                if (legacy_dct_set && cfg.diarization_cache_ttl_secs != derived) {
                    log_warn("config: [server] retain_terminal_hours=%lld overrides "
                             "[server] diarization_cache_ttl_secs=%lld "
                             "(unified knob wins per C.13 M-1)",
                             (long long)v,
                             (long long)cfg.diarization_cache_ttl_secs);
                }
                cfg.diarization_cache_ttl_secs = derived;
            } else {
                log_warn("config: invalid [server] retain_terminal_hours=%s; "
                         "keeping default %d", rth.c_str(),
                         cfg.retain_terminal_hours);
            }
        } else if (legacy_dct_set) {
            cfg.retain_terminal_hours = static_cast<int>(
                cfg.diarization_cache_ttl_secs / 3600);
            if (cfg.retain_terminal_hours <= 0)
                cfg.retain_terminal_hours = 24;
        }

        // iter-172 — meetings_root: the daemon's on-disk meeting-tree root.
        // Default is "./meetings" (the struct default); operator may override
        // via `[server] meetings_root`. The legacy single-file migration shim
        // (to_server_config) lifts the pre-split `output_dir` into this slot.
        std::string mr = get_val(entries, "server", "meetings_root", "");
        if (!mr.empty()) cfg.meetings_root = mr;
    }

    return cfg;
}

ClientConfig load_client_config(const fs::path& config_path) {
    ClientConfig cfg;
    fs::path path = config_path.empty() ? config_dir() / "client.yaml" : config_path;

    // DUAL primary: env-provided API key seeds api_key.
    for (size_t i = 0; i < NUM_PROVIDERS; ++i) {
        if (const char* key = std::getenv(PROVIDERS[i].env_var)) {
            cfg.api_key = key;
            break;
        }
    }

    if (!fs::exists(path)) return cfg;

    std::ifstream in(path);
    if (!in) return cfg;
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string raw = buf.str();
    auto entries = parse_yaml(raw);

    // Audio.
    cfg.device_pattern  = get_val(entries, "audio", "device_pattern", cfg.device_pattern);
    cfg.mic_source      = get_val(entries, "audio", "mic_source", "");
    cfg.monitor_source  = get_val(entries, "audio", "monitor_source", "");
    cfg.mic_only        = get_bool(entries, "audio", "mic_only", false);
    cfg.keep_sources    = get_bool(entries, "audio", "keep_sources", false);

    // Transcription (NO model — server picks).
    cfg.language   = get_val(entries, "transcription", "language", "");
    cfg.vocabulary = get_val(entries, "transcription", "vocabulary", "");

    // Summary — DUAL primary subset + E.2(a) style.
    cfg.provider  = get_val(entries, "summary", "provider", cfg.provider);
    cfg.api_url   = get_val(entries, "summary", "api_url", cfg.api_url);
    std::string fk = get_val(entries, "summary", "api_key", "");
    if (!fk.empty()) cfg.api_key = fk;
    cfg.api_model = get_val(entries, "summary", "model", cfg.api_model);
    cfg.no_summary = get_bool(entries, "summary", "disabled", false);
    cfg.llm_model = get_val(entries, "summary", "llm_model", "");
    cfg.llm_mmap  = get_bool(entries, "summary", "llm_mmap", false);
    cfg.summary_style = get_val(entries, "summary", "style", "");

    // Per-provider API keys.
    for (size_t i = 0; i < NUM_PROVIDERS; ++i) {
        std::string k = get_val(entries, "api_keys", PROVIDERS[i].name, "");
        if (!k.empty()) cfg.api_keys[PROVIDERS[i].name] = k;
    }

    // Output.
    std::string out = get_val(entries, "output", "directory", "");
    if (!out.empty()) cfg.output_dir = out;

    // Notes.
    std::string note_domain = get_val(entries, "notes", "domain", "");
    if (note_domain.empty())
        note_domain = get_val(entries, "obsidian", "domain", cfg.note.domain);
    cfg.note.domain = note_domain;
    std::string note_dir_val = get_val(entries, "notes", "directory", "");
    if (!note_dir_val.empty()) cfg.note_dir = note_dir_val;

    // Captions (client-side render knob).
    cfg.caption_normalize_display =
        get_bool(entries, "captions", "normalize_display", true);

    // [client] section — staging budget + caption latency + servers.
    {
        std::string smb = get_val(entries, "client", "staging_max_bytes", "");
        if (!smb.empty()) {
            long long v = std::atoll(smb.c_str());
            if (v > 0) cfg.staging_max_bytes = static_cast<size_t>(v);
            else
                log_warn("config: invalid [client] staging_max_bytes=%s; "
                         "keeping default %zu", smb.c_str(),
                         cfg.staging_max_bytes);
        }
    }
    {
        std::string clm = get_val(entries, "client", "caption_latency_ms", "");
        if (!clm.empty()) {
            long long v = std::atoll(clm.c_str());
            if (v >= 200 && v <= 2000) {
                cfg.caption_latency_ms = static_cast<int>(v);
            } else {
                log_warn("config: invalid [client] caption_latency_ms=%s "
                         "(must be in [200, 2000]); keeping default %d",
                         clm.c_str(), cfg.caption_latency_ms);
            }
        }
    }
    cfg.servers = parse_yaml_servers(raw, "client", "servers");

    return cfg;
}

void save_server_config(const ServerConfig& cfg, const fs::path& config_path) {
    fs::path path;
    if (config_path.empty()) {
        fs::path dir = config_dir();
        fs::create_directories(dir);
        path = dir / "daemon.yaml";
    } else {
        fs::create_directories(config_path.parent_path());
        path = config_path;
    }

    std::ofstream out(path);
    if (!out)
        throw RecmeetError("Cannot write daemon.yaml: " + path.string());

    out << "# recmeet daemon configuration\n\n"
        << "transcription:\n"
        << "  model: " << cfg.whisper_model << "\n";

    out << "\nsummary:\n"
        << "  provider: " << cfg.provider << "\n";
    if (!cfg.api_url.empty())
        out << "  api_url: \"" << cfg.api_url << "\"\n";
    out << "  model: " << cfg.api_model << "\n";
    if (!cfg.llm_model.empty())
        out << "  llm_model: \"" << cfg.llm_model << "\"\n";
    if (cfg.llm_mmap)
        out << "  llm_mmap: true\n";

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
        if (!cfg.diarize) out << "  enabled: false\n";
        if (cfg.num_speakers > 0) out << "  num_speakers: " << cfg.num_speakers << "\n";
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
        if (!cfg.speaker_id) out << "  enabled: false\n";
        if (cfg.speaker_threshold != 0.6f)
            out << "  threshold: " << cfg.speaker_threshold << "\n";
        if (!cfg.speaker_db.empty())
            out << "  database: \"" << cfg.speaker_db.string() << "\"\n";
    }

    if (!cfg.vad || cfg.vad_threshold != 0.5f || cfg.vad_min_silence != 0.5f ||
        cfg.vad_min_speech != 0.25f || cfg.vad_max_speech != 30.0f) {
        out << "\nvad:\n";
        if (!cfg.vad) out << "  enabled: false\n";
        if (cfg.vad_threshold != 0.5f) out << "  threshold: " << cfg.vad_threshold << "\n";
        if (cfg.vad_min_silence != 0.5f)
            out << "  min_silence: " << cfg.vad_min_silence << "\n";
        if (cfg.vad_min_speech != 0.25f)
            out << "  min_speech: " << cfg.vad_min_speech << "\n";
        if (cfg.vad_max_speech != 30.0f)
            out << "  max_speech: " << cfg.vad_max_speech << "\n";
    }

    // Captions (server side: enabled + model only).
    if (cfg.captions_enabled || !cfg.caption_model.empty()) {
        out << "\ncaptions:\n";
        if (cfg.captions_enabled) out << "  enabled: true\n";
        if (!cfg.caption_model.empty())
            out << "  model: " << cfg.caption_model << "\n";
    }

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

    if (cfg.web_port != 8384 || cfg.web_bind != "127.0.0.1") {
        out << "\nweb:\n";
        if (cfg.web_port != 8384) out << "  port: " << cfg.web_port << "\n";
        if (cfg.web_bind != "127.0.0.1")
            out << "  bind: \"" << cfg.web_bind << "\"\n";
    }

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
        bool emit_server_section =
            cfg.max_upload_bytes != 4ull * 1024 * 1024 * 1024 ||
            cfg.slot_postprocess != 1 || cfg.slot_streaming != 1 ||
            cfg.slot_model_download != 1 || !cfg.allow_client_downloads ||
            cfg.retain_terminal_hours != 24 ||
            cfg.diarization_cache_ttl_secs != 86400 ||
            cfg.meetings_root != fs::path("./meetings");
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
            if (cfg.retain_terminal_hours != 24)
                out << "  retain_terminal_hours: " << cfg.retain_terminal_hours << "\n";
            // Emit the legacy cache TTL only when it has diverged from the
            // derived `retain_terminal_hours * 3600` value (i.e. an operator
            // explicitly set the legacy knob without setting the unified one).
            if (cfg.diarization_cache_ttl_secs !=
                static_cast<int64_t>(cfg.retain_terminal_hours) * 3600 &&
                cfg.diarization_cache_ttl_secs != 86400)
                out << "  diarization_cache_ttl_secs: "
                    << cfg.diarization_cache_ttl_secs << "\n";
            // iter-172 — meetings_root: the daemon's view of the on-disk
            // meeting tree (the migration shim populates it from legacy
            // `output_dir`).
            if (cfg.meetings_root != fs::path("./meetings"))
                out << "  meetings_root: \"" << cfg.meetings_root.string() << "\"\n";
        }
    }

    out.close();
    chmod(path.c_str(), 0600);
}

void save_client_config(const ClientConfig& cfg, const fs::path& config_path) {
    fs::path path;
    if (config_path.empty()) {
        fs::path dir = config_dir();
        fs::create_directories(dir);
        path = dir / "client.yaml";
    } else {
        fs::create_directories(config_path.parent_path());
        path = config_path;
    }

    std::ofstream out(path);
    if (!out)
        throw RecmeetError("Cannot write client.yaml: " + path.string());

    out << "# recmeet client configuration\n\n"
        << "audio:\n"
        << "  device_pattern: \"" << cfg.device_pattern << "\"\n";
    if (!cfg.mic_source.empty())
        out << "  mic_source: \"" << cfg.mic_source << "\"\n";
    if (!cfg.monitor_source.empty())
        out << "  monitor_source: \"" << cfg.monitor_source << "\"\n";
    if (cfg.mic_only) out << "  mic_only: true\n";
    if (cfg.keep_sources) out << "  keep_sources: true\n";

    if (!cfg.language.empty() || !cfg.vocabulary.empty()) {
        out << "\ntranscription:\n";
        if (!cfg.language.empty())
            out << "  language: " << cfg.language << "\n";
        if (!cfg.vocabulary.empty())
            out << "  vocabulary: \"" << cfg.vocabulary << "\"\n";
    }

    out << "\nsummary:\n"
        << "  provider: " << cfg.provider << "\n";
    if (!cfg.api_url.empty())
        out << "  api_url: \"" << cfg.api_url << "\"\n";
    out << "  model: " << cfg.api_model << "\n";
    if (cfg.no_summary) out << "  disabled: true\n";
    if (!cfg.llm_model.empty())
        out << "  llm_model: \"" << cfg.llm_model << "\"\n";
    if (cfg.llm_mmap) out << "  llm_mmap: true\n";
    if (!cfg.summary_style.empty())
        out << "  style: " << cfg.summary_style << "\n";

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

    out << "\noutput:\n"
        << "  directory: \"" << cfg.output_dir.string() << "\"\n";

    out << "\nnotes:\n"
        << "  domain: " << cfg.note.domain << "\n";
    if (!cfg.note_dir.empty())
        out << "  directory: \"" << cfg.note_dir.string() << "\"\n";

    if (!cfg.caption_normalize_display) {
        out << "\ncaptions:\n"
            << "  normalize_display: false\n";
    }

    {
        constexpr size_t default_staging_max_bytes =
            static_cast<size_t>(500) * 1024 * 1024 * 1024;
        bool emit_client =
            cfg.staging_max_bytes != default_staging_max_bytes
            || cfg.caption_latency_ms != 500
            || !cfg.servers.empty();
        if (emit_client) {
            out << "\nclient:\n";
            if (cfg.staging_max_bytes != default_staging_max_bytes)
                out << "  staging_max_bytes: " << cfg.staging_max_bytes << "\n";
            if (cfg.caption_latency_ms != 500)
                out << "  caption_latency_ms: " << cfg.caption_latency_ms << "\n";
            if (!cfg.servers.empty()) {
                out << "  servers:\n";
                for (const auto& srv : cfg.servers) {
                    out << "    - name: " << srv.name << "\n"
                        << "      address: \"" << srv.address << "\"\n";
                }
            }
        }
    }

    out.close();
    chmod(path.c_str(), 0600);
}

void migrate_legacy_config_if_present(const fs::path& cfg_dir_in) {
    fs::path cfg_dir = cfg_dir_in.empty() ? config_dir() : cfg_dir_in;
    fs::path old_path    = cfg_dir / "config.yaml";
    fs::path daemon_path = cfg_dir / "daemon.yaml";
    fs::path client_path = cfg_dir / "client.yaml";
    fs::path backup_path = cfg_dir / "config.yaml.v1-backup";

    if (!fs::exists(old_path))
        return;  // nothing to migrate

    if (fs::exists(daemon_path) || fs::exists(client_path)) {
        log_info("config: migration skip — split files already exist");
        return;
    }

    // Load the monolithic legacy file, split, then write both halves.
    JobConfig legacy = load_legacy_config_as_job_config(old_path);
    ServerConfig srv = to_server_config(legacy);
    ClientConfig cli = to_client_config(legacy);

    save_server_config(srv, daemon_path);
    save_client_config(cli, client_path);

    // Atomic rename of legacy file to backup. fs::rename is atomic on the
    // same filesystem (config_dir is always a single FS in practice).
    fs::rename(old_path, backup_path);

    // chmod the new files explicitly (save_*_config already did so, but the
    // rename above does not change perms — preserve the security posture).
    chmod(daemon_path.c_str(), 0600);
    chmod(client_path.c_str(), 0600);

    log_info("config: migrated config.yaml → daemon.yaml + client.yaml "
             "(legacy preserved as config.yaml.v1-backup)");
}

} // namespace recmeet
