// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 3 of captions-mid-recording-ipc-verb (rev 4).
//
// Unit tests for `decide_caption_toggle_action` — the pure-function
// extraction of the captions menu callback's state-machine decision. The
// function takes four bools and returns one of five enum values; these
// tests cover every reachable outcome.

#include <catch2/catch_test_macros.hpp>

#include "tray_caption_toggle.h"

using namespace recmeet;

TEST_CASE("decide_caption_toggle_action: not recording → PersistOnly",
          "[tray-caption-toggle]") {
    // Regardless of toggle target or other flags, when not recording the
    // operator's gesture only affects the persisted preference. The GTK
    // callback persists the new value; no IPC traffic; no overlay change.
    CHECK(decide_caption_toggle_action(/*new=*/true,  /*rec=*/false,
                                       /*engine=*/false, /*pending=*/false)
          == CaptionToggleAction::PersistOnly);
    CHECK(decide_caption_toggle_action(/*new=*/false, /*rec=*/false,
                                       /*engine=*/false, /*pending=*/false)
          == CaptionToggleAction::PersistOnly);
    // Engine/pending flags are stale leftover state when not recording —
    // the function ignores them in this branch.
    CHECK(decide_caption_toggle_action(/*new=*/true,  /*rec=*/false,
                                       /*engine=*/true,  /*pending=*/true)
          == CaptionToggleAction::PersistOnly);
    CHECK(decide_caption_toggle_action(/*new=*/false, /*rec=*/false,
                                       /*engine=*/true,  /*pending=*/false)
          == CaptionToggleAction::PersistOnly);
}

TEST_CASE("decide_caption_toggle_action: recording, toggle OFF → HideImmediately",
          "[tray-caption-toggle]") {
    // Toggle OFF mid-recording is local-only (engine keeps running per the
    // monotonic-engine rule). All combinations of engine_started /
    // start_pending route to HideImmediately when new_state=false.
    CHECK(decide_caption_toggle_action(/*new=*/false, /*rec=*/true,
                                       /*engine=*/false, /*pending=*/false)
          == CaptionToggleAction::HideImmediately);
    CHECK(decide_caption_toggle_action(/*new=*/false, /*rec=*/true,
                                       /*engine=*/true,  /*pending=*/false)
          == CaptionToggleAction::HideImmediately);
    CHECK(decide_caption_toggle_action(/*new=*/false, /*rec=*/true,
                                       /*engine=*/false, /*pending=*/true)
          == CaptionToggleAction::HideImmediately);
    CHECK(decide_caption_toggle_action(/*new=*/false, /*rec=*/true,
                                       /*engine=*/true,  /*pending=*/true)
          == CaptionToggleAction::HideImmediately);
}

TEST_CASE("decide_caption_toggle_action: recording, ON, engine running → ShowImmediately",
          "[tray-caption-toggle]") {
    // Toggle ON when the engine is already running for this recording: no
    // IPC, just re-show the overlay window with whatever state has been
    // accumulated. start_pending is impossible in production once engine
    // is up (the caption.started event clears it), but the table still
    // tolerates the combination.
    CHECK(decide_caption_toggle_action(/*new=*/true,  /*rec=*/true,
                                       /*engine=*/true,  /*pending=*/false)
          == CaptionToggleAction::ShowImmediately);
    CHECK(decide_caption_toggle_action(/*new=*/true,  /*rec=*/true,
                                       /*engine=*/true,  /*pending=*/true)
          == CaptionToggleAction::ShowImmediately);
}

TEST_CASE("decide_caption_toggle_action: recording, ON, no engine, no pending → FireVerb",
          "[tray-caption-toggle]") {
    // The canonical mid-recording start case: operator wants captions, the
    // engine has not yet been built, and no verb is in flight. Tray fires
    // captions.start_engine and pre-shows the overlay with a placeholder.
    CHECK(decide_caption_toggle_action(/*new=*/true,  /*rec=*/true,
                                       /*engine=*/false, /*pending=*/false)
          == CaptionToggleAction::FireVerb);
}

TEST_CASE("decide_caption_toggle_action: recording, ON, no engine, pending → NoOp",
          "[tray-caption-toggle]") {
    // Operator double-clicked while the first verb's response is still
    // in flight (or while we're waiting on caption.started / caption.degraded).
    // GTK has already flipped the check mark by convention; the dispatcher
    // simply does no extra side effects. The in-flight verb's resolution
    // will reconcile state via the event handler.
    CHECK(decide_caption_toggle_action(/*new=*/true,  /*rec=*/true,
                                       /*engine=*/false, /*pending=*/true)
          == CaptionToggleAction::NoOp);
}
