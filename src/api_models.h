// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <string>
#include <vector>

namespace recmeet {

/// Returns true if a model ID looks like a chat/completion model (not embedding, TTS, etc.).
bool is_chat_model(const std::string& model_id);

/// Fetch available model IDs from an OpenAI-compatible /models endpoint.
std::vector<std::string> fetch_models(const std::string& models_url, const std::string& api_key);

} // namespace recmeet
