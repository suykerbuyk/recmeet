// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "util.h"
#include "note.h"

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
std::string resolve_api_key(const ProviderInfo& provider,
                            const std::map<std::string, std::string>& api_keys,
                            const std::string& legacy_key = "");

struct Config {
    // Audio
    std::string device_pattern = DEFAULT_DEVICE_PATTERN;
    std::string mic_source;     // empty = auto-detect
    std::string monitor_source; // empty = auto-detect
    bool mic_only = false;
    bool keep_sources = false;  // Keep mic.wav and monitor.wav after mixing

    // Transcription
    std::string whisper_model = "base";
    std::string language; // empty = auto-detect, otherwise ISO 639-1 code (e.g. "en")
    std::string vocabulary; // comma-separated vocabulary hints for whisper initial_prompt

    // Summarization
    std::string provider = "xai";
    std::string api_url; // empty = derived from provider
    std::string api_key; // legacy single key (fallback)
    std::string api_model = "grok-3";
    bool no_summary = false;

    // Per-provider API keys (env var > api_keys[provider] > api_key)
    std::map<std::string, std::string> api_keys;

    // Local LLM
    std::string llm_model; // path or name, empty = use HTTP API
    bool llm_mmap = false;  // use mmap for model loading (default: off to avoid swap thrashing)

    // Diarization (on by default when built with RECMEET_USE_SHERPA)
    bool diarize = true;
    int num_speakers = 0;  // 0 = auto-detect
    float cluster_threshold = 1.18f;  // clustering distance threshold (lower = more splitting)

    // Chunked diarization (T2.1/T2.2 — engaged when audio length exceeds the
    // pipeline threshold; otherwise the single-call path is used). Defaults
    // sized to keep each chunk's peak working set well under the iter-110
    // ~10 GB single-call boundary while still giving each chunk enough audio
    // to produce well-separated clusters. `chunk_minutes * 60` must exceed
    // `chunk_overlap_sec + 60` (positive spacing with ≥ 60 s minimum core);
    // load_config() falls back to defaults with a warning if the persisted
    // values violate this invariant.
    float chunk_minutes = 15.0f;
    float chunk_overlap_sec = 30.0f;
    /// Cosine-similarity floor for stitching chunk-local centroids into the
    /// global registry (matches the SherpaOnnxSpeakerEmbeddingManager metric).
    float stitch_threshold = 0.6f;

    // Speaker identification (cross-session voiceprint matching)
    bool speaker_id = true;  // enabled when speaker DB exists
    float speaker_threshold = 0.6f;  // cosine similarity threshold
    fs::path speaker_db;  // empty = default (~/.local/share/recmeet/speakers/)

    // VAD (on by default when built with RECMEET_USE_SHERPA)
    bool vad = true;
    float vad_threshold = 0.5f;
    float vad_min_silence = 0.5f;
    float vad_min_speech = 0.25f;
    float vad_max_speech = 30.0f;

    // Live captions (Phase 3 daemon wiring + Phase 4 surface). When
    // `captions_enabled` is true on a `record.start`, pipeline.cpp wires a
    // CaptionEngine to the live capture and broadcasts `caption` /
    // `caption.degraded` IPC events. `caption_model` names the streaming
    // model directory (under `~/.local/share/recmeet/models/sherpa/online/`);
    // empty resolves to the Phase-0.2-locked default at use time so a future
    // pin change picks up automatically.
    //
    // The default for `captions_enabled` is `false` — captions are an
    // explicit opt-in (CLI: `--show-captions`; tray: checkbox). The default
    // for `caption_model` is left as the empty string here; the resolver
    // (resolve_caption_model_dir / model_manager) treats empty as
    // "en-2023-06-26", keeping the lock in one place.
    bool captions_enabled = false;
    std::string caption_model;

    // Phase 5.5 — render-time caption normalization. The IPC `caption`
    // event payload always carries the raw engine output (ALL-CAPS, no
    // punctuation for the en-2023-06-26 streaming Zipformer); CLI / tray
    // pass it through `normalize_caption()` before display when this is
    // true. Disable for transcript-fidelity debugging.
    bool caption_normalize_display = true;

