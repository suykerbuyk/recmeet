// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Cancel & Discard (test-and-verification-hardening item #4) — pure
// decision helper for the tray-side click-to-confirm cancel gesture.
//
// Carved out of `tray.cpp` so the state-machine arithmetic (now-vs-armed
// comparison + 3 s window math) is unit-testable without booting GTK or
// fabricating a `TrayState`. The tray glues the decision into its menu
// activate handler:
//
//   * First click (Idle):   flip label, arm `cancel_armed_until_ms`,
//                           schedule `g_timeout_add` revert.
//   * Second click (Armed): disarm + fire `cancel_capture()` (no IPC —
//                           cancel is tray-local because the daemon is
//                           not involved before `process.submit`, and
//                           `process.submit` only runs after the user
//                           picks Submit from the post-Stop disposition
//                           dialog).
//   * Timer expiry:         revert label, disarm.
//
// Mirrors the structure of `tray_status.h` and the v1 reference at
// commit `d2d6dc5` (which extracted the same helper for the same
// reason). v2 cancel is tray-local — no `record.cancel` IPC verb is
// added because the daemon does not own the in-progress capture state.

#pragma once

#include <cstdint>

namespace recmeet {

/// Three-second click-to-confirm window. First click on the menu item
/// flips the label to a confirmation prompt and arms `cancel_armed_until_ms`
/// to `now + CANCEL_ARMED_WINDOW_MS`. Second click while still inside the
/// window fires the cancel; a click after the window expires re-arms
/// rather than firing (the operator must re-confirm). 3 s matches v1's
/// `d2d6dc5` and is short enough to forbid an accidental double-click
/// from cancelling a recording the operator is actively monitoring.
inline constexpr int64_t CANCEL_ARMED_WINDOW_MS = 3000;

/// Revert-timer schedule, 100 ms past the strict armed-window expiry.
/// The extra 100 ms gives the timer callback a small grace window in
/// which the GTK main loop can run; without it, a `g_timeout_add(3000,
/// ...)` revert could race a click that landed at exactly t=3000 and
/// flip the label back before the click handler ran. With 3100 ms the
/// click at t=3000 still sees `Armed` (the timer hasn't fired yet) and
/// the timer at t=3100 sees `Idle` (the click handler already disarmed).
inline constexpr int64_t CANCEL_REVERT_TIMER_MS = 3100;

/// Idle / Armed binary state. The label text and the click-handler
/// branch (arm vs fire) both gate on this enum. Returned by
/// `decide_cancel_button_state`.
enum class CancelButtonState {
    Idle,    ///< No outstanding arm. First click → arm.
    Armed,   ///< `cancel_armed_until_ms > now`. Second click → fire.
};

/// Decide whether the cancel-discard menu item is currently Idle or
/// Armed, given the current monotonic-time milliseconds and the time at
/// which the arm expires.
///
/// `cancel_armed_until_ms == 0` is the canonical Idle sentinel — the
/// arm has never been set, OR it was reset by the revert timer / click
/// handler. Any non-zero value strictly greater than `now_ms` is Armed;
/// any non-zero value <= `now_ms` (the arm expired but the revert timer
/// has not yet cleared the field — a transient state during the 100 ms
/// grace window) is treated as Idle so a click landing in that window
/// re-arms rather than fires.
///
/// Pure function: no globals, no GTK, no time queries. Same shape as
/// `decide_caption_toggle_action` in v1.
inline CancelButtonState decide_cancel_button_state(int64_t now_ms,
                                                    int64_t cancel_armed_until_ms) {
    if (cancel_armed_until_ms <= 0) return CancelButtonState::Idle;
    if (cancel_armed_until_ms > now_ms) return CancelButtonState::Armed;
    return CancelButtonState::Idle;
}

/// Menu label rendered when the cancel item is Idle. "Cancel & Discard"
/// matches v1's `d2d6dc5` and reads cleanly under the "Stop Recording"
/// row in the tray menu. The "& Discard" suffix is load-bearing — it
/// tells the operator that cancel does NOT keep the WAV in staging the
/// way Save-for-later does.
inline constexpr const char* CANCEL_LABEL_IDLE =
    "Cancel & Discard";

/// Menu label rendered when the cancel item is Armed. Phrased as an
/// imperative question so a casual hover-over operator immediately
/// understands the next click is the irreversible step. Mirrors v1.
inline constexpr const char* CANCEL_LABEL_ARMED =
    "Discard recording? click again to confirm";

} // namespace recmeet
