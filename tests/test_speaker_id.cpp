// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "speaker_id.h"

#include <fstream>

using namespace recmeet;

// ---------------------------------------------------------------------------
// Database persistence tests
// ---------------------------------------------------------------------------

TEST_CASE("speaker_id: default_speaker_db_dir is under data_dir", "[speaker_id]") {
    auto dir = default_speaker_db_dir();
    CHECK(dir.string().find("speakers") != std::string::npos);
}

TEST_CASE("speaker_id: load empty directory returns empty", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_empty";
    fs::create_directories(tmp);
    auto db = load_speaker_db(tmp);
    CHECK(db.empty());
    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: load non-existent directory returns empty", "[speaker_id]") {
    auto db = load_speaker_db("/tmp/recmeet_no_such_dir_xyz");
    CHECK(db.empty());
}

TEST_CASE("speaker_id: save and load round-trip", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_roundtrip";
    fs::remove_all(tmp);

    SpeakerProfile p;
    p.name = "Alice";
    p.created = "2026-01-01T00:00:00Z";
    p.updated = "2026-03-08T12:00:00Z";
    p.embeddings = {{0.1f, 0.2f, -0.3f}, {0.4f, 0.5f, -0.6f}};

    save_speaker(tmp, p);

    CHECK(fs::exists(tmp / "Alice.json"));

    auto db = load_speaker_db(tmp);
    REQUIRE(db.size() == 1);
    CHECK(db[0].name == "Alice");
    CHECK(db[0].created == "2026-01-01T00:00:00Z");
    CHECK(db[0].updated == "2026-03-08T12:00:00Z");
    REQUIRE(db[0].embeddings.size() == 2);
    REQUIRE(db[0].embeddings[0].size() == 3);
    CHECK(db[0].embeddings[0][0] == Catch::Approx(0.1f));
    CHECK(db[0].embeddings[0][1] == Catch::Approx(0.2f));
    CHECK(db[0].embeddings[0][2] == Catch::Approx(-0.3f));
    REQUIRE(db[0].embeddings[1].size() == 3);
    CHECK(db[0].embeddings[1][0] == Catch::Approx(0.4f));

    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: save multiple speakers", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_multi";
    fs::remove_all(tmp);

    SpeakerProfile p1;
    p1.name = "Bob";
    p1.created = p1.updated = "2026-01-01T00:00:00Z";
    p1.embeddings = {{1.0f, 2.0f}};

    SpeakerProfile p2;
    p2.name = "Carol";
    p2.created = p2.updated = "2026-02-01T00:00:00Z";
    p2.embeddings = {{3.0f, 4.0f}};

    save_speaker(tmp, p1);
    save_speaker(tmp, p2);

    auto db = load_speaker_db(tmp);
    CHECK(db.size() == 2);

    // Sorted by directory iteration order — check both names present
    bool has_bob = false, has_carol = false;
    for (const auto& p : db) {
        if (p.name == "Bob") has_bob = true;
        if (p.name == "Carol") has_carol = true;
    }
    CHECK(has_bob);
    CHECK(has_carol);

    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: remove speaker", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_remove";
    fs::remove_all(tmp);

    SpeakerProfile p;
    p.name = "Dave";
    p.created = p.updated = "2026-01-01T00:00:00Z";
    p.embeddings = {{1.0f}};
    save_speaker(tmp, p);

    CHECK(remove_speaker(tmp, "Dave") == true);
    CHECK(remove_speaker(tmp, "Dave") == false);  // already removed
    CHECK(load_speaker_db(tmp).empty());

    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: list_speakers returns sorted names", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_list";
    fs::remove_all(tmp);

    SpeakerProfile p1, p2;
    p1.name = "Zara";
    p1.created = p1.updated = "2026-01-01T00:00:00Z";
    p1.embeddings = {{1.0f}};
    p2.name = "Anna";
    p2.created = p2.updated = "2026-01-01T00:00:00Z";
    p2.embeddings = {{2.0f}};

    save_speaker(tmp, p1);
    save_speaker(tmp, p2);

    auto names = list_speakers(tmp);
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "Anna");
    CHECK(names[1] == "Zara");

    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: overwrite existing profile", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_overwrite";
    fs::remove_all(tmp);

    SpeakerProfile p;
    p.name = "Eve";
    p.created = p.updated = "2026-01-01T00:00:00Z";
    p.embeddings = {{1.0f, 2.0f}};
    save_speaker(tmp, p);

    // Add another embedding
    p.embeddings.push_back({3.0f, 4.0f});
    p.updated = "2026-03-08T00:00:00Z";
    save_speaker(tmp, p);

    auto db = load_speaker_db(tmp);
    REQUIRE(db.size() == 1);
    CHECK(db[0].name == "Eve");
    CHECK(db[0].embeddings.size() == 2);

    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: iso_now returns valid format", "[speaker_id]") {
    auto now = iso_now();
    // Should be like "2026-03-08T12:00:00Z"
    CHECK(now.size() == 20);
    CHECK(now[4] == '-');
    CHECK(now[7] == '-');
    CHECK(now[10] == 'T');
    CHECK(now[19] == 'Z');
}