    // Performance
    int threads = 0;  // 0 = auto-detect (hardware_concurrency - 1)

    // Logging
    std::string log_level_str = "error";  // "none", "error", "warn", "info", "debug"
    fs::path log_dir;            // empty = default (~/.local/share/recmeet/logs/)
    int log_retention_hours = 4; // hours of log history to keep

    // Output
    fs::path output_dir = "./meetings";
    bool output_dir_explicit = false;  // true when --output-dir passed on CLI
    fs::path note_dir;  // empty = use output_dir (same as audio)

    // Meeting notes
    NoteConfig note;

    // Context
    fs::path context_file;
    std::string context_inline;  // Inline context from dialog or --context-text CLI

    // Reprocess
    fs::path reprocess_dir;
    fs::path reprocess_batch_dir;
    bool reprocess_batch_dry_run = false;

    // Batch mode flag — propagates through IPC + subprocess JSON; daemon stores
    // per-job; tray reads via job.complete event. Suppresses per-meeting
    // notifications during a batch reprocess so the operator sees only the
    // end-of-batch summary notification.
    bool batch_mode = false;

    // Web server
    int web_port = 8384;
    std::string web_bind = "127.0.0.1";

    // IPC framing limits (Phase A.2). `max_message_bytes` caps NDJSON line
    // length per connection — accumulating reads past this without a `\n`
    // are treated as protocol abuse (slowloris, oversized frame) and the
    // fd is dropped. Cap is line-length only; binary frame payloads
    // (Phase C `0x01`/`0x02`/`0x03`) are bounded separately by
    // `max_upload_bytes`. Default 8 MB matches the plan body.
    size_t max_message_bytes = 8 * 1024 * 1024;

    // Maximum binary upload size (Phase C `process.submit`). Declared here
    // now so the schema is in place for C.2; A.2 itself does not enforce it.
    // Default 4 GB per the plan body.
    size_t max_upload_bytes = 4ull * 1024 * 1024 * 1024;

    // Maximum concurrent IPC clients (Phase A.3). Connections past this
    // cap that pass `accept()` are refused with a single-line JSON error
    // frame (`{"event":"error","kind":"server_full",...}`) and the fd is
    // closed before being registered in the client map. Default 16; the
    // listen backlog is automatically raised to `max_clients * 2` so the
    // kernel queue does not bottleneck before the daemon-side cap engages.
    size_t max_clients = 16;

    // Phase C.7 — server-side JobQueue typed-slot capacities. The daemon's
    // three independent capacity-1 slots: postprocess, streaming, and
    // model_download. A postprocess + a streaming + a model_download job
    // can all run concurrently; two jobs of the same kind queue serially.
    // Wired from `[server] slot.postprocess` / `slot.streaming` /
    // `slot.model_download`. Defaults all 1 (the typed-slot invariant);
    // a zero/negative override is treated as a typo and falls back to 1.
    int slot_postprocess = 1;
    int slot_streaming = 1;
    int slot_model_download = 1;

    // Phase C.7 — download initiation policy. When true (default) any
    // PSK-authenticated client may trigger a model download (directly via
    // models.ensure / models.update, or implicitly when a job it owns
    // dequeues against an uncached model). Operators set
    // `[server] allow_client_downloads: false` to disable client-driven
    // downloads entirely. Wired from `[server] allow_client_downloads`.
    bool allow_client_downloads = true;

    // Phase C.8 — server-resident voiceprint enrollment via process.submit.
    //
    // `enroll_mode` is set to true on Job::cfg by the daemon when the
    // originating process.submit carried `mode=enroll`. It is NOT a user-
    // facing knob — operators do not set it in config.yaml. The
    // pp_worker subprocess inspects this flag and runs the
    // diarization-only path: skip transcribe, skip summarize, skip
    // note-write, but persist a `diarization.json` artifact in
    // `out_dir` so the daemon can read the centroids back and populate
    // the diarization cache. `enroll_name` is the user-supplied name
    // the eventual `enroll.finalize` will store the voiceprint under;
    // the subprocess does not write to the speakers DB itself (the
    // daemon's `enroll.finalize` handler is the single writer, called
    // after the user picks a target cluster via the IPC dance).
    //
    // Both fields round-trip through `config_to_map` / `config_from_map`
    // so they survive the daemon → subprocess JSON-config write_job_config
    // boundary.
    bool        enroll_mode = false;
    std::string enroll_name;

