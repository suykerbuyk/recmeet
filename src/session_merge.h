// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "config.h"
#include "ipc_server.h"   // SessionCredentials, SessionPreferences

#include <functional>
#include <map>
#include <string>

namespace recmeet {

// Phase A.6.1 — subprocess credential merge.
//
// The postprocess subprocess (`main.cpp`) reads its `Config` solely from
// the JSON written by `daemon::write_job_config()`; it does NOT re-read
// env vars or daemon.yaml. Without this merge, every postprocess job
// loses summarization silently.
//
// `merge_creds_for_job` resolves the per-job `Config` from three sources
// with the precedence chain documented in the plan body:
//
//   1. Daemon env vars (`XAI_API_KEY` / `OPENAI_API_KEY` / `ANTHROPIC_API_KEY`)
//      — operator override on the daemon host wins outright.
//   2. `session.init` per-client credentials (the `SessionCredentials`
//      slot populated through the A.6 handshake).
//   3. `daemon.yaml` fallback — already loaded into `daemon_config` by
//      `load_config()` at daemon startup.
//
// `env_lookup` is a pure-function injection point so tests can drive the
// merge without `setenv` / `unsetenv`. The production callsite in
// daemon.cpp passes a thin wrapper around `std::getenv`. The function
// signature returns `std::map<std::string, std::string>` (rather than
// `const char*`) so tests can build a static lookup with zero dependency
// on the shell environment.
//
// Pure / stateless — takes the inputs and returns a merged Config. No
// global state, no I/O. The unit assertion required by iter-139 C-2
// hangs off this function directly.
//
// `job_cfg` is the starting point — usually daemon.yaml's `g_config`
// snapshot; the merge overlays env > session on top. Per-job fields
// (output_dir, reprocess_dir, etc.) that are not part of the A.6
// session pref surface pass through untouched.
//
// The `prefs` parameter also flows the A.6 preference fields
// (whisper_model, language, vocabulary, output_dir, note_dir,
// mic_source, monitor_source, llm_model, captions_enabled) into
// `job_cfg` so the subprocess sees the per-client view. `summarization_backend`
// drives provider/llm-path selection downstream: `"local"` forces a
// non-empty `llm_model`; `"http"` clears `llm_model` so the subprocess
// uses the HTTP path; empty leaves whatever the merge produced.
Config merge_creds_for_job(
    const Config& daemon_config,
    const SessionCredentials& session_creds,
    const SessionPreferences& session_prefs,
    const std::function<std::string(const std::string&)>& env_lookup);

// Convenience wrapper that uses `std::getenv` for the env-lookup. Used
// at the daemon's enqueue site. Tests use the injection form above.
Config merge_creds_for_job_with_real_env(
    const Config& daemon_config,
    const SessionCredentials& session_creds,
    const SessionPreferences& session_prefs);

} // namespace recmeet
