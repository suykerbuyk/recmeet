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
// MeetingSpeaker save/load tests (speakers.json)
// ---------------------------------------------------------------------------

TEST_CASE("speaker_id: save/load meeting speakers round-trip", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_meeting_spk_rt";
    fs::remove_all(tmp);

    std::vector<MeetingSpeaker> speakers = {
        {0, "Alice", true, {0.1f, 0.2f, -0.3f}, 45.5f, 0.87f},
        {1, "Speaker_02", false, {0.4f, 0.5f, -0.6f}, 23.1f, 0.0f},
    };

    save_meeting_speakers(tmp, speakers);
    CHECK(fs::exists(tmp / "speakers.json"));

    auto loaded = load_meeting_speakers(tmp);
    REQUIRE(loaded.size() == 2);

    CHECK(loaded[0].cluster_id == 0);
    CHECK(loaded[0].label == "Alice");
    CHECK(loaded[0].identified == true);
    CHECK(loaded[0].duration_sec == Catch::Approx(45.5f));
    CHECK(loaded[0].confidence == Catch::Approx(0.87f));
    REQUIRE(loaded[0].embedding.size() == 3);
    CHECK(loaded[0].embedding[0] == Catch::Approx(0.1f));
    CHECK(loaded[0].embedding[2] == Catch::Approx(-0.3f));

    CHECK(loaded[1].cluster_id == 1);
    CHECK(loaded[1].label == "Speaker_02");
    CHECK(loaded[1].identified == false);
    CHECK(loaded[1].duration_sec == Catch::Approx(23.1f));
    CHECK(loaded[1].confidence == Catch::Approx(0.0f));
    REQUIRE(loaded[1].embedding.size() == 3);

    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: load meeting speakers from nonexistent dir returns empty", "[speaker_id]") {
    auto loaded = load_meeting_speakers("/tmp/recmeet_no_such_meeting_dir_xyz");
    CHECK(loaded.empty());
}

TEST_CASE("speaker_id: meeting speakers embedding data preserved exactly", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_meeting_spk_emb";
    fs::remove_all(tmp);

    // Use a longer embedding to test preservation
    std::vector<float> emb(192);
    for (int i = 0; i < 192; ++i) emb[i] = static_cast<float>(i) * 0.001f - 0.1f;

    std::vector<MeetingSpeaker> speakers = {
        {0, "Test", true, emb, 10.0f, 0.95f},
    };

    save_meeting_speakers(tmp, speakers);
    auto loaded = load_meeting_speakers(tmp);
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].embedding.size() == 192);
    for (int i = 0; i < 192; ++i)
        CHECK(loaded[0].embedding[i] == Catch::Approx(emb[i]).margin(1e-4));

    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: save meeting speakers to empty vector writes valid file", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_meeting_spk_empty";
    fs::remove_all(tmp);

    save_meeting_speakers(tmp, {});
    auto loaded = load_meeting_speakers(tmp);
    CHECK(loaded.empty());

    fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// reset_speakers tests
// ---------------------------------------------------------------------------

TEST_CASE("speaker_id: reset removes all profiles", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_reset";
    fs::remove_all(tmp);

    SpeakerProfile p1, p2;
    p1.name = "Alice";
    p1.created = p1.updated = "2026-01-01T00:00:00Z";
    p1.embeddings = {{1.0f}};
    p2.name = "Bob";
    p2.created = p2.updated = "2026-01-01T00:00:00Z";
    p2.embeddings = {{2.0f}};

    save_speaker(tmp, p1);
    save_speaker(tmp, p2);
    CHECK(load_speaker_db(tmp).size() == 2);

    int removed = reset_speakers(tmp);
    CHECK(removed == 2);
    CHECK(load_speaker_db(tmp).empty());

    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: reset on empty dir returns 0", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_reset_empty";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    CHECK(reset_speakers(tmp) == 0);

    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: reset on nonexistent dir returns 0", "[speaker_id]") {
    CHECK(reset_speakers("/tmp/recmeet_no_such_dir_reset_xyz") == 0);
}

