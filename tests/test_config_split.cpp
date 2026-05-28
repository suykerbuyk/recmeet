// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase E.2 Wave 2.2a — split-config test suite.
//
// Validates the new ServerConfig / ClientConfig load+save APIs and the
// config_to_map / config_from_map overloads. The monolithic JobConfig struct
// is preserved alongside; Wave 2.2b retypes consumers and removes it.
// The legacy migration helper and its test cases were removed in
// v2-coexistence-with-v1 Phase 2C.
//
// All TEST_CASEs are tagged `[e2]` so the orchestrator can grep-verify the
// scope of Wave 2.2a.

#include <catch2/catch_test_macros.hpp>
#include "config.h"
#include "config_json.h"
#include "test_tmpdir.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

using namespace recmeet;

namespace {

fs::path make_tmp_dir(const char* slug) {
    fs::path dir = recmeet::test::tmp_path(slug);
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

// Helper: assert that a path's permissions are exactly 0600.
bool perms_are_0600(const fs::path& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    return (st.st_mode & 0777) == 0600;
}

} // namespace

// ===========================================================================
// (1) ServerConfig YAML round-trip
// ===========================================================================

TEST_CASE("[e2] ServerConfig YAML round-trip", "[e2][config_split]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_srv_rt");
    fs::path path = dir / "daemon.yaml";

    ServerConfig cfg;
    // Populate non-default values across all sections.
    cfg.whisper_model            = "small";
    cfg.llm_model                = "/models/llm.bin";
    cfg.llm_mmap                 = true;
    cfg.captions_enabled         = true;
    cfg.caption_model            = "en-2023-06-26";
    cfg.threads                  = 8;
    cfg.log_level_str            = "info";
    cfg.log_retention_hours      = 48;
    cfg.web_port                 = 9090;
    cfg.web_bind                 = "0.0.0.0";
    cfg.max_message_bytes        = 16 * 1024 * 1024;
    cfg.max_clients              = 32;
    cfg.max_upload_bytes         = 8ull * 1024 * 1024 * 1024;
    cfg.slot_postprocess         = 2;
    cfg.slot_streaming           = 3;
    cfg.slot_model_download      = 1;
    cfg.allow_client_downloads   = false;
    cfg.diarize                  = false;
    cfg.num_speakers             = 5;
    cfg.cluster_threshold        = 1.05f;
    cfg.chunk_minutes            = 10.0f;
    cfg.chunk_overlap_sec        = 20.0f;
    cfg.stitch_threshold         = 0.7f;
    cfg.speaker_id               = false;
    cfg.speaker_threshold        = 0.7f;
    cfg.speaker_db               = "/tmp/spk.db";
    cfg.vad_threshold            = 0.6f;
    cfg.vad_min_silence          = 0.4f;
    cfg.retain_terminal_hours    = 12;

    save_server_config(cfg, path);
    REQUIRE(fs::exists(path));
    CHECK(perms_are_0600(path));

    ServerConfig loaded = load_server_config(path);

    // Representative sample (~10+ fields).
    CHECK(loaded.whisper_model         == "small");
    CHECK(loaded.llm_model             == "/models/llm.bin");
    CHECK(loaded.llm_mmap              == true);
    CHECK(loaded.captions_enabled      == true);
    CHECK(loaded.caption_model         == "en-2023-06-26");
    CHECK(loaded.threads               == 8);
    CHECK(loaded.web_port              == 9090);
    CHECK(loaded.web_bind              == "0.0.0.0");
    CHECK(loaded.max_message_bytes     == 16ull * 1024 * 1024);
    CHECK(loaded.max_clients           == 32);
    CHECK(loaded.slot_postprocess      == 2);
    CHECK(loaded.slot_streaming        == 3);
    CHECK(loaded.allow_client_downloads == false);
    CHECK(loaded.diarize               == false);
    CHECK(loaded.num_speakers          == 5);
    CHECK(loaded.speaker_db.string()   == "/tmp/spk.db");
    CHECK(loaded.retain_terminal_hours == 12);

    fs::remove_all(dir);
}

// ===========================================================================
// (2) ClientConfig YAML round-trip
// ===========================================================================

