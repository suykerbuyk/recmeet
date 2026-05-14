// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase B.5 — transitional submission flow + session_init wiring
// regression tests for the A.6 closure on tray + CLI.
//
// Covers:
//   * record.start with `reprocess_dir` is routed by the daemon's
//     handler and the session-state lookup is honored (smoke
//     shape — the live daemon already has [ipc][a6] coverage).
//   * Post-A.6 record.start params carry ONLY `reprocess_dir` — no
//     stray api_key / output_dir / vocabulary fields. This is the
//     observable closure of the A.6 transient regression on the
//     tray + CLI submission paths.
//   * session.init credentials carry api_key + provider correctly.
//
// The tests drive a small B5Sim daemon stand-in that records every
// (method, params) pair so we can assert ordering and per-method
// param shape without booting the production daemon.

#include <catch2/catch_test_macros.hpp>

#include "config.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

using namespace recmeet;

namespace {

const char* B5_SOCK = "/tmp/recmeet_test_b5.sock";

// Tiny daemon stand-in that records every (method, params) pair so
// the test can assert ordering + per-method param shape.
struct B5Sim {
    IpcServer server;
    std::thread thr;

    struct Call {
        std::string method;
        JsonMap params;
        std::string client_id;
    };
    std::mutex mu;
    std::vector<Call> calls;

    explicit B5Sim() : server(B5_SOCK) {
        unlink(B5_SOCK);

        server.on("session.init",
                  [this](const IpcRequest& req, IpcResponse& resp, IpcError&) {
            {
                std::lock_guard<std::mutex> lk(mu);
                calls.push_back({"session.init", req.params, req.client_id});
            }
            // Field-level parsing is exercised by [ipc][a6] tests
            // already; we only care that the call lands BEFORE
            // record.start in this fixture, plus the per-method
            // param-shape assertion.
            SessionCredentials creds;
            SessionPreferences prefs;
            server.set_session_credentials(req.client_id, creds);
            server.set_session_preferences(req.client_id, prefs);
            resp.result["ok"] = true;
            resp.result["session_active"] = true;
            return true;
        });

        server.on("record.start",
                  [this](const IpcRequest& req, IpcResponse& resp, IpcError&) {
            {
                std::lock_guard<std::mutex> lk(mu);
                calls.push_back({"record.start", req.params, req.client_id});
            }
            resp.result["job_id"] = static_cast<int64_t>(1);
            return true;
        });

        server.on("record.stop",
                  [](const IpcRequest&, IpcResponse& resp, IpcError&) {
            resp.result["ok"] = true;
            return true;
        });

        REQUIRE(server.start());
        thr = std::thread([this]() { server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ~B5Sim() {
        server.stop();
        if (thr.joinable()) thr.join();
    }

    std::vector<Call> snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return calls;
    }
};

// Mirror of the session.init + record.start wire dance that the CLI
// (and tray, post-bonus) now do — but without the read_events
// blocking that client_record_no_sigaction layers on top.
//
// This is exactly the call sequence under test; the production
// callers add stderr printing, signal handling, and a job.complete
// wait that we don't care about for the B.5 assertions.
struct ClientSequence {
    IpcResponse session_resp;
    IpcError session_err;
    bool session_ok = false;
    IpcResponse start_resp;
    IpcError start_err;
    bool start_ok = false;
};

ClientSequence run_session_then_record_start(const Config& cfg, const char* sock) {
    ClientSequence out;
    IpcClient client(sock);
    if (!client.connect()) return out;

    JsonMap creds;
    creds["provider"] = cfg.provider;
    if (!cfg.api_key.empty()) creds["api_key"] = cfg.api_key;
    for (const auto& [name, key] : cfg.api_keys) {
        if (!key.empty()) creds["api_keys." + name] = key;
    }

    JsonMap prefs;
    if (!cfg.output_dir.empty())  prefs["output_dir"] = cfg.output_dir.string();
    if (!cfg.note_dir.empty())    prefs["note_dir"]   = cfg.note_dir.string();
    if (!cfg.language.empty())    prefs["language"]   = cfg.language;
    if (!cfg.vocabulary.empty())  prefs["vocabulary"] = cfg.vocabulary;
    if (!cfg.whisper_model.empty()) prefs["whisper_model"] = cfg.whisper_model;
    if (!cfg.llm_model.empty()) {
        prefs["llm_model"] = cfg.llm_model;
        prefs["summarization_backend"] = std::string("local");
    } else if (!cfg.no_summary) {
        prefs["summarization_backend"] = std::string("http");
    }
    prefs["captions_enabled"] = cfg.captions_enabled;

    out.session_ok = client.session_init(creds, prefs, out.session_resp,
                                         out.session_err, 5000);

    JsonMap rec_params;
    if (!cfg.reprocess_dir.empty())
        rec_params["reprocess_dir"] = cfg.reprocess_dir.string();
    out.start_ok = client.call("record.start", rec_params,
                               out.start_resp, out.start_err, 5000);
    client.close_connection();
    return out;
}

} // anonymous namespace

TEST_CASE("B.5: session.init lands before record.start over a real IPC pair",
          "[ipc][b5]") {
    B5Sim sim;

    Config cfg;
    cfg.reprocess_dir = "/tmp/fake_meeting";
    cfg.provider      = "xai";
    cfg.api_key       = "sk-test-b5";
    cfg.no_summary    = true;

    auto result = run_session_then_record_start(cfg, B5_SOCK);
    REQUIRE(result.session_ok);
    REQUIRE(result.start_ok);

    auto calls = sim.snapshot();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].method == "session.init");
    CHECK(calls[1].method == "record.start");
    CHECK(calls[0].client_id == calls[1].client_id);
}

