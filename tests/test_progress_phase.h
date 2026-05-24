// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Drop-in on_phase callback for pipeline-driven tests. Echoes phase
// transitions to stderr with elapsed-since-construction timestamps.
// Gate emission with RECMEET_TEST_PHASE_ECHO=1.

#pragma once

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace recmeet::test {

class PhaseEcho {
  public:
    PhaseEcho()
        : start_{std::chrono::steady_clock::now()},
          enabled_{[] {
              const char* s = std::getenv("RECMEET_TEST_PHASE_ECHO");
              return s && *s && std::string{s} != "0";
          }()} {}

    void operator()(const std::string& phase) const {
        if (!enabled_) return;
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start_)
                        .count();
        // Pre-format then emit with one << — line-atomic against the
        // listener's heartbeat thread.
        std::string line = "[phase] " + phase + " (T+" +
                           std::to_string(secs) + "s)\n";
        std::cerr << line;
    }

  private:
    std::chrono::steady_clock::time_point start_;
    bool enabled_;
};

}  // namespace recmeet::test
