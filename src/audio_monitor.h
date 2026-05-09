// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "audio_capture.h"  // for AudioChunkCallback typedef
#include "util.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace recmeet {

/// PulseAudio pa_simple fallback for .monitor sources.
/// Used when PipeWire CAPTURE_SINK fails (e.g., Bluetooth sinks).
class PulseMonitorCapture {
public:
    explicit PulseMonitorCapture(const std::string& source);
    ~PulseMonitorCapture();

    PulseMonitorCapture(const PulseMonitorCapture&) = delete;
    PulseMonitorCapture& operator=(const PulseMonitorCapture&) = delete;

    void start();
    void stop();
    std::vector<int16_t> drain();
    bool is_running() const;

    /// Install a streaming callback. Pass cb=nullptr to clear.
    /// Callback fires for every chunk inserted into the internal buffer.
    /// The samples pointer is valid only for the duration of the call.
    void set_audio_callback(AudioChunkCallback cb, void* userdata);

    // Test-only: directly inject a chunk through the buffer-append + callback
    // dispatch path without opening a PulseAudio stream. Hermetic unit tests
    // use this to exercise the callback wiring; production code never calls it.
    void _inject_for_test(const int16_t* samples, std::size_t n);

private:
    std::string source_;
    std::thread thread_;
    std::mutex buf_mtx_;
    std::vector<int16_t> buffer_;
    StopToken stop_;
    std::atomic<bool> running_{false};
    std::atomic<AudioChunkCallback> cb_{nullptr};
    std::atomic<void*> cb_userdata_{nullptr};
};

} // namespace recmeet
