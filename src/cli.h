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
};

/// Parse command-line arguments. Loads config file as defaults, then applies flag overrides.
CliResult parse_cli(int argc, char* argv[]);

} // namespace recmeet
