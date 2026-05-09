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
};

/// Load config. Uses path if provided, otherwise ~/.config/recmeet/config.yaml.
Config load_config(const fs::path& config_path = {});

/// Save config. Uses path if provided, otherwise ~/.config/recmeet/config.yaml.
void save_config(const Config& cfg, const fs::path& config_path = {});

} // namespace recmeet