TEST_CASE("speaker_id: load ignores non-json files", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_nonjson";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // Create a non-json file
    std::ofstream(tmp / "notes.txt") << "not a speaker profile";

    // Create a valid json profile
    SpeakerProfile p;
    p.name = "Frank";
    p.created = p.updated = "2026-01-01T00:00:00Z";
    p.embeddings = {{1.0f}};
    save_speaker(tmp, p);

    auto db = load_speaker_db(tmp);
    REQUIRE(db.size() == 1);
    CHECK(db[0].name == "Frank");

    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: load skips malformed json", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_malformed";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // Write malformed json
    std::ofstream(tmp / "bad.json") << "{ this is not valid json }";

    auto db = load_speaker_db(tmp);
    CHECK(db.empty());

    fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// merge_speakers with name map tests
// ---------------------------------------------------------------------------

TEST_CASE("merge_speakers: with speaker_names map uses enrolled names", "[diarize][speaker_id]") {
    std::vector<TranscriptSegment> transcript = {
        {0.0, 4.0, "Hello"},
        {5.0, 9.0, "Hi there"},
    };
    DiarizeResult diar;
    diar.num_speakers = 2;
    diar.segments = {
        {0.0, 5.0, 0},
        {5.0, 10.0, 1},
    };

    std::map<int, std::string> names = {{0, "John"}, {1, "Alice"}};
    auto result = merge_speakers(transcript, diar, names);
    REQUIRE(result.size() == 2);
    CHECK(result[0].text == "John: Hello");
    CHECK(result[1].text == "Alice: Hi there");
}

TEST_CASE("merge_speakers: partial name map falls back to Speaker_XX", "[diarize][speaker_id]") {
    std::vector<TranscriptSegment> transcript = {
        {0.0, 4.0, "Hello"},
        {5.0, 9.0, "Hi there"},
    };
    DiarizeResult diar;
    diar.num_speakers = 2;
    diar.segments = {
        {0.0, 5.0, 0},
        {5.0, 10.0, 1},
    };

    // Only speaker 0 is enrolled
    std::map<int, std::string> names = {{0, "John"}};
    auto result = merge_speakers(transcript, diar, names);
    REQUIRE(result.size() == 2);
    CHECK(result[0].text == "John: Hello");
    CHECK(result[1].text == "Speaker_02: Hi there");
}

TEST_CASE("merge_speakers: empty name map preserves original behavior", "[diarize][speaker_id]") {
    std::vector<TranscriptSegment> transcript = {
        {0.0, 4.0, "Hello"},
        {5.0, 9.0, "Hi there"},
    };
    DiarizeResult diar;
    diar.num_speakers = 2;
    diar.segments = {
        {0.0, 5.0, 0},
        {5.0, 10.0, 1},
    };

    auto result = merge_speakers(transcript, diar);
    REQUIRE(result.size() == 2);
    CHECK(result[0].text == "Speaker_01: Hello");
    CHECK(result[1].text == "Speaker_02: Hi there");
}
