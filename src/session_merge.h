// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "config.h"
#include "ipc_server.h"   // SessionCredentials, SessionPreferences
#include "pipeline.h"     // PostprocessInput

#include <functional>
#include <map>
#include <string>

namespace recmeet {

// Phase E.2 Wave 2.2b — daemon-side per-job JobConfig assembly.
//
// The postprocess subprocess (`main.cpp`) reads its `JobConfig` solely from
// the JSON written by `daemon::write_job_config()`; it does NOT re-read
// env vars or daemon.yaml. Without this assembly, every postprocess job
// would lose summarization / per-client preferences / per-call dynamics
// silently.
//
// `make_job_config` constructs the per-job `JobConfig` from FOUR sources
// with the precedence chain documented in the plan body and the
// W2.2b dispatch brief:
//
//   1. Daemon env vars (`XAI_API_KEY` / `OPENAI_API_KEY` / `ANTHROPIC_API_KEY`)
//      — operator override on the daemon host wins outright over the
//      seven dual-resident fields (provider, api_keys, api_url, api_model,
//      api_key, llm_model, llm_mmap).
//   2. `session.init` per-client preferences + credentials
//      (`SessionCredentials` + `SessionPreferences` populated through
//      the A.6 handshake).
//   3. `PostprocessInput` per-call dynamics — reprocess_dir, enroll_mode,
//      enroll_name, context_inline. These are the W2.2b (E.2 d.1) fields
//      that moved off ClientConfig because they are per-call, not
//      persisted to client.yaml.
//   4. `ServerConfig` fallback — daemon.yaml's view of provider, models,
//      diarization knobs, VAD knobs, speaker DB, log paths, etc. This is
//      the base layer; everything above overlays selectively.
//
// `env_lookup` is a pure-function injection point so tests can drive the
// assembly without `setenv` / `unsetenv`. The production call site in
// daemon.cpp passes a thin wrapper around `std::getenv` (via
// `make_job_config_with_real_env` below). The function signature returns
// `std::string` (rather than `const char*`) so tests can build a static
// lookup with zero dependency on the shell environment.
//
// Pure / stateless — takes the inputs and returns a constructed JobConfig.
// No global state, no I/O.
//
// The `prefs` parameter flows the A.6 preference fields
// (whisper_model, language, vocabulary, output_dir, note_dir,
// mic_source, monitor_source, llm_model, caption_latency_ms) into the
// JobConfig so the subprocess sees the per-client view.
// `summarization_backend` drives provider/llm-path selection downstream:
// `"local"` forces a non-empty `llm_model`; `"http"` clears `llm_model`
// so the subprocess uses the HTTP path; empty leaves whatever the
// assembly produced.
//
// Phase C (rev 5) — `captions_enabled` is NOT a session-prefs field: the
// JobConfig's captions toggle comes solely from `ServerConfig::
// captions_enabled` (the base-layer assignment at session_merge.cpp:48),
// which after Phase A1.4 carries the runtime-effective server capability.
JobConfig make_job_config(
    const ServerConfig& srv,
    const SessionCredentials& session_creds,
    const SessionPreferences& session_prefs,
    const PostprocessInput& input,
    const std::function<std::string(const std::string&)>& env_lookup);

// Convenience wrapper that uses `std::getenv` for the env-lookup. Used
// at the daemon's enqueue site. Tests use the injection form above.
JobConfig make_job_config_with_real_env(
    const ServerConfig& srv,
    const SessionCredentials& session_creds,
    const SessionPreferences& session_prefs,
    const PostprocessInput& input);

} // namespace recmeet
