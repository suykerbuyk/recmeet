// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase E.2 Wave 2.1 — additive JobConfig fields land BEFORE the file split
// (Wave 2.2) because the client.yaml schema references them. This file
// pins the three new fields:
//
//   • summary_style          — client preference, empty default
//   • caption_latency_ms     — client preference, [200, 2000] guarded
//   • servers (list of {name, address}) — preserved-not-precluded multi
//
// All TEST_CASEs are tagged `[e2]` so the orchestrator can grep-verify
// the scope of Wave 2.1.

#include <catch2/catch_test_macros.hpp>
#include "config.h"
#include "config_json.h"
#include "session_merge.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace recmeet;

namespace {

fs::path make_tmp_dir(const char* slug) {
    fs::path dir = fs::temp_directory_path() / slug;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

} // namespace

// ===========================================================================
// E.2(a) — summary_style round-trip
// ===========================================================================

TEST_CASE("[e2] summary_style round-trip", "[e2][config]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_summary_style_rt");
    fs::path path = dir / "config.yaml";

    JobConfig cfg;
    cfg.summary_style = "bullet";
    save_legacy_config_as_job_config(cfg, path);

    std::string content = read_file(path);
    // Should appear under the summary section as `style:`.
    auto summary_pos = content.find("summary:");
    REQUIRE(summary_pos != std::string::npos);
    CHECK(content.find("style: bullet", summary_pos) != std::string::npos);

    JobConfig loaded = load_legacy_config_as_job_config(path);
    CHECK(loaded.summary_style == "bullet");

    fs::remove_all(dir);
}

TEST_CASE("[e2] summary_style empty default emits no style: line",
          "[e2][config]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_summary_style_omit");
    fs::path path = dir / "config.yaml";

    JobConfig cfg;  // summary_style default empty
    save_legacy_config_as_job_config(cfg, path);

    std::string content = read_file(path);
    // Conservative match: any "style:" line under the summary block would
    // count as a leak. The save_legacy_config_as_job_config formatting uses "  style: ".
    CHECK(content.find("\n  style:") == std::string::npos);

    JobConfig loaded = load_legacy_config_as_job_config(path);
    CHECK(loaded.summary_style.empty());

    fs::remove_all(dir);
}

// ===========================================================================
// E.2(b) — caption_latency_ms valid range
// ===========================================================================

TEST_CASE("[e2] caption_latency_ms default is 500 with no override",
          "[e2][config]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_clat_default");
    JobConfig cfg = load_legacy_config_as_job_config(dir / "config.yaml");
    CHECK(cfg.caption_latency_ms == 500);
    fs::remove_all(dir);
}

TEST_CASE("[e2] caption_latency_ms valid 250 round-trips through YAML",
          "[e2][config]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_clat_valid");
    fs::path path = dir / "config.yaml";

    JobConfig cfg;
    cfg.caption_latency_ms = 250;
    save_legacy_config_as_job_config(cfg, path);

    std::string content = read_file(path);
    CHECK(content.find("client:") != std::string::npos);
    CHECK(content.find("caption_latency_ms: 250") != std::string::npos);

    JobConfig loaded = load_legacy_config_as_job_config(path);
    CHECK(loaded.caption_latency_ms == 250);

    fs::remove_all(dir);
}

TEST_CASE("[e2] caption_latency_ms invalid low falls back to default",
          "[e2][config]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_clat_invalid_low");
    fs::path path = dir / "config.yaml";
    {
        std::ofstream out(path);
        // 100 is below the 200 minimum — must warn and keep struct default.
        out << "client:\n  caption_latency_ms: 100\n";
    }
    JobConfig cfg = load_legacy_config_as_job_config(path);
    CHECK(cfg.caption_latency_ms == 500);
    fs::remove_all(dir);
}

TEST_CASE("[e2] caption_latency_ms invalid high falls back to default",
          "[e2][config]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_clat_invalid_high");
    fs::path path = dir / "config.yaml";
    {
        std::ofstream out(path);
        // 5000 is above the 2000 maximum — must warn and keep struct default.
        out << "client:\n  caption_latency_ms: 5000\n";
    }
    JobConfig cfg = load_legacy_config_as_job_config(path);
    CHECK(cfg.caption_latency_ms == 500);
    fs::remove_all(dir);
}

