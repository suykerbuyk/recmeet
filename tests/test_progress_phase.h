// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace recmeet::test {

/// Drop-in on_phase callback for pipeline-driven tests. Echoes phase
/// transitions to stderr with elapsed-since-construction timestamps.
/// Gate with RECMEET_TEST_PHASE_ECHO=1.
///
/// Pre-formats the full line into a std::string and emits with a single
/// `<<` so the write is line-atomic against the progress listener's
/// heartbeat thread (no mid-line interleave).
///
/// Note: `start_` clocks from construction. If a PhaseEcho is constructed
/// inside a thread lambda (e.g. captions stress test's reprocess thread),
/// `[phase] X (T+0s)` reflects time-since-thread-start, which is the
/// correct semantic for parallel-reprocess timing.
class PhaseEcho {
public:
    PhaseEcho() : start_{std::chrono::steady_clock::now()},
                  enabled_{[]{
                      const char* s = std::getenv("RECMEET_TEST_PHASE_ECHO");
                      return s && std::string{s} != "0";
                  }()} {}

    void operator()(const std::string& phase) {
        if (!enabled_) return;
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_).count();
        std::string line = "[phase] " + phase + " (T+" + std::to_string(secs) + "s)\n";
        std::cerr << line;
    }

private:
    std::chrono::steady_clock::time_point start_;
    bool enabled_;
};

}  // namespace recmeet::test
