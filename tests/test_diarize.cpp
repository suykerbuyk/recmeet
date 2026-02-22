#include <catch2/catch_test_macros.hpp>
#include "diarize.h"

using namespace recmeet;

TEST_CASE("format_speaker: 0 becomes Speaker_01", "[diarize]") {
    CHECK(format_speaker(0) == "Speaker_01");
}

TEST_CASE("format_speaker: 9 becomes Speaker_10", "[diarize]") {
    CHECK(format_speaker(9) == "Speaker_10");
}

TEST_CASE("format_speaker: single digit padding", "[diarize]") {
    CHECK(format_speaker(1) == "Speaker_02");
    CHECK(format_speaker(4) == "Speaker_05");
}

TEST_CASE("merge_speakers: empty transcript returns empty", "[diarize]") {
    DiarizeResult diar;
    diar.num_speakers = 2;
    diar.segments = {{0.0, 5.0, 0}, {5.0, 10.0, 1}};

    auto result = merge_speakers({}, diar);
    CHECK(result.empty());
}

TEST_CASE("merge_speakers: empty diarization assigns Speaker_01", "[diarize]") {
    std::vector<TranscriptSegment> transcript = {
        {0.0, 5.0, "Hello"},
        {5.0, 10.0, "World"},
    };
    DiarizeResult diar;

    auto result = merge_speakers(transcript, diar);
    REQUIRE(result.size() == 2);
    CHECK(result[0].text == "Speaker_01: Hello");
    CHECK(result[1].text == "Speaker_01: World");
}

TEST_CASE("merge_speakers: correct assignment by overlap", "[diarize]") {
    std::vector<TranscriptSegment> transcript = {
        {0.0, 4.0, "First segment"},
        {5.0, 9.0, "Second segment"},
    };
    DiarizeResult diar;
    diar.num_speakers = 2;
    diar.segments = {
        {0.0, 5.0, 0},   // Speaker 0 from 0-5s
        {5.0, 10.0, 1},  // Speaker 1 from 5-10s
    };

    auto result = merge_speakers(transcript, diar);
    REQUIRE(result.size() == 2);
    CHECK(result[0].text == "Speaker_01: First segment");
    CHECK(result[1].text == "Speaker_02: Second segment");
}

TEST_CASE("merge_speakers: boundary-straddling segment gets majority speaker", "[diarize]") {
    // Segment from 3.0 to 7.0 straddles the 5.0 boundary
    // Overlap with speaker 0 (0-5): min(7,5) - max(3,0) = 5-3 = 2.0s
    // Overlap with speaker 1 (5-10): min(7,10) - max(3,5) = 7-5 = 2.0s
    // Equal overlap — first match wins (speaker 0)
    std::vector<TranscriptSegment> transcript = {
        {3.0, 7.0, "Straddling segment"},
    };
    DiarizeResult diar;
    diar.num_speakers = 2;
    diar.segments = {
        {0.0, 5.0, 0},
        {5.0, 10.0, 1},
    };

    auto result = merge_speakers(transcript, diar);
    REQUIRE(result.size() == 1);
    // With equal overlap, first match wins → Speaker_01
    CHECK(result[0].text == "Speaker_01: Straddling segment");
}

TEST_CASE("merge_speakers: three speakers", "[diarize]") {
    std::vector<TranscriptSegment> transcript = {
        {0.0, 3.0, "A speaks"},
        {4.0, 7.0, "B speaks"},
        {8.0, 11.0, "C speaks"},
    };
    DiarizeResult diar;
    diar.num_speakers = 3;
    diar.segments = {
        {0.0, 4.0, 0},
        {4.0, 8.0, 1},
        {8.0, 12.0, 2},
    };

    auto result = merge_speakers(transcript, diar);
    REQUIRE(result.size() == 3);
    CHECK(result[0].text == "Speaker_01: A speaks");
    CHECK(result[1].text == "Speaker_02: B speaks");
    CHECK(result[2].text == "Speaker_03: C speaks");
}

TEST_CASE("merge_speakers: no overlap defaults to Speaker_01", "[diarize]") {
    std::vector<TranscriptSegment> transcript = {
        {20.0, 25.0, "Late segment"},
    };
    DiarizeResult diar;
    diar.num_speakers = 2;
    diar.segments = {
        {0.0, 5.0, 0},
        {5.0, 10.0, 1},
    };

    auto result = merge_speakers(transcript, diar);
    REQUIRE(result.size() == 1);
    // No overlap → best_overlap stays 0, best_speaker stays 0 → Speaker_01
    CHECK(result[0].text == "Speaker_01: Late segment");
}

TEST_CASE("merge_speakers: timestamps preserved through merge", "[diarize]") {
    std::vector<TranscriptSegment> transcript = {
        {1.5, 3.7, "Hello"},
        {4.2, 8.9, "World"},
    };
    DiarizeResult diar;
    diar.num_speakers = 1;
    diar.segments = {{0.0, 10.0, 0}};

    auto result = merge_speakers(transcript, diar);
    REQUIRE(result.size() == 2);
    CHECK(result[0].start == 1.5);
    CHECK(result[0].end == 3.7);
    CHECK(result[1].start == 4.2);
    CHECK(result[1].end == 8.9);
}
