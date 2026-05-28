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

#include <sys/types.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
                  const fs::path& unix_socket,
                  const fs::path& stderr_log = {})
        : transport_(transport)
        , address_(transport == Transport::TCP ? tcp_addr : std::string{})
        , socket_path_(transport == Transport::Unix ? unix_socket : fs::path{})
        , xdg_config_(xdg_config_dir)
        , stderr_log_(stderr_log)
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
            // RECMEET_E2E_DAEMON_STDERR=1 for debugging. When the caller
            // passed a `stderr_log` path, redirect stderr there so the
            // parent test can grep for daemon log lines (used by
            // captions-supported test T3 to assert the
            // "[startup] captions disabled at runtime: model directory not
            // found" line was emitted). When the env override is also set,
            // we honor the env (file capture is opt-in via ctor param,
            // operator debug override always wins).
            const char* keep_stderr = std::getenv("RECMEET_E2E_DAEMON_STDERR");
            if (keep_stderr && keep_stderr[0] != '\0') {
                // fall through — keep inherited stderr
            } else if (!stderr_log.empty()) {
                // Force info-level so the startup log lines we assert on
                // actually emit. The daemon default is "error" (see
                // config.cpp loader); without bumping this, tests that
                // grep startup INFO lines would see an empty file.
                setenv("RECMEET_LOG_LEVEL", "info", 1);
                int fd = ::open(stderr_log.c_str(),
                                O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) {
                    ::dup2(fd, STDERR_FILENO);
                    ::close(fd);
                }
            } else {
                int devnull = ::open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    ::dup2(devnull, STDERR_FILENO);
                    ::close(devnull);
                }
            }
            if (transport == Transport::TCP) {
                ::execl(daemon_bin.c_str(),
                        "recmeet-server",
                        "--listen", tcp_addr.c_str(),
                        static_cast<char*>(nullptr));
            } else {
                ::execl(daemon_bin.c_str(),
                        "recmeet-server",
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

    /// Path of the file the daemon's stderr was redirected to (when the
    /// optional ctor `stderr_log` argument was non-empty). Empty when no
    /// log redirection was requested. The file is populated by the child
    /// process — readers should slurp it after the daemon has had a chance
    /// to emit (typically after `~SpawnedDaemon` has reaped the child to
    /// guarantee final flush, but readers may also poll mid-run for
    /// startup-only lines like the captions runtime gate).
    const fs::path& stderr_log_path() const noexcept { return stderr_log_; }

private:
    pid_t pid_ = -1;
    Transport transport_;
    std::string address_;     // host:port for TCP, empty for Unix
    fs::path socket_path_;    // socket path for Unix, empty for TCP
    fs::path xdg_config_;
    fs::path stderr_log_;     // empty unless caller opted in
};

// ---------------------------------------------------------------------------
// find_daemon_binary — resolve the path to a built `recmeet-server`.
//
// Tests typically run from the worktree's build/ dir, so the binary sits at
// `<root>/build/recmeet-server`. For worktree-isolated builds
// (`make BUILD_DIR=build-foo`) we fall back to scanning siblings of `build/`
// for a `recmeet-server` binary. FAILs the calling test if neither resolves.
inline fs::path find_daemon_binary() {
    fs::path root = recmeet::test_helpers::find_project_root();
    REQUIRE(!root.empty());
    fs::path candidate = root / "build" / "recmeet-server";
    if (fs::exists(candidate)) return candidate;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        fs::path probe = entry.path() / "recmeet-server";
        if (fs::exists(probe)) return probe;
    }
    FAIL("recmeet-server binary not found under " << root.string());
    return {};
}

