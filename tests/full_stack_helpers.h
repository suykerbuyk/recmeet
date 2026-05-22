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
#include <string_view>
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
// find_tray_binary — resolve the path to a built `recmeet-tray`. Same
// sibling-scan fallback as find_daemon_binary; the tray binary is only
// emitted on a build that has GTK+AppIndicator (RECMEET_BUILD_TRAY=ON),
// so we surface the missing-binary case as a SKIP via a sentinel return
// (empty path) so the [full-stack][webui] test can check_skip rather
// than FAIL on a no-tray build configuration.
inline fs::path find_tray_binary() {
    fs::path root = recmeet::test_helpers::find_project_root();
    if (root.empty()) return {};
    fs::path candidate = root / "build" / "recmeet-tray";
    if (fs::exists(candidate)) return candidate;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        fs::path probe = entry.path() / "recmeet-tray";
        if (fs::exists(probe)) return probe;
    }
    return {};
}

// ---------------------------------------------------------------------------
// SpawnedTray — fork+exec'd real `recmeet-tray` child harness for the
// `[full-stack][webui]` real-daemon HTTP path.
//
// The tray binary takes `--daemon <addr>` (Unix socket path or host:port,
// per `src/tray.cpp` argv parser at line ~3673), plus `--listen-now`
// (bind the embedded WebUI immediately on startup) and `--headless` (no
// AppIndicator / GTK menu — required for CI hosts without a display).
// These are the same three flags `scripts/smoke.sh` uses.
//
// The embedded HTTP listener binds to a kernel-picked port via
// `httplib::Server::bind_to_any_port("127.0.0.1")` (src/tray_web.cpp:518).
// We discover the port by capturing the child's stderr (the tray's
// `log_init(..., stderr=true)` path emits "[INFO] [tid=...] [tray_web]
// embedded WebUI listening on http://127.0.0.1:NNN") and parsing the
// substring.
//
// Lifecycle:
//   ctor — pipe()+fork()+exec(); reads stderr until either the listening-on
//          line is parsed (success) or the 60s deadline expires / the child
//          dies first (FAIL with surfaced status).
//   dtor — SIGTERM, wait 5s with WNOHANG, escalate to SIGKILL.
//
// The 60s readiness budget is longer than SpawnedDaemon's 5s because the
// tray performs its own IPC connect to the daemon before binding the
// listener; in CI the model-cache cold start plus connect retry can push
// past 10s. Local steady-state startup is sub-second.
struct SpawnedTray {
    // Forking ctor.
    //
    //   tray_bin       — absolute path to a built `recmeet-tray` binary.
    //   daemon_socket  — Unix socket path the tray's IpcClient connects to.
    //                    Becomes the value of `--daemon <addr>`. Host:port
    //                    is also accepted by the tray's parser but the
    //                    full-stack test uses Unix.
    //   xdg_config_dir — XDG_CONFIG_HOME for the tray child. Should equal
    //                    the daemon's XDG_CONFIG_HOME so both pick up the
    //                    same daemon.yaml (meetings_root + speaker_db
    //                    pinning). The tray itself does not currently
    //                    require any keys in daemon.yaml, but pointing it
    //                    at the same dir is the contract scripts/smoke.sh
    //                    uses (rev-2 C1 in the smoke header).
    SpawnedTray(const fs::path& tray_bin,
                const fs::path& daemon_socket,
                const fs::path& xdg_config_dir)
        : daemon_socket_(daemon_socket)
        , xdg_config_(xdg_config_dir)
    {
        REQUIRE(fs::exists(tray_bin));
        REQUIRE(!daemon_socket.empty());

        // Stderr pipe — the tray's log_init writes to stderr; we read the
        // "embedded WebUI listening on http://127.0.0.1:NNN" line to
        // discover the kernel-picked port.
        int err_pipe[2];
        REQUIRE(::pipe(err_pipe) == 0);
        // Set the read end non-blocking so the parser loop can poll
        // without wedging on a hung child.
        int flags = ::fcntl(err_pipe[0], F_GETFL, 0);
        ::fcntl(err_pipe[0], F_SETFL, flags | O_NONBLOCK);

        pid_ = fork();
        REQUIRE(pid_ >= 0);

        if (pid_ == 0) {
            // Child: redirect stderr → pipe-write; close pipe-read.
            ::dup2(err_pipe[1], STDERR_FILENO);
            ::close(err_pipe[0]);
            ::close(err_pipe[1]);
            // Stdout to /dev/null — the tray emits no useful stdout, but
            // we don't want it polluting the test runner.
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDOUT_FILENO);
                ::close(devnull);
            }
            setenv("XDG_CONFIG_HOME", xdg_config_dir.c_str(), 1);
            // Force log level to "info" so the "embedded WebUI listening
            // on http://127.0.0.1:NNN" line shows up on stderr — that
            // log line is how this harness discovers the kernel-picked
            // port. The tray default is "error" (see config.h:152), which
            // would suppress the listening-on line and leave us with no
            // port-discovery channel. The daemon-side `RECMEET_LOG_LEVEL`
            // override path (src/config.cpp:378-379) is what we ride here.
            setenv("RECMEET_LOG_LEVEL", "info", 1);
            ::execl(tray_bin.c_str(),
                    "recmeet-tray",
                    "--daemon", daemon_socket.c_str(),
                    "--listen-now",
                    "--headless",
                    static_cast<char*>(nullptr));
            _exit(127);
        }

        // Parent: close write end; poll stderr for the listening-on line.
        ::close(err_pipe[1]);
        err_pipe_read_ = err_pipe[0];

        // 60s budget — see header comment. The tray reaches steady state
        // sub-second on a warm runner but can take 5-10s on a cold one
        // while the IPC connect retries through the jittered backoff.
        auto deadline = std::chrono::steady_clock::now() + 60s;
        std::string buf;
        constexpr std::string_view kNeedle =
            "embedded WebUI listening on http://127.0.0.1:";
        while (std::chrono::steady_clock::now() < deadline) {
            char tmp[1024];
            ssize_t n = ::read(err_pipe_read_, tmp, sizeof(tmp));
            if (n > 0) {
                buf.append(tmp, static_cast<size_t>(n));
                auto pos = buf.find(kNeedle);
                if (pos != std::string::npos) {
                    auto port_begin = pos + kNeedle.size();
                    auto port_end = port_begin;
                    while (port_end < buf.size() &&
                           buf[port_end] >= '0' && buf[port_end] <= '9') {
                        ++port_end;
                    }
                    if (port_end > port_begin) {
                        port_ = std::atoi(
                            buf.substr(port_begin,
                                       port_end - port_begin).c_str());
                        if (port_ > 0) break;
                    }
                }
            } else if (n == 0) {
                // EOF on child stderr — the child closed it (probably
                // exited). Drop through to the status-check below.
                break;
            } else {
                // EAGAIN/EWOULDBLOCK — no data yet. Has the child died?
                int status = 0;
                pid_t r = ::waitpid(pid_, &status, WNOHANG);
                if (r == pid_) {
                    if (WIFEXITED(status)) {
                        FAIL("tray child exited code "
                             << WEXITSTATUS(status)
                             << " before binding WebUI listener. "
                             << "Captured stderr: " << buf);
                    } else if (WIFSIGNALED(status)) {
                        FAIL("tray child killed by signal "
                             << WTERMSIG(status)
                             << " before binding WebUI listener. "
                             << "Captured stderr: " << buf);
                    }
                    FAIL("tray child reaped (status=" << status
                         << ") before binding WebUI listener");
                }
                std::this_thread::sleep_for(50ms);
            }
        }
        captured_stderr_ = buf;
        if (port_ <= 0) {
            int status = 0;
            pid_t r = ::waitpid(pid_, &status, WNOHANG);
            if (r == pid_ && WIFEXITED(status)) {
                FAIL("tray child exited code " << WEXITSTATUS(status)
                     << " before binding WebUI listener. "
                     << "Captured stderr: " << buf);
            } else if (r == pid_ && WIFSIGNALED(status)) {
                FAIL("tray child killed by signal " << WTERMSIG(status)
                     << " before binding WebUI listener. "
                     << "Captured stderr: " << buf);
            }
            FAIL("tray did not bind WebUI listener within 60s. "
                 << "Captured stderr: " << buf);
        }
    }

    ~SpawnedTray() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            auto deadline = std::chrono::steady_clock::now() + 5s;
            bool reaped = false;
            while (std::chrono::steady_clock::now() < deadline) {
                int status = 0;
                pid_t r = ::waitpid(pid_, &status, WNOHANG);
                if (r == pid_) { reaped = true; break; }
                std::this_thread::sleep_for(50ms);
            }
            if (!reaped) {
                ::kill(pid_, SIGKILL);
                int status = 0;
                ::waitpid(pid_, &status, 0);
            }
        }
        if (err_pipe_read_ >= 0) {
            ::close(err_pipe_read_);
            err_pipe_read_ = -1;
        }
    }

    SpawnedTray(const SpawnedTray&) = delete;
    SpawnedTray& operator=(const SpawnedTray&) = delete;

    pid_t pid() const noexcept { return pid_; }
    int port() const noexcept { return port_; }
    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    const std::string& captured_stderr() const noexcept {
        return captured_stderr_;
    }

private:
    pid_t pid_ = -1;
    int port_ = 0;
    int err_pipe_read_ = -1;
    fs::path daemon_socket_;
    fs::path xdg_config_;
    std::string captured_stderr_;
};

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
