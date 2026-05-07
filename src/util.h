// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

namespace recmeet {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

class RecmeetError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class DeviceError : public RecmeetError {
    using RecmeetError::RecmeetError;
};

class AudioValidationError : public RecmeetError {
    using RecmeetError::RecmeetError;
};

// ---------------------------------------------------------------------------
// Stop token — shared between signal handler, tray UI, and capture threads
// ---------------------------------------------------------------------------

struct StopToken {
    std::atomic<bool> requested{false};

    void request() { requested.store(true, std::memory_order_release); }
    bool stop_requested() const { return requested.load(std::memory_order_acquire); }
    void reset() { requested.store(false, std::memory_order_release); }
};

// ---------------------------------------------------------------------------
// Audio constants
// ---------------------------------------------------------------------------

constexpr int SAMPLE_RATE = 16000;
constexpr int CHANNELS = 1;
constexpr int SAMPLE_BITS = 16;
constexpr int BYTES_PER_SAMPLE = SAMPLE_BITS / 8;
constexpr int BYTES_PER_SEC = SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE;

// ---------------------------------------------------------------------------
// Path helpers (XDG-compliant)
// ---------------------------------------------------------------------------

/// ~/.config/recmeet/
fs::path config_dir();

/// ~/.local/share/recmeet/
fs::path data_dir();

/// ~/.local/share/recmeet/models/
fs::path models_dir();

// ---------------------------------------------------------------------------
// Audio file naming
// ---------------------------------------------------------------------------

constexpr const char* AUDIO_PREFIX = "audio_";
constexpr const char* LEGACY_AUDIO_NAME = "audio.wav";
constexpr const char* CONTEXT_PREFIX = "context_";
constexpr const char* LEGACY_CONTEXT_NAME = "context.json";
constexpr const char* SPEAKERS_PREFIX = "speakers_";
constexpr const char* LEGACY_SPEAKERS_NAME = "speakers.json";

/// Find the audio file in a meeting directory.
/// Prefers audio_YYYY-MM-DD_HH-MM.wav, falls back to audio.wav.
/// Returns empty path if dir doesn't exist or no audio file found.
fs::path find_audio_file(const fs::path& dir);

/// Find the context.json file in a meeting directory.
/// Prefers context_YYYY-MM-DD_HH-MM.json, falls back to context.json.
/// Returns empty path if dir doesn't exist or no matching file found.
fs::path find_context_file(const fs::path& dir);

/// Find the speakers.json file in a meeting directory.
/// Prefers speakers_YYYY-MM-DD_HH-MM.json, falls back to speakers.json.
/// Returns empty path if dir doesn't exist or no matching file found.
fs::path find_speakers_file(const fs::path& dir);

/// Derive the YYYY-MM-DD_HH-MM timestamp for a meeting directory.
/// Strategy:
///   1. If dir.filename() matches the YYYY-MM-DD_HH-MM pattern, return it
///      (collision suffix `_N` stripped).
///   2. Otherwise, parse the discovered audio filename via find_audio_file()
///      (strip "audio_" prefix and ".wav" suffix).
///   3. Otherwise, return "".
std::string derive_meeting_timestamp(const fs::path& dir);

// ---------------------------------------------------------------------------
// Output directory creation
// ---------------------------------------------------------------------------

/// Result of create_output_dir: the directory path and the clean timestamp
/// (without any collision suffix).
struct OutputDir {
    fs::path path;
    std::string timestamp;  // Always "YYYY-MM-DD_HH-MM" (no collision suffix)
};

/// Create timestamped output directory under base_dir, e.g. meetings/2026-02-20_14-30/
OutputDir create_output_dir(const fs::path& base_dir);

// ---------------------------------------------------------------------------
// Default device pattern
// ---------------------------------------------------------------------------

constexpr const char* DEFAULT_DEVICE_PATTERN = "";

// ---------------------------------------------------------------------------
// File writing helper
// ---------------------------------------------------------------------------

/// Write text content to a file, throwing RecmeetError on failure.
void write_text_file(const fs::path& path, const std::string& content);

// ---------------------------------------------------------------------------
// Thread count helper
// ---------------------------------------------------------------------------

/// Default thread count for inference engines: hardware_concurrency() - 1, minimum 1.
int default_thread_count();

// ---------------------------------------------------------------------------
// Process resident-set-size (Linux)
// ---------------------------------------------------------------------------

/// Read this process's resident set size in kilobytes from /proc/self/statm.
/// Returns 0 on read failure (non-Linux, transient errors, sandboxed /proc).
/// Does not allocate beyond a fixed-size stack buffer in the underlying read.
long read_self_rss_kb();

/// Format a heartbeat NDJSON line for `rss_kb` into a stack buffer and write
/// it to `fd` via raw write(2). Does not allocate, does not take the libc
/// stdio mutex - safe to call from a heartbeat thread under malloc-arena
/// lock contention. Returns the number of bytes written, or 0 on format
/// failure / partial write to a closed fd.
size_t write_heartbeat_ndjson(int fd, long rss_kb);

/// Write the canonical RSS-limit-exceeded stderr line to `fd` via raw
/// write(2). The string is fixed and distinctive ("child RSS limit
/// exceeded") so the daemon's last_stderr_line capture can surface it
/// in the user-facing error.
void write_rss_limit_msg(int fd);

/// Extract date/time from a directory name matching "YYYY-MM-DD_HH-MM" pattern.
/// Falls back to file mtime of audio_path, then to current time.
/// Returns {date_str, time_str} like {"2026-02-15", "14:30"}.
std::pair<std::string, std::string> resolve_meeting_time(
    const fs::path& out_dir, const fs::path& audio_path);

// ---------------------------------------------------------------------------
// systemd property line parser (T1C.2)
// ---------------------------------------------------------------------------

/// Parse a single line of `systemctl show -p MemoryX` output, of the form
/// "MemoryHigh=<value>\n". Returns LONG_MAX for "infinity", >=0 for byte
/// counts, or -1 on malformed input. Pure parsing — no I/O. Exposed for
/// unit testing of the daemon's MemoryHigh restore path.
long parse_memory_property_line(const char* line);

} // namespace recmeet
