// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Pure decision helper for `run_recording`'s post-loop branch: did the
// recording loop exit because of a normal stop (drain + write + validate)
// or because of an operator-issued cancel (skip drain + discard the
// freshly-minted output directory)?
//
// Mirrors the operator framing from the cancel-recording-and-discard plan:
// "the loop knows which signal fired." When both signals are set the
// operator-issued cancel wins — `record.cancel` arriving racy-late with
// `record.stop` should still discard, never persist a half-meaning recording.

#pragma once

namespace recmeet {

enum class RecordingExitAction {
    NormalDrain,    ///< Drain captures, write WAVs, validate, return PostprocessInput.
    CancelCleanup,  ///< Skip drain/write/validate; discard the output directory.
};

/// Pick the post-loop branch based on which stop tokens were signalled.
/// Cancel beats stop when both bits are set — see plan section "Race-handling
/// matrix" row "Cancel + record.stop both arrive in-loop".
inline RecordingExitAction decide_recording_exit(bool stop_requested,
                                                  bool cancel_requested) {
    if (cancel_requested) return RecordingExitAction::CancelCleanup;
    (void)stop_requested;  // value irrelevant once cancel is ruled out
    return RecordingExitAction::NormalDrain;
}

} // namespace recmeet
