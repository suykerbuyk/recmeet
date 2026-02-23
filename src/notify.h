// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <string>

namespace recmeet {

/// Initialize libnotify. Call once at startup.
void notify_init();

/// Send a desktop notification. Fire-and-forget — never throws.
void notify(const std::string& title, const std::string& body = "");

/// Clean up libnotify. Call once at shutdown.
void notify_cleanup();

} // namespace recmeet
