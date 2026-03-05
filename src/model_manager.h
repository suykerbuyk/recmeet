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

} // namespace recmeet
