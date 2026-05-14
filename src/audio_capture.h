// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "util.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace recmeet {

/// C-style audio chunk callback. Invoked from the capture thread for every
/// chunk appended to the internal buffer. Use a function pointer + void*
/// userdata pair (NOT std::function) so the call site allocates nothing —
/// this matters on the PipeWire RT-promoted thread.
///
/// Contract: callback body MUST be lock-free (or try_lock-only) and MUST NOT
/// log or allocate when invoked from PipeWireCapture (which runs on
/// PW_STREAM_FLAG_RT_PROCESS thread). PulseMonitorCapture has no RT
/// constraints but the same signature is used for symmetry.
using AudioChunkCallback = void(*)(const int16_t* samples, std::size_t n, void* userdata);

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

    /// Install a streaming callback. Pass cb=nullptr to clear.
    /// Callback fires for every chunk inserted into the internal buffer.
    /// The samples pointer is valid only for the duration of the call.
    /// See AudioChunkCallback for the RT-safety contract.
    ///
    /// Phase B.1: this is now a thin wrapper around the fan-out
    /// subscriber API below — calling it clears any existing
    /// subscribers and installs `cb` as the single subscriber. Legacy
    /// call sites that need exactly one consumer continue to work
    /// unchanged. New call sites that need multiple consumers (tray's
    /// WAV stager AND C.10's streaming uploader) should use
    /// `add_audio_subscriber` directly.
    void set_audio_callback(AudioChunkCallback cb, void* userdata);

    /// Subscriber handle returned by `add_audio_subscriber`. Opaque
    /// integer issued by the capture instance; values are unique only
    /// within a single capture lifetime. Zero is the never-issued
    /// sentinel: `remove_audio_subscriber(0)` is a no-op.
    using SubscriberHandle = std::uint64_t;

    /// Phase B.1 — register a streaming subscriber. Multiple
    /// subscribers may be registered simultaneously; each receives the
    /// same `(samples, n)` invocation on every chunk inserted into the
    /// internal buffer. The same RT-safety contract from
    /// `AudioChunkCallback` applies (no allocation, no logging, no
    /// blocking).
    ///
    /// Thread-safe: callable from any thread, including while the
    /// capture is running. Returns a handle suitable for
    /// `remove_audio_subscriber`.
    SubscriberHandle add_audio_subscriber(AudioChunkCallback cb, void* userdata);

    /// Phase B.1 — remove a subscriber previously installed via
    /// `add_audio_subscriber`. Passing an unknown / already-removed
    /// handle is a no-op. After this returns, the callback associated
    /// with `handle` is guaranteed not to be invoked again. Safe to
    /// call from any thread.
    void remove_audio_subscriber(SubscriberHandle handle);

    // Test-only: directly drive the buffer-append + callback dispatch path
    // without opening a PipeWire stream. Mirrors the body of on_process()
    // so callback wiring can be unit-tested hermetically. Production code
    // never calls this; the unit-test files are the only callers.
    void _inject_for_test(const int16_t* samples, std::size_t n);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recmeet
