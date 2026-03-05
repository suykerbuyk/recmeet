// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "config.h"
#include "ipc_protocol.h"

#include <string>

namespace recmeet {

// Serialize a Config to a JSON string for IPC transport.
std::string config_to_json(const Config& cfg);

// Deserialize a JSON string back to a Config.
// Returns a default Config on parse failure.
Config config_from_json(const std::string& json);

// Convert Config to/from JsonMap (for embedding in IPC params/results).
JsonMap config_to_map(const Config& cfg);
Config config_from_map(const JsonMap& map);

} // namespace recmeet
