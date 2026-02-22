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

/// Create timestamped output directory under base_dir, e.g. meetings/2026-02-20_14-30/
fs::path create_output_dir(const fs::path& base_dir);

// ---------------------------------------------------------------------------
// Default device pattern
// ---------------------------------------------------------------------------

constexpr const char* DEFAULT_DEVICE_PATTERN = "bd.h200|00:05:30:00:05:4E";

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

} // namespace recmeet