// ---------------------------------------------------------------------------
// find_tray_binary — resolve the path to a built `recmeet-client`. Same
// sibling-scan fallback as find_daemon_binary; the client binary is only
// emitted on a build that has GTK+AppIndicator (RECMEET_BUILD_TRAY=ON),
// so we surface the missing-binary case as a SKIP via a sentinel return
// (empty path) so the [full-stack][webui] test can check_skip rather
// than FAIL on a no-tray build configuration.
inline fs::path find_tray_binary() {
    fs::path root = recmeet::test_helpers::find_project_root();
    if (root.empty()) return {};
    fs::path candidate = root / "build" / "recmeet-client";
    if (fs::exists(candidate)) return candidate;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        fs::path probe = entry.path() / "recmeet-client";
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
                    "recmeet-client",
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
// write_minimal_test_config — write a minimal daemon config under
// `<xdg_config_dir>/recmeet-server/daemon.yaml` (v2 split-paths).
//
// Each disable_* flag corresponds to a top-level section the daemon
// honors at config load. Defaults match the existing `[e2e][thin-client]`
// test's needs (summary off, diarization + VAD as caller wants).
//
// v2-coexistence Phase 2C — the daemon no longer migrates the legacy
// `config.yaml` to `daemon.yaml`. Write directly to the file the daemon
// reads (`load_server_config()` resolves `server_config_dir() / "daemon.yaml"`,
// i.e. `$XDG_CONFIG_HOME/recmeet-server/daemon.yaml`).
inline void write_minimal_test_config(const fs::path& xdg_config_dir,
                                      bool disable_summary = true,
                                      bool disable_diarization = false,
                                      bool disable_vad = false) {
    fs::path cfg_dir = xdg_config_dir / "recmeet-server";
    fs::create_directories(cfg_dir);
    std::ofstream cfg(cfg_dir / "daemon.yaml");
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

// ---------------------------------------------------------------------------
// BackgroundProc — fork+exec'd auxiliary child process with SIGTERM-on-dtor.
//
// Same RAII shape as SpawnedDaemon / SpawnedTray, but stripped to the
// minimum needed to drive a short-lived helper executable (in this file's
// usage: `paplay` or `pw-cat` feeding a WAV file into a null-sink). The
// helper has no readiness contract — the test's PipeWireCapture callback
// is the readiness oracle (first chunk received = playback is producing
// frames), so the ctor here just fires the exec and returns.
//
// Stdout + stderr go to /dev/null by default. Set RECMEET_FULL_STACK_PROC_STDERR=1
// in the environment to keep the child's stderr on the test runner's TTY
// for debugging a stuck paplay / pw-cat call.
//
// argv layout: argv[0] is the executable path (passed to execvp so a bare
// name like "paplay" gets PATH-resolved, but an absolute path also works).
// argv[1..] are forwarded as-is.
//
// Lifetime: dtor SIGTERMs, polls waitpid for 3s, escalates to SIGKILL.
// The 3s budget is shorter than SpawnedDaemon's 5s because paplay/pw-cat
// have no postprocess subprocesses to flush — they're pure audio pipes.
struct BackgroundProc {
    // Forking ctor. argv must be non-empty (argv[0] = executable path);
    // argv[1..] are the program's arguments. The child's argv is built
    // from argv with a nullptr terminator appended for execvp.
    explicit BackgroundProc(const std::vector<std::string>& argv) {
        REQUIRE_FALSE(argv.empty());

        pid_ = fork();
        REQUIRE(pid_ >= 0);

        if (pid_ == 0) {
            // Child: redirect stdout to /dev/null unconditionally (these
            // helpers emit no useful stdout) and stderr to /dev/null
            // unless the operator opted in via env var.
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDOUT_FILENO);
                const char* keep_stderr =
                    std::getenv("RECMEET_FULL_STACK_PROC_STDERR");
                if (!keep_stderr || keep_stderr[0] == '\0') {
                    ::dup2(devnull, STDERR_FILENO);
                }
                ::close(devnull);
            }
            // Build a C-style argv from the std::string vector. The
            // pointers are owned by the strings in this stack-frame; since
            // execvp replaces the process image on success, they don't
            // need to outlive the call. On exec failure we _exit before
            // returning to any caller.
            std::vector<char*> c_argv;
            c_argv.reserve(argv.size() + 1);
            for (const auto& s : argv) {
                c_argv.push_back(const_cast<char*>(s.c_str()));
            }
            c_argv.push_back(nullptr);
            ::execvp(c_argv[0], c_argv.data());
            // exec failed (typically ENOENT for a missing helper); bail
            // out without running atexit handlers.
            _exit(127);
        }
    }

    ~BackgroundProc() {
        if (pid_ <= 0) return;
        ::kill(pid_, SIGTERM);
        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            pid_t r = ::waitpid(pid_, &status, WNOHANG);
            if (r == pid_) return;
            std::this_thread::sleep_for(50ms);
        }
        // Stuck — escalate.
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
    }

    BackgroundProc(const BackgroundProc&) = delete;
    BackgroundProc& operator=(const BackgroundProc&) = delete;

    pid_t pid() const noexcept { return pid_; }

private:
    pid_t pid_ = -1;
};

