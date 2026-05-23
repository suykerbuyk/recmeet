// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Unit tests for `cleanup_cancelled_recording_dir`. DaemonSim cannot drive
// run_recording end-to-end, so the filesystem-touching behaviour is
// exercised here directly with tmp dirs.

#include <catch2/catch_test_macros.hpp>

#include "pipeline_cleanup.h"
#include "util.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace recmeet;

namespace {

// Generate a unique tmp path under fs::temp_directory_path(). Does NOT
// create the directory — caller decides whether to populate it.
fs::path unique_tmp_path(const std::string& prefix) {
    static std::atomic<unsigned> counter{0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    unsigned salt = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s-%lld-%u-%d",
                  prefix.c_str(),
                  static_cast<long long>(now),
                  salt,
                  (int)getpid());
    return fs::temp_directory_path() / buf;
}

} // anonymous namespace

TEST_CASE("cleanup_cancelled_recording_dir: empty path → returns true (no-op)",
          "[pipeline][cleanup][cancel]") {
    CHECK(cleanup_cancelled_recording_dir(fs::path{}));
}

TEST_CASE("cleanup_cancelled_recording_dir: '/' → returns true (no-op)",
          "[pipeline][cleanup][cancel]") {
    // The sanity gate MUST short-circuit before any fs::remove_all call —
    // we rely on the early-return, not on filesystem permissions, to keep
    // the test from being dangerous.
    CHECK(cleanup_cancelled_recording_dir(fs::path{"/"}));
    // Confirm "/" is still present (it always is, but the assertion makes
    // the safety contract explicit).
    CHECK(fs::exists(fs::path{"/"}));
}

TEST_CASE("cleanup_cancelled_recording_dir: nonexistent path → returns true (no-op)",
          "[pipeline][cleanup][cancel]") {
    fs::path missing = unique_tmp_path("recmeet-nonexistent");
    REQUIRE_FALSE(fs::exists(missing));
    CHECK(cleanup_cancelled_recording_dir(missing));
    // Still doesn't exist after the call.
    CHECK_FALSE(fs::exists(missing));
}

TEST_CASE("cleanup_cancelled_recording_dir: tmp dir with files → returns true, dir gone",
          "[pipeline][cleanup][cancel]") {
    fs::path dir = unique_tmp_path("recmeet-cleanup-test");
    fs::create_directories(dir);
    REQUIRE(fs::exists(dir));

    {
        std::ofstream out(dir / "audio_2026-05-23_12-00.wav");
        out << "fake wav content";
    }
    {
        std::ofstream out(dir / "captions.vtt");
        out << "WEBVTT\n\n";
    }
    REQUIRE(fs::exists(dir / "audio_2026-05-23_12-00.wav"));
    REQUIRE(fs::exists(dir / "captions.vtt"));

    CHECK(cleanup_cancelled_recording_dir(dir));
    CHECK_FALSE(fs::exists(dir));
}

TEST_CASE("cleanup_cancelled_recording_dir: read-only parent blocks remove_all → returns false",
          "[pipeline][cleanup][cancel]") {
    // Root bypasses POSIX perms, so the "permission denied" path is
    // unreachable. Skip gracefully rather than emit a misleading failure.
    if (geteuid() == 0) {
        SUCCEED("Skipped: root bypasses POSIX permissions");
        return;
    }

    fs::path parent = unique_tmp_path("recmeet-cleanup-perm");
    fs::create_directories(parent);
    fs::path child = parent / "recording-dir";
    fs::create_directories(child);
    {
        std::ofstream out(child / "audio.wav");
        out << "fake";
    }
    REQUIRE(fs::exists(child / "audio.wav"));

    // chmod 0500 on parent: r-x ----- ----- — owner can traverse + read
    // names but cannot unlink entries inside. fs::remove_all on `child`
    // will try to unlink files inside `child`, which still has 0755,
    // but THEN it tries to remove `child` itself by unlinking it from
    // `parent`, which fails with EACCES.
    int rc = chmod(parent.c_str(), 0500);
    REQUIRE(rc == 0);

    bool ok = cleanup_cancelled_recording_dir(child);

    // Restore perms BEFORE asserting so test cleanup (and Catch2's
    // post-test teardown) can actually delete the tmp dir.
    chmod(parent.c_str(), 0755);

    CHECK_FALSE(ok);
    CHECK(fs::exists(child));

    // Tidy up.
    std::error_code ec;
    fs::remove_all(parent, ec);
}
