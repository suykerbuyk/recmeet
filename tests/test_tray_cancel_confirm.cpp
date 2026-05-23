// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Unit tests for the pure decision helper `decide_cancel_button_state`,
// the arm/confirm state machine for the tray "Cancel & Discard" menu item.
// See src/tray_cancel_confirm.h for the contract.

#include <catch2/catch_test_macros.hpp>

#include "tray_cancel_confirm.h"

using namespace recmeet;

TEST_CASE("decide_cancel_button_state: default-zero armed_until → Idle",
          "[tray][cancel]") {
    // armed_until_ms == 0 is the default-initialized state and must
    // always return Idle, independent of now_ms.
    CHECK(decide_cancel_button_state(/*now=*/100000, /*armed_until=*/0)
          == CancelButtonState::Idle);
}

TEST_CASE("decide_cancel_button_state: in-window → Armed",
          "[tray][cancel]") {
    // now=100000, armed_until=103000 — strictly inside the 3000ms window.
    CHECK(decide_cancel_button_state(/*now=*/100000, /*armed_until=*/103000)
          == CancelButtonState::Armed);
}

TEST_CASE("decide_cancel_button_state: boundary (now == armed_until) → Idle",
          "[tray][cancel]") {
    // Contract: the comparison is strictly less-than. At exactly
    // now_ms == armed_until_ms the window has expired, returning Idle.
    // The armed window is the half-open interval [arm_time, arm_time + 3000).
    CHECK(decide_cancel_button_state(/*now=*/103000, /*armed_until=*/103000)
          == CancelButtonState::Idle);
}

TEST_CASE("decide_cancel_button_state: expired (now > armed_until) → Idle",
          "[tray][cancel]") {
    CHECK(decide_cancel_button_state(/*now=*/104000, /*armed_until=*/103000)
          == CancelButtonState::Idle);
}
