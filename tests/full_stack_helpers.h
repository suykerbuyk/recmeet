// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 3 — shared helpers for `[full-stack]` / `[e2e]` tests that spawn
// a real `recmeet-daemon` child process. Promoted from the anonymous-
// namespace `DaemonChild` previously inlined in
// `tests/test_v2_thin_client_e2e.cpp` (lines 50-185 prior to Phase 3).
//
// The promotion adds Unix-domain-socket transport support next to the
// existing TCP+PSK auth path, so the same helper can drive both the
// existing `[e2e][thin-client][tcp]` test and the new
// `[full-stack][speaker-id]` test which connects over a Unix socket
// (the daemon's default transport).
//
// Kept header-only on purpose: the helper is small, has no link-surface
// dependencies beyond what the test TU already pulls in (`ipc_client`
// transitively pulls `ipc_protocol`), and shaving an extra .cpp
// translation unit out of the `recmeet_tests` build is worth more than
// the marginal compile-time savings of moving the bodies to a .cpp.
// Each `inline` here is intentional; the include guard plus inline
// linkage keep the One Definition Rule honored across multiple includers.

#pragma once

#include "test_helpers.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace recmeet::full_stack {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// SpawnedDaemon — fork+exec'd real `recmeet-daemon` child harness.
//
// Promoted from `tests/test_v2_thin_client_e2e.cpp` (Phase 3 of
// test-and-verification-hardening). Supports BOTH transports:
//
//   Transport::TCP   — `--listen <host:port>` + `RECMEET_AUTH_TOKEN` env
//                      (PSK gate on the AF_INET listener).
//   Transport::Unix  — `--socket <path>` (no PSK; AF_UNIX is local-only
//                      and authenticated by filesystem permissions).
//
// XDG_CONFIG_HOME is scoped to a caller-provided dir so the daemon picks
// up a per-test `config.yaml` (see `write_minimal_test_config`) without
// touching HOME. Models still resolve through XDG_DATA_HOME/HOME and the
// operator's cached `~/.local/share/recmeet/models/`, so a one-time CI
// model pre-fetch suffices.
//
// Lifecycle: ctor forks+execs the daemon and poll-connects until
// accept-ready (or surfaces a meaningful FAIL with exec/signal status if
// the child died first). dtor SIGTERMs, polls waitpid for 5s, then
// SIGKILLs as a final escalation.
struct SpawnedDaemon {
    enum class Transport { TCP, Unix };

    // Forking ctor.
    //
    //   daemon_bin       — absolute path to a built `recmeet-daemon` binary.
    //   transport        — TCP or Unix; selects --listen vs --socket.
    //   tcp_addr         — required when transport==TCP (host:port); ignored
    //                      otherwise.
    //   psk              — required when transport==TCP; pushed into the
    //                      child's RECMEET_AUTH_TOKEN env. Ignored on Unix.
    //   xdg_config_dir   — set as XDG_CONFIG_HOME in the child env so the
    //                      daemon's config.yaml read scopes to this test.
    //   unix_socket      — required when transport==Unix (filesystem path);
    //                      ignored otherwise.
    SpawnedDaemon(const fs::path& daemon_bin,
                  Transport transport,
                  const std::string& tcp_addr,
                  const std::string& psk,
                  const fs::path& xdg_config_dir,
                  const fs::path& unix_socket)
        : transport_(transport)
        , address_(transport == Transport::TCP ? tcp_addr : std::string{})
        , socket_path_(transport == Transport::Unix ? unix_socket : fs::path{})
        , xdg_config_(xdg_config_dir)
    {
        // Validate args by transport. Cheap, but the failure modes are
        // confusing enough (a missing --socket arg makes the daemon use the
        // XDG default, which works "accidentally" until two parallel tests
        // collide) that the upfront REQUIRE is worth it.
        if (transport == Transport::TCP) {
            REQUIRE_FALSE(tcp_addr.empty());
            REQUIRE_FALSE(psk.empty());
        } else {
            REQUIRE_FALSE(unix_socket.empty());
            // Pre-create the socket dir so the daemon's bind() doesn't fail
            // with ENOENT on a fresh worktree.
            std::error_code ec;
            fs::create_directories(unix_socket.parent_path(), ec);
        }

        pid_ = fork();
        REQUIRE(pid_ >= 0);

        if (pid_ == 0) {
            // Child: set up env and exec the daemon.
            setenv("XDG_CONFIG_HOME", xdg_config_dir.c_str(), 1);
            if (transport == Transport::TCP) {
                setenv("RECMEET_AUTH_TOKEN", psk.c_str(), 1);
            }
            // Quiet the daemon's stderr — the parent runs the test
            // assertions; log noise from a healthy daemon shouldn't confuse
            // the test runner output. Operator can override with
            // RECMEET_E2E_DAEMON_STDERR=1 for debugging.
            const char* keep_stderr = std::getenv("RECMEET_E2E_DAEMON_STDERR");
            if (!keep_stderr || keep_stderr[0] == '\0') {
                int devnull = ::open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    ::dup2(devnull, STDERR_FILENO);
                    ::close(devnull);
                }
            }
            if (transport == Transport::TCP) {
                ::execl(daemon_bin.c_str(),
                        "recmeet-daemon",
                        "--listen", tcp_addr.c_str(),
                        static_cast<char*>(nullptr));
            } else {
                ::execl(daemon_bin.c_str(),
                        "recmeet-daemon",
                        "--socket", unix_socket.c_str(),
                        static_cast<char*>(nullptr));
            }
            // exec failed; bail out without running atexit handlers.
            _exit(127);
        }

