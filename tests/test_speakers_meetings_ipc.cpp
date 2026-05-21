// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase E.6.1 — IPC round-trip tests for the 8 new speakers.* /
// meetings.* verbs that back the tray-bundled WebUI. Tag:
// [e6][ipc][speakers-meetings].
//
// Phase 2b conversion: the fixture now drives the PRODUCTION handlers
// via DaemonTestHarness instead of registering stub bodies. The test
// expectations are unchanged — every assertion still holds against
// the real `register_daemon_handlers()` body, because the stubs were
// originally cloned from the production handler shapes (with minor
// message-text deviations that were already covered by substring
// matches like `contains(err.message, "5 seconds")`).
//
// The harness wires `g_server_config.speaker_db` /
// `g_server_config.meetings_root` / `g_meeting_index` so the production
// handlers see the per-test state. Each TEST_CASE constructs a fresh
// `Fixture` (RAII over `DaemonTestHarness`) so per-test state is
// independent; the `g_batch_reidentify_running` global is also reset
// by the harness on construction.

#include <catch2/catch_test_macros.hpp>

#include "daemon_test_harness.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "json_util.h"
#include "meeting_index.h"
#include "meetings_browse.h"
#include "pipeline.h"
#include "speaker_id.h"
#include "util.h"
#include "uuid.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <unistd.h>

using namespace recmeet;
using recmeet::test::DaemonTestHarness;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Test fixture — drives the PRODUCTION handlers via DaemonTestHarness.
// Phase 2b: this fixture replaced an in-test stub-handler implementation
// with a wrapper that wires g_meeting_index / g_server_config so the real
// `register_daemon_handlers()` body sees the per-test state. The public
// `Fixture` surface (`db_dir`, `meetings_root`, `make_meeting`,
// `seed_speaker`, `seed_meeting_speakers`, `make_client`) is preserved so
// the existing TEST_CASE bodies remain unchanged.
// ---------------------------------------------------------------------------

namespace {

struct Fixture {
    DaemonTestHarness harness;
    fs::path db_dir;
    fs::path meetings_root;

    explicit Fixture(const std::string& /*name*/)
        : db_dir(harness.speaker_db_dir()),
          meetings_root(harness.meetings_dir())
    {
        harness.start();
    }

    std::unique_ptr<IpcClient> make_client() {
        return harness.make_client();
    }

    // Allocate a meeting directory, write a context.json with the given
    // meeting_id, and bind it into the daemon-global MeetingIndex.
    fs::path make_meeting(const std::string& name,
                          const std::string& meeting_id) {
        fs::path mp = meetings_root / name;
        fs::create_directories(mp);
        save_meeting_context(mp, /*context_inline=*/"", /*context_file=*/{},
                             /*timestamp=*/"", /*meeting_id=*/meeting_id);
        g_meeting_index->bind(meeting_id, mp);
        return mp;
    }

    void seed_speaker(const std::string& name, std::size_t enrollments = 1,
                      std::size_t dim = 4) {
        SpeakerProfile p;
        p.name = name;
        p.created = p.updated = "2026-01-01T00:00:00Z";
        for (std::size_t i = 0; i < enrollments; ++i) {
            std::vector<float> emb(dim, static_cast<float>(i + 1));
            p.embeddings.push_back(std::move(emb));
        }
        save_speaker(db_dir, p);
    }

    void seed_meeting_speakers(const fs::path& meeting_path,
                               std::vector<MeetingSpeaker> speakers) {
        save_meeting_speakers(meeting_path, speakers,
                              derive_meeting_timestamp(meeting_path));
    }
};

// The original stub bodies for all 8 verbs lived here. They were removed
// in the Phase 2b conversion (commit subject "test(speakers-meetings):
// drive production handlers via DaemonTestHarness"). The production
// handlers in src/daemon_handlers.cpp are now the single producer of the
// response shape for these tests.


// Convenience for "is this substring present?" assertions on JSON-blob
// string fields (response shapes use embedded JSON arrays).
bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

// ===========================================================================
// speakers.get
// ===========================================================================

TEST_CASE("speakers.get returns slim shape for an enrolled speaker",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("get_ok");
    fx.seed_speaker("Alice", /*enrollments=*/3, /*dim=*/192);

    auto client = fx.make_client();
    JsonMap params;
    params["name"] = std::string("Alice");
    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("speakers.get", params, resp, err));
    CHECK(json_val_as_string(resp.result["name"]) == "Alice");
    CHECK(json_val_as_int(resp.result["enrollments"]) == 3);
    CHECK(json_val_as_int(resp.result["embedding_count"]) == 3);
    CHECK(json_val_as_int(resp.result["embedding_dim"]) == 192);
    // No raw embedding blob in the response.
    CHECK(resp.result.find("embeddings") == resp.result.end());
}