TEST_CASE("[e2] caption_latency_ms boundary values are accepted",
          "[e2][config]") {
    {
        fs::path dir = make_tmp_dir("recmeet_test_e2_clat_lo_bound");
        fs::path path = dir / "config.yaml";
        std::ofstream out(path);
        out << "client:\n  caption_latency_ms: 200\n";
        out.close();
        JobConfig cfg = load_legacy_config_as_job_config(path);
        CHECK(cfg.caption_latency_ms == 200);
        fs::remove_all(dir);
    }
    {
        fs::path dir = make_tmp_dir("recmeet_test_e2_clat_hi_bound");
        fs::path path = dir / "config.yaml";
        std::ofstream out(path);
        out << "client:\n  caption_latency_ms: 2000\n";
        out.close();
        JobConfig cfg = load_legacy_config_as_job_config(path);
        CHECK(cfg.caption_latency_ms == 2000);
        fs::remove_all(dir);
    }
}

// ===========================================================================
// E.2(b) — session merge wires caption_latency_ms into JobConfig
// ===========================================================================

TEST_CASE("[e2] caption_latency_ms session merge propagates into Config",
          "[e2][session_merge]") {
    JobConfig daemon_yaml;
    daemon_yaml.caption_latency_ms = 500;  // struct default

    SessionCredentials sess;
    SessionPreferences prefs;
    prefs.caption_latency_ms = 750;

    auto env_lookup = [](const std::string&) { return std::string(); };
    JobConfig merged = merge_creds_for_job(daemon_yaml, sess, prefs, env_lookup);
    CHECK(merged.caption_latency_ms == 750);
}

// ===========================================================================
// E.2(c) — servers list round-trip
// ===========================================================================

TEST_CASE("[e2] servers list round-trip via save_config + load_config",
          "[e2][config]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_servers_rt");
    fs::path path = dir / "config.yaml";

    JobConfig cfg;
    cfg.servers.push_back(ServerEntry{"default", "unix:/tmp/x"});
    save_legacy_config_as_job_config(cfg, path);

    std::string content = read_file(path);
    auto client_pos = content.find("client:");
    REQUIRE(client_pos != std::string::npos);
    CHECK(content.find("servers:", client_pos) != std::string::npos);
    CHECK(content.find("- name: default", client_pos) != std::string::npos);
    CHECK(content.find("address: \"unix:/tmp/x\"", client_pos) != std::string::npos);

    JobConfig loaded = load_legacy_config_as_job_config(path);
    REQUIRE(loaded.servers.size() == 1);
    CHECK(loaded.servers[0].name == "default");
    CHECK(loaded.servers[0].address == "unix:/tmp/x");

    fs::remove_all(dir);
}

TEST_CASE("[e2] empty servers vector emits no servers block",
          "[e2][config]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_servers_omit");
    fs::path path = dir / "config.yaml";

    JobConfig cfg;  // default: empty servers vector
    save_legacy_config_as_job_config(cfg, path);

    std::string content = read_file(path);
    // No `servers:` key anywhere — the [client] section also stays
    // suppressed when ALL of its knobs are at default.
    CHECK(content.find("servers:") == std::string::npos);

    JobConfig loaded = load_legacy_config_as_job_config(path);
    CHECK(loaded.servers.empty());

    fs::remove_all(dir);
}

// ===========================================================================
// E.2 — config_to_map / config_from_map round-trip for the three new fields
// (the daemon→subprocess JSON-config boundary).
// ===========================================================================

TEST_CASE("[e2] config_json round-trip preserves summary_style + "
          "caption_latency_ms + servers",
          "[e2][config_json]") {
    JobConfig cfg;
    cfg.summary_style       = "actions";
    cfg.caption_latency_ms  = 800;
    cfg.servers.push_back(ServerEntry{"primary",
                                      "tcp:host.example:9000"});
    // A second entry to confirm the flattened-map indexing reconstructs
    // correctly even though v1 dispatch only honors index 0.
    cfg.servers.push_back(ServerEntry{"fallback",
                                      "unix:/run/recmeet.sock"});

    JsonMap m = config_to_map(cfg);
    JobConfig out = config_from_map(m);

    CHECK(out.summary_style == "actions");
    CHECK(out.caption_latency_ms == 800);
    REQUIRE(out.servers.size() == 2);
    CHECK(out.servers[0].name    == "primary");
    CHECK(out.servers[0].address == "tcp:host.example:9000");
    CHECK(out.servers[1].name    == "fallback");
    CHECK(out.servers[1].address == "unix:/run/recmeet.sock");
}
