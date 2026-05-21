// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase 2b — production-handler coverage for the three "admin" IPC verbs
// that had zero or stub-only coverage prior to this commit:
//
//   config.reload   — daemon SIGHUP analog; reloads daemon.yaml.
//   models.list     — enumerates the model cache state for every known
//                     model name (the WebUI's /v1/models surface).
//   models.update   — enqueues a force-download Job for every cached
//                     model, refreshing them in-place.
//
// Pattern: each test stands up a DaemonTestHarness, drives the verb
// through an IpcClient round-trip, and asserts the production response
// shape. The harness wires every g_* the production handler reads from
// (g_jobs / g_server_config / etc.) — there are NO stub handlers in
// this file.

#include <catch2/catch_test_macros.hpp>

#include "daemon_test_harness.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "json_util.h"
#include "job_queue.h"

#include <chrono>
#include <string>
#include <thread>

using namespace recmeet;
using recmeet::test::DaemonTestHarness;

// ===========================================================================
// config.reload
// ===========================================================================

TEST_CASE("config.reload returns ok=true on the default daemon path",
          "[ipc][admin][config-reload]") {
    DaemonTestHarness harness;
    harness.start();
    auto client = harness.make_client();

    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("config.reload", JsonMap{}, resp, err));
    CHECK(json_val_as_bool(resp.result["ok"]) == true);
}

// ===========================================================================
// models.list
// ===========================================================================

TEST_CASE("models.list returns a non-empty embedded-JSON models array",
          "[ipc][admin][models-list]") {
    DaemonTestHarness harness;
    harness.start();
    auto client = harness.make_client();

    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("models.list", JsonMap{}, resp, err));

    // The handler emits the model list as an embedded JSON-array
    // STRING — `resp.result["models"]` is a stringified `[...]` blob
    // (see daemon_handlers.cpp:2272-2286). The wire shape was inherited
    // from the WebUI translator. We assert structural markers rather
    // than re-parse the blob.
    auto it = resp.result.find("models");
    REQUIRE(it != resp.result.end());
    const std::string models_blob = json_val_as_string(it->second);
    CHECK(!models_blob.empty());
    CHECK(models_blob.front() == '[');
    CHECK(models_blob.back()  == ']');
    // The WHISPER_MODELS table is non-empty by construction; the
    // blob must therefore contain at least one `category":"whisper"`
    // entry regardless of cache state.
    CHECK(models_blob.find("\"category\":\"whisper\"") != std::string::npos);
}

// ===========================================================================
// models.update
// ===========================================================================

TEST_CASE("models.update returns PermissionDenied when allow_client_downloads=false",
          "[ipc][admin][models-update]") {
    DaemonTestHarness harness;
    harness.mutate_config([](ServerConfig& cfg) {
        cfg.allow_client_downloads = false;
    });
    harness.start();
    auto client = harness.make_client();

    IpcResponse resp;
    IpcError err;
    REQUIRE_FALSE(client->call("models.update", JsonMap{}, resp, err));
    CHECK(err.code == static_cast<int>(IpcErrorCode::PermissionDenied));
    CHECK(err.message.find("allow_client_downloads=false") != std::string::npos);
}

TEST_CASE("models.update returns ok=true with enqueued count when allow_client_downloads=true",
          "[ipc][admin][models-update]") {
    DaemonTestHarness harness;
    // Default config has allow_client_downloads=true.
    harness.start();
    auto client = harness.make_client();

    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("models.update", JsonMap{}, resp, err));
    CHECK(json_val_as_bool(resp.result["ok"]) == true);

    // `enqueued` is an integer ≥ 0 (it counts the cached models the
    // handler enqueued ModelDownload jobs for; on a CI runner with no
    // model cache, the count may legitimately be 0 — we just assert
    // the field is present and non-negative).
    auto eit = resp.result.find("enqueued");
    REQUIRE(eit != resp.result.end());
    CHECK(json_val_as_int(eit->second) >= 0);
}
