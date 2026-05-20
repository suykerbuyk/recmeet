// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase E.2 Wave 2.2a — split-config test suite.
//
// Validates the new ServerConfig / ClientConfig load+save APIs, the legacy
// migration helper (`migrate_legacy_config_if_present`), and the
// config_to_map / config_from_map overloads. The monolithic JobConfig struct
// is preserved alongside; Wave 2.2b retypes consumers and removes it.
//
// All TEST_CASEs are tagged `[e2]` so the orchestrator can grep-verify the
// scope of Wave 2.2a.

#include <catch2/catch_test_macros.hpp>
#include "config.h"
#include "config_json.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

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
// (4) migrate_legacy_config_if_present — fresh migration
// ===========================================================================

TEST_CASE("[e2] migrate_legacy_config_if_present — fresh migration",
          "[e2][config_split][migration]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_migrate_fresh");
    fs::path legacy = dir / "config.yaml";
    fs::path daemon_path = dir / "daemon.yaml";
    fs::path client_path = dir / "client.yaml";
    fs::path backup_path = dir / "config.yaml.v1-backup";

    // Write a legacy config with values in BOTH client-y and server-y sections.
    {
        std::ofstream out(legacy);
        out << "audio:\n"
               "  device_pattern: \"alsa:hw0\"\n"
               "  mic_only: true\n"
               "\ntranscription:\n"
               "  model: small\n"
               "  language: en\n"
               "  vocabulary: \"foo,bar\"\n"
               "\nsummary:\n"
               "  provider: anthropic\n"
               "  model: claude-sonnet-4-6\n"
               "  style: bullet\n"
               "\napi_keys:\n"
               "  anthropic: \"sk-legacy-mig\"\n"
               "\ndiarization:\n"
               "  enabled: false\n"
               "  num_speakers: 3\n"
               "\nspeaker_id:\n"
               "  enabled: false\n"
               "\nvad:\n"
               "  enabled: false\n"
               "\ncaptions:\n"
               "  enabled: true\n"
               "  model: en-2023-06-26\n"
               "  normalize_display: false\n"
               "\ngeneral:\n"
               "  threads: 4\n"
               "\nlogging:\n"
               "  level: info\n"
               "\noutput:\n"
               "  directory: \"/tmp/meetings\"\n"
               "\nnotes:\n"
               "  domain: engineering\n"
               "\nweb:\n"
               "  port: 9090\n"
               "\nipc:\n"
               "  max_clients: 32\n"
               "\nserver:\n"
               "  allow_client_downloads: false\n"
               "  retain_terminal_hours: 12\n"
               "\nclient:\n"
               "  caption_latency_ms: 750\n"
               "  staging_max_bytes: 1073741824\n"
               "  servers:\n"
               "    - name: default\n"
               "      address: \"unix:/tmp/x.sock\"\n";
    }
    // Read the legacy content so we can verify the backup matches.
    std::string legacy_content = read_file(legacy);

    migrate_legacy_config_if_present(dir);

    REQUIRE(fs::exists(daemon_path));
    REQUIRE(fs::exists(client_path));
    REQUIRE(fs::exists(backup_path));
    REQUIRE_FALSE(fs::exists(legacy));

    // Backup byte-for-byte matches the legacy content (atomic rename, no
    // copy+delete).
    CHECK(read_file(backup_path) == legacy_content);

    // Perms 0600 on both new files.
    CHECK(perms_are_0600(daemon_path));
    CHECK(perms_are_0600(client_path));

    // Values made it into the right struct.
    ServerConfig srv = load_server_config(daemon_path);
    ClientConfig cli = load_client_config(client_path);

    // Server-side (daemon.yaml):
    CHECK(srv.whisper_model            == "small");
    CHECK(srv.threads                  == 4);
    CHECK(srv.web_port                 == 9090);
    CHECK(srv.max_clients              == 32);
    CHECK(srv.allow_client_downloads   == false);
    CHECK(srv.retain_terminal_hours    == 12);
    CHECK(srv.diarize                  == false);
    CHECK(srv.num_speakers             == 3);
    CHECK(srv.speaker_id               == false);
    CHECK(srv.vad                      == false);
    CHECK(srv.captions_enabled         == true);
    CHECK(srv.caption_model            == "en-2023-06-26");
    CHECK(srv.log_level_str            == "info");
    // DUAL fallback: provider / api_keys also land on the daemon side.
    CHECK(srv.provider                 == "anthropic");
    CHECK(srv.api_keys["anthropic"]    == "sk-legacy-mig");

    // Client-side (client.yaml):
    CHECK(cli.device_pattern           == "alsa:hw0");
    CHECK(cli.mic_only                 == true);
    CHECK(cli.language                 == "en");
    CHECK(cli.vocabulary               == "foo,bar");
    CHECK(cli.summary_style            == "bullet");
    CHECK(cli.no_summary               == false);
    CHECK(cli.caption_latency_ms       == 750);
    CHECK(cli.caption_normalize_display == false);
    CHECK(cli.output_dir.string()      == "/tmp/meetings");
    CHECK(cli.note.domain              == "engineering");
    CHECK(cli.staging_max_bytes        == static_cast<size_t>(1073741824));
    REQUIRE(cli.servers.size()         == 1);
    CHECK(cli.servers[0].name          == "default");
    CHECK(cli.servers[0].address       == "unix:/tmp/x.sock");
    // DUAL primary: provider / api_keys land on the client side too.
    CHECK(cli.provider                 == "anthropic");
    CHECK(cli.api_keys["anthropic"]    == "sk-legacy-mig");

    fs::remove_all(dir);
}

