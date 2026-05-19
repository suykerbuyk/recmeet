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

// Phase E.2 Wave 2.2a — overloads for the split structs. Mirrors the flat
// `key`-based shape of `config_to_map(const Config&)` so the same
// JsonMap layout can carry either struct over the IPC boundary.
JsonMap config_to_map(const ServerConfig& cfg);
JsonMap config_to_map(const ClientConfig& cfg);

// Reconstruct a ServerConfig / ClientConfig from a flat JsonMap.
ServerConfig server_config_from_map(const JsonMap& map);
ClientConfig client_config_from_map(const JsonMap& map);

} // namespace recmeet
