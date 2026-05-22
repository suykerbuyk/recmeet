// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 3 of captions-mid-recording-ipc-verb (rev 4) — pure-function
// extraction of the captions-toggle menu callback's state-machine decision.
//
// The GTK callback in tray.cpp is a thin dispatcher that asks this function
// what to do based on four bools (new desired state + current recording /
// engine / start-pending bits), then performs the side effects. Keeping the
// decision in a pure function makes it testable without GTK, libconfig, or
// IPC dependencies.
//
// State table (case 1 wins ties — order matters):
//
//   recording=F                                      → PersistOnly
//   recording=T, new=T, engine_started=T             → ShowImmediately
//   recording=T, new=T, !engine_started, start_pending → NoOp (in-flight)
//   recording=T, new=T, !engine_started, !start_pending → FireVerb
//   recording=T, new=F                                → HideImmediately
//
// Persistence happens unconditionally in every branch via the GTK callback —
// this enum describes ONLY the engine/overlay side effects. The
// PersistOnly variant is the no-side-effects-on-engine/overlay case (the
// caller still persists cfg.captions_enabled).
//
// "Engine started for this recording" tracks whether the daemon emitted
// `caption.started` for the current recording (set by the tray's
// caption.started event handler). "start_pending" tracks whether a verb
// call's response said "queued" and the matching caption.started or
// caption.degraded (engine_error) event has not yet arrived.

#pragma once

namespace recmeet {

enum class CaptionToggleAction {
    PersistOnly,       ///< Not recording — persist new state; no IPC / no overlay change.
    FireVerb,          ///< Recording, toggle ON, engine not yet started, no in-flight verb: send captions.start_engine.
    ShowImmediately,   ///< Recording, toggle ON, engine already running: show overlay; no IPC.
    HideImmediately,   ///< Recording, toggle OFF: hide overlay; no IPC.
    NoOp,              ///< Recording, toggle ON, in-flight verb already pending: do nothing extra.
};

/// Decide what to do when the captions menu item is toggled. Pure function:
/// no GTK, no IPC, no filesystem — fully unit-testable.
inline CaptionToggleAction decide_caption_toggle_action(bool new_state,
                                                        bool recording,
                                                        bool engine_started,
                                                        bool start_pending) {
    if (!recording) {
        return CaptionToggleAction::PersistOnly;
    }
    if (!new_state) {
        // Toggle OFF mid-recording always hides immediately; the engine
        // keeps running until record.stop per the monotonic-engine rule.
        return CaptionToggleAction::HideImmediately;
    }
    // Toggle ON mid-recording — decide based on engine + pending state.
    if (engine_started) {
        return CaptionToggleAction::ShowImmediately;
    }
    if (start_pending) {
        return CaptionToggleAction::NoOp;
    }
    return CaptionToggleAction::FireVerb;
}

} // namespace recmeet