TEST_CASE("speakers.get returns InvalidParams for an unknown name",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("get_missing");
    auto client = fx.make_client();
    JsonMap params;
    params["name"] = std::string("Nobody");
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client->call("speakers.get", params, resp, err));
    CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(contains(err.message, "not_found"));
}

// ===========================================================================
// speakers.enroll
// ===========================================================================

TEST_CASE("speakers.enroll appends an embedding to a profile",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("enroll_ok");
    const std::string mid = new_uuid_v4();
    auto mp = fx.make_meeting("2026-05-20_10-00", mid);
    MeetingSpeaker s;
    s.cluster_id = 0;
    s.label = "Speaker_01";
    s.identified = false;
    s.embedding = {0.5f, 0.5f, 0.5f, 0.5f};
    s.duration_sec = 12.0f;
    s.confidence = 0.7f;
    fx.seed_meeting_speakers(mp, {s});

    auto client = fx.make_client();
    JsonMap params;
    params["name"] = std::string("Alice");
    params["meeting_id"] = mid;
    params["cluster_id"] = static_cast<int64_t>(0);
    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("speakers.enroll", params, resp, err));
    CHECK(json_val_as_bool(resp.result["ok"]) == true);
    CHECK(json_val_as_double(resp.result["duration_sec"]) == 12.0);
    // Confidence 0.7 → no warning expected.
    CHECK(resp.result.find("warning") == resp.result.end());

    // Verify the profile was actually written.
    auto profiles = load_speaker_db(fx.db_dir);
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].name == "Alice");
    CHECK(profiles[0].embeddings.size() == 1);
}

TEST_CASE("speakers.enroll rejects clusters with less than 5 seconds of audio",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("enroll_short");
    const std::string mid = new_uuid_v4();
    auto mp = fx.make_meeting("2026-05-20_10-01", mid);
    MeetingSpeaker s;
    s.cluster_id = 0;
    s.label = "Speaker_01";
    s.identified = false;
    s.embedding = {0.5f, 0.5f, 0.5f, 0.5f};
    s.duration_sec = 2.0f;  // < 5.0 quality gate
    s.confidence = 0.9f;
    fx.seed_meeting_speakers(mp, {s});

    auto client = fx.make_client();
    JsonMap params;
    params["name"] = std::string("Alice");
    params["meeting_id"] = mid;
    params["cluster_id"] = static_cast<int64_t>(0);
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client->call("speakers.enroll", params, resp, err));
    CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(contains(err.message, "5 seconds"));
}

// ===========================================================================
// speakers.remove_embedding
// ===========================================================================

TEST_CASE("speakers.remove_embedding decrements remaining on a multi-emb profile",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("rmemb_ok");
    fx.seed_speaker("Alice", /*enrollments=*/3, /*dim=*/4);

    auto client = fx.make_client();
    JsonMap params;
    params["name"] = std::string("Alice");
    params["index"] = static_cast<int64_t>(1);
    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("speakers.remove_embedding", params, resp, err));
    CHECK(json_val_as_bool(resp.result["ok"]) == true);
    CHECK(json_val_as_int(resp.result["remaining"]) == 2);

    // Verify profile still exists with 2 embeddings.
    auto profiles = load_speaker_db(fx.db_dir);
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].embeddings.size() == 2);
}

TEST_CASE("speakers.remove_embedding rejects out-of-range index",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("rmemb_oob");
    fx.seed_speaker("Alice", /*enrollments=*/2, /*dim=*/4);

    auto client = fx.make_client();
    JsonMap params;
    params["name"] = std::string("Alice");
    params["index"] = static_cast<int64_t>(5);
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client->call("speakers.remove_embedding", params, resp, err));
    CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(contains(err.message, "out of range"));
}

// ===========================================================================
// speakers.relabel
// ===========================================================================

