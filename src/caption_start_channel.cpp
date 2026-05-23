// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// See caption_start_channel.h for the design contract.

#include "caption_start_channel.h"

#include "log.h"

#include <mutex>
#include <string>
#include <utility>

namespace recmeet {

namespace {

struct ChannelState {
    std::mutex mu;
    bool engine_running = false;
    bool request_pending = false;
    bool is_worker_active = false;
    std::string pending_model_override;
};

ChannelState g_chan;

} // anonymous namespace

CaptionStartRequestResult request_caption_engine_start(
        const std::string& caption_model_override) {
    std::lock_guard<std::mutex> lk(g_chan.mu);
    if (!g_chan.is_worker_active) {
        return CaptionStartRequestResult::WorkerNotReady;
    }
    if (g_chan.engine_running) {
        return CaptionStartRequestResult::AlreadyRunning;
    }
    if (g_chan.request_pending) {
        if (!caption_model_override.empty() &&
            caption_model_override != g_chan.pending_model_override) {
            log_warn("captions: coalescing start_engine request "
                     "dropped non-matching override '%s' (pending '%s')",
                     caption_model_override.c_str(),
                     g_chan.pending_model_override.c_str());
        }
        return CaptionStartRequestResult::Queued;
    }
    g_chan.request_pending = true;
    g_chan.pending_model_override = caption_model_override;
    return CaptionStartRequestResult::Queued;
}

void poll_and_handle_caption_start_request(
        std::function<bool(const std::string&)> start_fn) {
    std::string override_model;
    {
        std::lock_guard<std::mutex> lk(g_chan.mu);
        if (!g_chan.is_worker_active) return;
        if (g_chan.engine_running) {
            // Defense: if the engine was marked running while a request
            // was pending, the pending flag should already be cleared by
            // mark_caption_engine_running(). Belt-and-braces clear here
            // too.
            g_chan.request_pending = false;
            g_chan.pending_model_override.clear();
            return;
        }
        if (!g_chan.request_pending) return;
        override_model = std::move(g_chan.pending_model_override);
        g_chan.pending_model_override.clear();
        g_chan.request_pending = false;
    }

    // Engine init runs OUTSIDE the channel mutex (1-2 s blocking).
    const bool ok = start_fn ? start_fn(override_model) : false;

    {
        std::lock_guard<std::mutex> lk(g_chan.mu);
        if (ok) {
            // Atomic transition to (T, F, T). If a verb fired during
            // start_fn, its pending=true is now cleared — the engine is
            // up, so the request is satisfied by the in-flight start.
            g_chan.engine_running = true;
            g_chan.request_pending = false;
            g_chan.pending_model_override.clear();
        }
        // On failure, leave engine_running=false. A pending request
        // that arrived during start_fn will be retried on the next
        // worker tick — that's the documented retry path. Operator's
        // repeat click after seeing failure expresses intent.
    }
}

void mark_caption_engine_running() {
    std::lock_guard<std::mutex> lk(g_chan.mu);
    g_chan.engine_running = true;
    // Drop any in-flight request — the engine is already up, so the
    // pending request is implicitly satisfied. Also covers the race
    // where a verb call lands during record.start engine setup.
    g_chan.request_pending = false;
    g_chan.pending_model_override.clear();
}

void mark_worker_active() {
    std::lock_guard<std::mutex> lk(g_chan.mu);
    g_chan.is_worker_active = true;
}

void clear_worker_active() {
    std::lock_guard<std::mutex> lk(g_chan.mu);
    g_chan.is_worker_active = false;
}

void reset_caption_start_channel() {
    std::lock_guard<std::mutex> lk(g_chan.mu);
    g_chan.engine_running = false;
    g_chan.request_pending = false;
    g_chan.is_worker_active = false;
    g_chan.pending_model_override.clear();
}

} // namespace recmeet
