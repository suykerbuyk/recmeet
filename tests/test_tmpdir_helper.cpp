// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Unit tests for the per-process test-root helper (test_tmpdir.h).
// Phase 1 of tmp-dir-isolation-full-stack-tests-v2.

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "test_tmpdir.h"

namespace fs = std::filesystem;

TEST_CASE("tmp_path returns a path under test_root", "[tmpdir]") {
    const auto p = recmeet::test::tmp_path("foo");
    REQUIRE(p.parent_path() == recmeet::test::test_root());
}

TEST_CASE("test_root is stable across multiple calls", "[tmpdir]") {
    const auto a = recmeet::test::test_root();
    const auto b = recmeet::test::test_root();
    REQUIRE(a == b);
}

TEST_CASE("test_root contains current PID when env var unset", "[tmpdir]") {
    if (std::getenv("RECMEET_TEST_ROOT") != nullptr) {
        SKIP("RECMEET_TEST_ROOT set; default-form PID check N/A");
    }
    const auto pid_str = std::to_string(::getpid());
    REQUIRE(recmeet::test::test_root().string().find(pid_str) != std::string::npos);
}

TEST_CASE("tmp_path produces distinct paths for distinct stems", "[tmpdir]") {
    const auto a = recmeet::test::tmp_path("a");
    const auto b = recmeet::test::tmp_path("b");
    CHECK(a != b);
}
