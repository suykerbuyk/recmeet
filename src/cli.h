// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "config.h"

namespace recmeet {

enum class DaemonMode { Auto, Force, Disable };

struct CliResult {
    JobConfig cfg;
    bool list_sources = false;
    bool show_help = false;
    bool show_version = false;
    bool show_status = false;
    bool send_stop = false;
    bool download_models = false;
    bool update_models = false;
    DaemonMode daemon_mode = DaemonMode::Auto;
    std::string daemon_addr;  // --daemon-addr ADDRESS (host:port or socket path)

    // Speaker enrollment
    std::string enroll_name;     // --enroll "Name"
    std::string enroll_from;     // --from <meeting_dir>
    int enroll_speaker = -1;     // --speaker N (1-based, -1 = interactive)
    bool list_speakers = false;  // --speakers
    std::string remove_speaker;  // --remove-speaker "Name"
    std::string identify_dir;    // --identify <meeting_dir>
    bool reset_speakers = false;  // --reset-speakers

    // Vocabulary management
    bool list_vocab = false;         // --list-vocab
    std::string add_vocab;           // --add-vocab "word"
    std::string remove_vocab;        // --remove-vocab "word"
    bool reset_vocab = false;        // --reset-vocab

    // Subprocess mode (daemon-internal, undocumented)
    bool progress_json = false;          // --progress-json
    std::string config_json_path;        // --config-json <path>

    // Live caption flags (Phase 4). The boolean tri-state is encoded as two
    // explicit bool fields to make precedence visible at the call site:
    //   --no-captions   sets caption_force_off = true
    //   --show-captions sets caption_force_on  = true
    // Both true is invalid (mutual exclusion enforced in parse_cli).
    // Neither set falls back to the config-level captions_enabled.
    //
    // `caption_show_on_stderr` mirrors `--show-captions` so the client-side
    // recording path can tail caption events to stderr (Phase 5.2 rendering
    // hooks in here). When the language guard disables captions, this is
    // also forced false so the client doesn't subscribe to a stream that
    // will never produce.
    bool caption_force_on = false;       // --show-captions
    bool caption_force_off = false;      // --no-captions
    bool caption_show_on_stderr = false; // --show-captions side effect
    bool list_caption_models = false;    // --list-caption-models

    // CLI usage error (e.g. invalid flag combination). When non-empty, main()
    // prints this to stderr and exits with code 2 (CLI usage error). Empty
    // means parse succeeded.
    std::string parse_error;

    // Phase 4 — operator-visible warning emitted by parse_cli (e.g. the
    // language-guard for `--show-captions --language fr`). Non-fatal:
    // main() prints to stderr but does NOT exit. Empty means no warning.
    std::string parse_warning;
};

/// Phase 4 — language-guard helper. Returns true if the configured
/// language is non-English (and not the unset/auto sentinel). Pure
/// helper so the language guard stays unit-testable.
bool caption_language_supported(const std::string& language);

/// Parse command-line arguments. Loads config file as defaults, then applies flag overrides.
CliResult parse_cli(int argc, char* argv[]);

} // namespace recmeet