// ---------------------------------------------------------------------------
// NullSink — load a PipeWire/PulseAudio module-null-sink for the lifetime
// of this object, exposing the resulting sink name + its `.monitor` source.
//
// Why pactl is preferred over pw-cli:
//   * `pactl load-module module-null-sink ...` prints the resulting
//     module-id (a single line on stdout) which we capture for an
//     unambiguous `pactl unload-module <id>` in the dtor. That handoff
//     guarantees we clean up exactly the module we loaded.
//   * `pw-cli load-module libpipewire-module-null-sink ...` returns
//     pw-cli's interactive prompt and a heterogeneous "added/removed"
//     log line that varies across pipewire versions; the cleanest
//     unload path is `pw-cli destroy-module <name>` (by name, not by
//     id), which is best-effort but fine for the fallback case.
//   * pactl exists on any host with pipewire-pulse (the operator's
//     Arch+Sway setup) or full PulseAudio. pw-cli is the pure-PipeWire
//     fallback for hosts that don't ship the PA compatibility shims.
//
// Unique sink name: `<prefix>_<pid>_<ts_ns>_<rand4>` — pid handles
// concurrent test-process invocations, nanosecond timestamp covers
// rapid back-to-back ctor calls in the same test binary, and the
// 4-hex-digit random suffix is belt-and-braces against extremely
// rare collisions if a stale module from a prior crashed run shares
// the same pid/ts.
//
// Lifetime:
//   ctor — composes the unique sink name, tries pactl first (captures
//          module id via popen on the load-module command), falls
//          back to pw-cli on non-zero exit. Issues a Catch2 FAIL if
//          BOTH backends fail (the calling test's SKIP gate is
//          expected to have already filtered out hosts with neither
//          tool available; reaching the ctor and failing to load is a
//          real environment fault, not a tool-presence gap).
//   dtor — pactl unload-module <id> (preferred) or pw-cli
//          destroy-module <name> (fallback). Best-effort; ignores
//          non-zero exit codes — the dtor must not throw.
struct NullSink {
    // Compose-and-load ctor. `prefix` is the static portion of the sink
    // name; the dynamic suffix (pid + ns timestamp + rand4) ensures
    // uniqueness across runs and processes. Typical prefix:
    // "recmeet_test_live".
    explicit NullSink(const std::string& prefix) {
        // Compose a unique sink name. We do all formatting in std::ostringstream
        // to keep the body free of stdio printf-format mismatches.
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
        std::random_device rd;
        std::uniform_int_distribution<unsigned> dist(0, 0xFFFF);
        std::ostringstream nm;
        nm << prefix << "_" << ::getpid() << "_" << now_ns << "_";
        nm << std::hex << dist(rd);
        name_ = nm.str();

        // Try pactl first — preferred for the module-id handoff.
        std::string pactl_cmd =
            "pactl load-module module-null-sink "
            "sink_name=" + name_ +
            " sink_properties=device.description=" + name_ +
            " 2>/dev/null";
        FILE* pipe = ::popen(pactl_cmd.c_str(), "r");
        if (pipe) {
            std::string out;
            char buf[128];
            while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
            int rc = ::pclose(pipe);
            // Trim trailing whitespace.
            while (!out.empty() &&
                   (out.back() == '\n' || out.back() == '\r' ||
                    out.back() == ' '  || out.back() == '\t')) {
                out.pop_back();
            }
            if (rc == 0 && !out.empty()) {
                // Validate the module id is a positive integer — pactl prints
                // the module index as a single decimal line on success.
                bool digits_only = true;
                for (char c : out) {
                    if (c < '0' || c > '9') { digits_only = false; break; }
                }
                if (digits_only) {
                    module_id_ = out;
                    backend_ = Backend::PaCtl;
                    return;
                }
            }
        }

        // Fallback: pw-cli. No module-id handoff; we'll unload by name.
        std::string pw_cmd =
            "pw-cli load-module libpipewire-module-null-sink "
            "media.class=Audio/Sink sink_name=" + name_ +
            " 2>/dev/null >/dev/null";
        int rc = std::system(pw_cmd.c_str());
        if (rc == 0) {
            backend_ = Backend::PwCli;
            return;
        }

        FAIL("NullSink: failed to load module-null-sink via both pactl "
             "and pw-cli (sink_name=" << name_ << "). "
             "Confirm a PipeWire or PulseAudio server is reachable.");
    }

    ~NullSink() {
        if (backend_ == Backend::PaCtl && !module_id_.empty()) {
            std::string cmd = "pactl unload-module " + module_id_
                            + " >/dev/null 2>&1";
            // Best-effort. We don't FAIL on a nonzero exit — the test
            // may already be in teardown and a noisy dtor would just
            // muddy the runner output.
            (void)std::system(cmd.c_str());
        } else if (backend_ == Backend::PwCli && !name_.empty()) {
            std::string cmd = "pw-cli destroy-module " + name_
                            + " >/dev/null 2>&1";
            (void)std::system(cmd.c_str());
        }
    }

    NullSink(const NullSink&) = delete;
    NullSink& operator=(const NullSink&) = delete;

    // The composed sink name (`<prefix>_<pid>_<ts_ns>_<rand4>`). Use this
    // as the `--device=<sink>` argument to paplay or `--target=<sink>`
    // to pw-cat; playback writes INTO the sink.
    const std::string& name() const noexcept { return name_; }

    // The PA-convention name of the .monitor source attached to a
    // null-sink. PipeWire mirrors the convention via the pipewire-pulse
    // shim. Pass this to PipeWireCapture; it appears as a regular source
    // (no STREAM_CAPTURE_SINK property required — `capture_sink=false`).
    std::string monitor_source_name() const { return name_ + ".monitor"; }

private:
    enum class Backend { None, PaCtl, PwCli };
    std::string name_;
    std::string module_id_;     // pactl-only; empty when Backend::PwCli
    Backend backend_ = Backend::None;
};

} // namespace recmeet::full_stack
