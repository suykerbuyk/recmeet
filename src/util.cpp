// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "util.h"
#include "log.h"

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <iomanip>
#include <sstream>
#include <system_error>
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
fs::path state_dir()  { return xdg_dir("XDG_STATE_HOME", ".local/state"); }
fs::path models_dir() { return data_dir() / "models"; }

// ---------------------------------------------------------------------------
// Phase D.5 — atomic_write_file
// ---------------------------------------------------------------------------
//
// Mirror of the C.11.4 staging→meeting WAV `atomic_relocate` durability
// tail: write to `<path>.tmp`, `fsync(file_fd)`, `rename(tmp, final)`,
// `fsync(parent_dir_fd)`. The journal + resume_token store both consume
// this primitive so the disjointness invariant ("a concurrent reader sees
// either the prior file or the new file, never a half-written one") is
// shared between the upload pipeline (C.11.4) and the persistence layer
// (D.5).
//
// EXDEV note: D.5's two callers (`PendingJobsJournal`, `ResumeTokenStore`)
// always write the `.tmp` next to the final file (same parent directory),
// so EXDEV cannot arise. The `atomic_relocate` cross-fs fallback in
// upload_session.cpp:139-219 stays where it is — its src/dst can land on
// different filesystems (tmpfs staging vs ext4 meetings) and that's the
// scenario that needs the copy-then-rename dance.
//
// `mode != 0` triggers an additional `chmod()` on the final file after
// the rename and before the dir fsync — used by ResumeTokenStore to
// enforce 0600 secrecy on the on-disk token cache. The mode is applied
// post-rename rather than at `open()` time so a partially-written `.tmp`
// can never become world-readable mid-write.
void atomic_write_file(const fs::path& path, const std::string& bytes,
                       int mode) {
    fs::path tmp = path;
    tmp += ".tmp";

    std::error_code ec;
    fs::remove(tmp, ec);  // best-effort: clear stale partial

    // Ensure the parent directory exists. This is a no-op when the caller
    // already created it (the journal + token store always call
    // create_directories first) but defensive against fresh installs.
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            throw RecmeetError("atomic_write_file: cannot mkdir " +
                               path.parent_path().string() + ": " +
                               ec.message());
        }
    }

    // O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC with 0600 default so the
    // partial-write window cannot leak token bytes to other users on
    // shared hosts. Mode 0600 is also the final mode for the token store
    // (preserved across rename per POSIX). For the journal, the caller
    // passes mode=0 (no chmod) and the file ends up at the open-time
    // 0600 mask — operator-visible only, which matches the journal's
    // sensitivity (job_ids + WAV paths are not secrets but no need to
    // share them with other host users either).
    int fd = ::open(tmp.string().c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                    0600);
    if (fd < 0) {
        throw RecmeetError("atomic_write_file: cannot open " + tmp.string() +
                           ": " + std::strerror(errno));
    }

    size_t remaining = bytes.size();
    const char* p = bytes.data();
    while (remaining > 0) {
        ssize_t w = ::write(fd, p, remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            int saved = errno;
            ::close(fd);
            fs::remove(tmp, ec);
            throw RecmeetError("atomic_write_file: write to " + tmp.string() +
                               " failed: " + std::strerror(saved));
        }
        p += w;
        remaining -= static_cast<size_t>(w);
    }

    if (::fsync(fd) != 0) {
        int saved = errno;
        ::close(fd);
        fs::remove(tmp, ec);
        throw RecmeetError("atomic_write_file: fsync of " + tmp.string() +
                           " failed: " + std::strerror(saved));
    }
    ::close(fd);

    fs::rename(tmp, path, ec);
    if (ec) {
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        throw RecmeetError("atomic_write_file: rename(" + tmp.string() +
                           " -> " + path.string() + ") failed: " +
                           ec.message());
    }

    if (mode != 0) {
        if (::chmod(path.string().c_str(), static_cast<mode_t>(mode)) != 0) {
            int saved = errno;
            throw RecmeetError("atomic_write_file: chmod(" + path.string() +
                               ") failed: " + std::strerror(saved));
        }
    }

    // Final step: fsync the parent directory so the rename entry survives
    // a crash. Best-effort — a chroot or sandboxed test that cannot
    // re-open the parent dir read-only would otherwise fail the write,
    // and the data is already on stable storage via the file fsync above.
    if (path.has_parent_path()) {
        int dfd = ::open(path.parent_path().string().c_str(),
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            (void)::fsync(dfd);
            (void)::close(dfd);
        }
    }
}

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

