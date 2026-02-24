// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "log.h"
#include "util.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace recmeet {

namespace {

LogLevel g_level = LogLevel::NONE;
FILE* g_file = nullptr;
std::mutex g_mutex;

void write_log(const char* level_str, const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time_t, &tm);
    fprintf(g_file, "%04d-%02d-%02d %02d:%02d:%02d [%s] ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, level_str);
    vfprintf(g_file, fmt, args);
    fprintf(g_file, "\n");
    fflush(g_file);
}

} // anonymous namespace

LogLevel parse_log_level(const std::string& s) {
    if (s == "info" || s == "INFO") return LogLevel::INFO;
    if (s == "warn" || s == "WARN") return LogLevel::WARN;
    if (s == "error" || s == "ERROR") return LogLevel::ERROR;
    if (s == "none" || s == "NONE") return LogLevel::NONE;
    return LogLevel::NONE;
}

const char* log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        default:              return "NONE";
    }
}

void log_init(LogLevel level, const fs::path& dir) {
    g_level = level;
    if (level == LogLevel::NONE) return;

    fs::path log_dir = dir.empty() ? (data_dir() / "logs") : dir;
    fs::create_directories(log_dir);

    // Daily log file
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time_t, &tm);
    char datebuf[16];
    strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &tm);

    fs::path log_path = log_dir / ("recmeet-" + std::string(datebuf) + ".log");
    g_file = fopen(log_path.c_str(), "a");
}

void log_shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    g_level = LogLevel::NONE;
}

void log_info(const char* fmt, ...) {
    if (g_level < LogLevel::INFO) return;
    va_list args;
    va_start(args, fmt);
    write_log("INFO", fmt, args);
    va_end(args);
}

void log_warn(const char* fmt, ...) {
    if (g_level < LogLevel::WARN) return;
    va_list args;
    va_start(args, fmt);
    write_log("WARN", fmt, args);
    va_end(args);
}

void log_error(const char* fmt, ...) {
    if (g_level < LogLevel::ERROR) return;
    va_list args;
    va_start(args, fmt);
    write_log("ERROR", fmt, args);
    va_end(args);
}

} // namespace recmeet