    // Phase C.8 — diarization cache TTL. Cache entries past this age are
    // lazily evicted on lookup. Default 24 h (86400 s). 0 = never expire
    // (useful for tests; not recommended in production — the cache is
    // in-memory only so a long-running daemon would accumulate forever).
    // Wired from `[server] diarization_cache_ttl_secs`.
    //
    // Phase C.13 (M-1) consolidates this with the resume_token TTL into a
    // single operator-facing knob (`retain_terminal_hours`, below). When
    // `retain_terminal_hours` is non-zero, the config loader derives this
    // value as `retain_terminal_hours * 3600` so an operator bumping the
    // unified knob keeps both lifetimes in lock-step. Legacy operators who
    // set `diarization_cache_ttl_secs` directly still work — see the
    // precedence rule documented on `retain_terminal_hours` below.
    int64_t diarization_cache_ttl_secs = 86400;

    // Phase D.6 — client-side staging disk-budget retention.
    //
    // The tray-side staging dir (~/.local/share/recmeet/staging/) is the
    // hop a recording spends between capture and either (a) batch upload
    // via process.submit + server-side fetch, or (b) save-for-later via
    // a .pending sidecar. D.6 enforces an operator-configurable hard
    // cap on its on-disk size — matching the journald MaxUse model.
    //
    // Triggers: (1) synchronous check at recording-start (projected
    // total = current + 460 MB) and (2) periodic 10 min GTK timer.
    // Eviction: safe-to-evict WAVs (NOT journal-referenced AND NO
    // .pending sidecar) sorted by mtime ascending, unlinked oldest-first
    // until total ≤ budget. Protected WAVs (journal OR sidecar) are
    // never auto-evicted — operator removes via `rm` or by deleting the
    // sidecar.
    //
    // Wired from `[client] staging_max_bytes` in config.yaml. Default
    // 500 GiB. Phase E.2 owns the Config split into ServerConfig +
    // ClientConfig; this field migrates with the client side then.
    // The `[client]` section is introduced here as a new YAML namespace
    // (mirrors the pattern used for `[ipc]`, `[server]`, etc. — each
    // landed when the first knob for that section was added). A
    // zero/negative override is treated as a typo and falls back to
    // the struct default rather than disabling the cap (defense-in-
    // depth — an over-large staging dir loses operator data through
    // the eviction sweep; we err on the conservative side).
    size_t staging_max_bytes = static_cast<size_t>(500) * 1024 * 1024 * 1024;

    // Phase C.13 (M-1) — consolidated terminal-state / session retention
    // knob. Drives BOTH `resume_token_ttl_hours` (the SessionManager TTL
    // — how long a disconnected client can hold its resume_token before
    // the GC sweep reaps it) AND `diarization_cache_ttl_secs`
    // (retain_terminal_hours * 3600 — the C.8 cache lifetime). Default
    // 24 h. 0 means "never expire" (test / dev only; the resume_token map
    // is in-memory only so it dies with the daemon either way).
    //
    // Wired from `[server] retain_terminal_hours`. **Precedence**: if this
    // key is present and >= 0 in the YAML, it OVERRIDES any explicit
    // `[server] diarization_cache_ttl_secs` setting (the unified knob
    // wins, by design — the M-1 finding was specifically that the two
    // could drift). When this key is absent, the loader falls back to the
    // legacy `diarization_cache_ttl_secs` field on disk and derives the
    // session TTL from the same number. Operators migrating from the old
    // single-knob setup get identical behavior without editing config.
    int retain_terminal_hours = 24;
};

/// Load config. Uses path if provided, otherwise ~/.config/recmeet/config.yaml.
Config load_config(const fs::path& config_path = {});

/// Save config. Uses path if provided, otherwise ~/.config/recmeet/config.yaml.
void save_config(const Config& cfg, const fs::path& config_path = {});

} // namespace recmeet