namespace {

/// Generic helper: find a file in `dir` whose name starts with `prefix` and
/// ends with `.json`. Falls back to `dir / legacy_name` if it exists.
/// Returns empty path if no match.
fs::path find_json_file(const fs::path& dir, const char* prefix, const char* legacy_name) {
    if (!fs::is_directory(dir)) return {};

    const size_t prefix_len = std::strlen(prefix);
    const std::string ext = ".json";

    // Prefer timestamped <prefix>YYYY-MM-DD_HH-MM.json
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const auto& name = entry.path().filename().string();
        if (name.size() > prefix_len + ext.size() &&
            name.compare(0, prefix_len, prefix) == 0 &&
            name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
            return entry.path();
        }
    }

    // Fall back to legacy filename
    fs::path legacy = dir / legacy_name;
    if (fs::exists(legacy)) return legacy;

    return {};
}

} // anonymous namespace

fs::path find_context_file(const fs::path& dir) {
    return find_json_file(dir, CONTEXT_PREFIX, LEGACY_CONTEXT_NAME);
}

fs::path find_speakers_file(const fs::path& dir) {
    return find_json_file(dir, SPEAKERS_PREFIX, LEGACY_SPEAKERS_NAME);
}

std::string derive_meeting_timestamp(const fs::path& dir) {
    // Strategy 1: parse the directory name itself.
    // Accept canonical "YYYY-MM-DD_HH-MM" and collision-suffixed
    // "YYYY-MM-DD_HH-MM_N" — strip the suffix and return the leading 16 chars.
    std::string dirname = dir.filename().string();
    if (dirname.size() >= 16) {
        int y, mo, d, h, mi;
        if (std::sscanf(dirname.c_str(), "%4d-%2d-%2d_%2d-%2d", &y, &mo, &d, &h, &mi) == 5 &&
            y >= 1970 && y <= 9999 && mo >= 1 && mo <= 12 &&
            d >= 1 && d <= 31 && h >= 0 && h <= 23 && mi >= 0 && mi <= 59) {
            char buf[17];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d", y, mo, d, h, mi);
            return std::string(buf);
        }
    }

    // Strategy 2: parse the discovered audio filename.
    fs::path audio = find_audio_file(dir);
    if (!audio.empty()) {
        std::string stem = audio.stem().string();
        const size_t prefix_len = std::strlen(AUDIO_PREFIX);
        if (stem.size() >= prefix_len + 16 &&
            stem.compare(0, prefix_len, AUDIO_PREFIX) == 0) {
            int y, mo, d, h, mi;
            if (std::sscanf(stem.c_str() + prefix_len, "%4d-%2d-%2d_%2d-%2d",
                            &y, &mo, &d, &h, &mi) == 5 &&
                y >= 1970 && y <= 9999 && mo >= 1 && mo <= 12 &&
                d >= 1 && d <= 31 && h >= 0 && h <= 23 && mi >= 0 && mi <= 59) {
                char buf[17];
                std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d", y, mo, d, h, mi);
                return std::string(buf);
            }
        }
    }

    return "";
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

// ---------------------------------------------------------------------------
// systemd property line parser (T1C.2)
// ---------------------------------------------------------------------------
//
// `systemctl show -p MemoryHigh` emits one line of the form:
//   MemoryHigh=10737418240\n          (numeric, always in bytes)
//   MemoryHigh=infinity\n             (no limit; literal string)
// Both must round-trip through the daemon's restore path: numeric values
// pass through as bytes, infinity must be restored as the literal string
// (snprintf("%ld", LONG_MAX) would clobber the "no limit" semantic by
// writing a finite number above MemoryMax that systemd silently clamps).

long parse_memory_property_line(const char* line) {
    if (!line) return -1;
    const char* eq = std::strchr(line, '=');
    if (!eq) return -1;
    eq++;
    if (std::strncmp(eq, "infinity", 8) == 0) return LONG_MAX;
    char* end = nullptr;
    long v = std::strtol(eq, &end, 10);
    return (end == eq) ? -1 : v;
}

bool is_valid_meeting_id(const std::string& s) {
    if (s.empty()) return true;       // sentinel: "no id assigned"
    if (s.size() != 36) return false; // UUID canonical text length

    // Hyphens at fixed offsets 8/13/18/23 (the 8-4-4-4-12 layout).
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
        return false;

    auto is_lower_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    };
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        if (!is_lower_hex(s[i])) return false;
    }

    // Version 4 — offset 14 is the version nibble.
    if (s[14] != '4') return false;

    // Variant 10xx — offset 19 must be one of {8,9,a,b}.
    char v = s[19];
    if (v != '8' && v != '9' && v != 'a' && v != 'b') return false;

    return true;
}

} // namespace recmeet
