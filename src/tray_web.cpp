// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase E.6.2 — tray-bundled WebUI HTTP listener.
//
// The tray owns an
// httplib::Server bound to 127.0.0.1:<kernel-picked-port> (via
// bind_to_any_port); requests under /api/* are translated to IPC calls
// against the daemon and the JSON response is forwarded to the browser.
// The four static assets (index.html, app.js, style.css, favicon.svg)
// are compiled into the binary via cmake/embed_assets.cmake; the static-
// asset handlers serve from the in-memory byte arrays with Cache-Control
// `no-cache` so binary upgrades don't strand a stale asset in the
// browser cache.
//
// Threading model: bind_to_any_port runs on the calling thread (the GTK
// main thread when invoked from on_open_speaker_ui); listen_after_bind
// runs on a dedicated std::thread so it does not block the GTK loop.
// stop() and join() run in stop_web_listener() at tray shutdown.
//
// IPC ownership: the IpcClient pointer handed in at start time is owned
// by the tray (TrayState::ipc). The listener thread captures the
// pointer and uses it via plain method calls; all calls go through the
// same fd which is single-writer single-reader (the IPC poll thread
// pumps responses inside `call()`), so the cross-thread access pattern
// is safe by inspection — every handler runs `client.call(...)`
// synchronously and returns before the next request lands.

#include "tray_web.h"

// cpp-httplib produces some deprecated-declaration warnings on older
// glibc (sigaction-related). Local pragma to keep this TU clean without
// loosening project-wide -Wall -Wextra.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <httplib.h>
#pragma GCC diagnostic pop

// Generated header — produced by cmake/embed_assets.cmake at configure time;
// resolved via the `${CMAKE_CURRENT_BINARY_DIR}/generated` include dir
// stamped onto both recmeet-client and recmeet_tests.
#include "web_assets.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "json_util.h"
#include "log.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace recmeet {

// ---------------------------------------------------------------------------
// File-static state
// ---------------------------------------------------------------------------

namespace {

httplib::Server*       g_server = nullptr;
std::thread            g_listener_thread;
std::atomic<int>       g_resolved_port{-1};
IpcClient*             g_client = nullptr;
std::mutex             g_lifecycle_mu;          // protects start/stop critical sections

constexpr int          kIpcCallTimeoutMs = 30000;

// ---------------------------------------------------------------------------
// JSON request-body helpers (tiny extractors — same minimal style as
// the deleted web.cpp; we never see arbitrarily nested objects on the
// /api/* surface, so a simple key-scanner is sufficient).
// ---------------------------------------------------------------------------

std::string json_get_str(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    while (pos < body.size() &&
           (body[pos] == ' ' || body[pos] == ':' || body[pos] == '\t'))
        ++pos;
    if (pos >= body.size() || body[pos] != '"') return "";
    ++pos;
    auto end = body.find('"', pos);
    if (end == std::string::npos) return "";
    return body.substr(pos, end - pos);
}

bool json_has_int(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < body.size() &&
           (body[pos] == ' ' || body[pos] == ':' || body[pos] == '\t'))
        ++pos;
    if (pos >= body.size()) return false;
    char c = body[pos];
    return (c == '-' || (c >= '0' && c <= '9'));
}

int64_t json_get_int(const std::string& body, const std::string& key,
                     int64_t def = -1) {
    if (!json_has_int(body, key)) return def;
    const std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    pos += needle.size();
    while (pos < body.size() &&
           (body[pos] == ' ' || body[pos] == ':' || body[pos] == '\t'))
        ++pos;
    return std::atoll(body.c_str() + pos);
}

