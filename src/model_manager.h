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

} // namespace recmeet
