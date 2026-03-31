// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <string>

namespace recmeet {

/// Escape a string for embedding inside a JSON string literal.
std::string json_escape(const std::string& s);

/// Extract a string value from JSON by key (simple, non-recursive).
std::string json_extract_string(const std::string& json, const std::string& key);

} // namespace recmeet