TEST_CASE("[e2] ClientConfig YAML round-trip", "[e2][config_split]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_cli_rt");
    fs::path path = dir / "client.yaml";

    ClientConfig cfg;
    cfg.device_pattern             = "alsa:hw0";
    cfg.mic_source                 = "mic-x";
    cfg.monitor_source             = "monitor-y";
    cfg.mic_only                   = true;
    cfg.keep_sources               = true;
    cfg.language                   = "en";
    cfg.vocabulary                 = "alpha,beta,gamma";
    cfg.summary_style              = "bullet";
    cfg.no_summary                 = true;
    cfg.caption_latency_ms         = 750;
    cfg.caption_normalize_display  = false;
    cfg.output_dir                 = "/tmp/meet";
    cfg.note.domain                = "engineering";
    cfg.note_dir                   = "/tmp/notes";
    cfg.staging_max_bytes          = static_cast<size_t>(50) * 1024 * 1024 * 1024;
    cfg.servers.push_back(ServerEntry{"primary", "tcp:host:9000"});
    cfg.servers.push_back(ServerEntry{"fallback", "unix:/run/x.sock"});

    save_client_config(cfg, path);
    REQUIRE(fs::exists(path));
    CHECK(perms_are_0600(path));

    ClientConfig loaded = load_client_config(path);

    CHECK(loaded.device_pattern            == "alsa:hw0");
    CHECK(loaded.mic_source                == "mic-x");
    CHECK(loaded.monitor_source            == "monitor-y");
    CHECK(loaded.mic_only                  == true);
    CHECK(loaded.keep_sources              == true);
    CHECK(loaded.language                  == "en");
    CHECK(loaded.vocabulary                == "alpha,beta,gamma");
    CHECK(loaded.summary_style             == "bullet");
    CHECK(loaded.no_summary                == true);
    CHECK(loaded.caption_latency_ms        == 750);
    CHECK(loaded.caption_normalize_display == false);
    CHECK(loaded.output_dir.string()       == "/tmp/meet");
    CHECK(loaded.note.domain               == "engineering");
    CHECK(loaded.note_dir.string()         == "/tmp/notes");
    CHECK(loaded.staging_max_bytes         == static_cast<size_t>(50) * 1024 * 1024 * 1024);
    REQUIRE(loaded.servers.size()          == 2);
    CHECK(loaded.servers[0].name           == "primary");
    CHECK(loaded.servers[0].address        == "tcp:host:9000");
    CHECK(loaded.servers[1].name           == "fallback");
    CHECK(loaded.servers[1].address        == "unix:/run/x.sock");

    fs::remove_all(dir);
}

// ===========================================================================
// (3) DUAL fields round-trip on both sides — H-2 reword pin
// ===========================================================================

TEST_CASE("[e2] DUAL provider/api_keys round-trip on ServerConfig",
          "[e2][config_split]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_dual_srv");
    fs::path path = dir / "daemon.yaml";

    ServerConfig cfg;
    cfg.provider = "anthropic";
    cfg.api_url  = "https://api.anthropic.com/v1";
    cfg.api_model = "claude-sonnet-4-6";
    // NOTE: legacy api_key is intentionally NOT written by save_*_config
    // (security: keys live in [api_keys] only). The struct field still
    // exists for env-var seed + DUAL fallback.
    cfg.api_keys["anthropic"] = "sk-anth-srv";
    cfg.api_keys["xai"]       = "sk-xai-srv";

    save_server_config(cfg, path);
    ServerConfig loaded = load_server_config(path);

    CHECK(loaded.provider                    == "anthropic");
    CHECK(loaded.api_url                     == "https://api.anthropic.com/v1");
    CHECK(loaded.api_model                   == "claude-sonnet-4-6");
    CHECK(loaded.api_keys["anthropic"]       == "sk-anth-srv");
    CHECK(loaded.api_keys["xai"]             == "sk-xai-srv");

    fs::remove_all(dir);
}

