#pragma once

#include "util.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace recmeet {

/// PipeWire pw_stream capture. Captures S16LE mono 16kHz audio from a named source.
class PipeWireCapture {
public:
    /// Construct a capture targeting the given PipeWire source name.
    /// For sink monitors, set capture_sink=true to use STREAM_CAPTURE_SINK property.
    explicit PipeWireCapture(const std::string& target, bool capture_sink = false);
    ~PipeWireCapture();

    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    /// Start capturing. Non-blocking — audio accumulates in internal buffer.
    void start();

    /// Stop capturing and tear down the PipeWire stream.
    void stop();

    /// Drain all accumulated samples from the buffer.
    std::vector<int16_t> drain();

    /// Check if the stream is actively capturing.
    bool is_running() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace recmeet
