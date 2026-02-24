// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <filesystem>

namespace recmeet {

namespace fs = std::filesystem;

enum class LogLevel { NONE = 0, ERROR = 1, WARN = 2, INFO = 3 };

/// Parse log level from string ("none", "error", "warn", "info").
/// Returns NONE for unrecognized strings.
LogLevel parse_log_level(const std::string& s);

/// Returns the string name for a log level ("NONE", "ERROR", "WARN", "INFO").
const char* log_level_name(LogLevel level);

/// Initialize logging. When level == NONE, all log functions are no-ops.
/// dir: directory for log files (default: ~/.local/share/recmeet/logs/).
/// Creates the directory if needed.
void log_init(LogLevel level, const fs::path& dir = "");

/// Flush and close the log file.
void log_shutdown();

/// Log at INFO level. No-op when level < INFO.
__attribute__((format(printf, 1, 2)))
void log_info(const char* fmt, ...);

/// Log at WARN level. No-op when level < WARN.
__attribute__((format(printf, 1, 2)))
void log_warn(const char* fmt, ...);

/// Log at ERROR level. No-op when level < ERROR.
__attribute__((format(printf, 1, 2)))
void log_error(const char* fmt, ...);

} // namespace recmeet