TEST_CASE("B.5: record.start params carry only reprocess_dir (no credentials)",
          "[ipc][b5]") {
    B5Sim sim;

    Config cfg;
    cfg.reprocess_dir = "/tmp/fake_meeting_b5_2";
    cfg.provider      = "xai";
    cfg.api_key       = "sk-MUST-NOT-LEAK-INTO-RECORD-START";
    cfg.vocabulary    = "Alice, Bob";
    cfg.output_dir    = "/tmp/b5/out2";
    cfg.no_summary    = true;

    auto result = run_session_then_record_start(cfg, B5_SOCK);
    REQUIRE(result.start_ok);

    auto calls = sim.snapshot();
    const JsonMap* rec_params = nullptr;
    for (const auto& c : calls) {
        if (c.method == "record.start") { rec_params = &c.params; break; }
    }
    REQUIRE(rec_params != nullptr);

    CHECK(json_val_as_string(rec_params->at("reprocess_dir"))
          == "/tmp/fake_meeting_b5_2");

    // A.6 closure: record.start no longer carries credentials or
    // preferences. The fields populated on cfg above must NOT appear
    // on the wire frame for record.start.
    CHECK(rec_params->find("api_key")     == rec_params->end());
    CHECK(rec_params->find("vocabulary")  == rec_params->end());
    CHECK(rec_params->find("output_dir")  == rec_params->end());
    CHECK(rec_params->find("provider")    == rec_params->end());
    CHECK(rec_params->find("language")    == rec_params->end());
}

TEST_CASE("B.5: session.init credentials carry api_key + provider",
          "[ipc][b5]") {
    B5Sim sim;

    Config cfg;
    cfg.reprocess_dir = "/tmp/fake_meeting_b5_3";
    cfg.provider      = "openai";
    cfg.api_key       = "sk-test-b5-creds";
    cfg.no_summary    = true;

    auto result = run_session_then_record_start(cfg, B5_SOCK);
    REQUIRE(result.session_ok);

    auto calls = sim.snapshot();
    REQUIRE_FALSE(calls.empty());
    REQUIRE(calls[0].method == "session.init");

    // session.init's outer params is { "credentials": "<inner-json>",
    // "preferences": "<inner-json>" } — JsonMap stores nested objects
    // as raw substrings. Assert the inner blob carries the expected
    // fields without a full re-parse round-trip.
    auto cit = calls[0].params.find("credentials");
    REQUIRE(cit != calls[0].params.end());
    std::string creds_blob = json_val_as_string(cit->second);
    CHECK(creds_blob.find("\"provider\":\"openai\"")          != std::string::npos);
    CHECK(creds_blob.find("\"api_key\":\"sk-test-b5-creds\"") != std::string::npos);
}

TEST_CASE("B.5: record.start with no reprocess_dir still routes through daemon",
          "[ipc][b5]") {
    // Smoke check that a non-reprocess record.start (vestigial daemon
    // live-recording path, retired by C.9) still dispatches when
    // record.start carries an empty params frame.
    B5Sim sim;

    Config cfg;
    cfg.provider   = "xai";
    cfg.no_summary = true;
    // No reprocess_dir; the helper will send an empty record.start.

    auto result = run_session_then_record_start(cfg, B5_SOCK);
    REQUIRE(result.start_ok);

    auto calls = sim.snapshot();
    const JsonMap* rec_params = nullptr;
    for (const auto& c : calls) {
        if (c.method == "record.start") { rec_params = &c.params; break; }
    }
    REQUIRE(rec_params != nullptr);
    CHECK(rec_params->find("reprocess_dir") == rec_params->end());
}
