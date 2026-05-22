// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 1 of captions-mid-recording-ipc-verb (rev 4).
//
// A small, mutex-guarded request channel that bridges the IPC poll thread
// (where the `captions.start_engine` verb handler runs) and the recording
// worker thread (the only thread allowed to construct a CaptionEngine via
// `try_start_caption_engine`, which is anonymous-namespace-scoped to
// pipeline.cpp).
//
// The channel tracks three independent flags (all guarded by an internal
// mutex):
//   - engine_running:   a CaptionEngine is currently running for the
//                       in-flight recording.
//   - request_pending:  a verb-side request to start the engine has been
//                       queued but not yet drained by the worker.
//   - is_worker_active: the recording worker is currently inside its 200 ms
//                       poll loop (i.e. between mark_worker_active() and
//                       clear_worker_active()). Closes the race window
//                       between `g_rec_stop.request()` and
//                       `g_recording.store(false)`.
//
// See the rev-4 plan ("Channel design") for the full state table.
//
// Tests MUST call reset_caption_start_channel() between TEST_CASEs (a
// Catch2 fixture is fine). The channel state is file-static and otherwise
// leaks across test boundaries.

#pragma once

#include <functional>
#include <string>

namespace recmeet {

/// Result of a verb-side request to start the caption engine.
enum class CaptionStartRequestResult {
    AlreadyRunning,   ///< engine already running for this recording.
    Queued,           ///< request accepted; worker will start engine on
                      ///< next 200 ms tick (or coalesces with an earlier
                      ///< pending request).
    WorkerNotReady,   ///< worker is not in its polling state (between
                      ///< recordings, or shutting down). Verb caller
                      ///< should respond with NotRecording.
};

/// Verb-side entrypoint. Non-blocking; returns immediately.
///
/// Concurrent requests coalesce: if a request is already pending, this
/// returns Queued and the second call's `caption_model_override` is
/// IGNORED (the first request's override wins). When the dropped override
/// is non-empty and differs from the pending one, the channel logs a
/// log_warn so debugging surfaces the coalesce.
CaptionStartRequestResult request_caption_engine_start(
    const std::string& caption_model_override);

/// Worker-side drain. Called by the recording worker on each 200 ms loop
/// tick. If a request is pending AND the worker is active AND no engine
/// is currently running, drains the request (atomically clearing the
/// pending flag), invokes `start_fn` outside the channel mutex (it may
/// block for 1-2 s loading the model), and updates internal state based
/// on the result.
///
/// `start_fn` signature: `(model_override) -> bool`; returns true on
/// successful engine construction. On success the channel atomically
/// transitions to `(engine_running=true, request_pending=false)` in a
/// single critical section. On failure the channel leaves
/// `engine_running=false` so a subsequent verb request can retry.
void poll_and_handle_caption_start_request(
    std::function<bool(const std::string&)> start_fn);

/// Worker-side marker called after a successful engine start (whether at
/// record.start setup time or mid-recording from
/// `poll_and_handle_caption_start_request`'s success path). Sets
/// `engine_running=true` and clears any pending request (coalesces a
/// verb that fired during setup — the engine is already up, so the
/// pending request is satisfied).
void mark_caption_engine_running();

/// Worker-side marker. Called IMMEDIATELY before entering the 200 ms
/// loop. Sets `is_worker_active=true`. After this,
/// `request_caption_engine_start` is willing to return Queued.
void mark_worker_active();

/// Worker-side marker. Called IMMEDIATELY after exiting the 200 ms loop
/// (before any teardown — cap.stop / caption.reset / drain). Sets
/// `is_worker_active=false`. After this,
/// `request_caption_engine_start` returns WorkerNotReady regardless of
/// `g_recording`'s value, closing the race window between
/// `g_rec_stop.request()` and `g_recording.store(false)`.
void clear_worker_active();

/// Reset all three flags (`engine_running`, `request_pending`,
/// `is_worker_active`) to false and clear the pending model override.
/// Called at `run_recording` entry AND exit, and also by tests in a
/// Catch2 fixture to clear cross-test state. No CV signaling — the
/// channel has no waiters.
void reset_caption_start_channel();

} // namespace recmeet
