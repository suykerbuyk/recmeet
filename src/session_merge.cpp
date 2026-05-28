// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "session_merge.h"

#include <cstdlib>

namespace recmeet {

namespace {

// Map provider name → env var name. Matches the `PROVIDERS[]` table in
// config.cpp; kept local to this TU so a future provider addition only
// requires one update site here.
const char* provider_env_var(const std::string& provider) {
    if (provider == "xai")       return "XAI_API_KEY";
    if (provider == "openai")    return "OPENAI_API_KEY";
    if (provider == "anthropic") return "ANTHROPIC_API_KEY";
    return nullptr;
}

} // anonymous namespace

JobConfig make_job_config(
    const ServerConfig& srv,
    const SessionCredentials& session_creds,
    const SessionPreferences& session_prefs,
    const PostprocessInput& input,
    const std::function<std::string(const std::string&)>& env_lookup)
{
    // -----------------------------------------------------------------
    // (1) Base layer — copy every field that exists on BOTH
    // ServerConfig and JobConfig from `srv`. The remaining JobConfig
    // fields (no_summary, summary_style, caption_latency_ms,
    // caption_normalize_display, device_pattern, mic_*, monitor_*, audio
    // flags, output_dir_explicit, language, vocabulary, note_*, context,
    // reprocess, batch flags) keep their JobConfig struct defaults
    // unless an explicit overlay fires below.
    // -----------------------------------------------------------------
    JobConfig cfg{};

    // Models (server-resident engines)
    cfg.whisper_model              = srv.whisper_model;
    cfg.llm_model                  = srv.llm_model;
    cfg.llm_mmap                   = srv.llm_mmap;

    // Captions (server runs CaptionEngine)
    cfg.captions_enabled           = srv.captions_enabled;
    cfg.caption_model              = srv.caption_model;

    // Dual-resident — `srv` is the FALLBACK rung; session + env overlay below.
    cfg.provider                   = srv.provider;
    cfg.api_url                    = srv.api_url;
    cfg.api_key                    = srv.api_key;
    cfg.api_model                  = srv.api_model;
    cfg.api_keys                   = srv.api_keys;

    // Diarization
    cfg.diarize                    = srv.diarize;
    cfg.num_speakers               = srv.num_speakers;
    cfg.cluster_threshold          = srv.cluster_threshold;
    cfg.chunk_minutes              = srv.chunk_minutes;
    cfg.chunk_overlap_sec          = srv.chunk_overlap_sec;
    cfg.stitch_threshold           = srv.stitch_threshold;

    // Speaker DB
    cfg.speaker_id                 = srv.speaker_id;
    cfg.speaker_threshold          = srv.speaker_threshold;
    cfg.speaker_db                 = srv.speaker_db;

    // VAD
    cfg.vad                        = srv.vad;
    cfg.vad_threshold              = srv.vad_threshold;
    cfg.vad_min_silence            = srv.vad_min_silence;
    cfg.vad_min_speech             = srv.vad_min_speech;
    cfg.vad_max_speech             = srv.vad_max_speech;

    // Performance
    cfg.threads                    = srv.threads;

    // Logging
    cfg.log_level_str              = srv.log_level_str;
    cfg.log_dir                    = srv.log_dir;
    cfg.log_retention_hours        = srv.log_retention_hours;

    // Web server (legacy fields; the standalone web binary was folded
    // into recmeet-client in Phase E.6.2. Kept on the JobConfig wire shape
    // for subprocess-format stability; tray no longer reads them.)
    cfg.web_port                   = srv.web_port;
    cfg.web_bind                   = srv.web_bind;

    // IPC limits
    cfg.max_message_bytes          = srv.max_message_bytes;
    cfg.max_upload_bytes           = srv.max_upload_bytes;
    cfg.max_clients                = srv.max_clients;

    // JobQueue slot capacities
    cfg.slot_postprocess           = srv.slot_postprocess;
    cfg.slot_streaming             = srv.slot_streaming;
    cfg.slot_model_download        = srv.slot_model_download;

    // Downloads policy
    cfg.allow_client_downloads     = srv.allow_client_downloads;

    // Retention (consolidated terminal-state knob)
    cfg.retain_terminal_hours      = srv.retain_terminal_hours;
    cfg.diarization_cache_ttl_secs = srv.diarization_cache_ttl_secs;

    // Output — daemon's view of meetings root is the JobConfig.output_dir
    // initial value (per iter-172 plan addendum on meetings_root).
    cfg.output_dir                 = srv.meetings_root;

    // -----------------------------------------------------------------
    // (2) Per-job dynamics from PostprocessInput — these are the
    // W2.2b (E.2 d.1) fields that moved off ClientConfig because they
    // are per-call, not persisted to client.yaml.
    // -----------------------------------------------------------------
    if (!input.reprocess_dir.empty())  cfg.reprocess_dir  = input.reprocess_dir;
    if (!input.enroll_name.empty())    cfg.enroll_name    = input.enroll_name;
    if (!input.context_inline.empty()) cfg.context_inline = input.context_inline;
    cfg.enroll_mode = input.enroll_mode;  // bool — always copied

    // -----------------------------------------------------------------
    // (3) Session credentials overlay — session wins over `srv` for
    // the dual-resident fields.
    // -----------------------------------------------------------------

    // Provider selection — session wins over daemon.yaml when set.
    if (!session_creds.provider.empty())
        cfg.provider = session_creds.provider;

    // Per-provider api_keys map — session overlays daemon.yaml. We
    // merge entry-by-entry so a session that only supplies one
    // provider's key does not wipe the others from daemon.yaml.
    for (const auto& [k, v] : session_creds.api_keys) {
        if (!v.empty()) cfg.api_keys[k] = v;
    }

    // Legacy single api_key field — session > daemon.yaml.
    if (!session_creds.api_key.empty())
        cfg.api_key = session_creds.api_key;

    // -----------------------------------------------------------------
    // (4) Env-var override — highest precedence. Once we know which
    // provider this job uses (cfg.provider, possibly already overlaid
    // from session above), check the matching env var. A non-empty env
    // value wins over both session and daemon.yaml.
    //
    // The env var is the operator override on the daemon host — they
    // can rotate a key by restarting the daemon with a new env value
    // without touching client config or daemon.yaml.
    // -----------------------------------------------------------------
    const char* env_name = provider_env_var(cfg.provider);
    if (env_name) {
        std::string env_val = env_lookup(env_name);
        if (!env_val.empty()) {
            cfg.api_key = env_val;
            cfg.api_keys[cfg.provider] = env_val;
        }
    }

    // -----------------------------------------------------------------
    // (5) Session preferences — overlay onto the daemon.yaml defaults.
    // Empty string at the session layer means "leave the daemon.yaml
    // value alone"; non-empty overlays. The booleans always replace
    // (no "unset" sentinel — the handler validates them).
    // -----------------------------------------------------------------
    if (!session_prefs.output_dir.empty())     cfg.output_dir   = session_prefs.output_dir;
    if (!session_prefs.note_dir.empty())       cfg.note_dir     = session_prefs.note_dir;
    if (!session_prefs.language.empty())       cfg.language     = session_prefs.language;
    if (!session_prefs.vocabulary.empty())     cfg.vocabulary   = session_prefs.vocabulary;
    if (!session_prefs.mic_source.empty())     cfg.mic_source   = session_prefs.mic_source;
    if (!session_prefs.monitor_source.empty()) cfg.monitor_source = session_prefs.monitor_source;
    if (!session_prefs.whisper_model.empty())  cfg.whisper_model = session_prefs.whisper_model;

    // v2-coexistence-with-v1 Phase 2B — diarize/vad overlay. Bool fields
    // use std::optional<bool> rather than empty-string sentinels (no natural
    // bool sentinel — false is a valid value). has_value() means "the
    // session.init / update_prefs handler observed the key on the wire."
    if (session_prefs.diarize.has_value())  cfg.diarize = *session_prefs.diarize;
    if (session_prefs.vad.has_value())      cfg.vad     = *session_prefs.vad;

    // summarization_backend: explicit selector — `local` means use the
    // local llama path, `http` means HTTP API. Empty leaves whatever
    // daemon.yaml + session llm_model produced.
    if (session_prefs.summarization_backend == "local") {
        if (!session_prefs.llm_model.empty())
            cfg.llm_model = session_prefs.llm_model;
        // else keep whatever was already in cfg.llm_model
    } else if (session_prefs.summarization_backend == "http") {
        // Force HTTP by clearing any local model path the daemon.yaml
        // (or session) may have set. This is the "the subprocess does
        // not have to infer from llm_model presence" path from the plan.
        cfg.llm_model.clear();
    } else {
        // Backend not specified — still let the session's llm_model
        // override if it was provided (matches the daemon.yaml-style
        // "presence implies local").
        if (!session_prefs.llm_model.empty())
            cfg.llm_model = session_prefs.llm_model;
    }

    // caption_latency_ms is the only captions-related field that flows
    // from session prefs into the merged JobConfig (Phase E.2(b)). The
    // value propagates into the JSON config the streaming CaptionEngine
    // reads. The session.init / session.update_prefs handlers enforce the
    // [200, 2000] range upstream; this assignment does not re-validate.
    //
    // Phase C (rev 5) — `cfg.captions_enabled` is no longer overlaid from
    // session prefs: under v2 always-stream the server alone owns the
    // captions toggle (`ServerConfig::captions_enabled`, AND'd with
    // runtime capability at daemon startup). The base-layer assignment
    // at line 48 (`cfg.captions_enabled = srv.captions_enabled;`) is the
    // sole site that sets the JobConfig captions toggle, and after Phase
    // A1.4 it carries the runtime-effective value.
    cfg.caption_latency_ms  = session_prefs.caption_latency_ms;

    return cfg;
}

JobConfig make_job_config_with_real_env(
    const ServerConfig& srv,
    const SessionCredentials& session_creds,
    const SessionPreferences& session_prefs,
    const PostprocessInput& input)
{
    auto getenv_wrapper = [](const std::string& name) -> std::string {
        const char* v = std::getenv(name.c_str());
        return v ? std::string(v) : std::string();
    };
    return make_job_config(srv, session_creds, session_prefs, input,
                           getenv_wrapper);
}

} // namespace recmeet
