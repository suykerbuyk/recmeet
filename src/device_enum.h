// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <string>
#include <vector>

namespace recmeet {

struct AudioSource {
    std::string name;
    std::string description;
    bool is_monitor;       // true if .monitor suffix
};

struct DetectedSources {
    std::string mic;       // empty if not found
    std::string monitor;   // empty if not found
    std::vector<AudioSource> all;
};

/// List all PulseAudio/PipeWire sources via pa_context introspection.
std::vector<AudioSource> list_sources();

/// Auto-detect mic and monitor sources matching regex pattern.
DetectedSources detect_sources(const std::string& pattern);

} // namespace recmeet