TEST_CASE("[e2] DUAL provider/api_keys round-trip on ClientConfig",
          "[e2][config_split]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_dual_cli");
    fs::path path = dir / "client.yaml";

    ClientConfig cfg;
    cfg.provider = "anthropic";
    cfg.api_url  = "https://api.anthropic.com/v1";
    cfg.api_model = "claude-sonnet-4-6";
    // NOTE: legacy api_key is intentionally NOT written by save_*_config
    // (security: keys live in [api_keys] only).
    cfg.api_keys["anthropic"] = "sk-anth-cli";
    cfg.api_keys["openai"]    = "sk-openai-cli";

    save_client_config(cfg, path);
    ClientConfig loaded = load_client_config(path);

    CHECK(loaded.provider              == "anthropic");
    CHECK(loaded.api_url               == "https://api.anthropic.com/v1");
    CHECK(loaded.api_model             == "claude-sonnet-4-6");
    CHECK(loaded.api_keys["anthropic"] == "sk-anth-cli");
    CHECK(loaded.api_keys["openai"]    == "sk-openai-cli");

    fs::remove_all(dir);
}

// ===========================================================================
// (4) config_json overloads — ServerConfig round-trip
// ===========================================================================

TEST_CASE("[e2] config_json overloads — ServerConfig round-trip",
          "[e2][config_split][config_json]") {
    ServerConfig srv;
    srv.whisper_model            = "medium";
    srv.llm_model                = "/models/llm.gguf";
    srv.llm_mmap                 = true;
    srv.captions_enabled         = true;
    srv.caption_model            = "en-2023-06-26";
    srv.threads                  = 16;
    srv.web_port                 = 12345;
    srv.web_bind                 = "0.0.0.0";
    srv.max_message_bytes        = 32ull * 1024 * 1024;
    srv.max_upload_bytes         = 2ull * 1024 * 1024 * 1024;
    srv.max_clients              = 64;
    srv.slot_postprocess         = 3;
    srv.allow_client_downloads   = false;
    srv.diarize                  = false;
    srv.num_speakers             = 7;
    srv.speaker_id               = false;
    srv.speaker_db               = "/var/lib/spk.db";
    srv.vad                      = false;
    srv.retain_terminal_hours    = 6;
    srv.provider                 = "anthropic";
    srv.api_key                  = "sk-legacy";
    srv.api_keys["anthropic"]    = "sk-anth";

    JsonMap m = config_to_map(srv);
    ServerConfig out = server_config_from_map(m);

    CHECK(out.whisper_model            == srv.whisper_model);
    CHECK(out.llm_model                == srv.llm_model);
    CHECK(out.llm_mmap                 == srv.llm_mmap);
    CHECK(out.captions_enabled         == srv.captions_enabled);
    CHECK(out.caption_model            == srv.caption_model);
    CHECK(out.threads                  == srv.threads);
    CHECK(out.web_port                 == srv.web_port);
    CHECK(out.web_bind                 == srv.web_bind);
    CHECK(out.max_message_bytes        == srv.max_message_bytes);
    CHECK(out.max_upload_bytes         == srv.max_upload_bytes);
    CHECK(out.max_clients              == srv.max_clients);
    CHECK(out.slot_postprocess         == srv.slot_postprocess);
    CHECK(out.allow_client_downloads   == srv.allow_client_downloads);
    CHECK(out.diarize                  == srv.diarize);
    CHECK(out.num_speakers             == srv.num_speakers);
    CHECK(out.speaker_id               == srv.speaker_id);
    CHECK(out.speaker_db.string()      == srv.speaker_db.string());
    CHECK(out.vad                      == srv.vad);
    CHECK(out.retain_terminal_hours    == srv.retain_terminal_hours);
    CHECK(out.provider                 == srv.provider);
    CHECK(out.api_key                  == srv.api_key);
    CHECK(out.api_keys["anthropic"]    == "sk-anth");
}

// ===========================================================================
// (9) config_json overloads — ClientConfig round-trip
// ===========================================================================

