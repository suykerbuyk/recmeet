// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "config.h"
#include "speaker_id.h"
#include "util.h"

#include <httplib.h>

#include <fstream>
#include <thread>

using namespace recmeet;

// ---------------------------------------------------------------------------
// Test server helper — starts recmeet-web on a random port in a thread
// ---------------------------------------------------------------------------

namespace {

struct TestServer {
    httplib::Server server;
    std::thread thread;
    int port = 0;
    fs::path speaker_db_dir;
    fs::path output_dir;
    fs::path web_root;
    std::mutex speaker_mu;

    void setup_routes() {
        server.set_default_headers({
            {"Access-Control-Allow-Origin", "*"},
        });

        server.Get("/api/speakers", [this](const httplib::Request&, httplib::Response& res) {
            auto profiles = load_speaker_db(speaker_db_dir);
            std::string out = "[";
            for (size_t i = 0; i < profiles.size(); ++i) {
                if (i > 0) out += ",";
                const auto& p = profiles[i];
                out += "{\"name\":\"" + p.name + "\",\"enrollments\":" +
                       std::to_string(p.embeddings.size()) +
                       ",\"created\":\"" + p.created +
                       "\",\"updated\":\"" + p.updated + "\"}";
            }
            out += "]";
            res.set_content(out, "application/json");
        });

        server.Get(R"(/api/speakers/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            auto name = req.matches[1].str();
            auto profiles = load_speaker_db(speaker_db_dir);
            for (const auto& p : profiles) {
                if (p.name == name) {
                    std::string out = "{\"name\":\"" + p.name +
                        "\",\"enrollments\":" + std::to_string(p.embeddings.size()) +
                        ",\"created\":\"" + p.created +
                        "\",\"updated\":\"" + p.updated +
                        "\",\"embedding_dim\":" +
                        std::to_string(p.embeddings.empty() ? 0 : p.embeddings[0].size()) + "}";
                    res.set_content(out, "application/json");
                    return;
                }
            }
            res.status = 404;
            res.set_content(R"({"error":"speaker not found"})", "application/json");
        });

        server.Delete(R"(/api/speakers/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
            auto name = req.matches[1].str();
            std::lock_guard<std::mutex> lock(speaker_mu);
            if (remove_speaker(speaker_db_dir, name)) {
                res.set_content(R"({"ok":true})", "application/json");
            } else {
                res.status = 404;
                res.set_content(R"({"error":"speaker not found"})", "application/json");
            }
        });

        server.Post("/api/speakers/reset", [this](const httplib::Request&, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(speaker_mu);
            int removed = reset_speakers(speaker_db_dir);
            res.set_content(R"({"ok":true,"removed":)" + std::to_string(removed) + "}", "application/json");
        });

        server.Post("/api/speakers/enroll", [this](const httplib::Request& req, httplib::Response& res) {
            // Minimal JSON parsing
            auto get_str = [&](const std::string& key) -> std::string {
                std::string needle = "\"" + key + "\":\"";
                auto pos = req.body.find(needle);
                if (pos == std::string::npos) {
                    needle = "\"" + key + "\": \"";
                    pos = req.body.find(needle);
                    if (pos == std::string::npos) return "";
                }
                pos += needle.size();
                auto end = req.body.find('"', pos);
                return end == std::string::npos ? "" : req.body.substr(pos, end - pos);
            };
            auto get_int = [&](const std::string& key) -> int {
                std::string needle = "\"" + key + "\":";
                auto pos = req.body.find(needle);
                if (pos == std::string::npos) {
                    needle = "\"" + key + "\": ";
                    pos = req.body.find(needle);
                    if (pos == std::string::npos) return -1;
                }
                pos += needle.size();
                while (pos < req.body.size() && req.body[pos] == ' ') ++pos;
                return std::atoi(req.body.c_str() + pos);
            };

            auto name = get_str("name");
            auto meeting_dir_name = get_str("meeting_dir");
            int cluster_id = get_int("cluster_id");

            if (name.empty() || meeting_dir_name.empty() || cluster_id < 0) {
                res.status = 400;
                res.set_content(R"({"error":"missing required fields"})", "application/json");
                return;
            }

            // Validate no path traversal
            if (meeting_dir_name.find('/') != std::string::npos ||
                meeting_dir_name.find("..") != std::string::npos) {
                res.status = 400;
                res.set_content(R"({"error":"invalid meeting_dir"})", "application/json");
                return;
            }

            auto meeting_path = output_dir / meeting_dir_name;
            if (!fs::is_directory(meeting_path)) {
                res.status = 404;
                res.set_content(R"({"error":"meeting directory not found"})", "application/json");
                return;
            }

#if RECMEET_USE_SHERPA
            auto meeting_speakers = load_meeting_speakers(meeting_path);
            if (meeting_speakers.empty()) {
                res.status = 404;
                res.set_content(R"({"error":"no speakers.json in meeting directory"})", "application/json");
                return;
            }

            const MeetingSpeaker* found = nullptr;
            for (const auto& s : meeting_speakers) {
                if (s.cluster_id == cluster_id) {
                    found = &s;
                    break;
                }
            }
            if (!found || found->embedding.empty()) {
                res.status = 404;
                res.set_content(R"({"error":"cluster_id not found or no embedding"})", "application/json");
                return;
            }

            std::lock_guard<std::mutex> lock(speaker_mu);

            SpeakerProfile profile;
            auto profiles = load_speaker_db(speaker_db_dir);
            for (const auto& p : profiles) {
                if (p.name == name) { profile = p; break; }
            }
            if (profile.name.empty()) {
                profile.name = name;
                profile.created = iso_now();
            }
            profile.updated = iso_now();
            profile.embeddings.push_back(found->embedding);
            save_speaker(speaker_db_dir, profile);

            res.set_content(R"({"ok":true})", "application/json");
#else
            res.status = 501;
            res.set_content(R"({"error":"enrollment requires sherpa-onnx"})", "application/json");
#endif
        });