// ===========================================================================
// (5) migrate_legacy_config_if_present — idempotency
// ===========================================================================

TEST_CASE("[e2] migrate_legacy_config_if_present — idempotency",
          "[e2][config_split][migration]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_migrate_idem");
    fs::path legacy = dir / "config.yaml";
    fs::path daemon_path = dir / "daemon.yaml";
    fs::path client_path = dir / "client.yaml";
    fs::path backup_path = dir / "config.yaml.v1-backup";

    {
        std::ofstream out(legacy);
        out << "transcription:\n  model: tiny\n";
    }

    // First call performs the migration.
    migrate_legacy_config_if_present(dir);
    REQUIRE(fs::exists(daemon_path));
    REQUIRE(fs::exists(client_path));
    REQUIRE(fs::exists(backup_path));

    // Snapshot post-migration state.
    std::string daemon_before = read_file(daemon_path);
    std::string client_before = read_file(client_path);
    std::string backup_before = read_file(backup_path);

    // Second call must be a no-op.
    migrate_legacy_config_if_present(dir);

    // No legacy file recreated.
    CHECK_FALSE(fs::exists(legacy));
    // Files unchanged byte-for-byte.
    CHECK(read_file(daemon_path) == daemon_before);
    CHECK(read_file(client_path) == client_before);
    CHECK(read_file(backup_path) == backup_before);

    fs::remove_all(dir);
}

// ===========================================================================
// (6) migrate_legacy_config_if_present — no legacy file
// ===========================================================================

TEST_CASE("[e2] migrate_legacy_config_if_present — no legacy file",
          "[e2][config_split][migration]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_migrate_nolegacy");

    // Empty dir — no exception, no files written.
    REQUIRE_NOTHROW(migrate_legacy_config_if_present(dir));
    CHECK_FALSE(fs::exists(dir / "daemon.yaml"));
    CHECK_FALSE(fs::exists(dir / "client.yaml"));
    CHECK_FALSE(fs::exists(dir / "config.yaml.v1-backup"));

    fs::remove_all(dir);
}

// ===========================================================================
// (7) migrate_legacy_config_if_present — split files already present
// ===========================================================================

TEST_CASE("[e2] migrate_legacy_config_if_present — split files already present",
          "[e2][config_split][migration]") {
    fs::path dir = make_tmp_dir("recmeet_test_e2_migrate_already");
    fs::path legacy = dir / "config.yaml";
    fs::path daemon_path = dir / "daemon.yaml";
    fs::path client_path = dir / "client.yaml";

    // Pre-create all three files.
    { std::ofstream out(legacy);      out << "transcription:\n  model: legacy\n"; }
    { std::ofstream out(daemon_path); out << "# preexisting daemon\n"; }
    { std::ofstream out(client_path); out << "# preexisting client\n"; }

    std::string legacy_before = read_file(legacy);
    std::string daemon_before = read_file(daemon_path);
    std::string client_before = read_file(client_path);

    migrate_legacy_config_if_present(dir);

    // Legacy NOT renamed (we don't clobber existing split state).
    CHECK(fs::exists(legacy));
    CHECK(read_file(legacy) == legacy_before);
    CHECK(read_file(daemon_path) == daemon_before);
    CHECK(read_file(client_path) == client_before);
    CHECK_FALSE(fs::exists(dir / "config.yaml.v1-backup"));

    fs::remove_all(dir);
}

// ===========================================================================
// (8) config_json overloads — ServerConfig round-trip
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