        // Parent: poll-connect until accept-ready. 5s budget; longer than
        // typical local startup (~200 ms) and tolerant of CI cold-start
        // (~1-2 s with model pre-cache lookups). Surface child status before
        // failing — exec error vs SIGSEGV vs still-binding tell very
        // different stories.
        auto deadline = std::chrono::steady_clock::now() + 5s;
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (transport == Transport::TCP) {
                if (probe_tcp(tcp_addr)) { ready = true; break; }
            } else {
                if (probe_unix(unix_socket.string())) { ready = true; break; }
            }
            std::this_thread::sleep_for(50ms);
        }
        if (!ready) {
            int status = 0;
            pid_t r = ::waitpid(pid_, &status, WNOHANG);
            if (r == pid_ && WIFEXITED(status)) {
                FAIL("daemon child exited code " << WEXITSTATUS(status)
                     << " before becoming "
                     << (transport == Transport::TCP ? "TCP" : "Unix")
                     << "-ready");
            } else if (r == pid_ && WIFSIGNALED(status)) {
                FAIL("daemon child killed by signal " << WTERMSIG(status)
                     << " before becoming "
                     << (transport == Transport::TCP ? "TCP" : "Unix")
                     << "-ready");
            }
            FAIL("daemon child did not become "
                 << (transport == Transport::TCP ? "TCP" : "Unix")
                 << "-ready within 5s at "
                 << (transport == Transport::TCP
                        ? tcp_addr
                        : unix_socket.string()));
        }
    }

    ~SpawnedDaemon() {
        if (pid_ <= 0) return;
        // Polite first — postprocess subprocesses get a chance to flush.
        ::kill(pid_, SIGTERM);
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            pid_t r = ::waitpid(pid_, &status, WNOHANG);
            if (r == pid_) {
                // On Unix transport, the daemon should unlink the socket on
                // a clean SIGTERM. Best-effort cleanup in case it didn't.
                if (transport_ == Transport::Unix && !socket_path_.empty()) {
                    std::error_code ec;
                    fs::remove(socket_path_, ec);
                }
                return;
            }
            std::this_thread::sleep_for(50ms);
        }
        // Stuck — escalate.
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        if (transport_ == Transport::Unix && !socket_path_.empty()) {
            std::error_code ec;
            fs::remove(socket_path_, ec);
        }
    }

    SpawnedDaemon(const SpawnedDaemon&) = delete;
    SpawnedDaemon& operator=(const SpawnedDaemon&) = delete;

    // For orchestrator verification + readiness probes.
    pid_t pid() const noexcept { return pid_; }
    Transport transport() const noexcept { return transport_; }
    // For TCP: "host:port"; empty for Unix.
    const std::string& address() const noexcept { return address_; }
    // For Unix: socket path; empty for TCP.
    const fs::path& socket_path() const noexcept { return socket_path_; }
    // Address suitable for `IpcClient(addr)` — host:port on TCP, socket
    // path on Unix. The IpcClient parser distinguishes by the
    // host:port pattern (see ipc_protocol.cpp:parse_ipc_address).
    std::string client_address() const {
        return transport_ == Transport::TCP ? address_ : socket_path_.string();
    }

    // Connect-probe — single-shot, sub-second. Same shape as the original
    // anonymous-namespace helper; exposed as a static so the ctor's
    // readiness loop can drive both transports through one branch.
    static bool probe_tcp(const std::string& host_port) {
        auto colon = host_port.rfind(':');
        if (colon == std::string::npos) return false;
        std::string host = host_port.substr(0, colon);
        int port = std::atoi(host_port.substr(colon + 1).c_str());

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        struct sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
            ::close(fd);
            return false;
        }
        bool ok = (::connect(fd, reinterpret_cast<struct sockaddr*>(&sa),
                             sizeof(sa)) == 0);
        ::close(fd);
        return ok;
    }

    // Unix-socket readiness probe — symmetric to `probe_tcp`. Same 50ms-
    // tick / 5s-budget pattern is driven from the ctor; this is the
    // single-shot building block.
    static bool probe_unix(const std::string& path) {
        if (path.empty()) return false;
        // `connect()` against an AF_UNIX/SOCK_STREAM endpoint is the canonical
        // accept-ready probe: the bind step creates the inode, but accept()
        // only starts succeeding once the daemon's `listen()` has fired.
        if (path.size() >= sizeof(((struct sockaddr_un*)nullptr)->sun_path)) {
            return false;
        }
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        struct sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
        bool ok = (::connect(fd, reinterpret_cast<struct sockaddr*>(&sa),
                             sizeof(sa)) == 0);
        ::close(fd);
        return ok;
    }

