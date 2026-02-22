#pragma once

#include "util.h"

#include <atomic>
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

private:
    std::string source_;
    std::thread thread_;
    std::mutex buf_mtx_;
    std::vector<int16_t> buffer_;
    StopToken stop_;
    std::atomic<bool> running_{false};
};

} // namespace recmeet
