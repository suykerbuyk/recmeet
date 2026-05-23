// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase E.6.3 — `recmeet-tray --listen-now / --headless` flag-parser
// tests. Tag: `[e6][cli]`.
//
// These tests subprocess-spawn the real `recmeet-tray` binary to exercise
// the pre-`gtk_init` flag pre-parse in `main()`. They cannot import the
// flag-parser as a function because the parse loop lives inside `main()`
// itself (intentional — gating gtk_init requires the flags to be
// pre-parsed there, not in a callable helper).
//
// Test cases:
//   1. `--headless` alone is REJECTED at startup (exit 1 + diagnostic
//      message on stderr). The combination is structurally pointless
//      (no GUI, no listener → nothing to interact with), and the smoke
//      gate relies on a clean stderr message instead of a confusing
//      non-event.
//   2. `--listen-now` alone is accepted (process starts and would run a
//      GUI tray — we verify by sending SIGTERM via timeout and checking
//      the diagnostic isn't emitted). This shape requires a usable
//      display, so on a CI host without DISPLAY it may still exit
//      nonzero from gtk_init, but the --headless rejection message must
//      not appear.
//   3. `--listen-now --headless` is accepted (the CI/smoke shape). We
//      spawn the binary against a non-existent daemon socket, give it
//      a brief moment to bind the listener, then SIGTERM. Acceptance
//      criterion: no rejection diagnostic, and the binary obeys SIGTERM.
//   4. Default (neither flag) — the existing GUI tray path. Same
//      acceptance as #2: rejection message must not appear; on a
//      headless CI host gtk_init may still exit nonzero.
//
// We DO NOT attempt to launch a daemon or hit the listener — that is
// the smoke-script's job (`scripts/smoke.sh`). These are unit-level
// CLI-surface tests.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "test_tmpdir.h"

#ifndef RECMEET_BUILD_DIR_DEFAULT
#define RECMEET_BUILD_DIR_DEFAULT "./build"
#endif

extern char** environ;