        server.Get("/api/meetings", [this](const httplib::Request&, httplib::Response& res) {
            std::string out = "[";
            bool first = true;
            if (fs::is_directory(output_dir)) {
                // Collect and sort
                std::vector<std::string> dirs;
                for (const auto& entry : fs::directory_iterator(output_dir)) {
                    if (!entry.is_directory()) continue;
                    if (find_audio_file(entry.path()).empty()) continue;
                    dirs.push_back(entry.path().filename().string());
                }
                std::sort(dirs.begin(), dirs.end(), std::greater<>());
                for (const auto& d : dirs) {
                    if (!first) out += ",";
                    first = false;
                    auto p = output_dir / d;
                    bool has_spk = fs::exists(p / "speakers.json");
                    int spk_count = 0;
#if RECMEET_USE_SHERPA
                    if (has_spk) spk_count = static_cast<int>(load_meeting_speakers(p).size());
#endif
                    std::string date = (d.size() >= 10 && d[4] == '-' && d[7] == '-') ? d.substr(0, 10) : "";
                    out += "{\"name\":\"" + d + "\",\"has_speakers_json\":" +
                           (has_spk ? "true" : "false") +
                           ",\"speaker_count\":" + std::to_string(spk_count) +
                           ",\"date\":\"" + date + "\"}";
                }
            }
            out += "]";
            res.set_content(out, "application/json");
        });

        server.Get(R"(/api/meetings/([^/]+)/speakers)", [this](const httplib::Request& req, httplib::Response& res) {
            auto dir_name = req.matches[1].str();
            auto meeting_path = output_dir / dir_name;
            if (!fs::is_directory(meeting_path)) {
                res.status = 404;
                res.set_content(R"({"error":"meeting directory not found"})", "application/json");
                return;
            }
#if RECMEET_USE_SHERPA
            auto speakers = load_meeting_speakers(meeting_path);
            std::string out = "[";
            for (size_t i = 0; i < speakers.size(); ++i) {
                if (i > 0) out += ",";
                const auto& s = speakers[i];
                out += "{\"cluster_id\":" + std::to_string(s.cluster_id) +
                       ",\"label\":\"" + s.label +
                       "\",\"identified\":" + (s.identified ? "true" : "false") +
                       ",\"duration_sec\":" + std::to_string(s.duration_sec) +
                       ",\"confidence\":" + std::to_string(s.confidence) + "}";
            }
            out += "]";
            res.set_content(out, "application/json");
#else
            res.set_content("[]", "application/json");
#endif
        });

        // Static file serving
        if (!web_root.empty() && fs::is_directory(web_root)) {
            server.set_mount_point("/", web_root.string());
        }
    }

    void start() {
        setup_routes();
        // Bind to port 0 for random assignment
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this]() { server.listen_after_bind(); });
        // Wait for server to be ready
        server.wait_until_ready();
    }

    void stop() {
        server.stop();
        if (thread.joinable()) thread.join();
    }

    ~TestServer() { stop(); }

    httplib::Client client() {
        return httplib::Client("127.0.0.1", port);
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("web: GET /api/speakers returns empty array when no speakers", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_speakers_empty";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");
    fs::create_directories(tmp / "meetings");

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Get("/api/speakers");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body == "[]");

    fs::remove_all(tmp);
}