TEST_CASE("speakers.relabel mutates meeting label and returns old_label",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("relabel_ok");
    const std::string mid = new_uuid_v4();
    auto mp = fx.make_meeting("2026-05-20_11-00", mid);

    MeetingSpeaker s;
    s.cluster_id = 0;
    s.label = "Speaker_01";  // unidentified default name
    s.identified = false;
    s.embedding = {0.1f, 0.2f, 0.3f, 0.4f};
    s.duration_sec = 60.0f;
    s.confidence = 0.0f;
    fx.seed_meeting_speakers(mp, {s});

    auto client = fx.make_client();
    JsonMap params;
    params["meeting_id"] = mid;
    params["cluster_id"] = static_cast<int64_t>(0);
    params["new_label"] = std::string("Bob");
    // update_profile defaults true
    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("speakers.relabel", params, resp, err));
    CHECK(json_val_as_bool(resp.result["ok"]) == true);
    CHECK(json_val_as_string(resp.result["old_label"]) == "Speaker_01");

    // Verify meeting speakers.json mutated.
    auto reloaded = load_meeting_speakers(mp);
    REQUIRE(reloaded.size() == 1);
    CHECK(reloaded[0].label == "Bob");
    CHECK(reloaded[0].identified == true);
    CHECK(reloaded[0].confidence == 1.0f);
    // Verify new profile was created in DB.
    auto profiles = load_speaker_db(fx.db_dir);
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].name == "Bob");
}

TEST_CASE("speakers.relabel rejects unknown cluster_id",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("relabel_unk");
    const std::string mid = new_uuid_v4();
    auto mp = fx.make_meeting("2026-05-20_11-01", mid);
    MeetingSpeaker s;
    s.cluster_id = 0;
    s.label = "Speaker_01";
    s.identified = false;
    s.duration_sec = 30.0f;
    fx.seed_meeting_speakers(mp, {s});

    auto client = fx.make_client();
    JsonMap params;
    params["meeting_id"] = mid;
    params["cluster_id"] = static_cast<int64_t>(99);
    params["new_label"] = std::string("Bob");
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client->call("speakers.relabel", params, resp, err));
    CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(contains(err.message, "cluster_id not found"));
}

// ===========================================================================
// speakers.batch_reidentify
// ===========================================================================

TEST_CASE("speakers.batch_reidentify returns async:true on idle start",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("batch_start");
    auto client = fx.make_client();
    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("speakers.batch_reidentify", resp, err));
    CHECK(json_val_as_bool(resp.result["ok"]) == true);
    CHECK(json_val_as_bool(resp.result["async"]) == true);
    CHECK_FALSE(json_val_as_string(resp.result["started_at"]).empty());
    // The production handler's worker either (a) exits synchronously when
    // the speaker DB is empty — clears g_batch_reidentify_running on the
    // same thread that returned `started_at` — or (b) detaches a thread
    // that runs re_identify_meeting over each known meeting. The test
    // fixture seeds no speakers, so (a) is what happens here; we
    // additionally settle the flag to make this test self-contained
    // when followed by another batch test.
    g_batch_reidentify_running.store(false);
}

TEST_CASE("speakers.batch_reidentify rejects overlapping calls with InvalidParams",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("batch_busy");
    auto client = fx.make_client();
    // Simulate an in-flight batch by holding the g_batch_reidentify_running
    // flag from the test thread. The production handler's first
    // `compare_exchange_strong` (daemon_handlers.cpp:2039) will observe
    // the flag set and short-circuit with the InvalidParams reject path.
    //
    // Driving this through two back-to-back IPC calls is unreliable in
    // tests because the production worker for an empty speaker DB exits
    // synchronously (no detached thread on the empty path; see
    // daemon_handlers.cpp:2058-2061). The Phase 3 daemon-spawning
    // harness will exercise the full async-worker path when sherpa
    // models are available.
    g_batch_reidentify_running.store(true);

    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client->call("speakers.batch_reidentify", resp, err));
    CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(contains(err.message, "batch in progress"));

    // Restore the flag so other tests start clean.
    g_batch_reidentify_running.store(false);
}

// ===========================================================================
// meetings.list
// ===========================================================================

TEST_CASE("meetings.list returns one entry per directory with the slim shape",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("mlist_ok");
    // V2 meeting: has meeting_id (UUID) + a Meeting_*.md note.
    const std::string mid = new_uuid_v4();
    auto mp = fx.make_meeting("2026-05-20_12-00", mid);
    {
        std::ofstream out(mp / "Meeting_2026-05-20_12-00.md");
        out << "# Meeting note\n";
    }
    // Legacy V1 meeting: no context.json with meeting_id at all. We
    // explicitly write a context.json missing the meeting_id field so the
    // index sees nothing for it. The dir alone is enough for
    // discover_meetings to enumerate it.
    fs::path legacy_dir = fx.meetings_root / "2026-05-20_09-00_legacy";
    fs::create_directories(legacy_dir);
    // Write a context.json without meeting_id (load_meeting_id returns "" for it).
    std::ofstream(legacy_dir / "context.json") << "{}\n";

    auto client = fx.make_client();
    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("meetings.list", resp, err));
    CHECK(json_val_as_int(resp.result["count"]) == 2);
    const std::string arr = json_val_as_string(resp.result["meetings"]);
    // Both meeting dir names should appear.
    CHECK(contains(arr, "2026-05-20_12-00"));
    CHECK(contains(arr, "2026-05-20_09-00_legacy"));
    // The V2 meeting carries its UUID; the legacy one is null.
    CHECK(contains(arr, mid));
    CHECK(contains(arr, "\"meeting_id\":null"));
    // The V2 meeting has has_summary:true (Meeting_*.md present).
    CHECK(contains(arr, "\"has_summary\":true"));
}

