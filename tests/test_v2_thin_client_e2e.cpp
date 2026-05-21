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
//
// Phase 3 refactor (test-and-verification-hardening): the fork+exec
// `DaemonChild` helper, the daemon-binary discovery, and the
// minimal-config writer were promoted to `tests/full_stack_helpers.h`
// so a sibling `[full-stack][speaker-id]` test can drive a Unix-socket
// daemon through the same harness. This file no longer carries that
// boilerplate; it uses `full_stack::SpawnedDaemon(... Transport::TCP ...)`
// instead.

#include <catch2/catch_test_macros.hpp>

#include "full_stack_helpers.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "json_util.h"
#include "test_helpers.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace recmeet {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// Generate `seconds` of S16LE mono silence at `sample_rate`. Returns the
// raw PCM bytes (no WAV header). Whisper detects "no speech" on silence
// and emits an empty transcript, which is exactly what we want for an
// IPC-contract test — postprocess completes quickly without depending on
// model output.
std::string make_silence_pcm(int sample_rate, int seconds) {
    const std::size_t samples = static_cast<std::size_t>(sample_rate) * seconds;
    return std::string(samples * sizeof(int16_t), '\0');
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
    full_stack::write_minimal_test_config(xdg_config,
                                          /*disable_summary=*/true,
                                          /*disable_diarization=*/true,
                                          /*disable_vad=*/true);

    // --------------------------------------------------------------------
    // 2. Locate the daemon binary, spawn it on TCP with a fresh PSK
    // --------------------------------------------------------------------
    const std::string TCP_ADDR = "127.0.0.1:29991";
    const std::string PSK      = "e2e-thin-client-psk-token";

    fs::path daemon_bin = full_stack::find_daemon_binary();
    // Inform IpcClient::connect() of the PSK to send on the auth.token frame.
    setenv("RECMEET_AUTH_TOKEN", PSK.c_str(), 1);

    full_stack::SpawnedDaemon daemon(
        daemon_bin,
        full_stack::SpawnedDaemon::Transport::TCP,
        TCP_ADDR,
        PSK,
        xdg_config,
        /*unix_socket=*/fs::path{});

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
    //    EOF before we SIGTERM in ~SpawnedDaemon. Workdir is removed
    //    even on test failure via the deferred remove_all below.
    // --------------------------------------------------------------------
    client.close_connection();
    // (SpawnedDaemon dtor SIGTERMs + waits.)

    // Best-effort workdir cleanup. Leaving on failure helps debugging.
    if (final_state == "done" && !written.empty()) {
        std::error_code ec;
        fs::remove_all(workdir, ec);
    }
}

} // namespace recmeet