TEST_CASE("[e2] config_json overloads — ClientConfig round-trip",
          "[e2][config_split][config_json]") {
    ClientConfig cli;
    cli.device_pattern            = "alsa:hw1";
    cli.mic_source                = "mic-src";
    cli.monitor_source            = "mon-src";
    cli.mic_only                  = true;
    cli.keep_sources              = true;
    cli.language                  = "fr";
    cli.vocabulary                = "x,y,z";
    cli.summary_style             = "actions";
    cli.no_summary                = true;
    cli.caption_latency_ms        = 1500;
    cli.caption_normalize_display = false;
    cli.output_dir                = "/var/meet";
    cli.output_dir_explicit       = true;
    cli.note_dir                  = "/var/notes";
    cli.note.domain               = "ops";
    cli.note.tags = {"alpha", "beta"};
    // E.2(d.1) Wave 2.2b: context_*, reprocess_*, batch_mode, enroll_* were
    // moved off ClientConfig onto JobConfig only. They are no longer settable
    // here; the negative round-trip below ("[e2.2b][client-config-drop]")
    // covers the silent-ignore-on-load behavior.
    cli.staging_max_bytes         = static_cast<size_t>(100) * 1024 * 1024;
    cli.servers.push_back(ServerEntry{"a", "tcp:host1:1"});
    cli.servers.push_back(ServerEntry{"b", "unix:/run/y.sock"});
    cli.provider                  = "openai";
    cli.api_key                   = "sk-legacy-cli";
    cli.api_keys["openai"]        = "sk-openai-cli";

    JsonMap m = config_to_map(cli);
    ClientConfig out = client_config_from_map(m);

    CHECK(out.device_pattern            == cli.device_pattern);
    CHECK(out.mic_source                == cli.mic_source);
    CHECK(out.monitor_source            == cli.monitor_source);
    CHECK(out.mic_only                  == cli.mic_only);
    CHECK(out.keep_sources              == cli.keep_sources);
    CHECK(out.language                  == cli.language);
    CHECK(out.vocabulary                == cli.vocabulary);
    CHECK(out.summary_style             == cli.summary_style);
    CHECK(out.no_summary                == cli.no_summary);
    CHECK(out.caption_latency_ms        == cli.caption_latency_ms);
    CHECK(out.caption_normalize_display == cli.caption_normalize_display);
    CHECK(out.output_dir.string()       == cli.output_dir.string());
    CHECK(out.output_dir_explicit       == cli.output_dir_explicit);
    CHECK(out.note_dir.string()         == cli.note_dir.string());
    CHECK(out.note.domain               == cli.note.domain);
    REQUIRE(out.note.tags.size()        == 2);
    CHECK(out.note.tags[0]              == "alpha");
    CHECK(out.note.tags[1]              == "beta");
    CHECK(out.staging_max_bytes         == cli.staging_max_bytes);
    REQUIRE(out.servers.size()          == 2);
    CHECK(out.servers[0].name           == "a");
    CHECK(out.servers[0].address        == "tcp:host1:1");
    CHECK(out.servers[1].name           == "b");
    CHECK(out.servers[1].address        == "unix:/run/y.sock");
    CHECK(out.provider                  == cli.provider);
    CHECK(out.api_key                   == cli.api_key);
    CHECK(out.api_keys["openai"]        == "sk-openai-cli");
}

// ===========================================================================
// caption_latency_ms boundary guard on load_client_config
// ===========================================================================

TEST_CASE("[e2] load_client_config — caption_latency_ms out-of-range falls back",
          "[e2][config_split]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_cli_clm_invalid");
    fs::path path = dir / "client.yaml";
    {
        std::ofstream out(path);
        out << "client:\n  caption_latency_ms: 5000\n";
    }
    ClientConfig cfg = load_client_config(path);
    CHECK(cfg.caption_latency_ms == 500);

    fs::remove_all(dir);
}

// ===========================================================================
// E.2(d.1) Wave 2.2b — per-job dynamic fields dropped from ClientConfig
// ===========================================================================
//
// Step 2 of phase-e2-wave-2-2b-config-split-finish.md removed 8 fields from
// ClientConfig: context_file, context_inline, reprocess_dir,
// reprocess_batch_dir, reprocess_batch_dry_run, batch_mode, enroll_mode,
// enroll_name. These now live on JobConfig only and transit at job-enqueue
// time via session.init / PostprocessInput, never via the at-rest
// ClientConfig.
//
// A `client.yaml` that carries one of these keys (vanishingly rare — Wave
// 2.2a only just landed) is silently ignored on load. We pick silent-ignore
// over warn-or-reject to match the existing YAML round-trip behavior for
// unknown keys: ClientConfig has never warned on unknown YAML keys, and
// retroactively flipping that policy here would surprise operators who
// hand-edit client.yaml.
//
// The YAML save path (`save_client_config`) already does not emit any of
// these keys (verified by reading the function — it has no branch that
// references the dropped fields), so the silent-ignore behavior is purely
// on the load side: if a hand-edited or stale `client.yaml` contains
// `enroll_mode: true`, the loaded ClientConfig has no `enroll_mode` field
// to assign it to, and the YAML key drops on the floor.

