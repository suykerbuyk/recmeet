// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Unit tests for the pure decision helper `decide_recording_exit`.
// Mirrors the operator framing in cancel-recording-and-discard rev 4:
// "the loop knows which signal fired" — and cancel beats stop when both
// signals are set (operator's late-cancel-after-stop should still discard).

#include <catch2/catch_test_macros.hpp>

#include "pipeline_exit.h"

using namespace recmeet;

TEST_CASE("decide_recording_exit: neither signalled → NormalDrain",
          "[pipeline][cancel]") {
    CHECK(decide_recording_exit(/*stop=*/false, /*cancel=*/false)
          == RecordingExitAction::NormalDrain);
}

TEST_CASE("decide_recording_exit: stop only → NormalDrain",
          "[pipeline][cancel]") {
    CHECK(decide_recording_exit(/*stop=*/true, /*cancel=*/false)
          == RecordingExitAction::NormalDrain);
}

TEST_CASE("decide_recording_exit: cancel only → CancelCleanup",
          "[pipeline][cancel]") {
    CHECK(decide_recording_exit(/*stop=*/false, /*cancel=*/true)
          == RecordingExitAction::CancelCleanup);
}

TEST_CASE("decide_recording_exit: both signalled → CancelCleanup (cancel wins)",
          "[pipeline][cancel]") {
    CHECK(decide_recording_exit(/*stop=*/true, /*cancel=*/true)
          == RecordingExitAction::CancelCleanup);
}