TEST_CASE("web: GET /api/speakers returns speaker list after enrollment", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_speakers_list";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");
    fs::create_directories(tmp / "meetings");

    SpeakerProfile p;
    p.name = "Alice";
    p.created = "2026-01-01T00:00:00Z";
    p.updated = "2026-03-08T12:00:00Z";
    p.embeddings = {{0.1f, 0.2f, -0.3f}};
    save_speaker(tmp / "speakers", p);

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Get("/api/speakers");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body.find("\"Alice\"") != std::string::npos);
    CHECK(res->body.find("\"enrollments\":1") != std::string::npos);

    fs::remove_all(tmp);
}

TEST_CASE("web: GET /api/speakers/:name returns 404 for unknown", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_speakers_404";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Get("/api/speakers/Nobody");
    REQUIRE(res);
    CHECK(res->status == 404);

    fs::remove_all(tmp);
}

TEST_CASE("web: GET /api/speakers/:name returns detail for known speaker", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_speakers_detail";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");

    SpeakerProfile p;
    p.name = "Bob";
    p.created = "2026-01-01T00:00:00Z";
    p.updated = "2026-03-08T12:00:00Z";
    p.embeddings = {{0.1f, 0.2f}};
    save_speaker(tmp / "speakers", p);

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Get("/api/speakers/Bob");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body.find("\"Bob\"") != std::string::npos);
    CHECK(res->body.find("\"embedding_dim\":2") != std::string::npos);

    fs::remove_all(tmp);
}

TEST_CASE("web: DELETE /api/speakers/:name removes speaker", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_speakers_delete";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");

    SpeakerProfile p;
    p.name = "Carol";
    p.created = p.updated = "2026-01-01T00:00:00Z";
    p.embeddings = {{1.0f}};
    save_speaker(tmp / "speakers", p);

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Delete("/api/speakers/Carol");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body.find("\"ok\":true") != std::string::npos);

    // Verify gone
    auto res2 = cli.Get("/api/speakers");
    REQUIRE(res2);
    CHECK(res2->body == "[]");

    fs::remove_all(tmp);
}

TEST_CASE("web: DELETE /api/speakers/:name returns 404 for unknown", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_speakers_del404";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Delete("/api/speakers/Nobody");
    REQUIRE(res);
    CHECK(res->status == 404);

    fs::remove_all(tmp);
}

TEST_CASE("web: POST /api/speakers/reset removes all speakers", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_speakers_reset";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");

    SpeakerProfile p1, p2;
    p1.name = "Alice";
    p1.created = p1.updated = "2026-01-01T00:00:00Z";
    p1.embeddings = {{1.0f}};
    p2.name = "Bob";
    p2.created = p2.updated = "2026-01-01T00:00:00Z";
    p2.embeddings = {{2.0f}};
    save_speaker(tmp / "speakers", p1);
    save_speaker(tmp / "speakers", p2);

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Post("/api/speakers/reset", "", "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body.find("\"ok\":true") != std::string::npos);
    CHECK(res->body.find("\"removed\":2") != std::string::npos);

    // Verify empty
    auto res2 = cli.Get("/api/speakers");
    REQUIRE(res2);
    CHECK(res2->body == "[]");

    fs::remove_all(tmp);
}

TEST_CASE("web: POST /api/speakers/enroll enrolls from meeting speakers.json", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_enroll";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");
    fs::create_directories(tmp / "meetings" / "2026-03-08_14-30");

    // Create a fake audio.wav
    std::ofstream(tmp / "meetings" / "2026-03-08_14-30" / "audio.wav") << "RIFF";

#if RECMEET_USE_SHERPA
    // Create speakers.json with pre-computed embeddings
    std::vector<MeetingSpeaker> mtg_speakers = {
        {0, "Speaker_01", false, {0.5f, 0.6f, 0.7f}, 30.0f, 0.0f},
        {1, "Speaker_02", false, {0.8f, 0.9f, 1.0f}, 20.0f, 0.0f},
    };
    save_meeting_speakers(tmp / "meetings" / "2026-03-08_14-30", mtg_speakers);

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Post("/api/speakers/enroll",
        R"({"name":"John","meeting_dir":"2026-03-08_14-30","cluster_id":0})",
        "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body.find("\"ok\":true") != std::string::npos);

    // Verify enrolled
    auto profiles = load_speaker_db(tmp / "speakers");
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].name == "John");
    CHECK(profiles[0].embeddings.size() == 1);
    CHECK(profiles[0].embeddings[0].size() == 3);
#endif

    fs::remove_all(tmp);
}

TEST_CASE("web: POST /api/speakers/enroll returns 404 for invalid meeting dir", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_enroll_404";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");
    fs::create_directories(tmp / "meetings");

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Post("/api/speakers/enroll",
        R"({"name":"John","meeting_dir":"nonexistent","cluster_id":0})",
        "application/json");
    REQUIRE(res);
    CHECK(res->status == 404);

    fs::remove_all(tmp);
}

