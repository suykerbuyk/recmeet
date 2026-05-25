// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C — V2 captions always-stream daemon-side dead-code cleanup.
// Tests T10 + T11 from
// `agentctx/tasks/v2-captions-always-stream-client-renders.md` (rev 5).
//
// Layers:
//
//   T10 (`[captions][phase-c][grep-invariant]`) — build/suite hygiene
//     check. With the Phase C edits landed, no source file under `src/`
//     may read `session_prefs.captions_enabled`,
//     `prefs.captions_enabled`, `req.captions_enabled`, or
//     `sr.captions_enabled` as a struct-field access. The
//     `SessionPreferences::captions_enabled` and
//     `StreamRequest::captions_enabled` fields are gone; any surviving
//     reader is a compile-time bug. We use std::regex to walk the source
//     tree and assert zero matches.
//
//   T11 (`[captions][phase-c][full-stack]`) — wire-shape regression:
//     `session.init` and `session.update_prefs` accept a prefs payload
//     that OMITS `captions_enabled` (the post-B5 norm), and the daemon's
//     validation does not reject the absence. The session.init response
//     still surfaces `captions_supported` (from the server-side
//     ServerConfig::captions_enabled), which is the only captions-state
//     wire field the client consumes post-Phase-C.

#include <catch2/catch_test_macros.hpp>

#include "daemon_test_harness.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace recmeet;
using recmeet::test::DaemonTestHarness;
namespace fs = std::filesystem;

// CMake passes the absolute src/ path so the test can locate it
// regardless of where catch_discover_tests sets WORKING_DIRECTORY. If
// the build configuration neglects to define this, fall back to "src/"
// so a manual `./recmeet_tests` from the repo root still works.
#ifndef RECMEET_SRC_DIR_DEFAULT
#define RECMEET_SRC_DIR_DEFAULT "src"
#endif

namespace {

// Walk `root` recursively and return all regular-file paths whose
// extension is one of {.cpp, .h, .hpp}. Filenames-only filter keeps the
// scan focused on first-party C++ source (no vendor blobs, no JSON
// fixtures, no build artifacts).
std::vector<fs::path> collect_cxx_sources(const fs::path& root) {
    std::vector<fs::path> out;
    if (!fs::exists(root) || !fs::is_directory(root)) return out;
    for (auto& ent : fs::recursive_directory_iterator(root)) {
        if (!ent.is_regular_file()) continue;
        auto ext = ent.path().extension().string();
        if (ext == ".cpp" || ext == ".h" || ext == ".hpp") {
            out.push_back(ent.path());
        }
    }
    return out;
}

// A single forbidden-read site: file + 1-based line + matched line text.
struct ForbiddenSite {
    fs::path file;
    int      line = 0;
    std::string text;
};

// Scan `path` line-by-line for any of the forbidden struct-field reads.
// We match `<receiver>.captions_enabled` where `<receiver>` is one of
// the four post-Phase-C dead names. Word-boundary-anchored on both ends
// so we don't catch `g_tray.cap.captions_enabled_for_recording` etc.
std::vector<ForbiddenSite> scan_file(const fs::path& path) {
    std::vector<ForbiddenSite> hits;
    std::ifstream f(path);
    if (!f) return hits;
    // The dead receivers from C3.1: session_prefs / prefs / req / sr /
    // from_req. Each one was used by the deleted parser/merge sites.
    // Word boundaries on the field name (`\b`) so the
    // `captions_enabled_for_recording` snapshot field on tray.cpp doesn't
    // false-positive.
    static const std::regex kForbidden(
        R"(\b(session_prefs|prefs|req|sr|from_req)\.captions_enabled\b)");
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        if (std::regex_search(line, kForbidden)) {
            hits.push_back({path, lineno, line});
        }
    }
    return hits;
}

} // anonymous namespace

// ============================================================================
// T10 — grep invariant: zero forbidden readers of the deleted fields.
// ============================================================================
TEST_CASE("Phase C — no orphan readers of deleted captions_enabled fields",
          "[captions][phase-c][grep-invariant]") {
    const fs::path src_root = RECMEET_SRC_DIR_DEFAULT;
    INFO("scanning " << src_root.string());
    auto files = collect_cxx_sources(src_root);
    // Sanity: the source tree must actually be reachable. If this fails,
    // the test wiring is broken (RECMEET_SRC_DIR_DEFAULT pointed at a
    // missing path) — surface as a clear failure rather than silent pass.
    REQUIRE_FALSE(files.empty());

    std::vector<ForbiddenSite> all_hits;
    for (auto& p : files) {
        auto hits = scan_file(p);
        for (auto& h : hits) all_hits.push_back(std::move(h));
    }

    if (!all_hits.empty()) {
        std::ostringstream oss;
        oss << "found " << all_hits.size()
            << " forbidden reader(s) of deleted captions_enabled fields:\n";
        for (auto& h : all_hits) {
            oss << "  " << h.file.string() << ":" << h.line << "  " << h.text << "\n";
        }
        UNSCOPED_INFO(oss.str());
    }
    CHECK(all_hits.empty());
}

