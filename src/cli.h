// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "config.h"

namespace recmeet {

enum class DaemonMode { Auto, Force, Disable };

struct CliResult {
    Config cfg;
    bool list_sources = false;
    bool show_help = false;
    bool show_version = false;
    bool show_status = false;
    bool send_stop = false;
    bool download_models = false;
    bool update_models = false;
    DaemonMode daemon_mode = DaemonMode::Auto;

    // Speaker enrollment
    std::string enroll_name;     // --enroll "Name"
    std::string enroll_from;     // --from <meeting_dir>
    int enroll_speaker = -1;     // --speaker N (1-based, -1 = interactive)
    bool list_speakers = false;  // --speakers
    std::string remove_speaker;  // --remove-speaker "Name"
    std::string identify_dir;    // --identify <meeting_dir>
};

/// Parse command-line arguments. Loads config file as defaults, then applies flag overrides.
CliResult parse_cli(int argc, char* argv[]);

} // namespace recmeet
