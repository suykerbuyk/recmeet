// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// See pipeline_cleanup.h for the design contract.

#include "pipeline_cleanup.h"

#include "log.h"

#include <exception>
#include <filesystem>

namespace recmeet {

bool cleanup_cancelled_recording_dir(const fs::path& out_dir) {
    if (out_dir.empty() || out_dir == "/" || !fs::exists(out_dir)) {
        return true;
    }
    try {
        fs::remove_all(out_dir);
        log_info("cancel: removed %s", out_dir.c_str());
        return true;
    } catch (const std::exception& e) {
        log_warn("cancel: cleanup of %s failed: %s",
                 out_dir.c_str(), e.what());
        return false;
    }
}

} // namespace recmeet