private:
    pid_t pid_ = -1;
    Transport transport_;
    std::string address_;     // host:port for TCP, empty for Unix
    fs::path socket_path_;    // socket path for Unix, empty for TCP
    fs::path xdg_config_;
};

// ---------------------------------------------------------------------------
// find_daemon_binary — resolve the path to a built `recmeet-daemon`.
//
// Tests typically run from the worktree's build/ dir, so the binary sits at
// `<root>/build/recmeet-daemon`. For worktree-isolated builds
// (`make BUILD_DIR=build-foo`) we fall back to scanning siblings of `build/`
// for a `recmeet-daemon` binary. FAILs the calling test if neither resolves.
inline fs::path find_daemon_binary() {
    fs::path root = recmeet::test_helpers::find_project_root();
    REQUIRE(!root.empty());
    fs::path candidate = root / "build" / "recmeet-daemon";
    if (fs::exists(candidate)) return candidate;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        fs::path probe = entry.path() / "recmeet-daemon";
        if (fs::exists(probe)) return probe;
    }
    FAIL("recmeet-daemon binary not found under " << root.string());
    return {};
}

// ---------------------------------------------------------------------------
// write_minimal_test_config — write a minimal daemon config.yaml under
// `<xdg_config_dir>/recmeet/config.yaml`.
//
// Each disable_* flag corresponds to a top-level section the daemon
// honors at config load. Defaults match the existing `[e2e][thin-client]`
// test's needs (summary off, diarization + VAD as caller wants).
inline void write_minimal_test_config(const fs::path& xdg_config_dir,
                                      bool disable_summary = true,
                                      bool disable_diarization = false,
                                      bool disable_vad = false) {
    fs::path cfg_dir = xdg_config_dir / "recmeet";
    fs::create_directories(cfg_dir);
    std::ofstream cfg(cfg_dir / "config.yaml");
    REQUIRE(cfg.is_open());
    cfg << "# recmeet full-stack test config\n";
    if (disable_summary) {
        cfg << "summary:\n"
            << "  disabled: true\n";
    }
    if (disable_diarization) {
        cfg << "diarization:\n"
            << "  enabled: false\n";
    }
    if (disable_vad) {
        cfg << "vad:\n"
            << "  enabled: false\n";
    }
}

} // namespace recmeet::full_stack