TEST_CASE("speaker_id: reset preserves non-json files", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_reset_preserve";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // Create a non-json file
    std::ofstream(tmp / "notes.txt") << "keep me";

    SpeakerProfile p;
    p.name = "Eve";
    p.created = p.updated = "2026-01-01T00:00:00Z";
    p.embeddings = {{1.0f}};
    save_speaker(tmp, p);

    int removed = reset_speakers(tmp);
    CHECK(removed == 1);
    CHECK(fs::exists(tmp / "notes.txt"));
    CHECK(load_speaker_db(tmp).empty());

    fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// remove_embedding tests
// ---------------------------------------------------------------------------

TEST_CASE("speaker_id: remove_embedding exact match removes embedding", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_rm_emb_exact";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    SpeakerProfile p;
    p.name = "Alice";
    p.created = p.updated = "2026-01-01T00:00:00Z";
    p.embeddings = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};
    save_speaker(tmp, p);

    CHECK(remove_embedding(tmp, "Alice", {1.0f, 2.0f, 3.0f}));
    auto loaded = load_speaker_db(tmp);
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].embeddings.size() == 1);
    CHECK(loaded[0].embeddings[0] == std::vector<float>{4.0f, 5.0f, 6.0f});
    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: remove_embedding epsilon tolerance matches near-equal", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_rm_emb_eps";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    SpeakerProfile p;
    p.name = "Bob";
    p.created = p.updated = "2026-01-01T00:00:00Z";
    p.embeddings = {{1.0f, 2.0f, 3.0f}};
    save_speaker(tmp, p);

    // Slightly different values within epsilon tolerance
    CHECK(remove_embedding(tmp, "Bob", {1.0000001f, 2.0000001f, 3.0000001f}));
    CHECK(load_speaker_db(tmp).empty()); // profile deleted (last embedding)
    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: remove_embedding no match returns false", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_rm_emb_nomatch";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    SpeakerProfile p;
    p.name = "Charlie";
    p.created = p.updated = "2026-01-01T00:00:00Z";
    p.embeddings = {{1.0f, 2.0f, 3.0f}};
    save_speaker(tmp, p);

    CHECK_FALSE(remove_embedding(tmp, "Charlie", {9.0f, 9.0f, 9.0f}));
    CHECK(load_speaker_db(tmp).size() == 1); // unchanged
    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: remove_embedding last embedding deletes profile", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_rm_emb_last";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    SpeakerProfile p;
    p.name = "Dana";
    p.created = p.updated = "2026-01-01T00:00:00Z";
    p.embeddings = {{1.0f, 2.0f}};
    save_speaker(tmp, p);

    CHECK(remove_embedding(tmp, "Dana", {1.0f, 2.0f}));
    CHECK(load_speaker_db(tmp).empty());
    CHECK_FALSE(fs::exists(tmp / "Dana.json"));
    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: remove_embedding nonexistent speaker returns false", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_rm_emb_nospeaker";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    CHECK_FALSE(remove_embedding(tmp, "Nobody", {1.0f}));
    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: remove_embedding empty vector returns false", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_rm_emb_empty";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    SpeakerProfile p;
    p.name = "Eve";
    p.created = p.updated = "2026-01-01T00:00:00Z";
    p.embeddings = {{1.0f}};
    save_speaker(tmp, p);

    CHECK_FALSE(remove_embedding(tmp, "Eve", {}));
    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: remove_embedding preserves other embeddings", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_rm_emb_preserve";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    SpeakerProfile p;
    p.name = "Frank";
    p.created = p.updated = "2026-01-01T00:00:00Z";
    p.embeddings = {{1.0f}, {2.0f}, {3.0f}};
    save_speaker(tmp, p);

    CHECK(remove_embedding(tmp, "Frank", {2.0f}));
    auto loaded = load_speaker_db(tmp);
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].embeddings.size() == 2);
    CHECK(loaded[0].embeddings[0] == std::vector<float>{1.0f});
    CHECK(loaded[0].embeddings[1] == std::vector<float>{3.0f});
    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: remove_embedding dimension mismatch returns false", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_rm_emb_dim";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    SpeakerProfile p;
    p.name = "Grace";
    p.created = p.updated = "2026-01-01T00:00:00Z";
    p.embeddings = {{1.0f, 2.0f, 3.0f}};
    save_speaker(tmp, p);

    CHECK_FALSE(remove_embedding(tmp, "Grace", {1.0f, 2.0f})); // wrong dimension
    CHECK(load_speaker_db(tmp).size() == 1); // unchanged
    fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// relabel_meeting_speaker tests
// ---------------------------------------------------------------------------

TEST_CASE("speaker_id: relabel changes label and flags", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_relabel";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::vector<MeetingSpeaker> spks = {
        {0, "Speaker_01", false, {1.0f}, 10.0f, 0.0f},
        {1, "Speaker_02", false, {2.0f}, 20.0f, 0.0f},
    };
    save_meeting_speakers(tmp, spks);

    CHECK(relabel_meeting_speaker(tmp, 0, "John"));
    auto loaded = load_meeting_speakers(tmp);
    REQUIRE(loaded.size() == 2);
    CHECK(loaded[0].label == "John");
    CHECK(loaded[0].identified == true);
    CHECK(loaded[0].confidence == 1.0f);
    CHECK(loaded[1].label == "Speaker_02"); // unchanged
    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: relabel unknown cluster_id returns false", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_relabel_unknown";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::vector<MeetingSpeaker> spks = {
        {0, "Speaker_01", false, {1.0f}, 10.0f, 0.0f},
    };
    save_meeting_speakers(tmp, spks);

    CHECK_FALSE(relabel_meeting_speaker(tmp, 99, "John"));
    fs::remove_all(tmp);
}

TEST_CASE("speaker_id: relabel empty speakers returns false", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_relabel_empty";
    fs::remove_all(tmp);
    CHECK_FALSE(relabel_meeting_speaker(tmp, 0, "John"));
}

TEST_CASE("speaker_id: relabel custom confidence value", "[speaker_id]") {
    auto tmp = fs::temp_directory_path() / "recmeet_test_spk_relabel_conf";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::vector<MeetingSpeaker> spks = {
        {0, "Speaker_01", false, {1.0f}, 10.0f, 0.0f},
    };
    save_meeting_speakers(tmp, spks);

    CHECK(relabel_meeting_speaker(tmp, 0, "Bob", 0.95f));
    auto loaded = load_meeting_speakers(tmp);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].confidence == Catch::Approx(0.95f));
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
