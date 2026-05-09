// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 2 — Streaming ASR engine.
//
// Owns a sherpa-onnx streaming recognizer + a single-producer / single-consumer
// ring buffer + an ASR worker thread. Producer is the audio capture thread
// (PipeWire RT-promoted); consumer is the worker thread that feeds the
// recognizer and emits CaptionResult callbacks.
//
// Lifecycle:
//   CaptionEngine eng;
//   eng.start(opts, on_result, ud, on_degraded, ud);   // spawns worker
//   capture.set_audio_callback(&CaptionEngine::on_audio_chunk, &eng);  // wire producer
//   ... audio flows ...
//   capture.set_audio_callback(nullptr, nullptr);      // stop the producer first
//   eng.stop();                                        // joins worker, frees recognizer
//
// Threading contract:
//   - `on_audio_chunk()` (producer) is lock-free, non-allocating, non-logging.
//     Safe to call from a PipeWire RT-promoted thread.
//   - `start()` / `stop()` / dtor serialize via an internal mutex and are
//     re-entrant safe (second `stop()` is a no-op).
//   - `is_running()` / `last_error()` are safe to call concurrently with
//     callbacks (atomics behind the API).
//
// When RECMEET_USE_SHERPA is OFF the class compiles to stubs:
//   - `start()` returns false and sets last_error()
//     to "captions require RECMEET_USE_SHERPA=ON build".
//   - All other methods are trivially safe no-ops.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace recmeet {

/// One recognizer output. `text` is the engine's raw hypothesis (ALL-CAPS for
/// the default sherpa-onnx-streaming-zipformer-en-2023-06-26 zipformer locked
/// in Phase 0.2). `is_partial=true` means a hypothesis update mid-utterance;
/// `is_partial=false` means an endpoint was detected and the hypothesis is
/// final for this utterance.
struct CaptionResult {
    std::string text;
    bool is_partial = true;
    int64_t timestamp_ms = 0;  ///< wall-clock since CaptionEngine::start()
};

/// Reasons we may emit a degraded-mode signal. Producer-side overflow is the
/// only one wired in Phase 2; future causes (ModelStarvation, etc.) may join.
enum class CaptionDegradedReason {
    BufferOverrun,
};

using CaptionResultCallback   = void(*)(const CaptionResult& r, void* userdata);
using CaptionDegradedCallback = void(*)(CaptionDegradedReason r, void* userdata);

/// Optional test seam for the worker-thread scheduling syscall. The default
/// implementation calls sched_setscheduler(SCHED_BATCH); on EPERM it falls
/// back to nice(+10). Tests inject a stub via CaptionEngine::Options to drive
/// the EPERM-fallback path deterministically.
///
/// Returns:
///   0  = SCHED_BATCH was applied (no-op fallback needed).
///   1  = nice(+10) fallback was applied (EPERM observed).
///  -1  = both attempts failed (still safe — engine continues with default
///        scheduling). The engine logs at DEBUG only; never WARN/ERROR.
using SchedulerSetter = int (*)(void* userdata);

class CaptionEngine {
public:
    struct Options {
        std::string model_dir;       ///< e.g. ~/.local/share/recmeet/models/sherpa/online/en-2023-06-26/
        int num_threads = 1;         ///< capped at min(2, this) inside start()
        std::string decoding_method = "greedy_search";
        int32_t sample_rate = 16000;
        bool enable_endpoint = true;

        /// Test seam — when non-null, used in place of the default
        /// sched_setscheduler/nice fallback. The engine is the sole caller.
        SchedulerSetter scheduler_setter = nullptr;
        void* scheduler_setter_userdata = nullptr;

        /// Test seam — when > 0, ring-buffer is sized to this many int16
        /// samples (rounded up to a power of two). Defaults to ~2 seconds at
        /// 16 kHz mono = 32768 samples.
        std::size_t ring_capacity_override = 0;

        /// Test seam — when > 0, the worker uses this many milliseconds as
        /// its idle/poll interval instead of the default. Lets tests drive
        /// the consumer loop on a tight cadence.
        int worker_poll_ms_override = 0;

        /// Test seam — when true, start() skips creating the sherpa-onnx
        /// recognizer and stream entirely. The worker thread still runs and
        /// drains the ring buffer (discarding samples) so backpressure /
        /// drop-watermark / scheduler-setter / RAII paths remain exercisable
        /// without a real model on disk. Production code never sets this.
        bool _no_recognizer_for_test = false;
    };

    CaptionEngine();
    ~CaptionEngine();
    CaptionEngine(const CaptionEngine&) = delete;
    CaptionEngine& operator=(const CaptionEngine&) = delete;

    /// Start the recognizer + worker thread. Returns true on success;
    /// on failure, `last_error()` carries a human-readable message.
    bool start(const Options& opts,
               CaptionResultCallback on_result, void* result_userdata,
               CaptionDegradedCallback on_degraded, void* degraded_userdata);

    /// Producer-side audio sink. Compatible with AudioChunkCallback. Call as:
    ///   capture.set_audio_callback(&CaptionEngine::on_audio_chunk, engine_ptr);
    /// Lock-free, non-allocating, non-logging. Drops oldest samples on
    /// overflow and sets an atomic flag the worker reads.
    static void on_audio_chunk(const int16_t* samples, std::size_t n, void* engine_ptr);

    /// Test-only: same body as on_audio_chunk() but on an instance, returns
    /// the number of samples dropped due to overflow. Production code must
    /// use on_audio_chunk() so its address is stable for set_audio_callback.
    std::size_t _push_samples_for_test(const int16_t* samples, std::size_t n);

    /// Test-only: number of samples currently sitting in the ring buffer.
    /// Atomic, safe to call from any thread.
    std::size_t _ring_occupancy_for_test() const;

    /// Test-only: ring buffer capacity (in samples).
    std::size_t _ring_capacity_for_test() const;

    /// Stop the worker thread, join, destroy recognizer + stream. Idempotent.
    /// Caller is responsible for unsubscribing the producer-side callback
    /// from the capture instance before (or shortly after) calling stop().
    void stop();

    /// True between start() success and stop().
    bool is_running() const;

    /// Last error message; empty if no error has been observed.
    std::string last_error() const;

    /// Number of degraded events emitted since start(). Test introspection.
    std::size_t _degraded_events_emitted_for_test() const;

    /// Effective num_threads after the min(2, ...) cap. Test introspection.
    int _effective_num_threads_for_test() const;

    // Public for the .cpp's free-function helpers (worker thread, ring drain).
    // The struct is fully defined inside caption_engine.cpp; consumers see
    // only the forward declaration here. The unique_ptr keeps the dtor a
    // single-source-of-truth for cleanup.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace recmeet
