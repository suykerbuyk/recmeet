// Unit tests for the per-process test-root helper. Verifies the
// path-shape contract: tmp_path() composes under test_root(), test_root()
// is stable across calls, the default form embeds the PID, and distinct
// stems yield distinct paths. Env-var override and on-disk creation are
// exercised by every test run (the magic-static is initialized before any
// TEST_CASE body runs), so a dedicated re-init test would need a hook
// that defeats the contract.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <unistd.h>

#include "test_tmpdir.h"

TEST_CASE("tmp_path composes under test_root", "[harness][tmpdir]") {
    const auto root = recmeet::test::test_root();
    const auto p = recmeet::test::tmp_path("foo");
    REQUIRE(p.parent_path() == root);
    REQUIRE(p.filename() == "foo");
}

TEST_CASE("test_root is stable across calls", "[harness][tmpdir]") {
    const auto a = recmeet::test::test_root();
    const auto b = recmeet::test::test_root();
    REQUIRE(a == b);
}

TEST_CASE("test_root default form embeds the PID", "[harness][tmpdir]") {
    // This check only matters when RECMEET_TEST_ROOT is unset — that's the
    // default-behavior contract. If a caller pinned the root via env var,
    // skip the substring assertion (the env-var path is opaque by design).
    if (std::getenv("RECMEET_TEST_ROOT")) {
        SUCCEED("RECMEET_TEST_ROOT set; default-form check not applicable");
        return;
    }
    const auto root = recmeet::test::test_root();
    const std::string pid = std::to_string(::getpid());
    const std::string leaf = root.filename().string();
    REQUIRE(leaf.find(pid) != std::string::npos);
}

TEST_CASE("tmp_path with distinct stems yields distinct paths", "[harness][tmpdir]") {
    const auto a = recmeet::test::tmp_path("a");
    const auto b = recmeet::test::tmp_path("b");
    REQUIRE(a != b);
}
