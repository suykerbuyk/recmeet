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

/// Find the audio file in a meeting directory.
/// Prefers audio_YYYY-MM-DD_HH-MM.wav, falls back to audio.wav.
/// Returns empty path if dir doesn't exist or no audio file found.
fs::path find_audio_file(const fs::path& dir);

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

/// Extract date/time from a directory name matching "YYYY-MM-DD_HH-MM" pattern.
/// Falls back to file mtime of audio_path, then to current time.
/// Returns {date_str, time_str} like {"2026-02-15", "14:30"}.
std::pair<std::string, std::string> resolve_meeting_time(
    const fs::path& out_dir, const fs::path& audio_path);

} // namespace recmeet
