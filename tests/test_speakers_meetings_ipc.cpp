// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase E.6.1 — IPC round-trip tests for the 8 new speakers.* /
// meetings.* verbs that back the tray-bundled WebUI. Tag:
// [e6][ipc][speakers-meetings].
//
// Pattern mirrors the speakers.list / speakers.remove / speakers.reset
// tests in test_ipc_integration.cpp (which itself mirrors the daemon's
// handler bodies in src/daemon.cpp). The test registers handler bodies
// that read from closure-captured `db_dir` / `meetings_root` /
// `MeetingIndex*` rather than the daemon's `g_server_config` /
// `g_meeting_index`; the helpers (`load_speaker_db`, `save_speaker`,
// `load_meeting_speakers`, `discover_meetings`, …) are the same library
// helpers the production handlers call, so the round-trip validates
// both the wire shape and the helper-library behaviour.

#include <catch2/catch_test_macros.hpp>

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
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Test fixture — registers all 8 production handler bodies against a per-test
// IpcServer with closure-captured `db_dir` / `meetings_root` / `MeetingIndex`.
// Mirrors the daemon's handlers in src/daemon.cpp:3699+.
// ---------------------------------------------------------------------------

namespace {

// Atomic flag for the batch_reidentify "in-progress" reject path. Reset
// at fixture construction so per-test state is independent.
static std::atomic<bool> g_test_batch_running{false};

// Helper that mirrors the static is_safe_dirname() in daemon.cpp / web.cpp.
bool t_is_safe_dirname(const std::string& name) {
    if (name.empty()) return false;
    if (name == "." || name == "..") return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find('\0') != std::string::npos) return false;
    return true;
}

struct Fixture {
    fs::path tmp_root;
    fs::path db_dir;
    fs::path meetings_root;
    std::string sock_path;
    MeetingIndex meeting_index;
    IpcServer server;
    std::thread srv_thread;

