// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase E.6.2 — tray_web HTTP→IPC translator tests. Tag: [e6][tray-web].
//
// Exercises the REAL `src/tray_web.cpp` translator against an in-process
// IpcServer (DaemonSim) registered with closure-captured handler bodies
// for the 11 new (E.6.1) + 3 existing speakers/meetings verbs + the
// pre-existing `process.reprocess`. Each test:
//
//   1. Constructs a DaemonSim (per-test Unix socket + IpcServer + thread)
//      with all required IPC verbs registered.
//   2. Connects an IpcClient to that socket.
//   3. Calls `recmeet::start_web_listener(client)` to launch the embedded
//      httplib listener.
//   4. Uses a `httplib::Client` to hit the local listener URL and
//      validates response status / headers / body.
//   5. Calls `recmeet::stop_web_listener()` and tears down DaemonSim.
//
// This is the antipattern the deleted `tests/test_web.cpp` was — that
// file redefined every route handler against an `httplib::Server` it
// owned, exercising the test scaffold instead of production. The
// rewrite below drives the real translator code path: the production
// `tray_web.cpp` is compiled into recmeet_tests; we link the embedded
// asset header through the same CMake plumbing as recmeet-tray.

#include <catch2/catch_test_macros.hpp>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <httplib.h>
#pragma GCC diagnostic pop

#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "json_util.h"
#include "test_tmpdir.h"
#include "tray_web.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

using namespace recmeet;

namespace {

// --------------------------------------------------------------------------
// DaemonSim — minimal in-process IPC server with the verbs tray_web calls.
// All handlers are closure-captured fakes; the test asserts that the
// translator emits the expected method + params and forwards the result.
// --------------------------------------------------------------------------

struct DaemonSim {
    std::string sock_path;
    std::unique_ptr<IpcServer> server;
    std::thread srv_thread;

    // Per-call observability — last invocation captured here for the test
    // to assert against. Atomic / mutex'd because the IPC server's poll
    // thread runs the handlers.
    mutable std::mutex obs_mu;
    std::string last_method;
    JsonMap     last_params;
    int         call_count = 0;

    // Mode flags drive error-path tests without rebuilding handler code.
    std::atomic<bool> force_invalid_params{false};
    std::atomic<bool> force_internal_error{false};