namespace {

std::string build_dir() {
    if (const char* env = std::getenv("RECMEET_BUILD_DIR"); env && *env) {
        return env;
    }
    return RECMEET_BUILD_DIR_DEFAULT;
}

std::string tray_path() {
    return build_dir() + "/recmeet-tray";
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// Spawn the tray binary with `args`, capturing stderr.
// `wait_ms` is how long to wait for natural exit before SIGTERM'ing.
// If the process exits within wait_ms, returns the exit status.
// If it has to be killed, returns kKilledExitMarker (a sentinel != any
// natural exit code) so the caller can distinguish "exited" from "had
// to kill".
struct SpawnResult {
    int  exit_code     = 0;
    bool had_to_kill   = false;
    std::string stderr_out;
};

constexpr int kKilledExitMarker = -1;

SpawnResult spawn_tray(std::initializer_list<const char*> argv_args,
                       int wait_ms,
                       const char* daemon_addr = nullptr) {
    // Default to a per-process /tmp/recmeet/<pid>_<ms>/ socket path so the
    // tray under test addresses a non-existent daemon socket isolated from
    // the host. Caller may override with an explicit path.
    std::string default_daemon_addr;
    if (daemon_addr == nullptr) {
        default_daemon_addr =
            recmeet::test::tmp_path("recmeet_test_cli_no_daemon.sock").string();
        daemon_addr = default_daemon_addr.c_str();
    }
    SpawnResult r;
    std::string tray = tray_path();

    int err_pipe[2];
    if (pipe(err_pipe) != 0) {
        r.exit_code = 99;
        return r;
    }
    // Set read end non-blocking so we can drain at the end without hanging.
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

    // Build argv vector. argv[0] is the binary path, then --daemon
    // <sock> so the tray doesn't try to fall back to env-default, then
    // the caller-supplied flags.
    std::vector<std::string> arg_storage;
    arg_storage.push_back(tray);
    arg_storage.emplace_back("--daemon");
    arg_storage.emplace_back(daemon_addr);
    for (auto a : argv_args) arg_storage.emplace_back(a);

    std::vector<char*> argv;
    for (auto& s : arg_storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // Redirect stderr to our pipe.
    posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
    // Close-on-exec the write end after dup2 by closing it in child.
    posix_spawn_file_actions_addclose(&fa, err_pipe[1]);
    posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
    // Discard stdout (tray normally has none of substance for these tests).
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid = -1;
    int rc = posix_spawn(&pid, tray.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(err_pipe[1]);  // parent doesn't write
    if (rc != 0) {
        close(err_pipe[0]);
        r.exit_code = 100;
        return r;
    }

    // Wait up to wait_ms for natural exit, polling every 25ms.
    int status = 0;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            // Exited naturally.
            if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
            else r.exit_code = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            // Drain stderr.
            std::array<char, 4096> buf{};
            ssize_t n;
            while ((n = read(err_pipe[0], buf.data(), buf.size())) > 0)
                r.stderr_out.append(buf.data(), static_cast<size_t>(n));
            close(err_pipe[0]);
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    // Didn't exit on its own — SIGTERM it, give it a brief grace period,
    // then SIGKILL if still alive.
    r.had_to_kill = true;
    kill(pid, SIGTERM);
    for (int i = 0; i < 40; ++i) {  // up to 1s
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (waitpid(pid, &status, WNOHANG) != pid) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    r.exit_code = kKilledExitMarker;

    // Drain stderr (non-blocking).
    std::array<char, 4096> buf{};
    ssize_t n;
    while ((n = read(err_pipe[0], buf.data(), buf.size())) > 0)
        r.stderr_out.append(buf.data(), static_cast<size_t>(n));
    close(err_pipe[0]);
    return r;
}

// Stable substring of the rejection diagnostic emitted by
// preparse_runtime_flags. If this changes in src/tray.cpp the test
// fails — intentional: the smoke gate matches on this exact string.
constexpr std::string_view kRejectMsg = "--headless requires --listen-now";

} // namespace

// ============================================================================
// Case 1 — --headless alone is REJECTED at startup.
// ============================================================================
TEST_CASE("recmeet-tray --headless alone is rejected with exit 1",
          "[e6][cli]") {
    auto r = spawn_tray({"--headless"}, 2000);
    // Process must have exited on its own (no kill needed) — the
    // rejection is structurally a fast-fail before any main loop.
    INFO("stderr: " << r.stderr_out);
    REQUIRE_FALSE(r.had_to_kill);
    CHECK(r.exit_code == 1);
    CHECK(contains(r.stderr_out, kRejectMsg));
}

// ============================================================================
// Case 2 — --listen-now alone is ACCEPTED (no rejection diagnostic).
// ============================================================================
//
// On a CI host without DISPLAY/WAYLAND_DISPLAY, gtk_init terminates the
// process via g_critical before we get to the main loop; we accept that
// shape too. The contract under test here is *the absence of the
// --headless-alone rejection diagnostic*, not the process living to
// SIGTERM time. The diagnostic-vs-anything-else split is the
// load-bearing assertion.
TEST_CASE("recmeet-tray --listen-now alone is accepted (no rejection)",
          "[e6][cli]") {
    auto r = spawn_tray({"--listen-now"}, 1500);
    INFO("stderr: " << r.stderr_out);
    CHECK_FALSE(contains(r.stderr_out, kRejectMsg));
    // If the process is still alive at the deadline, that means it
    // entered the GTK main loop successfully. If it exited early on a
    // headless CI host (gtk_init crash), the exit code is whatever
    // gtk_init produces — non-1 from our preparse. We don't assert on
    // exit_code here because both shapes are legitimate; the contract
    // is "the preparse did NOT reject this combination".
}

// ============================================================================
// Case 3 — --listen-now --headless is ACCEPTED (the CI/smoke shape).
// ============================================================================
//
// This is the shape the smoke gate runs in. The binary should bind the
// embedded HTTP listener and enter the GMainLoop. We give it 1.5s to
// reach that state, then SIGTERM; it must have been alive at SIGTERM
// time (i.e. `had_to_kill == true`) AND must not have emitted the
// rejection diagnostic.
TEST_CASE("recmeet-tray --listen-now --headless is accepted (smoke shape)",
          "[e6][cli]") {
    auto r = spawn_tray({"--listen-now", "--headless"}, 1500);
    INFO("stderr: " << r.stderr_out);
    CHECK_FALSE(contains(r.stderr_out, kRejectMsg));
    // The headless tray binds a kernel-picked port (we pointed --daemon
    // at a non-existent socket so it never successfully connects; the
    // reconnect timer is asynchronous and does not exit the loop on
    // failure), so it should be sitting in g_main_loop_run when we
    // SIGTERM it.
    CHECK(r.had_to_kill);
}

// ============================================================================
// Case 4 — default (neither flag) is the existing GUI-tray path.
// ============================================================================
//
// Same shape as case 2: rejection diagnostic must not appear. On a CI
// host without DISPLAY the process exits early from gtk_init; on a
// developer's machine it lives until SIGTERM. Either is acceptable —
// the contract under test is the absence of the rejection diagnostic.
TEST_CASE("recmeet-tray default (no flags) is the GUI path (no rejection)",
          "[e6][cli]") {
    auto r = spawn_tray({}, 1500);
    INFO("stderr: " << r.stderr_out);
    CHECK_FALSE(contains(r.stderr_out, kRejectMsg));
}