TEST_CASE("meetings.list returns empty list when meetings_root is empty",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("mlist_empty");
    auto client = fx.make_client();
    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("meetings.list", resp, err));
    CHECK(json_val_as_int(resp.result["count"]) == 0);
    CHECK(json_val_as_string(resp.result["meetings"]) == "[]");
}

// ===========================================================================
// meetings.speakers
// ===========================================================================

TEST_CASE("meetings.speakers returns slim per-speaker shape without embeddings",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("mspk_ok");
    const std::string mid = new_uuid_v4();
    auto mp = fx.make_meeting("2026-05-20_13-00", mid);
    MeetingSpeaker s1;
    s1.cluster_id = 0;
    s1.label = "Alice";
    s1.identified = true;
    s1.embedding = {0.1f, 0.2f, 0.3f};
    s1.duration_sec = 45.0f;
    s1.confidence = 0.92f;
    MeetingSpeaker s2;
    s2.cluster_id = 1;
    s2.label = "Speaker_02";
    s2.identified = false;
    s2.embedding = {0.4f, 0.5f, 0.6f};
    s2.duration_sec = 12.5f;
    s2.confidence = 0.0f;
    fx.seed_meeting_speakers(mp, {s1, s2});

    auto client = fx.make_client();
    JsonMap params;
    params["meeting_id"] = mid;
    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("meetings.speakers", params, resp, err));
    CHECK(json_val_as_int(resp.result["count"]) == 2);
    const std::string arr = json_val_as_string(resp.result["speakers"]);
    CHECK(contains(arr, "\"label\":\"Alice\""));
    CHECK(contains(arr, "\"label\":\"Speaker_02\""));
    CHECK(contains(arr, "\"cluster_id\":0"));
    CHECK(contains(arr, "\"cluster_id\":1"));
    CHECK(contains(arr, "\"identified\":true"));
    CHECK(contains(arr, "\"identified\":false"));
    // No "embedding" or "embeddings" field — biometric blob omitted.
    CHECK_FALSE(contains(arr, "\"embedding\""));
}

TEST_CASE("meetings.speakers rejects unknown meeting_id",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("mspk_unk");
    auto client = fx.make_client();
    JsonMap params;
    params["meeting_id"] = new_uuid_v4();  // never bound
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client->call("meetings.speakers", params, resp, err));
    CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(contains(err.message, "unknown meeting_id"));
}

// ===========================================================================
// meetings.read_note
// ===========================================================================

TEST_CASE("meetings.read_note returns the markdown content of Meeting_*.md",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("mread_ok");
    const std::string mid = new_uuid_v4();
    auto mp = fx.make_meeting("2026-05-20_14-00", mid);
    const std::string body = "# Title\n\nHello world.\n";
    {
        std::ofstream out(mp / "Meeting_2026-05-20_14-00.md");
        out << body;
    }
    auto client = fx.make_client();
    JsonMap params;
    params["meeting_id"] = mid;
    IpcResponse resp;
    IpcError err;
    REQUIRE(client->call("meetings.read_note", params, resp, err));
    CHECK(json_val_as_string(resp.result["path"]) == "Meeting_2026-05-20_14-00.md");
    CHECK(json_val_as_string(resp.result["content"]) == body);
}

TEST_CASE("meetings.read_note returns InvalidParams not_found when no Meeting_*.md",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("mread_missing");
    const std::string mid = new_uuid_v4();
    fx.make_meeting("2026-05-20_14-01", mid);  // dir exists, no Meeting_*.md
    auto client = fx.make_client();
    JsonMap params;
    params["meeting_id"] = mid;
    IpcResponse resp;
    IpcError err;
    CHECK_FALSE(client->call("meetings.read_note", params, resp, err));
    CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(contains(err.message, "not_found"));
}
