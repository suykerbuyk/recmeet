// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "util.h"
#include "log.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace recmeet {

static fs::path xdg_dir(const char* env_var, const char* fallback_suffix) {
    if (const char* val = std::getenv(env_var); val && val[0] != '\0')
        return fs::path(val) / "recmeet";
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / fallback_suffix / "recmeet";
    return fs::path(".") / fallback_suffix / "recmeet";
}

fs::path config_dir() { return xdg_dir("XDG_CONFIG_HOME", ".config"); }
fs::path data_dir()   { return xdg_dir("XDG_DATA_HOME", ".local/share"); }
fs::path models_dir() { return data_dir() / "models"; }

fs::path find_audio_file(const fs::path& dir) {
    if (!fs::is_directory(dir)) return {};

    // Prefer timestamped audio_YYYY-MM-DD_HH-MM.wav
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const auto& name = entry.path().filename().string();
        if (name.size() > 6 &&
            name.compare(0, 6, AUDIO_PREFIX) == 0 &&
            name.size() >= 4 &&
            name.compare(name.size() - 4, 4, ".wav") == 0) {
            return entry.path();
        }
    }

    // Fall back to legacy audio.wav
    fs::path legacy = dir / LEGACY_AUDIO_NAME;
    if (fs::exists(legacy)) return legacy;

    return {};
}

OutputDir create_output_dir(const fs::path& base_dir) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d_%H-%M");
    std::string timestamp = oss.str();

    fs::path dir = base_dir / timestamp;
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
        return {dir, timestamp};
    }

    // Handle collision — suffix only on directory name, timestamp stays clean
    for (int i = 2; i < 100; ++i) {
        fs::path candidate = base_dir / (timestamp + "_" + std::to_string(i));
        if (!fs::exists(candidate)) {
            fs::create_directories(candidate);
            return {candidate, timestamp};
        }
    }
    throw RecmeetError("Too many sessions in the same minute.");
}

void write_text_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path);
    if (!out)
        throw RecmeetError("Failed to write file: " + path.string());
    out << content;
    if (!out)
        throw RecmeetError("Write error: " + path.string());
}

int default_thread_count() {
    unsigned n = std::thread::hardware_concurrency();
    return (n > 1) ? static_cast<int>(n - 1) : 1;
}

long read_self_rss_kb() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages_total = 0;
    long pages_resident = 0;
    int n = std::fscanf(f, "%ld %ld", &pages_total, &pages_resident);
    std::fclose(f);
    if (n != 2 || pages_resident <= 0) return 0;
    long page_kb = ::sysconf(_SC_PAGESIZE) / 1024;
    if (page_kb <= 0) return 0;
    return pages_resident * page_kb;
}

size_t write_heartbeat_ndjson(int fd, long rss_kb) {
    char buf[96];
    int n = std::snprintf(buf, sizeof(buf),
        "{\"event\":\"heartbeat\",\"data\":{\"rss_kb\":%ld}}\n", rss_kb);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) return 0;
    ssize_t written = 0;
    while (written < n) {
        ssize_t w = ::write(fd, buf + written, n - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        written += w;
    }
    return written > 0 ? static_cast<size_t>(written) : 0;
}

void write_rss_limit_msg(int fd) {
    static const char msg[] =
        "child RSS limit exceeded - split audio (ffmpeg) "
        "or raise RECMEET_RSS_LIMIT_MB\n";
    ssize_t mw = 0;
    ssize_t mlen = static_cast<ssize_t>(sizeof(msg) - 1);
    while (mw < mlen) {
        ssize_t w = ::write(fd, msg + mw, mlen - mw);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        mw += w;
    }
}

std::pair<std::string, std::string> resolve_meeting_time(
    const fs::path& out_dir, const fs::path& audio_path) {

    // Priority 1: parse audio filename — audio_YYYY-MM-DD_HH-MM.wav
    std::string stem = audio_path.stem().string();
    if (stem.size() >= 22 && stem.compare(0, 6, AUDIO_PREFIX) == 0) {
        int y, mo, d, h, mi;
        if (std::sscanf(stem.c_str() + 6, "%4d-%2d-%2d_%2d-%2d", &y, &mo, &d, &h, &mi) == 5 &&
            y >= 1970 && y <= 9999 && mo >= 1 && mo <= 12 &&
            d >= 1 && d <= 31 && h >= 0 && h <= 23 && mi >= 0 && mi <= 59) {
            char date_buf[16], time_buf[8];
            std::snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", y, mo, d);
            std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d", h, mi);
            return {date_buf, time_buf};
        }
    }

    // Priority 2: parse directory name — YYYY-MM-DD_HH-MM (possibly with _N suffix)
    std::string dirname = out_dir.filename().string();
    if (dirname.size() >= 16) {
        int y, mo, d, h, mi;
        if (std::sscanf(dirname.c_str(), "%4d-%2d-%2d_%2d-%2d", &y, &mo, &d, &h, &mi) == 5 &&
            y >= 1970 && y <= 9999 && mo >= 1 && mo <= 12 &&
            d >= 1 && d <= 31 && h >= 0 && h <= 23 && mi >= 0 && mi <= 59) {
            char date_buf[16], time_buf[8];
            std::snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", y, mo, d);
            std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d", h, mi);
            return {date_buf, time_buf};
        }
    }

    // Fallback: audio file modification time (use stat for C++17 compatibility)
    if (fs::exists(audio_path)) {
        struct stat st;
        if (::stat(audio_path.c_str(), &st) == 0) {
            std::tm tm{};
            localtime_r(&st.st_mtime, &tm);
            char date_buf[16], time_buf[8];
            std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);
            std::strftime(time_buf, sizeof(time_buf), "%H:%M", &tm);
            log_info("Meeting time from audio mtime: %s %s", date_buf, time_buf);
            return {date_buf, time_buf};
        }
    }

    // Final fallback: current time
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time_t, &tm);
    char date_buf[16], time_buf[8];
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);
    std::strftime(time_buf, sizeof(time_buf), "%H:%M", &tm);
    return {date_buf, time_buf};
}

} // namespace recmeet
