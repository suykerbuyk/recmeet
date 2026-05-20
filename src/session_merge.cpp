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

JobConfig merge_creds_for_job(
    const JobConfig& daemon_config,
    const SessionCredentials& session_creds,
    const SessionPreferences& session_prefs,
    const std::function<std::string(const std::string&)>& env_lookup)
{
    // Start from the daemon.yaml snapshot — this gives us provider /
    // model / DB paths / VAD / diarization knobs that operators configure
    // once at install time. The session and env layers overlay only the
    // fields they explicitly carry.
    JobConfig cfg = daemon_config;

    // ---------------------------------------------------------------
    // Provider selection — session wins over daemon.yaml when set.
    // ---------------------------------------------------------------
    if (!session_creds.provider.empty())
        cfg.provider = session_creds.provider;

    // ---------------------------------------------------------------
    // Per-provider api_keys map — session overlays daemon.yaml. We
    // merge entry-by-entry so a session that only supplies one
    // provider's key does not wipe the others from daemon.yaml.
    // ---------------------------------------------------------------
    for (const auto& [k, v] : session_creds.api_keys) {
        if (!v.empty()) cfg.api_keys[k] = v;
    }

    // ---------------------------------------------------------------
    // Legacy single api_key field — session > daemon.yaml.
    // ---------------------------------------------------------------
    if (!session_creds.api_key.empty())
        cfg.api_key = session_creds.api_key;

    // ---------------------------------------------------------------
    // Env-var override — highest precedence. Once we know which
    // provider this job uses (cfg.provider, possibly already overlaid
    // from session above), check the matching env var. A non-empty env
    // value wins over both session and daemon.yaml.
    //
    // The env var is the operator override on the daemon host — they
    // can rotate a key by restarting the daemon with a new env value
    // without touching client config or daemon.yaml.
    // ---------------------------------------------------------------
    const char* env_name = provider_env_var(cfg.provider);
    if (env_name) {
        std::string env_val = env_lookup(env_name);
        if (!env_val.empty()) {
            cfg.api_key = env_val;
            cfg.api_keys[cfg.provider] = env_val;
        }
    }

    // ---------------------------------------------------------------
    // Session preferences — overlay onto the daemon.yaml defaults.
    // Empty string at the session layer means "leave the daemon.yaml
    // value alone"; non-empty overlays. The booleans always replace
    // (no "unset" sentinel — the handler validates them).
    // ---------------------------------------------------------------
    if (!session_prefs.output_dir.empty())     cfg.output_dir   = session_prefs.output_dir;
    if (!session_prefs.note_dir.empty())       cfg.note_dir     = session_prefs.note_dir;
    if (!session_prefs.language.empty())       cfg.language     = session_prefs.language;
    if (!session_prefs.vocabulary.empty())     cfg.vocabulary   = session_prefs.vocabulary;
    if (!session_prefs.mic_source.empty())     cfg.mic_source   = session_prefs.mic_source;
    if (!session_prefs.monitor_source.empty()) cfg.monitor_source = session_prefs.monitor_source;
    if (!session_prefs.whisper_model.empty())  cfg.whisper_model = session_prefs.whisper_model;

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

    // captions_enabled / caption_latency_ms come straight from session
    // prefs. captions_enabled is opt-in for the C.10 streaming caption
    // path. Phase E.2(b) landed `caption_latency_ms` on Config, so the
    // session value now propagates into the merged Config the daemon
    // hands to the subprocess (and into the JSON config the streaming
    // CaptionEngine reads). The session.init / session.update_prefs
    // handlers enforce the [200, 2000] range upstream; this assignment
    // does not re-validate.
    cfg.captions_enabled    = session_prefs.captions_enabled;
    cfg.caption_latency_ms  = session_prefs.caption_latency_ms;

    return cfg;
}

JobConfig merge_creds_for_job_with_real_env(
    const JobConfig& daemon_config,
    const SessionCredentials& session_creds,
    const SessionPreferences& session_prefs)
{
    auto getenv_wrapper = [](const std::string& name) -> std::string {
        const char* v = std::getenv(name.c_str());
        return v ? std::string(v) : std::string();
    };
    return merge_creds_for_job(daemon_config, session_creds,
                               session_prefs, getenv_wrapper);
}

} // namespace recmeet
