// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Unit tests for PhaseEcho (tests/test_progress_phase.h).
// Phase 2a of test-runner-progress-heartbeat-v2-port.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "test_progress_phase.h"

namespace {

// RAII helper to save / restore the RECMEET_TEST_PHASE_ECHO env var so
// SECTIONs and adjacent TEST_CASEs do not leak state across each other.
class EnvGuard {
public:
    EnvGuard() {
        const char* prev = std::getenv("RECMEET_TEST_PHASE_ECHO");
        if (prev) {
            had_ = true;
            prev_ = prev;
        }
    }
    ~EnvGuard() {
        if (had_) {
            ::setenv("RECMEET_TEST_PHASE_ECHO", prev_.c_str(), 1);
        } else {
            ::unsetenv("RECMEET_TEST_PHASE_ECHO");
        }
    }
private:
    bool had_ = false;
    std::string prev_;
};

// RAII helper to redirect std::cerr to a stringstream buffer.
class CerrCapture {
public:
    CerrCapture() : prev_(std::cerr.rdbuf(buf_.rdbuf())) {}
    ~CerrCapture() { std::cerr.rdbuf(prev_); }
    std::string str() const { return buf_.str(); }
private:
    std::stringstream buf_;
    std::streambuf* prev_;
};

}  // namespace

TEST_CASE("PhaseEcho is a no-op when RECMEET_TEST_PHASE_ECHO is unset",
          "[util][progress]") {
    EnvGuard guard;
    ::unsetenv("RECMEET_TEST_PHASE_ECHO");

    CerrCapture cap;
    recmeet::test::PhaseEcho echo;
    echo("transcribing");

    REQUIRE(cap.str().empty());
}

TEST_CASE("PhaseEcho emits to stderr when RECMEET_TEST_PHASE_ECHO is set",
          "[util][progress]") {
    EnvGuard guard;
    ::setenv("RECMEET_TEST_PHASE_ECHO", "1", 1);

    CerrCapture cap;
    recmeet::test::PhaseEcho echo;
    echo("transcribing");

    const auto out = cap.str();
    REQUIRE(out.find("[phase] transcribing") != std::string::npos);
    // Line ends with a newline (single-write atomicity invariant).
    REQUIRE(!out.empty());
    REQUIRE(out.back() == '\n');
}

TEST_CASE("Two PhaseEcho instances track elapsed time independently",
          "[util][progress]") {
    EnvGuard guard;
    ::setenv("RECMEET_TEST_PHASE_ECHO", "1", 1);

    CerrCapture cap;
    recmeet::test::PhaseEcho a;
    // Sleep long enough that A's clock has advanced at least one full
    // second beyond B's by the time we emit. PhaseEcho prints elapsed
    // seconds (integer truncation), so we need >= 1s of separation to
    // observe a difference deterministically.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    recmeet::test::PhaseEcho b;

    a("X");
    b("X");

    const auto out = cap.str();
    // Expect two lines, each containing "[phase] X (T+Ns)" with N
    // different across the two emissions.
    REQUIRE(out.find("[phase] X (T+1s)") != std::string::npos);
    REQUIRE(out.find("[phase] X (T+0s)") != std::string::npos);
}
