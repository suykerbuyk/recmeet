// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <filesystem>

namespace recmeet {

namespace fs = std::filesystem;

enum class LogLevel { NONE = 0, ERROR = 1, WARN = 2, INFO = 3, DEBUG = 4 };

/// Parse log level from string ("none", "error", "warn", "info", "debug").
/// Returns NONE for unrecognized strings.
LogLevel parse_log_level(const std::string& s);

/// Returns the string name for a log level ("NONE", "ERROR", "WARN", "INFO", "DEBUG").
const char* log_level_name(LogLevel level);

/// Initialize logging.
/// level: minimum level to log. NONE disables all logging.
/// dir: directory for log files (default: ~/.local/share/recmeet/logs/).
/// retention_hours: delete log files older than this (default: 4).
/// stderr_output: also write log lines to stderr (for journald / interactive).
void log_init(LogLevel level, const fs::path& dir = "",
              int retention_hours = 4, bool stderr_output = false);

/// Flush and close the log file.
void log_shutdown();

/// Log at ERROR level. No-op when level < ERROR.
__attribute__((format(printf, 1, 2)))
void log_error(const char* fmt, ...);

/// Log at WARN level. No-op when level < WARN.
__attribute__((format(printf, 1, 2)))
void log_warn(const char* fmt, ...);

/// Log at INFO level. No-op when level < INFO.
__attribute__((format(printf, 1, 2)))
void log_info(const char* fmt, ...);

/// Log at DEBUG level. No-op when level < DEBUG.
__attribute__((format(printf, 1, 2)))
void log_debug(const char* fmt, ...);

} // namespace recmeet
