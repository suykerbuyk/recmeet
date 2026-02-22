#pragma once

#include "util.h"

#include <string>

namespace recmeet {

/// Check whether a whisper model is already cached locally.
bool is_whisper_model_cached(const std::string& model_name);

/// Ensure a whisper model is available locally, downloading if needed.
/// Returns the path to the GGUF model file.
/// Model names: tiny, base, small, medium, large-v3
fs::path ensure_whisper_model(const std::string& model_name);

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
#endif

} // namespace recmeet
