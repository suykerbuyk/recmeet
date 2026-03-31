// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "log.h"
#include "util.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <sys/syscall.h>
#include <unistd.h>

namespace recmeet {

namespace {

LogLevel g_level = LogLevel::NONE;
FILE* g_file = nullptr;
std::mutex g_mutex;
bool g_stderr = false;
fs::path g_log_dir;
int g_retention_hours = 4;
int g_current_hour = -1;

/// Parse YYYY-MM-DD-HH from a log filename and return the timepoint.
/// Returns epoch (time_t 0) on parse failure.
std::chrono::system_clock::time_point parse_log_filename_time(const std::string& name) {
    // Expect: recmeet-YYYY-MM-DD-HH.log
    std::tm tm{};
    if (sscanf(name.c_str(), "recmeet-%4d-%2d-%2d-%2d.log",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour) != 4)
        return std::chrono::system_clock::time_point{};
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == -1) return std::chrono::system_clock::time_point{};
    return std::chrono::system_clock::from_time_t(t);
}

void purge_old_logs() {
    if (g_log_dir.empty() || g_retention_hours <= 0) return;
    auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(g_retention_hours);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(g_log_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto name = entry.path().filename().string();
        if (name.substr(0, 8) != "recmeet-" || name.substr(name.size() - 4) != ".log")
            continue;
        auto file_time = parse_log_filename_time(name);
        if (file_time == std::chrono::system_clock::time_point{}) continue;
        if (file_time < cutoff)
            fs::remove(entry.path(), ec);
    }
}

/// Open the hourly log file for the given time. Closes any existing file.
void open_hourly_file(const std::tm& tm) {
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "recmeet-%04d-%02d-%02d-%02d.log",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour);
    fs::path log_path = g_log_dir / buf;
    g_file = fopen(log_path.c_str(), "a");
    g_current_hour = tm.tm_hour;
}

void write_log(const char* level_str, const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file && !g_stderr) return;

    // Timestamp with milliseconds
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) .count() % 1000;
    std::tm tm{};
    localtime_r(&time_t_now, &tm);

    // Hourly rotation
    if (g_file && tm.tm_hour != g_current_hour) {
        open_hourly_file(tm);
        purge_old_logs();
    }

    // Thread ID via syscall for broad glibc compatibility
    pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));

    // Format the complete line into a stack buffer
    char header[128];
    int hlen = snprintf(header, sizeof(header),
        "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] [tid=%d] ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, (int)millis,
        level_str, (int)tid);

    char body[2048];
    int blen = vsnprintf(body, sizeof(body), fmt, args);
    if (blen < 0) blen = 0;

    if (g_file) {
        fwrite(header, 1, hlen, g_file);
        fwrite(body, 1, blen, g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
    if (g_stderr) {
        fwrite(header, 1, hlen, stderr);
        fwrite(body, 1, blen, stderr);
        fputc('\n', stderr);
    }
}

} // anonymous namespace

LogLevel parse_log_level(const std::string& s) {
    if (s == "debug" || s == "DEBUG") return LogLevel::DEBUG;
    if (s == "info" || s == "INFO") return LogLevel::INFO;
    if (s == "warn" || s == "WARN") return LogLevel::WARN;
    if (s == "error" || s == "ERROR") return LogLevel::ERROR;
    if (s == "none" || s == "NONE") return LogLevel::NONE;
    return LogLevel::NONE;
}

const char* log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
        default:              return "NONE";
    }
}

void log_init(LogLevel level, const fs::path& dir,
              int retention_hours, bool stderr_output) {
    g_level = level;
    g_stderr = stderr_output && level != LogLevel::NONE;
    g_retention_hours = retention_hours;
    if (level == LogLevel::NONE) return;

    g_log_dir = dir.empty() ? (data_dir() / "logs") : dir;
    fs::create_directories(g_log_dir);

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time_t_now, &tm);

    open_hourly_file(tm);
    purge_old_logs();
}

void log_shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    g_level = LogLevel::NONE;
    g_stderr = false;
    g_current_hour = -1;
}

void log_error(const char* fmt, ...) {
    if (g_level < LogLevel::ERROR) return;
    va_list args;
    va_start(args, fmt);
    write_log("ERROR", fmt, args);
    va_end(args);
}

void log_warn(const char* fmt, ...) {
    if (g_level < LogLevel::WARN) return;
    va_list args;
    va_start(args, fmt);
    write_log("WARN", fmt, args);
    va_end(args);
}

void log_info(const char* fmt, ...) {
    if (g_level < LogLevel::INFO) return;
    va_list args;
    va_start(args, fmt);
    write_log("INFO", fmt, args);
    va_end(args);
}

void log_debug(const char* fmt, ...) {
    if (g_level < LogLevel::DEBUG) return;
    va_list args;
    va_start(args, fmt);
    write_log("DEBUG", fmt, args);
    va_end(args);
}

} // namespace recmeet
