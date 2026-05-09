// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "util.h"

#include <cstdint>
#include <string>
#include <vector>

namespace recmeet {

/// Model cache status information.
struct ModelStatus {
    std::string name;        // e.g. "base", "segmentation"
    std::string category;    // "whisper", "sherpa", "vad"
    bool cached = false;
    int64_t size_bytes = 0;  // file size if cached
    std::string path;        // full path
};

/// List all known models and their cache status.
std::vector<ModelStatus> list_cached_models();

/// Check whether a whisper model is already cached locally.
bool is_whisper_model_cached(const std::string& model_name);

/// Ensure a whisper model is available locally, downloading if needed.
/// Returns the path to the GGUF model file.
/// Model names: tiny, base, small, medium, large-v3
fs::path ensure_whisper_model(const std::string& model_name);

/// Force (re-)download a whisper model even if already cached.
fs::path download_whisper_model(const std::string& model_name);

#if RECMEET_USE_LLAMA
/// Ensure a llama model is available locally, downloading if needed.
/// Returns the path to the GGUF model file.
fs::path ensure_llama_model(const std::string& model_name);
#endif

#if RECMEET_USE_SHERPA
struct SherpaModelPaths {
    fs::path segmentation;  // path to segmentation model.onnx
    fs::path embedding;     // path to embedding .onnx
};

/// Check whether sherpa diarization models are cached locally.
bool is_sherpa_model_cached();

/// Ensure sherpa diarization models are available, downloading if needed.
/// Returns paths to the segmentation and embedding model files.
SherpaModelPaths ensure_sherpa_models();

/// Force (re-)download sherpa diarization models.
SherpaModelPaths download_sherpa_models();

/// Check whether the Silero VAD model is cached locally.
bool is_vad_model_cached();

/// Ensure the Silero VAD model is available, downloading if needed.
/// Returns the path to silero_vad.onnx.
fs::path ensure_vad_model();

/// Force (re-)download the Silero VAD model.
fs::path download_vad_model();
#endif

// ---------------------------------------------------------------------------
// Phase 4 — Streaming caption (online ASR) model manager.
//
// Caption models live alongside the diarization sherpa models but in a
// separate "online" subdir so the layout mirrors the sherpa-onnx model zoo:
//   ~/.local/share/recmeet/models/sherpa/online/<name>/
//      encoder-...onnx
//      decoder-...onnx
//      joiner-...onnx
//      tokens.txt
//
// A model is considered cached when the directory exists and contains at
// least one encoder-*.onnx, decoder-*.onnx, joiner-*.onnx and a tokens.txt
// (filenames vary across model-zoo entries; the prefix-match keeps us
// resilient).
//
// In sherpa-OFF builds the cache check is harmless (still returns true if
// files are on disk) and `ensure_caption_model()` throws a clear error
// because the build cannot drive the streaming recognizer regardless.
// ---------------------------------------------------------------------------

/// List of curated streaming caption models (by short name). Used by the
/// `--list-caption-models` CLI flag to print availability.
std::vector<std::string> known_caption_models();

/// Human-readable download size hint for a caption model name (e.g. "~74 MB").
/// Returns empty string if the name is unknown. Used by the CLI pre-flight
/// prompt to set operator expectations.
std::string caption_model_size_hint(const std::string& name);

/// Resolve the on-disk directory for a streaming caption model. If `name`
/// is empty, returns the directory for the default model
/// (`en-2023-06-26`). Existence is NOT checked — see
/// `is_caption_model_cached()` for that.
fs::path caption_model_dir(const std::string& name);

/// Return true if the caption model directory contains the four required
/// file shapes (encoder-*, decoder-*, joiner-*.onnx + tokens.txt). Empty
/// `name` resolves to the default. Resilient to filename variation in the
/// sherpa-onnx model zoo.
bool is_caption_model_cached(const std::string& name);

#if RECMEET_USE_SHERPA
/// Ensure a streaming caption model is available locally, downloading if
/// needed. Returns the model directory path on success. Empty `name`
/// resolves to the default. Throws RecmeetError on download / extraction
/// failure or when `name` is not in the curated list.
fs::path ensure_caption_model(const std::string& name);

/// Force (re-)download a streaming caption model. Returns the model
/// directory path. Empty `name` resolves to the default.
fs::path download_caption_model(const std::string& name);
#else
/// Sherpa-OFF stub. Always throws RecmeetError with the canonical message
/// so the no-sherpa path fails clearly at the same call site.
fs::path ensure_caption_model(const std::string& name);
#endif

} // namespace recmeet