TEST_CASE("[e2.2b] client.yaml with dropped per-job keys is silently ignored on load",
          "[e2.2b][client-config-drop][config_split]") {
    fs::path dir = make_tmp_dir("recmeet_test_e22b_drop");
    fs::path path = dir / "client.yaml";

    // Hand-write a client.yaml that carries `enroll_mode: true` plus
    // representative reprocess + context keys. None of these keys are
    // emitted by `save_client_config` so this shape only arises from a
    // stale (pre-2.2b) file or hand-editing.
    {
        std::ofstream out(path);
        out << "# stale pre-2.2b client.yaml fragment\n"
            << "enroll_mode: true\n"
            << "enroll_name: alice\n"
            << "reprocess_dir: \"/tmp/r1\"\n"
            << "context_inline: \"hello world\"\n"
            << "batch_mode: true\n"
            // Real keys that load_client_config DOES honor — sanity that
            // the load itself succeeded and we're not just failing early.
            << "client:\n"
            << "  caption_latency_ms: 750\n";
    }

    // Load must succeed (no exception, no throw).
    ClientConfig loaded = load_client_config(path);

    // Real keys round-tripped.
    CHECK(loaded.caption_latency_ms == 750);

    // The dropped keys' compile-time absence on ClientConfig is the
    // type-system guarantee: any line below that referenced
    // `loaded.enroll_mode` etc. would fail to compile. The runtime
    // guarantee we can assert here is that load_client_config() did
    // not throw and returned a usable struct.
    SUCCEED("load_client_config silently ignores dropped per-job keys");

    fs::remove_all(dir);
}

// JSON-map round-trip companion: client_config_from_map() also silently
// drops the 8 keys if a JSON wire-format payload accidentally carries them.
// (The legitimate sender is now config_to_map(const JobConfig&), not
// config_to_map(const ClientConfig&), but a misrouted map should not crash.)
TEST_CASE("[e2.2b] client_config_from_map silently ignores dropped per-job keys",
          "[e2.2b][client-config-drop][config_json]") {
    JsonMap m;
    m["device_pattern"]            = std::string("alsa:hw0");
    m["staging_max_bytes"]         = static_cast<int64_t>(1024);
    // Stale keys that used to round-trip via ClientConfig pre-2.2b.
    m["enroll_mode"]               = true;
    m["enroll_name"]               = std::string("alice");
    m["reprocess_dir"]             = std::string("/tmp/r1");
    m["reprocess_batch_dir"]       = std::string("/tmp/r2");
    m["reprocess_batch_dry_run"]   = true;
    m["batch_mode"]                = true;
    m["context_file"]              = std::string("/tmp/ctx.txt");
    m["context_inline"]            = std::string("ctx");

    ClientConfig cfg = client_config_from_map(m);

    // Real keys made it through.
    CHECK(cfg.device_pattern  == "alsa:hw0");
    CHECK(cfg.staging_max_bytes == static_cast<size_t>(1024));
    // The dropped keys' compile-time absence is the guarantee; the
    // runtime guarantee here is that client_config_from_map did not
    // throw and returned a usable struct.
    SUCCEED("client_config_from_map silently ignores dropped per-job keys");
}

// ===========================================================================
// v2-coexistence-with-v1 Phase 2A — ClientConfig log_* / transcription
// (whisper_model + diarize + vad) / captions.enabled round-trip.
//
// DUPLICATE-not-MOVE: ServerConfig keeps its same-named fields unchanged;
// ClientConfig gains parallel slots so the tray can persist last-selected
// UI state in client.yaml. The whisper_model + diarize + vad triple is the
// only triplet that crosses the wire (session.init prefs); captions_enabled
// and log_* stay client-local (Phase C retirement honored for captions;
// log_* govern the tray's own logger output).
// ===========================================================================