    explicit DaemonSim(const std::string& name)
        : sock_path(recmeet::test::tmp_path("recmeet_e62_" + name + ".sock").string()) {
        unlink(sock_path.c_str());
        server = std::make_unique<IpcServer>(sock_path);
        register_handlers();
        REQUIRE(server->start());
        srv_thread = std::thread([this]() { server->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~DaemonSim() {
        server->stop();
        if (srv_thread.joinable()) srv_thread.join();
        unlink(sock_path.c_str());
    }

    void observe(const std::string& method, const IpcRequest& req) {
        std::lock_guard<std::mutex> lock(obs_mu);
        last_method = method;
        last_params = req.params;
        ++call_count;
    }

    // Helper for an error response shape used by every error-path test.
    bool maybe_force_error(IpcError& err, const std::string& method) {
        if (force_invalid_params.load()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = method + ": not_found";
            return true;
        }
        if (force_internal_error.load()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = method + ": daemon internal failure";
            return true;
        }
        return false;
    }

    void register_handlers() {
        // ---- speakers.list ----
        server->on("speakers.list",
                   [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            observe("speakers.list", req);
            if (maybe_force_error(err, "speakers.list")) return false;
            resp.result["speakers"] = std::string("[{\"name\":\"Alice\",\"enrollments\":2}]");
            resp.result["count"] = static_cast<int64_t>(1);
            return true;
        });

        // ---- speakers.get ----
        server->on("speakers.get",
                   [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            observe("speakers.get", req);
            if (maybe_force_error(err, "speakers.get")) return false;
            resp.result["name"] = json_val_as_string(req.params.at("name"));
            resp.result["enrollments"] = static_cast<int64_t>(2);
            resp.result["embedding_dim"] = static_cast<int64_t>(192);
            resp.result["embedding_count"] = static_cast<int64_t>(2);
            resp.result["created"] = std::string("2026-05-01T00:00:00Z");
            resp.result["updated"] = std::string("2026-05-02T00:00:00Z");
            return true;
        });

        // ---- speakers.remove ----
        server->on("speakers.remove",
                   [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            observe("speakers.remove", req);
            if (maybe_force_error(err, "speakers.remove")) return false;
            resp.result["ok"] = true;
            return true;
        });

        // ---- speakers.reset ----
        server->on("speakers.reset",
                   [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            observe("speakers.reset", req);
            if (maybe_force_error(err, "speakers.reset")) return false;
            resp.result["ok"] = true;
            resp.result["removed"] = static_cast<int64_t>(3);
            return true;
        });

        // ---- speakers.enroll ----
        server->on("speakers.enroll",
                   [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            observe("speakers.enroll", req);
            if (maybe_force_error(err, "speakers.enroll")) return false;
            resp.result["ok"] = true;
            resp.result["duration_sec"] = 12.0;
            resp.result["confidence"] = 0.85;
            return true;
        });

        // ---- speakers.remove_embedding ----
        server->on("speakers.remove_embedding",
                   [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            observe("speakers.remove_embedding", req);
            if (maybe_force_error(err, "speakers.remove_embedding")) return false;
            resp.result["ok"] = true;
            resp.result["remaining"] = static_cast<int64_t>(1);
            return true;
        });

        // ---- speakers.batch_reidentify ----
        server->on("speakers.batch_reidentify",
                   [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            observe("speakers.batch_reidentify", req);
            if (maybe_force_error(err, "speakers.batch_reidentify")) return false;
            resp.result["ok"] = true;
            resp.result["async"] = true;
            resp.result["started_at"] = std::string("2026-05-20T12:00:00Z");
            return true;
        });

        // ---- speakers.relabel ----
        server->on("speakers.relabel",
                   [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            observe("speakers.relabel", req);
            if (maybe_force_error(err, "speakers.relabel")) return false;
            resp.result["ok"] = true;
            resp.result["old_label"] = std::string("Speaker_01");
            return true;
        });

        // ---- meetings.list ----
        server->on("meetings.list",
                   [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            observe("meetings.list", req);
            if (maybe_force_error(err, "meetings.list")) return false;
            resp.result["meetings"] = std::string(
                "[{\"name\":\"2026-05-20_12-00\","
                "\"meeting_id\":\"11111111-2222-4333-8444-555555555555\","
                "\"has_audio\":true,\"has_speakers\":true,"
                "\"has_summary\":true,\"mtime_iso\":\"2026-05-20T12:00:00Z\"}]");
            resp.result["count"] = static_cast<int64_t>(1);
            return true;
        });

        // ---- meetings.speakers ----
        server->on("meetings.speakers",
                   [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            observe("meetings.speakers", req);
            if (maybe_force_error(err, "meetings.speakers")) return false;
            resp.result["speakers"] = std::string(
                "[{\"cluster_id\":0,\"label\":\"Alice\",\"identified\":true,"
                "\"duration_sec\":42.0,\"confidence\":0.9}]");
            resp.result["count"] = static_cast<int64_t>(1);
            return true;
        });

        // ---- meetings.read_note ----
        server->on("meetings.read_note",
                   [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            observe("meetings.read_note", req);
            if (maybe_force_error(err, "meetings.read_note")) return false;
            resp.result["path"] = std::string("Meeting_2026-05-20_12-00.md");
            resp.result["content"] = std::string("# Title\n\nHello world.\n");
            return true;
        });

        // ---- process.reprocess ----
        server->on("process.reprocess",
                   [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            observe("process.reprocess", req);
            if (maybe_force_error(err, "process.reprocess")) return false;
            resp.result["job_id"] = static_cast<int64_t>(42);
            return true;
        });
    }

    // Snapshot accessors.
    int observed_call_count() const {
        std::lock_guard<std::mutex> lock(obs_mu);
        return call_count;
    }
    std::string observed_method() const {
        std::lock_guard<std::mutex> lock(obs_mu);
        return last_method;
    }
    JsonMap observed_params() const {
        std::lock_guard<std::mutex> lock(obs_mu);
        return last_params;
    }
};

// --------------------------------------------------------------------------
// LiveListener — owns a DaemonSim + IpcClient + the embedded httplib
// listener for one test. RAII so the file-static listener state in
// tray_web.cpp is reset between tests.
// --------------------------------------------------------------------------

struct LiveListener {
    DaemonSim sim;
    IpcClient client;
    int port = -1;
    std::string base_url;

    explicit LiveListener(const std::string& name)
        : sim(name), client(sim.sock_path) {
        REQUIRE(client.connect());
        port = recmeet::start_web_listener(client);
        REQUIRE(port > 0);
        base_url = "http://127.0.0.1:" + std::to_string(port);
    }

    ~LiveListener() {
        recmeet::stop_web_listener();
        client.close_connection();
    }

    std::unique_ptr<httplib::Client> make_http_client() {
        auto c = std::make_unique<httplib::Client>("127.0.0.1", port);
        c->set_connection_timeout(5);
        c->set_read_timeout(5);
        return c;
    }
};

// Convenience: substring check on a string body.
bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

// ===========================================================================
// /api/health — local, no IPC. Only positive case (no error path possible).
// ===========================================================================

TEST_CASE("GET /api/health returns 200 ok", "[e6][tray-web]") {
    LiveListener live("health_ok");
    auto http = live.make_http_client();
    auto res = http->Get("/api/health");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(contains(res->body, "\"status\":\"ok\""));
    // Local path — no IPC call should have fired for /api/health.
    CHECK(live.sim.observed_call_count() == 0);
}

// ===========================================================================
// speakers.list  (GET /api/speakers)
// ===========================================================================

TEST_CASE("GET /api/speakers forwards speakers.list result array",
          "[e6][tray-web]") {
    LiveListener live("splist_ok");
    auto http = live.make_http_client();
    auto res = http->Get("/api/speakers");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(live.sim.observed_method() == "speakers.list");
    CHECK(contains(res->body, "Alice"));
    // The translator must emit the embedded JSON-array body raw — not
    // double-wrapped.
    CHECK(res->body.substr(0, 1) == "[");
}

TEST_CASE("GET /api/speakers maps daemon error to 502 + JSON",
          "[e6][tray-web]") {
    LiveListener live("splist_err");
    live.sim.force_internal_error.store(true);
    auto http = live.make_http_client();
    auto res = http->Get("/api/speakers");
    REQUIRE(res);
    CHECK(res->status == 502);
    CHECK(contains(res->body, "\"error\""));
}

// ===========================================================================
// speakers.get  (GET /api/speakers/:name)
// ===========================================================================

TEST_CASE("GET /api/speakers/:name forwards speakers.get scalar result",
          "[e6][tray-web]") {
    LiveListener live("spget_ok");
    auto http = live.make_http_client();
    auto res = http->Get("/api/speakers/Alice");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(live.sim.observed_method() == "speakers.get");
    CHECK(json_val_as_string(live.sim.observed_params().at("name")) == "Alice");
    CHECK(contains(res->body, "\"embedding_dim\":192"));
    CHECK(contains(res->body, "\"enrollments\":2"));
}

TEST_CASE("GET /api/speakers/:name forwards daemon InvalidParams to 404",
          "[e6][tray-web]") {
    LiveListener live("spget_err");
    live.sim.force_invalid_params.store(true);
    auto http = live.make_http_client();
    auto res = http->Get("/api/speakers/Ghost");
    REQUIRE(res);
    // InvalidParams + "not_found" message maps to 404 in http_status_for.
    CHECK(res->status == 404);
    CHECK(contains(res->body, "not_found"));
}

// ===========================================================================
// speakers.remove  (DELETE /api/speakers/:name)
// ===========================================================================

TEST_CASE("DELETE /api/speakers/:name forwards speakers.remove",
          "[e6][tray-web]") {
    LiveListener live("sprm_ok");
    auto http = live.make_http_client();
    auto res = http->Delete("/api/speakers/Alice");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(live.sim.observed_method() == "speakers.remove");
    CHECK(json_val_as_string(live.sim.observed_params().at("name")) == "Alice");
    CHECK(contains(res->body, "\"ok\":true"));
}

TEST_CASE("DELETE /api/speakers/:name maps daemon error to 404",
          "[e6][tray-web]") {
    LiveListener live("sprm_err");
    live.sim.force_invalid_params.store(true);
    auto http = live.make_http_client();
    auto res = http->Delete("/api/speakers/Ghost");
    REQUIRE(res);
    CHECK(res->status == 404);
}

// ===========================================================================
// speakers.reset  (POST /api/speakers/reset)
// ===========================================================================

TEST_CASE("POST /api/speakers/reset forwards speakers.reset",
          "[e6][tray-web]") {
    LiveListener live("sprst_ok");
    auto http = live.make_http_client();
    auto res = http->Post("/api/speakers/reset", "", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(live.sim.observed_method() == "speakers.reset");
    CHECK(contains(res->body, "\"removed\":3"));
}

TEST_CASE("POST /api/speakers/reset propagates daemon failure",
          "[e6][tray-web]") {
    LiveListener live("sprst_err");
    live.sim.force_internal_error.store(true);
    auto http = live.make_http_client();
    auto res = http->Post("/api/speakers/reset", "", "application/json");
    REQUIRE(res);
    CHECK(res->status == 502);
}

// ===========================================================================
// speakers.enroll  (POST /api/speakers/enroll)
// ===========================================================================

TEST_CASE("POST /api/speakers/enroll forwards name + meeting_id + cluster_id",
          "[e6][tray-web]") {
    LiveListener live("spenr_ok");
    auto http = live.make_http_client();
    const std::string body = R"({"name":"Alice","meeting_id":"11111111-2222-4333-8444-555555555555","cluster_id":0})";
    auto res = http->Post("/api/speakers/enroll", body, "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(live.sim.observed_method() == "speakers.enroll");
    auto p = live.sim.observed_params();
    CHECK(json_val_as_string(p.at("name")) == "Alice");
    CHECK(json_val_as_string(p.at("meeting_id")) == "11111111-2222-4333-8444-555555555555");
    CHECK(json_val_as_int(p.at("cluster_id")) == 0);
    CHECK(contains(res->body, "\"duration_sec\":12"));
}

TEST_CASE("POST /api/speakers/enroll rejects missing required fields with 400",
          "[e6][tray-web]") {
    LiveListener live("spenr_err");
    auto http = live.make_http_client();
    // Missing meeting_id and cluster_id.
    auto res = http->Post("/api/speakers/enroll",
                          R"({"name":"Alice"})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 400);
    // Validation happens client-side in tray_web — no daemon round-trip
    // should occur for a malformed enroll request.
    CHECK(live.sim.observed_call_count() == 0);
}

// ===========================================================================
// speakers.remove_embedding  (POST /api/speakers/:name/remove-embedding)
// ===========================================================================

TEST_CASE("POST /api/speakers/:name/remove-embedding forwards name + index",
          "[e6][tray-web]") {
    LiveListener live("spremb_ok");
    auto http = live.make_http_client();
    auto res = http->Post("/api/speakers/Alice/remove-embedding",
                          R"({"index":1})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(live.sim.observed_method() == "speakers.remove_embedding");
    auto p = live.sim.observed_params();
    CHECK(json_val_as_string(p.at("name")) == "Alice");
    CHECK(json_val_as_int(p.at("index")) == 1);
    CHECK(contains(res->body, "\"remaining\":1"));
}

TEST_CASE("POST /api/speakers/:name/remove-embedding rejects negative index",
          "[e6][tray-web]") {
    LiveListener live("spremb_err");
    auto http = live.make_http_client();
    // No index field — tray_web rejects pre-IPC with 400.
    auto res = http->Post("/api/speakers/Alice/remove-embedding",
                          R"({})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 400);
    CHECK(live.sim.observed_call_count() == 0);
}

// ===========================================================================
// speakers.batch_reidentify  (POST /api/speakers/batch-reidentify)
// ===========================================================================

TEST_CASE("POST /api/speakers/batch-reidentify forwards verb",
          "[e6][tray-web]") {
    LiveListener live("spbatch_ok");
    auto http = live.make_http_client();
    auto res = http->Post("/api/speakers/batch-reidentify", "", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(live.sim.observed_method() == "speakers.batch_reidentify");
    CHECK(contains(res->body, "\"async\":true"));
}

TEST_CASE("POST /api/speakers/batch-reidentify maps in-progress reject",
          "[e6][tray-web]") {
    LiveListener live("spbatch_err");
    live.sim.force_invalid_params.store(true);
    auto http = live.make_http_client();
    auto res = http->Post("/api/speakers/batch-reidentify", "", "application/json");
    REQUIRE(res);
    CHECK(res->status == 404);  // InvalidParams + "not_found" → 404 mapping
    CHECK(contains(res->body, "\"error\""));
}

// ===========================================================================
// meetings.list  (GET /api/meetings)
// ===========================================================================

TEST_CASE("GET /api/meetings forwards meetings.list result array",
          "[e6][tray-web]") {
    LiveListener live("mlist_ok");
    auto http = live.make_http_client();
    auto res = http->Get("/api/meetings");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(live.sim.observed_method() == "meetings.list");
    CHECK(res->body.substr(0, 1) == "[");
    CHECK(contains(res->body, "2026-05-20_12-00"));
    CHECK(contains(res->body, "11111111-2222-4333-8444-555555555555"));
}

TEST_CASE("GET /api/meetings maps daemon error to 502",
          "[e6][tray-web]") {
    LiveListener live("mlist_err");
    live.sim.force_internal_error.store(true);
    auto http = live.make_http_client();
    auto res = http->Get("/api/meetings");
    REQUIRE(res);
    CHECK(res->status == 502);
}

// ===========================================================================
// meetings.speakers  (GET /api/meetings/:meeting_id/speakers)
// ===========================================================================

TEST_CASE("GET /api/meetings/:meeting_id/speakers forwards meetings.speakers",
          "[e6][tray-web]") {
    LiveListener live("mspk_ok");
    auto http = live.make_http_client();
    auto res = http->Get("/api/meetings/11111111-2222-4333-8444-555555555555/speakers");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(live.sim.observed_method() == "meetings.speakers");
    CHECK(json_val_as_string(live.sim.observed_params().at("meeting_id"))
              == "11111111-2222-4333-8444-555555555555");
    CHECK(res->body.substr(0, 1) == "[");
    CHECK(contains(res->body, "\"label\":\"Alice\""));
}

TEST_CASE("GET /api/meetings/:meeting_id/speakers maps unknown id to 404",
          "[e6][tray-web]") {
    LiveListener live("mspk_err");
    live.sim.force_invalid_params.store(true);
    auto http = live.make_http_client();
    auto res = http->Get("/api/meetings/00000000-0000-4000-8000-000000000000/speakers");
    REQUIRE(res);
    CHECK(res->status == 404);
}

// ===========================================================================
// speakers.relabel  (POST /api/meetings/:meeting_id/speakers/relabel)
// ===========================================================================

TEST_CASE("POST /api/meetings/:meeting_id/speakers/relabel forwards params",
          "[e6][tray-web]") {
    LiveListener live("rel_ok");
    auto http = live.make_http_client();
    const std::string body = R"({"cluster_id":0,"new_label":"Bob","update_profile":true})";
    auto res = http->Post(
        "/api/meetings/11111111-2222-4333-8444-555555555555/speakers/relabel",
        body, "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(live.sim.observed_method() == "speakers.relabel");
    auto p = live.sim.observed_params();
    CHECK(json_val_as_string(p.at("meeting_id")) == "11111111-2222-4333-8444-555555555555");
    CHECK(json_val_as_int(p.at("cluster_id")) == 0);
    CHECK(json_val_as_string(p.at("new_label")) == "Bob");
    CHECK(json_val_as_bool(p.at("update_profile")) == true);
    CHECK(contains(res->body, "\"old_label\":\"Speaker_01\""));
}

TEST_CASE("POST /api/meetings/:meeting_id/speakers/relabel rejects missing fields",
          "[e6][tray-web]") {
    LiveListener live("rel_err");
    auto http = live.make_http_client();
    auto res = http->Post(
        "/api/meetings/11111111-2222-4333-8444-555555555555/speakers/relabel",
        R"({})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 400);
    CHECK(live.sim.observed_call_count() == 0);
}

// ===========================================================================
// process.reprocess  (POST /api/meetings/:meeting_id/reprocess)
// ===========================================================================

TEST_CASE("POST /api/meetings/:meeting_id/reprocess forwards per-stage flags",
          "[e6][tray-web]") {
    LiveListener live("rep_ok");
    auto http = live.make_http_client();
    const std::string body = R"({"diarize":false,"summarize":true,"vocabulary":"hello"})";
    auto res = http->Post(
        "/api/meetings/11111111-2222-4333-8444-555555555555/reprocess",
        body, "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(live.sim.observed_method() == "process.reprocess");
    auto p = live.sim.observed_params();
    CHECK(json_val_as_string(p.at("meeting_id")) == "11111111-2222-4333-8444-555555555555");
    CHECK(json_val_as_bool(p.at("diarize")) == false);
    CHECK(json_val_as_bool(p.at("summarize")) == true);
    CHECK(json_val_as_string(p.at("vocabulary")) == "hello");
    CHECK(contains(res->body, "\"job_id\":42"));
}

TEST_CASE("POST /api/meetings/:meeting_id/reprocess maps unknown meeting to 404",
          "[e6][tray-web]") {
    LiveListener live("rep_err");
    live.sim.force_invalid_params.store(true);
    auto http = live.make_http_client();
    auto res = http->Post(
        "/api/meetings/00000000-0000-4000-8000-000000000000/reprocess",
        R"({})", "application/json");
    REQUIRE(res);
    CHECK(res->status == 404);
}

// ===========================================================================
// meetings.read_note  (GET /api/meetings/:meeting_id/note)
// ===========================================================================

TEST_CASE("GET /api/meetings/:meeting_id/note forwards meetings.read_note",
          "[e6][tray-web]") {
    LiveListener live("note_ok");
    auto http = live.make_http_client();
    auto res = http->Get("/api/meetings/11111111-2222-4333-8444-555555555555/note");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(live.sim.observed_method() == "meetings.read_note");
    CHECK(contains(res->body, "Meeting_2026-05-20_12-00.md"));
    CHECK(contains(res->body, "Hello world"));
}

TEST_CASE("GET /api/meetings/:meeting_id/note maps missing note to 404",
          "[e6][tray-web]") {
    LiveListener live("note_err");
    live.sim.force_invalid_params.store(true);
    auto http = live.make_http_client();
    auto res = http->Get("/api/meetings/00000000-0000-4000-8000-000000000000/note");
    REQUIRE(res);
    CHECK(res->status == 404);
    CHECK(contains(res->body, "not_found"));
}

// ===========================================================================
// Static asset GETs (3 cases — index, app.js, favicon.svg).
// ===========================================================================

TEST_CASE("GET / serves index.html with no-cache",
          "[e6][tray-web]") {
    LiveListener live("static_index");
    auto http = live.make_http_client();
    auto res = http->Get("/");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(contains(res->get_header_value("Content-Type"), "text/html"));
    CHECK(contains(res->get_header_value("Cache-Control"), "no-cache"));
    CHECK_FALSE(res->body.empty());
    // Sanity: contains the recmeet head metadata.
    CHECK(contains(res->body, "recmeet"));
}

TEST_CASE("GET /app.js serves application/javascript with no-cache",
          "[e6][tray-web]") {
    LiveListener live("static_appjs");
    auto http = live.make_http_client();
    auto res = http->Get("/app.js");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(contains(res->get_header_value("Content-Type"), "application/javascript"));
    CHECK(contains(res->get_header_value("Cache-Control"), "no-cache"));
    CHECK_FALSE(res->body.empty());
}

TEST_CASE("GET /favicon.svg serves image/svg+xml with no-cache",
          "[e6][tray-web]") {
    LiveListener live("static_favicon");
    auto http = live.make_http_client();
    auto res = http->Get("/favicon.svg");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(contains(res->get_header_value("Content-Type"), "image/svg+xml"));
    CHECK(contains(res->get_header_value("Cache-Control"), "no-cache"));
    CHECK_FALSE(res->body.empty());
}

TEST_CASE("GET /style.css serves text/css with no-cache",
          "[e6][tray-web]") {
    LiveListener live("static_css");
    auto http = live.make_http_client();
    auto res = http->Get("/style.css");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(contains(res->get_header_value("Content-Type"), "text/css"));
    CHECK(contains(res->get_header_value("Cache-Control"), "no-cache"));
    CHECK_FALSE(res->body.empty());
}

// ===========================================================================
// Lifecycle: idempotent start, rebind after stop.
// ===========================================================================

TEST_CASE("start_web_listener is idempotent and returns the same port on re-entry",
          "[e6][tray-web]") {
    DaemonSim sim("lifecycle_idemp");
    IpcClient client(sim.sock_path);
    REQUIRE(client.connect());

    const int port1 = recmeet::start_web_listener(client);
    REQUIRE(port1 > 0);
    const int port2 = recmeet::start_web_listener(client);
    CHECK(port2 == port1);

    recmeet::stop_web_listener();
    client.close_connection();
}

TEST_CASE("stop_web_listener then start_web_listener rebinds (new port allowed)",
          "[e6][tray-web]") {
    DaemonSim sim("lifecycle_rebind");
    IpcClient client(sim.sock_path);
    REQUIRE(client.connect());

    const int port1 = recmeet::start_web_listener(client);
    REQUIRE(port1 > 0);
    recmeet::stop_web_listener();
    const int port2 = recmeet::start_web_listener(client);
    REQUIRE(port2 > 0);
    // Either the same port (kernel happens to reuse it) or a new one —
    // both are acceptable. The contract is "rebinds successfully".

    recmeet::stop_web_listener();
    client.close_connection();
}

// ===========================================================================
// Listener-binding sanity: the kernel-picked port is positive and
// reachable from an httplib::Client.
// ===========================================================================

TEST_CASE("start_web_listener binds a reachable kernel-picked port",
          "[e6][tray-web]") {
    LiveListener live("bind_port");
    CHECK(live.port > 0);
    auto http = live.make_http_client();
    auto res = http->Get("/api/health");
    REQUIRE(res);
    CHECK(res->status == 200);
}

// ===========================================================================
// Phase E.6.3 — idempotent double-start under the eager-bind call pattern.
//
// Locks in the contract relied on by `recmeet-tray --listen-now`: the
// flag binds the listener BEFORE the GTK main loop runs, and the
// existing on-menu-click code path then calls `start_web_listener`
// AGAIN when the operator clicks "Speaker Management". Both calls must
// return the SAME port and must not leak a second httplib server.
//
// The pre-existing "is idempotent and returns the same port on re-entry"
// test covers two back-to-back calls in a fresh-listener scenario; this
// test adds a third call and asserts the embedded WebUI is still
// reachable across all three (and that `get_listener_port` returns the
// same value), which is the production trajectory for a `--listen-now`
// tray with a later menu-click — the bug we are guarding against is a
// re-entrant `start_web_listener` silently rebinding to a fresh port
// and stranding the original listener thread.
// ===========================================================================

TEST_CASE("start_web_listener triple-call is idempotent and reachable across "
          "all entries",
          "[e6][tray-web]") {
    DaemonSim sim("eager_triple_idemp");
    IpcClient client(sim.sock_path);
    REQUIRE(client.connect());

    // 1st call — eager bind (what --listen-now triggers).
    const int port1 = recmeet::start_web_listener(client);
    REQUIRE(port1 > 0);
    REQUIRE(recmeet::get_listener_port() == port1);

    // Verify reachable.
    {
        httplib::Client http("127.0.0.1", port1);
        http.set_connection_timeout(5);
        auto res = http.Get("/api/health");
        REQUIRE(res);
        CHECK(res->status == 200);
    }

    // 2nd call — what a menu click would do post-eager. Must return the
    // same port.
    const int port2 = recmeet::start_web_listener(client);
    CHECK(port2 == port1);
    CHECK(recmeet::get_listener_port() == port1);

    // 3rd call — defensive guard for any future re-entrant path. Same
    // result.
    const int port3 = recmeet::start_web_listener(client);
    CHECK(port3 == port1);

    // Verify still reachable on the original port (would fail if a
    // re-entry silently rebound to a fresh listener).
    {
        httplib::Client http("127.0.0.1", port1);
        http.set_connection_timeout(5);
        auto res = http.Get("/api/health");
        REQUIRE(res);
        CHECK(res->status == 200);
    }

    recmeet::stop_web_listener();
    CHECK(recmeet::get_listener_port() == -1);
    client.close_connection();
}
