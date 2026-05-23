// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 2 of cancel-recording-and-discard (rev 4) — pure-function
// extraction of the tray "Cancel & Discard" menu item's arm/confirm
// state machine.
//
// The GTK callback in tray.cpp is a thin dispatcher that asks this
// function whether the menu item is currently in the Idle (first-click
// arms) or Armed (second-click confirms) state, then performs the side
// effects (label flip, IPC dispatch). Keeping the decision in a pure
// function makes it testable without GTK, GLib timers, or IPC
// dependencies — mirrors the decide_caption_toggle_action pattern.
//
// Contract:
//
//   armed_until_ms == 0 (default-initialized state)  → Idle
//   armed_until_ms >  0, now_ms <  armed_until_ms    → Armed
//   armed_until_ms >  0, now_ms >= armed_until_ms    → Idle (window expired)
//
// The boundary is strictly less-than: at exactly now_ms == armed_until_ms
// the window has expired. This matches the operator framing — the armed
// window is the half-open interval [arm_time, arm_time + 3000) ms.

#pragma once

#include <cstdint>

namespace recmeet {

enum class CancelButtonState {
    Idle,    ///< default — first click should arm
    Armed,   ///< within the 3000ms confirmation window — second click confirms
};

/// Returns Armed if now_ms < armed_until_ms, else Idle.
/// `armed_until_ms == 0` (the default-initialized state) always returns Idle.
inline CancelButtonState decide_cancel_button_state(int64_t now_ms,
                                                    int64_t armed_until_ms) {
    if (armed_until_ms <= 0) return CancelButtonState::Idle;
    if (now_ms < armed_until_ms) return CancelButtonState::Armed;
    return CancelButtonState::Idle;
}

} // namespace recmeet