TEST_CASE("[client][config] ClientConfig logging + transcription + captions blocks "
          "round-trip via YAML",
          "[client][config][v2-coexistence]") {
    fs::path dir = make_tmp_dir("recmeet_test_client_v2_fields_rt");
    fs::path path = dir / "client.yaml";

    ClientConfig cfg;
    // Logging block — diverged from struct defaults so all three fields emit.
    cfg.log_level_str       = "debug";
    cfg.log_dir             = "/tmp/recmeet-client/logs";
    cfg.log_retention_hours = 12;
    // Transcription block — new fields alongside language/vocabulary.
    cfg.whisper_model       = "small.en";
    cfg.diarize             = false;
    cfg.vad                 = false;
    cfg.language            = "en";
    cfg.vocabulary          = "alpha,beta";
    // Captions block — client-local overlay-visible preference.
    cfg.captions_enabled    = false;

    save_client_config(cfg, path);
    REQUIRE(fs::exists(path));
    CHECK(perms_are_0600(path));

    ClientConfig loaded = load_client_config(path);

    // Logging round-trip.
    CHECK(loaded.log_level_str       == "debug");
    CHECK(loaded.log_dir.string()    == "/tmp/recmeet-client/logs");
    CHECK(loaded.log_retention_hours == 12);
    // Transcription round-trip.
    CHECK(loaded.whisper_model       == "small.en");
    CHECK(loaded.diarize             == false);
    CHECK(loaded.vad                 == false);
    CHECK(loaded.language            == "en");
    CHECK(loaded.vocabulary          == "alpha,beta");
    // Captions round-trip.
    CHECK(loaded.captions_enabled    == false);

    fs::remove_all(dir);
}

TEST_CASE("[client][config] ClientConfig defaults apply when blocks absent",
          "[client][config][v2-coexistence]") {
    fs::path dir = make_tmp_dir("recmeet_test_client_v2_fields_defaults");
    fs::path path = dir / "client.yaml";

    // Write a client.yaml that omits the logging / new transcription /
    // captions.enabled keys entirely. Loader must return struct defaults.
    {
        std::ofstream out(path);
        out << "audio:\n"
               "  device_pattern: \"alsa:hw0\"\n";
    }

    ClientConfig loaded = load_client_config(path);

    // Struct defaults preserved.
    CHECK(loaded.log_level_str       == "error");
    CHECK(loaded.log_dir.empty());
    CHECK(loaded.log_retention_hours == 4);
    CHECK(loaded.whisper_model.empty());
    CHECK(loaded.diarize             == true);
    CHECK(loaded.vad                 == true);
    CHECK(loaded.captions_enabled    == true);

    fs::remove_all(dir);
}

// ===========================================================================
// v2-coexistence-with-v1 Phase 2G — fresh server (empty XDG dirs / no
// server.yaml on disk) reports ServerConfig.captions_enabled = true. With
// Phase 2C deleting the migration helper's `srv.captions_enabled = true`
// override, the cascade now relies entirely on `load_server_config`'s YAML
// loader returning the struct default for an absent `captions.enabled` key.
// ===========================================================================

TEST_CASE("[server][config] fresh server reports captions_enabled = true",
          "[server][config][v2-coexistence]") {
    fs::path dir = make_tmp_dir("recmeet_test_server_v2_fresh_captions");
    fs::path path = dir / "daemon.yaml";

    REQUIRE_FALSE(fs::exists(path));

    // load_server_config against a non-existent path returns a default-
    // constructed ServerConfig with every YAML-derived field at its struct
    // default. ServerConfig.captions_enabled = true (see config.h:432).
    ServerConfig srv = load_server_config(path);
    CHECK(srv.captions_enabled == true);

    fs::remove_all(dir);
}

// ===========================================================================
// v2-coexistence-with-v1 Phase 2F.1 — load_cli_config projects ServerConfig
// + ClientConfig into JobConfig for CLI consumption.
// ===========================================================================