TEST_CASE("web: GET /api/meetings lists meeting directories", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_meetings";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");
    fs::create_directories(tmp / "meetings" / "2026-03-07_10-00");
    fs::create_directories(tmp / "meetings" / "2026-03-08_14-30");

    // Create audio.wav in each
    std::ofstream(tmp / "meetings" / "2026-03-07_10-00" / "audio.wav") << "RIFF";
    std::ofstream(tmp / "meetings" / "2026-03-08_14-30" / "audio.wav") << "RIFF";

    // Create a dir without audio.wav (should be excluded)
    fs::create_directories(tmp / "meetings" / "not-a-meeting");

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Get("/api/meetings");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body.find("2026-03-08_14-30") != std::string::npos);
    CHECK(res->body.find("2026-03-07_10-00") != std::string::npos);
    CHECK(res->body.find("not-a-meeting") == std::string::npos);

    // Most recent should come first
    auto pos1 = res->body.find("2026-03-08_14-30");
    auto pos2 = res->body.find("2026-03-07_10-00");
    CHECK(pos1 < pos2);

    fs::remove_all(tmp);
}

TEST_CASE("web: GET /api/meetings/:dir/speakers returns meeting speaker data", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_meeting_speakers";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");
    fs::create_directories(tmp / "meetings" / "2026-03-08_14-30");
    std::ofstream(tmp / "meetings" / "2026-03-08_14-30" / "audio.wav") << "RIFF";

#if RECMEET_USE_SHERPA
    std::vector<MeetingSpeaker> mtg_speakers = {
        {0, "Alice", true, {0.1f, 0.2f}, 30.0f, 0.92f},
    };
    save_meeting_speakers(tmp / "meetings" / "2026-03-08_14-30", mtg_speakers);
#endif

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Get("/api/meetings/2026-03-08_14-30/speakers");
    REQUIRE(res);
    CHECK(res->status == 200);

#if RECMEET_USE_SHERPA
    CHECK(res->body.find("\"Alice\"") != std::string::npos);
    CHECK(res->body.find("\"cluster_id\":0") != std::string::npos);
#else
    CHECK(res->body == "[]");
#endif

    fs::remove_all(tmp);
}

TEST_CASE("web: static file serving returns index.html", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_static";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");
    fs::create_directories(tmp / "meetings");
    fs::create_directories(tmp / "webroot");
    std::ofstream(tmp / "webroot" / "index.html") << "<html><body>Hello</body></html>";

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.web_root = tmp / "webroot";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Get("/index.html");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body.find("Hello") != std::string::npos);

    fs::remove_all(tmp);
}

TEST_CASE("web: GET /api/meetings discovers new-format audio files", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_meetings_newformat";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");

    // Meeting with new-format audio (no legacy audio.wav)
    fs::create_directories(tmp / "meetings" / "2026-03-09_10-00");
    std::ofstream(tmp / "meetings" / "2026-03-09_10-00" / "audio_2026-03-09_10-00.wav") << "RIFF";

    // Meeting with legacy audio.wav
    fs::create_directories(tmp / "meetings" / "2026-03-08_14-30");
    std::ofstream(tmp / "meetings" / "2026-03-08_14-30" / "audio.wav") << "RIFF";

    // Meeting with no audio (should be excluded)
    fs::create_directories(tmp / "meetings" / "2026-03-07_09-00");

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Get("/api/meetings");
    REQUIRE(res);
    CHECK(res->status == 200);

    // Both meeting formats should appear
    CHECK(res->body.find("2026-03-09_10-00") != std::string::npos);
    CHECK(res->body.find("2026-03-08_14-30") != std::string::npos);
    // No-audio meeting excluded
    CHECK(res->body.find("2026-03-07_09-00") == std::string::npos);

    fs::remove_all(tmp);
}

TEST_CASE("web: GET /api/meetings discovers meeting with both audio formats", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_meetings_both";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");

    // Meeting with both new and legacy format
    fs::create_directories(tmp / "meetings" / "2026-03-09_15-00");
    std::ofstream(tmp / "meetings" / "2026-03-09_15-00" / "audio_2026-03-09_15-00.wav") << "RIFF";
    std::ofstream(tmp / "meetings" / "2026-03-09_15-00" / "audio.wav") << "RIFF";

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Get("/api/meetings");
    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body.find("2026-03-09_15-00") != std::string::npos);

    fs::remove_all(tmp);
}

TEST_CASE("web: GET /nonexistent-api returns 404", "[web]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_web_404";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "speakers");
    fs::create_directories(tmp / "meetings");

    TestServer srv;
    srv.speaker_db_dir = tmp / "speakers";
    srv.output_dir = tmp / "meetings";
    srv.start();

    auto cli = srv.client();
    auto res = cli.Get("/api/nonexistent");
    REQUIRE(res);
    CHECK(res->status == 404);

    fs::remove_all(tmp);
}
