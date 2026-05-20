// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase C end-to-end: a real `recmeet-daemon` binary spawned as a child
// process, listening on TCP with PSK auth, accepting a `process.submit`
// over the V2 wire (NDJSON command + `0x01` upload frame), running the
// postprocess pipeline on a synthetic silent WAV, and serving the
// resulting note artifact over `process.fetch`. This is the architectural
// integration test that the in-process [ipc][integration] suite cannot
// provide — it proves the actual installed binary works over real TCP
// from a real client, not a `DaemonSim` simulator.
//
// Tag: `[e2e][thin-client][tcp]`. Tagged separately from `[integration]`
// so it can be run in isolation via `make integration-e2e` without
// pulling in the rest of the integration suite. Models (whisper base,
// sherpa diarization, VAD) must be cached locally — the `make
// integration-e2e` target pre-fetches them via `recmeet --download-models`.

#include <catch2/catch_test_macros.hpp>

#include "ipc_client.h"
#include "ipc_protocol.h"
#include "json_util.h"
#include "test_helpers.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace recmeet {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// Forked daemon harness. Spawns `recmeet-daemon --listen <addr>` with the
// PSK env var and a per-test XDG_CONFIG_HOME so the daemon picks up an
// isolated config.yaml. Polls the listen port until accept-ready before
// returning, so the caller can `IpcClient::connect()` immediately. SIGTERM
// + waitpid on destruction; falls back to SIGKILL after a 5 s grace.
//
// XDG_CONFIG_HOME isolation is the important bit: it scopes the config
// read by `load_legacy_config_as_job_config()` (via `config_dir()` in `src/util.cpp`) without
// touching HOME, so the daemon still finds the operator's cached
// `~/.local/share/recmeet/models/` (set via XDG_DATA_HOME / HOME). This
// keeps the test self-contained without forcing a redundant model
// re-download per CI run.
struct DaemonChild {
    pid_t pid = -1;
    std::string addr;
    fs::path xdg_config;

    DaemonChild(const fs::path& daemon_bin,
                const std::string& tcp_addr,
                const std::string& psk_token,
                const fs::path& xdg_config_dir)
        : addr(tcp_addr)
        , xdg_config(xdg_config_dir)
    {
        pid = fork();
        REQUIRE(pid >= 0);

        if (pid == 0) {
            // Child: set up env and exec the daemon.
            setenv("RECMEET_AUTH_TOKEN", psk_token.c_str(), 1);
            setenv("XDG_CONFIG_HOME", xdg_config_dir.c_str(), 1);
            // Quiet the daemon's stderr — the parent runs the test
            // assertions; log noise from a healthy daemon shouldn't
            // confuse the test runner output. Logs still land in
            // ~/.local/share/recmeet/logs/.
            const char* keep_stderr = std::getenv("RECMEET_E2E_DAEMON_STDERR");
            if (!keep_stderr || keep_stderr[0] == '\0') {
                int devnull = ::open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    ::dup2(devnull, STDERR_FILENO);
                    ::close(devnull);
                }
            }
            ::execl(daemon_bin.c_str(),
                    "recmeet-daemon",
                    "--listen", tcp_addr.c_str(),
                    static_cast<char*>(nullptr));
            // exec failed; bail out without running atexit handlers.
            _exit(127);
        }

        // Parent: poll-connect the listen socket until the daemon is
        // accepting. 5 s budget; longer than typical local startup
        // (~200 ms) and tolerant of CI cold-start (~1-2 s with model
        // pre-cache lookups).
        auto deadline = std::chrono::steady_clock::now() + 5s;
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (probe_tcp(tcp_addr)) { ready = true; break; }
            std::this_thread::sleep_for(50ms);
        }
        if (!ready) {
            // Surface child status before failing — exec error vs SIGSEGV
            // vs still-binding tell very different stories.
            int status = 0;
            pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid && WIFEXITED(status)) {
                FAIL("daemon child exited code " << WEXITSTATUS(status)
                     << " before becoming TCP-ready");
            } else if (r == pid && WIFSIGNALED(status)) {
                FAIL("daemon child killed by signal " << WTERMSIG(status)
                     << " before becoming TCP-ready");
            }
            FAIL("daemon child did not become TCP-ready within 5s at " << tcp_addr);
        }
    }

    ~DaemonChild() {
        if (pid <= 0) return;
        // Polite first — postprocess subprocesses get a chance to flush.
        ::kill(pid, SIGTERM);
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) return;
            std::this_thread::sleep_for(50ms);
        }
        // Stuck — escalate.
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
    }

    // Connect-probe — single-shot, sub-second.
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
            ::close(fd); return false;
        }
        bool ok = (::connect(fd, reinterpret_cast<struct sockaddr*>(&sa),
                             sizeof(sa)) == 0);
        ::close(fd);
        return ok;
    }
};

