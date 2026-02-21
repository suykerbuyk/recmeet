#pragma once

#include "config.h"

namespace recmeet {

struct CliResult {
    Config cfg;
    bool list_sources = false;
    bool show_help = false;
    bool show_version = false;
};

/// Parse command-line arguments. Loads config file as defaults, then applies flag overrides.
CliResult parse_cli(int argc, char* argv[]);

} // namespace recmeet