TEST_CASE("[cli][config] load_cli_config with empty dirs returns defaults",
          "[cli][config][v2-coexistence]") {
    fs::path dir = make_tmp_dir("recmeet_test_cli_config_empty");
    fs::path server_path = dir / "server.yaml";
    fs::path client_path = dir / "client.yaml";

    REQUIRE_FALSE(fs::exists(server_path));
    REQUIRE_FALSE(fs::exists(client_path));

    JobConfig cfg = load_cli_config(server_path, client_path);

    // From ServerConfig defaults.
    CHECK(cfg.captions_enabled  == true);    // 2G cascade
    CHECK(cfg.diarize           == true);
    CHECK(cfg.vad               == true);
    CHECK(cfg.log_level_str     == "error");
    CHECK(cfg.log_retention_hours == 4);
    CHECK(cfg.whisper_model     == "base");
    // From ClientConfig defaults.
    CHECK(cfg.output_dir.string() == "./meetings");
    CHECK(cfg.language.empty());
    CHECK(cfg.vocabulary.empty());

    fs::remove_all(dir);
}

TEST_CASE("[cli][config] load_cli_config projects server.yaml + client.yaml "
          "into JobConfig",
          "[cli][config][v2-coexistence]") {
    fs::path dir = make_tmp_dir("recmeet_test_cli_config_populated");
    fs::path server_path = dir / "daemon.yaml";
    fs::path client_path = dir / "client.yaml";

    {
        ServerConfig srv;
        srv.whisper_model       = "medium";
        srv.diarize             = false;
        srv.vad                 = false;
        srv.threads             = 8;
        srv.log_level_str       = "debug";
        srv.log_retention_hours = 24;
        srv.captions_enabled    = false;
        save_server_config(srv, server_path);
    }
    {
        ClientConfig cli;
        cli.language            = "fr";
        cli.vocabulary          = "alpha,beta";
        cli.output_dir          = "/tmp/cli-test-meetings";
        cli.note_dir            = "/tmp/cli-test-notes";
        cli.provider            = "anthropic";
        cli.api_model           = "claude-x";
        // log_* on ClientConfig should NOT be projected — CLI uses server's
        // log_*. We set a divergent value here to verify exclusion.
        cli.log_level_str       = "warn";
        cli.log_retention_hours = 99;
        save_client_config(cli, client_path);
    }

    JobConfig cfg = load_cli_config(server_path, client_path);

    // Server-projected fields.
    CHECK(cfg.whisper_model       == "medium");
    CHECK(cfg.diarize             == false);
    CHECK(cfg.vad                 == false);
    CHECK(cfg.threads             == 8);
    CHECK(cfg.log_level_str       == "debug");      // from server, not client
    CHECK(cfg.log_retention_hours == 24);           // from server, not client
    CHECK(cfg.captions_enabled    == false);

    // Client-projected fields.
    CHECK(cfg.language               == "fr");
    CHECK(cfg.vocabulary             == "alpha,beta");
    CHECK(cfg.output_dir.string()    == "/tmp/cli-test-meetings");
    CHECK(cfg.note_dir.string()      == "/tmp/cli-test-notes");
    CHECK(cfg.provider               == "anthropic");
    CHECK(cfg.api_model              == "claude-x");

    fs::remove_all(dir);
}

// ===========================================================================
// v2-coexistence-with-v1 Phase 2F.2 — vocab CLI write path updates
// client.yaml's vocabulary field via load-mutate-save; does NOT modify
// server.yaml. Exercises the helper functions directly to mirror what
// main.cpp's --add-vocab handler does.
// ===========================================================================

TEST_CASE("[cli][vocab] vocab write touches client.yaml only",
          "[cli][vocab][v2-coexistence]") {
    fs::path dir = make_tmp_dir("recmeet_test_cli_vocab_split");
    fs::path server_path = dir / "daemon.yaml";
    fs::path client_path = dir / "client.yaml";

    // Stage server.yaml with a single attribute we can verify is unchanged.
    {
        ServerConfig srv;
        srv.threads = 7;  // arbitrary diverged value
        save_server_config(srv, server_path);
    }
    // Stage client.yaml with an existing vocabulary + other fields the
    // operator might have set.
    {
        ClientConfig cli;
        cli.vocabulary = "foo";
        cli.language   = "en";
        cli.output_dir = "/tmp/operator-meetings";
        save_client_config(cli, client_path);
    }

    std::string server_before = [&]{
        std::ifstream in(server_path);
        std::stringstream ss; ss << in.rdbuf(); return ss.str();
    }();

    // Mirror main.cpp's --add-vocab handler load-mutate-save flow.
    ClientConfig client_cfg = load_client_config(client_path);
    client_cfg.vocabulary += ", bar";
    save_client_config(client_cfg, client_path);

    // server.yaml byte-for-byte unchanged.
    std::string server_after = [&]{
        std::ifstream in(server_path);
        std::stringstream ss; ss << in.rdbuf(); return ss.str();
    }();
    CHECK(server_before == server_after);

    // client.yaml's vocabulary updated; other fields preserved.
    ClientConfig reloaded = load_client_config(client_path);
    CHECK(reloaded.vocabulary       == "foo, bar");
    CHECK(reloaded.language         == "en");
    CHECK(reloaded.output_dir.string() == "/tmp/operator-meetings");

    fs::remove_all(dir);
}