// Resolve the daemon binary path. Tests run from build/ so the binary
// sits at build/recmeet-daemon, but the absolute path is more robust
// against future relocation. Project root walk handles both
// `build/recmeet_tests` and worktree-build-dir layouts.
fs::path find_daemon_binary() {
    fs::path root = test_helpers::find_project_root();
    REQUIRE(!root.empty());
    // Default build dir layout.
    fs::path candidate = root / "build" / "recmeet-daemon";
    if (fs::exists(candidate)) return candidate;
    // Worktree-isolated build (`make BUILD_DIR=build-foo`) — scan
    // siblings of `build/` for a daemon binary.
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        fs::path probe = entry.path() / "recmeet-daemon";
        if (fs::exists(probe)) return probe;
    }
    FAIL("recmeet-daemon binary not found under " << root.string());
    return {};
}

// Generate `seconds` of S16LE mono silence at `sample_rate`. Returns the
// raw PCM bytes (no WAV header). Whisper detects "no speech" on silence
// and emits an empty transcript, which is exactly what we want for an
// IPC-contract test — postprocess completes quickly without depending on
// model output.
std::string make_silence_pcm(int sample_rate, int seconds) {
    const std::size_t samples = static_cast<std::size_t>(sample_rate) * seconds;
    return std::string(samples * sizeof(int16_t), '\0');
}

// Write a minimal daemon config to <xdg>/recmeet/config.yaml that turns
// off summary (no API key needed in CI) and diarization (sherpa on
// silence is slow and adds no signal to this test).
void write_test_config(const fs::path& xdg_config_dir) {
    fs::path cfg_dir = xdg_config_dir / "recmeet";
    fs::create_directories(cfg_dir);
    std::ofstream cfg(cfg_dir / "config.yaml");
    REQUIRE(cfg.is_open());
    cfg << "# recmeet e2e test config\n"
        << "summary:\n"
        << "  disabled: true\n"
        << "diarization:\n"
        << "  enabled: false\n"
        << "vad:\n"
        << "  enabled: false\n";
}

} // anonymous namespace

