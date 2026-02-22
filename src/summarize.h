#pragma once

#include "util.h"

#include <string>
#include <vector>

namespace recmeet {

/// Escape a string for embedding inside a JSON string literal.
std::string json_escape(const std::string& s);

/// Extract a string value from JSON by key (simple, non-recursive).
std::string json_extract_string(const std::string& json, const std::string& key);

/// Build the user prompt for meeting summarization (exposed for testing).
std::string build_user_prompt(const std::string& transcript, const std::string& context = "");

/// Returns true if a model ID looks like a chat/completion model (not embedding, TTS, etc.).
bool is_chat_model(const std::string& model_id);

/// Fetch available model IDs from an OpenAI-compatible /models endpoint.
std::vector<std::string> fetch_models(const std::string& models_url, const std::string& api_key);

/// Summarize a transcript using an HTTP API (Grok, OpenAI-compatible).
std::string summarize_http(const std::string& transcript,
                            const std::string& api_url,
                            const std::string& api_key,
                            const std::string& model = "grok-3",
                            const std::string& context = "");

#if RECMEET_USE_LLAMA
/// Summarize a transcript using a local llama.cpp model.
/// threads: number of CPU threads (0 = use default_thread_count()).
std::string summarize_local(const std::string& transcript,
                             const fs::path& model_path,
                             const std::string& context = "",
                             int threads = 0);
#endif

} // namespace recmeet
