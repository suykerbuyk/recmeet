// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "device_enum.h"

using namespace recmeet;

// These tests require a running PipeWire/PulseAudio server.
// Tagged [integration] so they can be filtered out in CI.

TEST_CASE("list_sources: returns at least one source", "[device_enum][integration]") {
    auto sources = list_sources();
    REQUIRE_FALSE(sources.empty());

    // Every source should have a non-empty name
    for (const auto& src : sources) {
        CHECK_FALSE(src.name.empty());
    }
}

TEST_CASE("list_sources: at least one monitor source exists", "[device_enum][integration]") {
    auto sources = list_sources();
    bool found_monitor = false;
    for (const auto& src : sources) {
        if (src.is_monitor) {
            found_monitor = true;
            break;
        }
    }
    CHECK(found_monitor);
}

TEST_CASE("detect_sources: finds BD-H200 by default pattern", "[device_enum][integration]") {
    // This test is specific to the dev machine with BD-H200 connected
    auto result = detect_sources("bd.h200|00:05:30:00:05:4E");

    // At least one should be found (mic or monitor)
    bool found_any = !result.mic.empty() || !result.monitor.empty();
    if (!found_any) {
        WARN("BD-H200 not connected — skipping device-specific checks");
        return;
    }

    if (!result.mic.empty())
        CHECK(result.mic.find("00:05:30:00:05:4E") != std::string::npos);
    if (!result.monitor.empty())
        CHECK(result.monitor.find("00:05:30:00:05:4E") != std::string::npos);
}

TEST_CASE("detect_sources: non-matching pattern returns empty", "[device_enum][integration]") {
    auto result = detect_sources("ZZZZZ_nonexistent_device_pattern_12345");
    CHECK(result.mic.empty());
    CHECK(result.monitor.empty());
    // But all_sources should still be populated
    CHECK_FALSE(result.all.empty());
}