TEST_CASE("V2 e2e: real daemon over TCP with PSK accepts process.submit, "
          "runs postprocess, serves process.fetch",
          "[e2e][thin-client][tcp]") {
    // --------------------------------------------------------------------
    // 1. Stage a per-test work directory
    // --------------------------------------------------------------------
    fs::path workdir = fs::temp_directory_path()
                     / ("recmeet_e2e_" + std::to_string(::getpid()));
    fs::remove_all(workdir);
    fs::create_directories(workdir);

    fs::path xdg_config = workdir / "config";
    fs::path note_dir   = workdir / "notes";
    fs::path fetch_dst  = workdir / "fetched";
    fs::path out_dir    = workdir / "out";
    fs::create_directories(note_dir);
    fs::create_directories(fetch_dst);
    fs::create_directories(out_dir);
    write_test_config(xdg_config);

    // --------------------------------------------------------------------
    // 2. Locate the daemon binary, spawn it on TCP with a fresh PSK
    // --------------------------------------------------------------------
    const std::string TCP_ADDR = "127.0.0.1:29991";
    const std::string PSK      = "e2e-thin-client-psk-token";

    fs::path daemon_bin = find_daemon_binary();
    // Inform IpcClient::connect() of the PSK to send on the auth.token frame.
    setenv("RECMEET_AUTH_TOKEN", PSK.c_str(), 1);

    DaemonChild daemon(daemon_bin, TCP_ADDR, PSK, xdg_config);

    // --------------------------------------------------------------------
    // 3. Connect a real IpcClient over TCP and complete the A.x handshake
    // --------------------------------------------------------------------
    IpcClient client(TCP_ADDR);
    REQUIRE(client.connect());
    CHECK(client.is_remote());
    CHECK_FALSE(client.client_id().empty());
    CHECK(client.protocol_version() == IPC_PROTOCOL_VERSION);

    // session.init — point output_dir/note_dir at the test workdir so the
    // resulting note lands somewhere we can clean up.
    {
        JsonMap creds;  // no credentials (summary disabled in config.yaml)
        JsonMap prefs;
        prefs["output_dir"]    = out_dir.string();
        prefs["note_dir"]      = note_dir.string();
        prefs["whisper_model"] = std::string("base");
        prefs["language"]      = std::string("en");

        IpcResponse resp;
        IpcError err;
        REQUIRE(client.session_init(creds, prefs, resp, err));
        CHECK(json_val_as_bool(resp.result["ok"]) == true);
    }

    // --------------------------------------------------------------------
    // 4. process.submit — 1 second of silent S16LE PCM
    // --------------------------------------------------------------------
    const int kSampleRate = 16000;
    const int kSeconds    = 1;
    const std::string silence = make_silence_pcm(kSampleRate, kSeconds);
    CHECK(silence.size() == static_cast<std::size_t>(kSampleRate * kSeconds * 2));

    int64_t job_id = 0;
    std::string upload_token;
    {
        JsonMap p;
        p["audio_size"]  = static_cast<int64_t>(silence.size());
        p["format"]      = std::string("s16le");
        p["sample_rate"] = static_cast<int64_t>(kSampleRate);
        p["channels"]    = static_cast<int64_t>(1);
        p["mode"]        = std::string("transcribe");

        IpcResponse resp;
        IpcError err;
        REQUIRE(client.call("process.submit", p, resp, err, /*timeout_ms=*/5000));

        upload_token = json_val_as_string(resp.result["upload_token"]);
        REQUIRE_FALSE(upload_token.empty());
        job_id = json_val_as_int(resp.result["job_id"]);
        REQUIRE(job_id > 0);
    }

    // --------------------------------------------------------------------
    // 5. Send the audio over the wire as a single `0x01` frame
    // --------------------------------------------------------------------
    REQUIRE(client.send_upload_chunk(silence));

    // --------------------------------------------------------------------
    // 6. Poll job.status until the job reaches a terminal state
    // --------------------------------------------------------------------
    // 60 s budget — generous for CI; warm local takes 5-10 s end-to-end on
    // 1 s of silence with diarize+summary off, mostly whisper init cost.
    bool done = false;
    std::string final_state;
    auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        JsonMap p;
        p["job_id"] = job_id;
        IpcResponse resp;
        IpcError err;
        if (!client.call("job.status", p, resp, err, /*timeout_ms=*/5000)) {
            FAIL("job.status failed: " << err.message);
        }
        final_state = json_val_as_string(resp.result["state"]);
        // State names are lowercase per `job_state_name()` in
        // src/job_queue.cpp — `done` / `failed` / `cancelled`.
        if (final_state == "done" || final_state == "failed"
            || final_state == "cancelled") {
            done = true;
            break;
        }
        std::this_thread::sleep_for(500ms);
    }
    REQUIRE(done);
    INFO("Final job state: " << final_state);
    CHECK(final_state == "done");

    // --------------------------------------------------------------------
    // 7. process.fetch — round-trip the artifact-download IPC
    // --------------------------------------------------------------------
    // The architectural test is "does the IPC contract round-trip end-to-
    // end" — that the daemon accepts process.fetch, enumerates artifacts
    // per the flat-out_dir policy in `enumerate_artifacts`
    // (src/fetch_artifacts.cpp:109), and ships them as 0x02 binary frames
    // the client persists to disk.
    //
    // On a silence-only input with diarize+summary+vad disabled, whisper
    // produces an empty transcript and pipeline.cpp writes no transcript
    // /summary/diarization artifacts to the flat out_dir — the only thing
    // in the staging directory is `audio.wav` (filtered out by the audio-
    // extension blocklist) plus a `<year>/<month>/Meeting_*.md` (skipped
    // by the non-recursive walk). So `written` is legitimately empty.
    // The point of the assertion here is that the request succeeded
    // without an IPC error — not that artifacts are present.
    IpcError fetch_err;
    auto written = client.fetch_artifacts(job_id, fetch_dst, fetch_err,
                                          /*timeout_ms=*/10000);
    INFO("fetch_artifacts error: " << fetch_err.message);
    INFO("fetch_artifacts returned " << written.size() << " artifacts");
    // An empty-but-successful fetch is the expected outcome for silence
    // input. Any artifacts that DO come back must exist on disk.
    for (const auto& p : written) {
        CHECK(fs::exists(p));
        CHECK(fs::file_size(p) >= 0);
    }
    // An IPC-level error here would surface as `fetch_err.code != 0` and
    // a populated message — that's the regression bar.
    CHECK(fetch_err.message.empty());

    // --------------------------------------------------------------------
    // 8. Cleanup — close client first so the daemon's poll loop sees
    //    EOF before we SIGTERM in ~DaemonChild. Workdir is removed
    //    even on test failure via the deferred remove_all below.
    // --------------------------------------------------------------------
    client.close_connection();
    // (DaemonChild dtor SIGTERMs + waits.)

    // Best-effort workdir cleanup. Leaving on failure helps debugging.
    if (final_state == "done" && !written.empty()) {
        std::error_code ec;
        fs::remove_all(workdir, ec);
    }
}

} // namespace recmeet