// Returns true when `key` is present AND parses as a true/false literal.
// `out` receives the parsed value when present; left untouched otherwise.
bool json_get_bool(const std::string& body, const std::string& key, bool& out) {
    const std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < body.size() &&
           (body[pos] == ' ' || body[pos] == ':' || body[pos] == '\t'))
        ++pos;
    if (pos + 3 < body.size() && body.compare(pos, 4, "true") == 0) {
        out = true;  return true;
    }
    if (pos + 4 < body.size() && body.compare(pos, 5, "false") == 0) {
        out = false; return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// JSON error body builder
// ---------------------------------------------------------------------------

std::string json_error(const std::string& msg) {
    return "{\"error\":\"" + json_escape(msg) + "\"}";
}

// Map an IpcError to an HTTP status. The daemon's `speakers.*` and
// `meetings.*` handlers use InvalidParams for both "validation failed"
// and "not found" — there is no distinct NotFound code on the wire.
// MethodNotFound is rare (would indicate a daemon predating E.6.1).
int http_status_for(const IpcError& err) {
    using C = IpcErrorCode;
    if (err.code == static_cast<int>(C::InvalidParams)) {
        // Distinguish "not_found" / "unknown meeting_id" from generic
        // validation failures so the browser can render a 404 vs. a 400.
        const std::string& m = err.message;
        if (m.find("not_found") != std::string::npos
            || m.find("unknown meeting_id") != std::string::npos)
            return 404;
        return 400;
    }
    if (err.code == static_cast<int>(C::MethodNotFound)) return 501;
    if (err.code == static_cast<int>(C::PermissionDenied)) return 403;
    if (err.code == static_cast<int>(C::Busy)) return 409;
    return 502;  // InternalError + everything else
}

// ---------------------------------------------------------------------------
// IPC plumbing
// ---------------------------------------------------------------------------

// Wraps client->call with a 502 error response on disconnect / framing
// failure. Returns true when the call succeeded; on failure populates
// res with an error JSON body and an appropriate status.
bool call_ipc(const std::string& method, const JsonMap& params,
              IpcResponse& resp, httplib::Response& res) {
    if (!g_client) {
        res.status = 502;
        res.set_content(json_error("tray ipc client unavailable"),
                        "application/json");
        return false;
    }
    IpcError err;
    if (!g_client->call(method, params, resp, err, kIpcCallTimeoutMs)) {
        res.status = http_status_for(err);
        res.set_content(json_error(err.message.empty()
                                       ? method + ": daemon error"
                                       : err.message),
                        "application/json");
        return false;
    }
    return true;
}

// Forward a daemon response where the payload is a JSON array embedded
// as a string under `array_field` (e.g. meetings.list → "meetings",
// meetings.speakers → "speakers"). Emits the raw array body to the
// browser — same wire shape the pre-E.6.2 web binary spoke.
void forward_array_field(const IpcResponse& resp,
                         const std::string& array_field,
                         httplib::Response& res) {
    auto it = resp.result.find(array_field);
    const std::string body = (it != resp.result.end())
                                  ? json_val_as_string(it->second)
                                  : "[]";
    res.set_content(body, "application/json");
}

// Forward the whole `result` map as a JSON object. Used for the
// scalar-shape responses (speakers.get, speakers.relabel, etc.).
void forward_result_object(const IpcResponse& resp, httplib::Response& res) {
    res.set_content(serialize_json_map(resp.result), "application/json");
}

// ---------------------------------------------------------------------------
// Static-asset serving (Cache-Control: no-cache per plan M-E6-4 reasoning)
// ---------------------------------------------------------------------------

void serve_asset(httplib::Response& res, const unsigned char* bytes,
                 std::size_t len, const char* content_type) {
    res.set_header("Cache-Control", "no-cache");
    res.set_content(reinterpret_cast<const char*>(bytes), len, content_type);
}

// ---------------------------------------------------------------------------
// Path-validation helper used by the few endpoints that surface a name
// or label component into a daemon parameter. Mirrors the same
// is_safe_dirname rule the daemon-side handlers enforce; the daemon will
// catch a bypass anyway, but we surface a 400 here too so the browser
// gets a clean error instead of a generic daemon-side InvalidParams.
// ---------------------------------------------------------------------------

bool is_safe_dirname(const std::string& name) {
    if (name.empty()) return false;
    if (name == "." || name == "..") return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find('\0') != std::string::npos) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Endpoint registration
// ---------------------------------------------------------------------------

void register_handlers(httplib::Server& server) {
    // -------------------- 1. Static assets --------------------

    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        serve_asset(res,
                    recmeet::web_assets::index_html_bytes,
                    recmeet::web_assets::index_html_len,
                    recmeet::web_assets::index_html_content_type);
    });
    server.Get("/index.html", [](const httplib::Request&, httplib::Response& res) {
        serve_asset(res,
                    recmeet::web_assets::index_html_bytes,
                    recmeet::web_assets::index_html_len,
                    recmeet::web_assets::index_html_content_type);
    });
    server.Get("/app.js", [](const httplib::Request&, httplib::Response& res) {
        serve_asset(res,
                    recmeet::web_assets::app_js_bytes,
                    recmeet::web_assets::app_js_len,
                    recmeet::web_assets::app_js_content_type);
    });
    server.Get("/style.css", [](const httplib::Request&, httplib::Response& res) {
        serve_asset(res,
                    recmeet::web_assets::style_css_bytes,
                    recmeet::web_assets::style_css_len,
                    recmeet::web_assets::style_css_content_type);
    });
    server.Get("/favicon.svg", [](const httplib::Request&, httplib::Response& res) {
        serve_asset(res,
                    recmeet::web_assets::favicon_svg_bytes,
                    recmeet::web_assets::favicon_svg_len,
                    recmeet::web_assets::favicon_svg_content_type);
    });

    // -------------------- 2. /api/health (local) --------------------

    server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // -------------------- 3. speakers.list --------------------

    server.Get("/api/speakers", [](const httplib::Request&, httplib::Response& res) {
        IpcResponse resp;
        JsonMap params;
        if (!call_ipc("speakers.list", params, resp, res)) return;
        // speakers.list returns { speakers: "[...]", count: N }.
        forward_array_field(resp, "speakers", res);
    });

    // -------------------- 4. speakers.get --------------------

    server.Get(R"(/api/speakers/([^/]+))",
               [](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.matches[1].str();
        if (!is_safe_dirname(name)) {
            res.status = 400;
            res.set_content(json_error("invalid speaker name"), "application/json");
            return;
        }
        IpcResponse resp;
        JsonMap params;
        params["name"] = name;
        if (!call_ipc("speakers.get", params, resp, res)) return;
        forward_result_object(resp, res);
    });

    // -------------------- 5. speakers.remove --------------------

    server.Delete(R"(/api/speakers/([^/]+))",
                  [](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.matches[1].str();
        if (!is_safe_dirname(name)) {
            res.status = 400;
            res.set_content(json_error("invalid speaker name"), "application/json");
            return;
        }
        IpcResponse resp;
        JsonMap params;
        params["name"] = name;
        if (!call_ipc("speakers.remove", params, resp, res)) return;
        forward_result_object(resp, res);
    });

    // -------------------- 6. speakers.reset --------------------

    server.Post("/api/speakers/reset",
                [](const httplib::Request&, httplib::Response& res) {
        IpcResponse resp;
        JsonMap params;
        if (!call_ipc("speakers.reset", params, resp, res)) return;
        forward_result_object(resp, res);
    });

    // -------------------- 7. speakers.enroll --------------------

    server.Post("/api/speakers/enroll",
                [](const httplib::Request& req, httplib::Response& res) {
        const std::string name = json_get_str(req.body, "name");
        const std::string meeting_id = json_get_str(req.body, "meeting_id");
        const int64_t cluster_id = json_get_int(req.body, "cluster_id", -1);
        if (name.empty() || meeting_id.empty() || cluster_id < 0) {
            res.status = 400;
            res.set_content(json_error("missing required fields: name, meeting_id, cluster_id"),
                            "application/json");
            return;
        }
        if (!is_safe_dirname(name)) {
            res.status = 400;
            res.set_content(json_error("invalid speaker name"), "application/json");
            return;
        }
        IpcResponse resp;
        JsonMap params;
        params["name"] = name;
        params["meeting_id"] = meeting_id;
        params["cluster_id"] = cluster_id;
        if (!call_ipc("speakers.enroll", params, resp, res)) return;
        forward_result_object(resp, res);
    });

    // -------------------- 8. speakers.remove_embedding --------------------

    server.Post(R"(/api/speakers/([^/]+)/remove-embedding)",
                [](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.matches[1].str();
        if (!is_safe_dirname(name)) {
            res.status = 400;
            res.set_content(json_error("invalid speaker name"), "application/json");
            return;
        }
        const int64_t index = json_get_int(req.body, "index", -1);
        if (index < 0) {
            res.status = 400;
            res.set_content(json_error("missing or invalid 'index' field"),
                            "application/json");
            return;
        }
        IpcResponse resp;
        JsonMap params;
        params["name"] = name;
        params["index"] = index;
        if (!call_ipc("speakers.remove_embedding", params, resp, res)) return;
        forward_result_object(resp, res);
    });

    // -------------------- 9. speakers.batch_reidentify --------------------

    server.Post("/api/speakers/batch-reidentify",
                [](const httplib::Request&, httplib::Response& res) {
        IpcResponse resp;
        JsonMap params;
        if (!call_ipc("speakers.batch_reidentify", params, resp, res)) return;
        forward_result_object(resp, res);
    });

    // -------------------- 10. meetings.list --------------------

    server.Get("/api/meetings",
               [](const httplib::Request&, httplib::Response& res) {
        IpcResponse resp;
        JsonMap params;
        if (!call_ipc("meetings.list", params, resp, res)) return;
        forward_array_field(resp, "meetings", res);
    });

    // -------------------- 11. meetings.speakers --------------------

    server.Get(R"(/api/meetings/([^/]+)/speakers)",
               [](const httplib::Request& req, httplib::Response& res) {
        std::string meeting_id = req.matches[1].str();
        if (meeting_id.empty()) {
            res.status = 400;
            res.set_content(json_error("missing meeting_id"), "application/json");
            return;
        }
        IpcResponse resp;
        JsonMap params;
        params["meeting_id"] = meeting_id;
        if (!call_ipc("meetings.speakers", params, resp, res)) return;
        forward_array_field(resp, "speakers", res);
    });

    // -------------------- 12. speakers.relabel --------------------

    server.Post(R"(/api/meetings/([^/]+)/speakers/relabel)",
                [](const httplib::Request& req, httplib::Response& res) {
        std::string meeting_id = req.matches[1].str();
        const int64_t cluster_id = json_get_int(req.body, "cluster_id", -1);
        const std::string new_label = json_get_str(req.body, "new_label");
        if (meeting_id.empty() || cluster_id < 0 || new_label.empty()) {
            res.status = 400;
            res.set_content(json_error("missing required fields: meeting_id, cluster_id, new_label"),
                            "application/json");
            return;
        }
        IpcResponse resp;
        JsonMap params;
        params["meeting_id"] = meeting_id;
        params["cluster_id"] = cluster_id;
        params["new_label"] = new_label;
        // update_profile is optional (default true on the daemon side).
        bool update_profile = true;
        if (json_get_bool(req.body, "update_profile", update_profile))
            params["update_profile"] = update_profile;
        if (!call_ipc("speakers.relabel", params, resp, res)) return;
        forward_result_object(resp, res);
    });

    // -------------------- 13. process.reprocess --------------------

    server.Post(R"(/api/meetings/([^/]+)/reprocess)",
                [](const httplib::Request& req, httplib::Response& res) {
        std::string meeting_id = req.matches[1].str();
        if (meeting_id.empty()) {
            res.status = 400;
            res.set_content(json_error("missing meeting_id"), "application/json");
            return;
        }
        IpcResponse resp;
        JsonMap params;
        params["meeting_id"] = meeting_id;
        // Per-stage flags — forward only when explicitly present in the
        // body so we don't accidentally override the daemon defaults.
        bool flag_val = true;
        if (json_get_bool(req.body, "diarize", flag_val))
            params["diarize"] = flag_val;
        if (json_get_bool(req.body, "summarize", flag_val))
            params["summarize"] = flag_val;
        // `vocabulary` is a string override; forward when present (even
        // empty, which clears the per-job prompt — daemon honors the
        // empty-string semantics).
        const std::string vocab = json_get_str(req.body, "vocabulary");
        if (!vocab.empty()) params["vocabulary"] = vocab;
        if (!call_ipc("process.reprocess", params, resp, res)) return;
        forward_result_object(resp, res);
    });

    // -------------------- 14. meetings.read_note --------------------

    server.Get(R"(/api/meetings/([^/]+)/note)",
               [](const httplib::Request& req, httplib::Response& res) {
        std::string meeting_id = req.matches[1].str();
        if (meeting_id.empty()) {
            res.status = 400;
            res.set_content(json_error("missing meeting_id"), "application/json");
            return;
        }
        IpcResponse resp;
        JsonMap params;
        params["meeting_id"] = meeting_id;
        if (!call_ipc("meetings.read_note", params, resp, res)) return;
        forward_result_object(resp, res);
    });
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int start_web_listener(IpcClient& client) {
    std::lock_guard<std::mutex> lock(g_lifecycle_mu);

    // Idempotent re-entry: already bound → return resolved port.
    if (g_server != nullptr && g_resolved_port.load() > 0) {
        return g_resolved_port.load();
    }

    g_client = &client;
    g_server = new httplib::Server();

    register_handlers(*g_server);

    int port = g_server->bind_to_any_port("127.0.0.1");
    if (port <= 0) {
        log_error("[tray_web] bind_to_any_port failed");
        delete g_server;
        g_server = nullptr;
        g_client = nullptr;
        return -1;
    }
    g_resolved_port.store(port);

    httplib::Server* srv = g_server;
    g_listener_thread = std::thread([srv]() {
        if (!srv->listen_after_bind()) {
            log_warn("[tray_web] listen_after_bind returned false");
        }
    });

    // httplib's stop() short-circuits when is_running_ is still false
    // (set inside listen_internal AFTER the thread starts). Wait until
    // the listener flips is_running_ so a quick start→stop cycle from
    // the GTK main thread (or a test) doesn't race the listener-thread
    // startup and leak the socket.
    srv->wait_until_ready();

    log_info("[tray_web] embedded WebUI listening on http://127.0.0.1:%d", port);
    return port;
}

void stop_web_listener() {
    std::lock_guard<std::mutex> lock(g_lifecycle_mu);
    if (g_server == nullptr) return;
    g_server->stop();
    if (g_listener_thread.joinable()) g_listener_thread.join();
    delete g_server;
    g_server = nullptr;
    g_resolved_port.store(-1);
    g_client = nullptr;
    log_info("[tray_web] embedded WebUI stopped");
}

int get_listener_port() {
    return g_resolved_port.load();
}

} // namespace recmeet