// ===========================================================================
// v2-coexistence-with-v1 Phase 2E.4 — on_edit_config bootstrap-on-missing
// creates a valid client.yaml at client_config_dir() / "client.yaml". The
// tray's GTK callback cannot be invoked from the test harness, but the
// underlying primitive (save_client_config on a fresh ClientConfig produces
// a re-loadable YAML file at the expected path) IS testable.
// ===========================================================================

TEST_CASE("[tray][edit_config] bootstrap creates valid client.yaml",
          "[tray][edit_config][v2-coexistence]") {
    fs::path dir = make_tmp_dir("recmeet_test_tray_edit_config_bootstrap");
    fs::path path = dir / "client.yaml";

    REQUIRE_FALSE(fs::exists(path));

    // Mirror the on_edit_config bootstrap branch: fresh ClientConfig
    // (struct defaults) saved at the target path.
    ClientConfig cfg;
    save_client_config(cfg, path);

    REQUIRE(fs::exists(path));
    CHECK(perms_are_0600(path));

    // The bootstrapped file must round-trip through the loader.
    ClientConfig reloaded = load_client_config(path);
    CHECK(reloaded.log_level_str    == "error");
    CHECK(reloaded.captions_enabled == true);
    CHECK(reloaded.diarize          == true);
    CHECK(reloaded.vad              == true);

    fs::remove_all(dir);
}

// ===========================================================================
// v2-coexistence-with-v1 Phase 2E.1 — tray boot via load_client_config:
// with no client.yaml present, returns struct defaults; with populated
// client.yaml, returns the persisted values. Mirrors the tray startup
// rewire at src/tray.cpp:4037.
// ===========================================================================

TEST_CASE("[tray][config] load_client_config returns defaults when absent",
          "[tray][config][v2-coexistence]") {
    fs::path dir = make_tmp_dir("recmeet_test_tray_config_empty");
    fs::path path = dir / "client.yaml";

    REQUIRE_FALSE(fs::exists(path));

    ClientConfig cfg = load_client_config(path);
    CHECK(cfg.log_level_str    == "error");
    CHECK(cfg.log_retention_hours == 4);
    CHECK(cfg.whisper_model.empty());
    CHECK(cfg.diarize          == true);
    CHECK(cfg.vad              == true);
    CHECK(cfg.captions_enabled == true);
    CHECK(cfg.provider         == "xai");

    fs::remove_all(dir);
}

TEST_CASE("[tray][config] load_client_config returns persisted values",
          "[tray][config][v2-coexistence]") {
    fs::path dir = make_tmp_dir("recmeet_test_tray_config_populated");
    fs::path path = dir / "client.yaml";

    {
        ClientConfig cfg;
        cfg.log_level_str       = "info";
        cfg.log_retention_hours = 8;
        cfg.whisper_model       = "large-v3";
        cfg.diarize             = false;
        cfg.vad                 = false;
        cfg.captions_enabled    = false;
        cfg.language            = "es";
        cfg.output_dir          = "/tmp/tray-out";
        save_client_config(cfg, path);
    }

    ClientConfig loaded = load_client_config(path);
    CHECK(loaded.log_level_str       == "info");
    CHECK(loaded.log_retention_hours == 8);
    CHECK(loaded.whisper_model       == "large-v3");
    CHECK(loaded.diarize             == false);
    CHECK(loaded.vad                 == false);
    CHECK(loaded.captions_enabled    == false);
    CHECK(loaded.language            == "es");
    CHECK(loaded.output_dir.string() == "/tmp/tray-out");

    fs::remove_all(dir);
}
