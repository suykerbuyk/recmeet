// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Unit tests for recmeet::test::PhaseEcho. Verifies env-gating
// (RECMEET_TEST_PHASE_ECHO), stderr emission shape, and independent
// elapsed-time tracking across instances.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "test_progress_phase.h"

namespace {

// RAII redirect of std::cerr to a stringstream for capture. Restores the
// original rdbuf on destruction so a thrown REQUIRE doesn't leak the swap
// into subsequent tests.
class CerrCapture {
  public:
    CerrCapture() : prev_{std::cerr.rdbuf(buf_.rdbuf())} {}
    ~CerrCapture() { std::cerr.rdbuf(prev_); }
    CerrCapture(const CerrCapture&) = delete;
    CerrCapture& operator=(const CerrCapture&) = delete;
    std::string str() const { return buf_.str(); }

  private:
    std::stringstream buf_{};
    std::streambuf* prev_;
};

}  // namespace

TEST_CASE("PhaseEcho: no emission when env var unset", "[util][phase-echo]") {
    // Ensure the env var is unset (a parent shell or earlier test could
    // have set it). PhaseEcho captures enabled-ness at construction.
    unsetenv("RECMEET_TEST_PHASE_ECHO");

    CerrCapture cap;
    recmeet::test::PhaseEcho echo;
    echo("transcribing");
    echo("diarizing");
    REQUIRE(cap.str().empty());
}

TEST_CASE("PhaseEcho: emits formatted line when env var set to 1",
          "[util][phase-echo]") {
    setenv("RECMEET_TEST_PHASE_ECHO", "1", 1);

    CerrCapture cap;
    recmeet::test::PhaseEcho echo;
    echo("transcribing");
    const std::string out = cap.str();

    unsetenv("RECMEET_TEST_PHASE_ECHO");

    REQUIRE(out.find("[phase] transcribing (T+") != std::string::npos);
    REQUIRE(out.find("s)\n") != std::string::npos);
}

TEST_CASE("PhaseEcho: two instances track elapsed time independently",
          "[util][phase-echo]") {
    setenv("RECMEET_TEST_PHASE_ECHO", "1", 1);

    recmeet::test::PhaseEcho first;
    // Sleep long enough that the second instance reads a smaller elapsed
    // value than the first when both emit moments apart.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    recmeet::test::PhaseEcho second;

    CerrCapture cap;
    first("alpha");
    second("alpha");
    const std::string out = cap.str();

    unsetenv("RECMEET_TEST_PHASE_ECHO");

    // Both should have emitted exactly one line.
    REQUIRE(out.find("[phase] alpha (T+") != std::string::npos);
    // Strings must differ — `first` started ~1.1s earlier so its T+Ns
    // value is strictly larger than `second`'s.
    const auto split = out.find('\n');
    REQUIRE(split != std::string::npos);
    const std::string line_first  = out.substr(0, split);
    const std::string line_second = out.substr(split + 1);
    REQUIRE_FALSE(line_second.empty());
    REQUIRE(line_first != line_second);
}