// ============================================================================
// T11 — session.init / session.update_prefs accept a payload that omits
//       `captions_enabled` (the post-B5 norm). The daemon must not reject
//       the absence, and the session.init response must still surface
//       `captions_supported` so the client gate stays driven by the
//       server-owned single source of truth.
// ============================================================================
TEST_CASE("Phase C — session.init accepts prefs without captions_enabled key",
          "[captions][phase-c][full-stack]") {
    DaemonTestHarness h;
    // The harness's constructor defensively sets
    // ServerConfig::captions_enabled = false (daemon_test_harness.h:108).
    // Emulate the post-startup state where Phase A1.4's gate has written
    // the runtime-effective `true` into g_server_config so session.init
    // surfaces captions_supported=true (matching tests/test_captions_phase_b.cpp).
    h.mutate_config([](ServerConfig& cfg) {
        cfg.captions_enabled = true;
    });
    h.start();

    IpcClient c(h.socket_path());
    REQUIRE(c.connect());

    SECTION("session.init: prefs payload that OMITS captions_enabled "
            "is accepted without rejection") {
        JsonMap creds;
        JsonMap prefs;
        // Deliberately do NOT set prefs["captions_enabled"]. Set a couple
        // of non-captions keys so the payload exercises the actual parser
        // (validate_prefs_payload + parse_preferences_into) rather than
        // an empty-prefs no-op.
        prefs["whisper_model"]      = std::string("base.en");
        prefs["caption_latency_ms"] = int64_t(500);

        IpcResponse resp;
        IpcError err;
        REQUIRE(c.session_init(creds, prefs, resp, err));
        CHECK(json_val_as_bool(resp.result["ok"]) == true);
        CHECK(json_val_as_bool(resp.result["session_active"]) == true);

        // Phase A2.1: captions_supported is the single wire-side captions
        // state field clients consume post-Phase-C. With the harness's
        // mutate_config flipping captions_enabled to true, the response
        // surfaces true.
        auto cs_it = resp.result.find("captions_supported");
        REQUIRE(cs_it != resp.result.end());
        CHECK(json_val_as_bool(cs_it->second) == true);
    }

    SECTION("session.update_prefs: patch that OMITS captions_enabled "
            "is accepted without rejection") {
        // Establish a clean slot via session.init (also without the key).
        {
            JsonMap creds, prefs;
            prefs["whisper_model"] = std::string("base.en");
            IpcResponse resp;
            IpcError err;
            REQUIRE(c.session_init(creds, prefs, resp, err));
        }

        // Patch only a non-captions field. The daemon's
        // validate_prefs_payload helper at daemon_handlers.cpp's session
        // handlers never validated captions_enabled, so absence has
        // always been silent — Phase C preserves that contract.
        JsonMap patch;
        patch["caption_latency_ms"] = int64_t(800);
        IpcResponse resp;
        IpcError err;
        REQUIRE(c.session_update_prefs(patch, resp, err));
        CHECK(json_val_as_bool(resp.result["ok"]) == true);
    }

    SECTION("session.init: prefs payload that STILL sends captions_enabled "
            "is tolerated (parser silently ignores)") {
        // Backward-compat: any v1-era client still emitting
        // captions_enabled must not cause a session.init failure. The
        // daemon's parser no longer looks for the key, so the value is
        // simply dropped — assert the call still succeeds and the
        // captions_supported response field is still present.
        JsonMap creds;
        JsonMap prefs;
        prefs["captions_enabled"] = true;  // legacy client artifact
        prefs["whisper_model"]    = std::string("base.en");

        IpcResponse resp;
        IpcError err;
        REQUIRE(c.session_init(creds, prefs, resp, err));
        CHECK(json_val_as_bool(resp.result["ok"]) == true);
        auto cs_it = resp.result.find("captions_supported");
        REQUIRE(cs_it != resp.result.end());
    }

    c.close_connection();
}