    Fixture(const std::string& name)
        : tmp_root(fs::temp_directory_path() / ("recmeet_e6_" + name)),
          db_dir(tmp_root / "speakers"),
          meetings_root(tmp_root / "meetings"),
          sock_path("/tmp/recmeet_e6_" + name + ".sock"),
          server(sock_path)
    {
        fs::remove_all(tmp_root);
        fs::create_directories(db_dir);
        fs::create_directories(meetings_root);
        unlink(sock_path.c_str());
        g_test_batch_running.store(false);

        register_handlers();
        REQUIRE(server.start());
        srv_thread = std::thread([this]() { server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~Fixture() {
        server.stop();
        if (srv_thread.joinable()) srv_thread.join();
        fs::remove_all(tmp_root);
        unlink(sock_path.c_str());
    }

    std::unique_ptr<IpcClient> make_client() {
        auto c = std::make_unique<IpcClient>(sock_path);
        REQUIRE(c->connect());
        return c;
    }

    // Allocate a meeting directory, write a context.json with the given
    // meeting_id, and bind it into the test-local MeetingIndex. Returns
    // the meeting path.
    fs::path make_meeting(const std::string& name, const std::string& meeting_id) {
        fs::path mp = meetings_root / name;
        fs::create_directories(mp);
        // Write context.json with the meeting_id field. timestamp empty
        // → legacy "context.json" filename.
        save_meeting_context(mp, /*context_inline=*/"", /*context_file=*/{},
                             /*timestamp=*/"", /*meeting_id=*/meeting_id);
        meeting_index.bind(meeting_id, mp);
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

    // Write a speakers.json into the meeting dir.
    void seed_meeting_speakers(const fs::path& meeting_path,
                               std::vector<MeetingSpeaker> speakers) {
        save_meeting_speakers(meeting_path, speakers,
                              derive_meeting_timestamp(meeting_path));
    }

    void register_handlers() {
        // ----- speakers.get -----
        server.on("speakers.get",
                  [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            auto it = req.params.find("name");
            std::string name = (it != req.params.end())
                ? json_val_as_string(it->second) : "";
            if (!t_is_safe_dirname(name)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.get: invalid 'name'";
                return false;
            }
            auto profiles = load_speaker_db(db_dir);
            const SpeakerProfile* found = nullptr;
            for (const auto& p : profiles) {
                if (p.name == name) { found = &p; break; }
            }
            if (!found) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.get: not_found";
                return false;
            }
            const int64_t emb_count = static_cast<int64_t>(found->embeddings.size());
            const int64_t emb_dim = emb_count > 0
                ? static_cast<int64_t>(found->embeddings[0].size()) : 0;
            resp.result["name"] = found->name;
            resp.result["enrollments"] = emb_count;
            resp.result["created"] = found->created;
            resp.result["updated"] = found->updated;
            resp.result["embedding_dim"] = emb_dim;
            resp.result["embedding_count"] = emb_count;
            return true;
        });

        // ----- speakers.enroll -----
        server.on("speakers.enroll",
                  [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            std::string name;
            std::string meeting_id;
            int64_t cluster_id = -1;
            {
                auto it = req.params.find("name");
                if (it != req.params.end()) name = json_val_as_string(it->second);
                auto it2 = req.params.find("meeting_id");
                if (it2 != req.params.end())
                    meeting_id = json_val_as_string(it2->second);
                auto it3 = req.params.find("cluster_id");
                if (it3 != req.params.end())
                    cluster_id = json_val_as_int(it3->second, -1);
            }
            if (!t_is_safe_dirname(name)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "invalid name";
                return false;
            }
            if (meeting_id.empty() || !is_valid_meeting_id(meeting_id)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "invalid meeting_id";
                return false;
            }
            if (cluster_id < 0) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "invalid cluster_id";
                return false;
            }
            auto hit = meeting_index.find(meeting_id);
            if (!hit.has_value()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "unknown meeting_id";
                return false;
            }
            const fs::path meeting_path = *hit;
            auto speakers = load_meeting_speakers(meeting_path);
            const MeetingSpeaker* spk = nullptr;
            for (const auto& s : speakers) {
                if (s.cluster_id == static_cast<int>(cluster_id)) { spk = &s; break; }
            }
            if (!spk) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "cluster_id not found";
                return false;
            }
            if (spk->embedding.empty()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "no embedding data";
                return false;
            }
            if (spk->duration_sec < 5.0f) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "less than 5 seconds of audio";
                return false;
            }
            auto profiles = load_speaker_db(db_dir);
            SpeakerProfile profile;
            for (const auto& p : profiles) {
                if (p.name == name) { profile = p; break; }
            }
            if (profile.name.empty()) {
                profile.name = name;
                profile.created = iso_now();
            }
            profile.updated = iso_now();
            profile.embeddings.push_back(spk->embedding);
            save_speaker(db_dir, profile);
            resp.result["ok"] = true;
            resp.result["duration_sec"] = static_cast<double>(spk->duration_sec);
            resp.result["confidence"] = static_cast<double>(spk->confidence);
            if (spk->confidence > 0.0f && spk->confidence < 0.5f) {
                std::string warning = "Low confidence ("
                    + std::to_string(spk->confidence).substr(0, 4)
                    + ") — this enrollment may be inaccurate";
                resp.result["warning"] = warning;
            }
            return true;
        });

        // ----- speakers.remove_embedding -----
        server.on("speakers.remove_embedding",
                  [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            std::string name;
            int64_t index = -1;
            {
                auto it = req.params.find("name");
                if (it != req.params.end()) name = json_val_as_string(it->second);
                auto it2 = req.params.find("index");
                if (it2 != req.params.end()) index = json_val_as_int(it2->second, -1);
            }
            if (!t_is_safe_dirname(name)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "invalid name";
                return false;
            }
            if (index < 0) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "invalid index";
                return false;
            }
            auto profiles = load_speaker_db(db_dir);
            SpeakerProfile* found = nullptr;
            for (auto& p : profiles) {
                if (p.name == name) { found = &p; break; }
            }
            if (!found) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "not_found";
                return false;
            }
            if (static_cast<std::size_t>(index) >= found->embeddings.size()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "index out of range";
                return false;
            }
            found->embeddings.erase(found->embeddings.begin()
                                    + static_cast<std::ptrdiff_t>(index));
            if (found->embeddings.empty()) {
                remove_speaker(db_dir, name);
                resp.result["ok"] = true;
                resp.result["remaining"] = static_cast<int64_t>(0);
            } else {
                found->updated = iso_now();
                save_speaker(db_dir, *found);
                resp.result["ok"] = true;
                resp.result["remaining"] =
                    static_cast<int64_t>(found->embeddings.size());
            }
            return true;
        });

        // ----- speakers.relabel -----
        server.on("speakers.relabel",
                  [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            std::string meeting_id;
            std::string new_label;
            int64_t cluster_id = -1;
            bool update_profile = true;
            {
                auto it = req.params.find("meeting_id");
                if (it != req.params.end())
                    meeting_id = json_val_as_string(it->second);
                auto it2 = req.params.find("cluster_id");
                if (it2 != req.params.end())
                    cluster_id = json_val_as_int(it2->second, -1);
                auto it3 = req.params.find("new_label");
                if (it3 != req.params.end())
                    new_label = json_val_as_string(it3->second);
                auto it4 = req.params.find("update_profile");
                if (it4 != req.params.end())
                    update_profile = json_val_as_bool(it4->second, true);
            }
            if (meeting_id.empty() || !is_valid_meeting_id(meeting_id)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "invalid meeting_id";
                return false;
            }
            if (cluster_id < 0) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "invalid cluster_id";
                return false;
            }
            if (new_label.empty() || !t_is_safe_dirname(new_label)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "invalid new_label";
                return false;
            }
            auto hit = meeting_index.find(meeting_id);
            if (!hit.has_value()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "unknown meeting_id";
                return false;
            }
            const fs::path meeting_path = *hit;
            auto meeting_speakers = load_meeting_speakers(meeting_path);
            MeetingSpeaker* spk = nullptr;
            for (auto& s : meeting_speakers) {
                if (s.cluster_id == static_cast<int>(cluster_id)) { spk = &s; break; }
            }
            if (!spk) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "cluster_id not found";
                return false;
            }
            const std::string old_label = spk->label;
            if (update_profile && !spk->embedding.empty()) {
                if (spk->identified && !old_label.empty())
                    remove_embedding(db_dir, old_label, spk->embedding);
                auto profiles = load_speaker_db(db_dir);
                SpeakerProfile profile;
                for (const auto& p : profiles) {
                    if (p.name == new_label) { profile = p; break; }
                }
                if (profile.name.empty()) {
                    profile.name = new_label;
                    profile.created = iso_now();
                }
                profile.updated = iso_now();
                profile.embeddings.push_back(spk->embedding);
                save_speaker(db_dir, profile);
            }
            spk->label = new_label;
            spk->identified = true;
            spk->confidence = 1.0f;
            save_meeting_speakers(meeting_path, meeting_speakers,
                                  derive_meeting_timestamp(meeting_path));
            resp.result["ok"] = true;
            resp.result["old_label"] = old_label;
            return true;
        });

        // ----- speakers.batch_reidentify -----
        // We don't run the actual sherpa re_identify_meeting here — the
        // [e6] suite must be runnable in the default (non-[full-stack])
        // unit pass which does not load sherpa models. Mirror the
        // production handler's "immediate-return + atomic flag + detached
        // worker" structure; the worker body itself is a no-op for the
        // test (just clears the flag after a tiny sleep so the second
        // call reliably observes the in-progress state).
        server.on("speakers.batch_reidentify",
                  [](const IpcRequest&, IpcResponse& resp, IpcError& err) {
            bool expected = false;
            if (!g_test_batch_running.compare_exchange_strong(expected, true)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "speakers.batch_reidentify: batch in progress";
                return false;
            }
            std::thread worker([]() {
                // Keep the flag held long enough for the in-progress-reject
                // assertion to fire reliably (caller polls within ~50 ms).
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                g_test_batch_running.store(false);
            });
            worker.detach();
            resp.result["ok"] = true;
            resp.result["async"] = true;
            resp.result["started_at"] = iso_now();
            return true;
        });

        // ----- meetings.list -----
        server.on("meetings.list",
                  [this](const IpcRequest&, IpcResponse& resp, IpcError& err) {
            try {
                auto meetings = discover_meetings(meetings_root);
                std::string arr = "[";
                for (std::size_t i = 0; i < meetings.size(); ++i) {
                    if (i > 0) arr += ",";
                    const auto& m = meetings[i];
                    arr += "{\"name\":\"" + json_escape(m.name) + "\"";
                    if (m.meeting_id.has_value())
                        arr += ",\"meeting_id\":\"" + json_escape(*m.meeting_id) + "\"";
                    else
                        arr += ",\"meeting_id\":null";
                    arr += ",\"has_audio\":";
                    arr += (m.has_audio ? "true" : "false");
                    arr += ",\"has_speakers\":";
                    arr += (m.has_speakers ? "true" : "false");
                    arr += ",\"has_summary\":";
                    arr += (m.has_summary ? "true" : "false");
                    arr += ",\"mtime_iso\":\"" + json_escape(m.mtime_iso) + "\"}";
                }
                arr += "]";
                resp.result["meetings"] = arr;
                resp.result["count"] = static_cast<int64_t>(meetings.size());
                return true;
            } catch (const std::exception& e) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = e.what();
                return false;
            }
        });

        // ----- meetings.speakers -----
        server.on("meetings.speakers",
                  [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            std::string meeting_id;
            {
                auto it = req.params.find("meeting_id");
                if (it != req.params.end())
                    meeting_id = json_val_as_string(it->second);
            }
            if (meeting_id.empty() || !is_valid_meeting_id(meeting_id)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "invalid meeting_id";
                return false;
            }
            auto hit = meeting_index.find(meeting_id);
            if (!hit.has_value()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "unknown meeting_id";
                return false;
            }
            const fs::path meeting_path = *hit;
            auto speakers = load_meeting_speakers(meeting_path);
            std::string arr = "[";
            for (std::size_t i = 0; i < speakers.size(); ++i) {
                if (i > 0) arr += ",";
                const auto& s = speakers[i];
                arr += "{\"cluster_id\":" + std::to_string(s.cluster_id)
                    + ",\"label\":\"" + json_escape(s.label) + "\""
                    + ",\"identified\":" + (s.identified ? "true" : "false")
                    + ",\"confidence\":" + std::to_string(s.confidence)
                    + ",\"duration_sec\":" + std::to_string(s.duration_sec)
                    + "}";
            }
            arr += "]";
            resp.result["speakers"] = arr;
            resp.result["count"] = static_cast<int64_t>(speakers.size());
            return true;
        });

        // ----- meetings.read_note -----
        server.on("meetings.read_note",
                  [this](const IpcRequest& req, IpcResponse& resp, IpcError& err) {
            std::string meeting_id;
            {
                auto it = req.params.find("meeting_id");
                if (it != req.params.end())
                    meeting_id = json_val_as_string(it->second);
            }
            if (meeting_id.empty() || !is_valid_meeting_id(meeting_id)) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "invalid meeting_id";
                return false;
            }
            auto hit = meeting_index.find(meeting_id);
            if (!hit.has_value()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "unknown meeting_id";
                return false;
            }
            const fs::path meeting_path = *hit;
            fs::path note_path;
            std::error_code ec;
            if (fs::is_directory(meeting_path, ec)) {
                for (const auto& entry : fs::directory_iterator(meeting_path, ec)) {
                    if (ec) break;
                    if (!entry.is_regular_file()) continue;
                    const std::string fname = entry.path().filename().string();
                    static constexpr const char* PREFIX = "Meeting_";
                    static constexpr const char* SUFFIX = ".md";
                    const std::size_t pre_n = std::strlen(PREFIX);
                    const std::size_t suf_n = std::strlen(SUFFIX);
                    if (fname.size() < pre_n + suf_n) continue;
                    if (fname.compare(0, pre_n, PREFIX) != 0) continue;
                    if (fname.compare(fname.size() - suf_n, suf_n, SUFFIX) != 0) continue;
                    note_path = entry.path();
                    break;
                }
            }
            if (note_path.empty()) {
                err.code = static_cast<int>(IpcErrorCode::InvalidParams);
                err.message = "not_found";
                return false;
            }
            std::ifstream in(note_path, std::ios::binary);
            if (!in) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "cannot open";
                return false;
            }
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            resp.result["path"] = note_path.filename().string();
            resp.result["content"] = content;
            return true;
        });
    }
};

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
    // Drain the worker so subsequent fixtures start clean (the detached
    // worker sleeps ~200 ms then clears the flag).
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

TEST_CASE("speakers.batch_reidentify rejects overlapping calls with InvalidParams",
          "[e6][ipc][speakers-meetings]") {
    Fixture fx("batch_busy");
    auto client = fx.make_client();
    IpcResponse resp1;
    IpcError err1;
    REQUIRE(client->call("speakers.batch_reidentify", resp1, err1));
    CHECK(json_val_as_bool(resp1.result["async"]) == true);

    // Second call while the worker is still running (sleeps 200 ms) must
    // be rejected with InvalidParams "batch in progress".
    IpcResponse resp2;
    IpcError err2;
    CHECK_FALSE(client->call("speakers.batch_reidentify", resp2, err2));
    CHECK(err2.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(contains(err2.message, "batch in progress"));

    // Let the worker finish so the global flag clears for the next test.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
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
