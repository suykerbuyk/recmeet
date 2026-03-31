// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "audio_monitor.h"
#include "log.h"

#include <pulse/simple.h>
#include <pulse/error.h>

#include <sys/syscall.h>
#include <unistd.h>

namespace recmeet {

PulseMonitorCapture::PulseMonitorCapture(const std::string& source)
    : source_(source) {}

PulseMonitorCapture::~PulseMonitorCapture() {
    stop();
}

void PulseMonitorCapture::start() {
    log_debug("pa-monitor: start ENTER (source=%s)", source_.c_str());
    stop_.reset();
    running_ = true;

    thread_ = std::thread([this] {
        log_debug("pa-monitor: worker ENTER (tid=%d)", (int)syscall(SYS_gettid));
        pa_sample_spec ss = {};
        ss.format = PA_SAMPLE_S16LE;
        ss.rate = SAMPLE_RATE;
        ss.channels = CHANNELS;

        int error = 0;
        pa_simple* s = pa_simple_new(
            nullptr,              // server
            "recmeet",            // name
            PA_STREAM_RECORD,
            source_.c_str(),      // device
            "monitor-capture",    // stream name
            &ss,
            nullptr,              // channel map
            nullptr,              // buffer attributes
            &error);

        if (!s) {
            log_error("pa_simple_new failed: %s", pa_strerror(error));
            running_ = false;
            return;
        }
        log_debug("pa-monitor: connected to PulseAudio");

        // Read in chunks of ~100ms
        constexpr size_t chunk_samples = SAMPLE_RATE / 10;
        int16_t chunk[chunk_samples];

        while (!stop_.stop_requested()) {
            if (pa_simple_read(s, chunk, sizeof(chunk), &error) < 0) {
                log_error("pa_simple_read failed: %s", pa_strerror(error));
                break;
            }
            std::lock_guard lk(buf_mtx_);
            buffer_.insert(buffer_.end(), chunk, chunk + chunk_samples);
            // Warn once when buffer exceeds ~120 minutes of audio (230 MB)
            constexpr size_t WARN_SAMPLES = SAMPLE_RATE * 60 * 120;
            if (buffer_.size() >= WARN_SAMPLES &&
                buffer_.size() - chunk_samples < WARN_SAMPLES) {
                log_warn("Audio buffer exceeds 120 minutes (%.0f MB). "
                        "Memory usage will continue to grow.",
                        buffer_.size() * sizeof(int16_t) / (1024.0 * 1024.0));
            }
        }

        pa_simple_free(s);
        {
            std::lock_guard lk(buf_mtx_);
            log_debug("pa-monitor: worker EXIT (buffered=%zu samples)", buffer_.size());
        }
        running_ = false;
    });
}

void PulseMonitorCapture::stop() {
    log_debug("pa-monitor: stop ENTER");
    stop_.request();
    if (thread_.joinable())
        thread_.join();
    running_ = false;
    log_debug("pa-monitor: stop EXIT (thread joined)");
}

std::vector<int16_t> PulseMonitorCapture::drain() {
    std::lock_guard lk(buf_mtx_);
    std::vector<int16_t> out;
    out.swap(buffer_);
    return out;
}

bool PulseMonitorCapture::is_running() const {
    return running_;
}

} // namespace recmeet
