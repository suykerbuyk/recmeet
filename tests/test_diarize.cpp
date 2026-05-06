// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "diarize.h"
#include "util.h"

#include <cmath>

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

// ===========================================================================
// T2.1 — chunked diarization stitching (`stitch_chunks` + `diarize_chunked`)
//
// These exercise the stitch helper directly with handcrafted DiarizeResult
// inputs (rev 7 L-4'). End-to-end real-audio runs are deferred to the
// `[benchmark][t2-1]` bench since pyannote VAD output is not deterministic
// enough for unit-grade assertions on segment boundaries.
// ===========================================================================

#if RECMEET_USE_SHERPA

namespace {

// Build a square ChunkExtents for tests where PCM<->core are the same span
// or where we hand-craft global boundaries; samples count derived from sec.
ChunkExtents make_extents(double pcm_start_sec, double pcm_end_sec,
                          double core_start_sec, double core_end_sec) {
    ChunkExtents e{};
    e.pcm_start_samples = static_cast<size_t>(pcm_start_sec * SAMPLE_RATE);
    e.pcm_end_samples   = static_cast<size_t>(pcm_end_sec   * SAMPLE_RATE);
    e.core_start_sec    = core_start_sec;
    e.core_end_sec      = core_end_sec;
    e.offset_sec        = pcm_start_sec;
    return e;
}

// Build a chunk-local DiarizeResult quickly.
DiarizeResult make_chunk(std::initializer_list<DiarizeSegment> segs,
                         int num_speakers) {
    DiarizeResult r;
    r.segments = segs;
    r.num_speakers = num_speakers;
    return r;
}

// Build a unit-vector raw centroid in `dim` dimensions with magnitude `mag`,
// pointing along axis `axis` (0..dim-1). Useful for orthogonality tests.
std::vector<float> axis_centroid(int dim, int axis, float mag = 1.0f) {
    std::vector<float> v(dim, 0.0f);
    if (axis >= 0 && axis < dim) v[axis] = mag;
    return v;
}

// Build a centroid pointing in (cos(θ), sin(θ)) direction in 2-D for fine
// cosine-similarity boundary control.
std::vector<float> angle_centroid_2d(double theta, float mag = 1.0f) {
    return {static_cast<float>(mag * std::cos(theta)),
            static_cast<float>(mag * std::sin(theta))};
}

float l2(const std::vector<float>& v) {
    double s = 0.0;
    for (float x : v) s += static_cast<double>(x) * static_cast<double>(x);
    return static_cast<float>(std::sqrt(s));
}

} // namespace

TEST_CASE("stitch: identical raw centroid across chunks merges to one global",
          "[diarize][stitch][t2-1]") {
    // Two chunks; each owns one chunk-local speaker with the SAME raw
    // centroid (cosine similarity ≈ 1) → one global ID.
    DiarizeResult c0 = make_chunk({{0.0, 10.0, 0}}, 1);
    DiarizeResult c1 = make_chunk({{0.0, 10.0, 0}}, 1);

    auto cent = axis_centroid(8, 0, 2.5f);  // raw, non-unit norm
    std::map<int, std::vector<float>> m0{{0, cent}};
    std::map<int, std::vector<float>> m1{{0, cent}};

    std::vector<ChunkExtents> ext = {
        make_extents(0.0, 30.0, 0.0, 30.0),
        make_extents(30.0, 60.0, 30.0, 60.0),
    };

    DiarizeChunkConfig cfg;
    auto out = stitch_chunks({c0, c1}, {m0, m1}, ext, cfg, /*num_speakers=*/0);

    REQUIRE(out.diar.num_speakers == 1);
    REQUIRE(out.centroids.size() == 1);
    CHECK(out.centroids.count(0) == 1);
    REQUIRE(out.diar.segments.size() == 2);
    CHECK(out.diar.segments[0].speaker == 0);
    CHECK(out.diar.segments[1].speaker == 0);
}

TEST_CASE("stitch: orthogonal centroids produce distinct globals",
          "[diarize][stitch][t2-1]") {
    DiarizeResult c0 = make_chunk({{0.0, 5.0, 0}}, 1);
    DiarizeResult c1 = make_chunk({{0.0, 5.0, 0}}, 1);

    std::map<int, std::vector<float>> m0{{0, axis_centroid(8, 0)}};
    std::map<int, std::vector<float>> m1{{0, axis_centroid(8, 1)}};

    std::vector<ChunkExtents> ext = {
        make_extents(0.0, 30.0, 0.0, 30.0),
        make_extents(30.0, 60.0, 30.0, 60.0),
    };

    auto out = stitch_chunks({c0, c1}, {m0, m1}, ext,
                             DiarizeChunkConfig{}, /*num_speakers=*/0);

    REQUIRE(out.diar.num_speakers == 2);
    REQUIRE(out.centroids.size() == 2);
    CHECK(out.diar.segments.size() == 2);
    CHECK(out.diar.segments[0].speaker != out.diar.segments[1].speaker);
}

TEST_CASE("stitch: speaker A in chunk 0 and B in chunk 1 produce 2 globals",
          "[diarize][stitch][t2-1]") {
    // Chunk 0 has only speaker A (centroid axis 0); chunk 1 has only
    // speaker B (axis 1). No false merge.
    DiarizeResult c0 = make_chunk({{0.0, 5.0, 0}}, 1);
    DiarizeResult c1 = make_chunk({{0.0, 5.0, 0}}, 1);

    std::map<int, std::vector<float>> m0{{0, axis_centroid(8, 0)}};
    std::map<int, std::vector<float>> m1{{0, axis_centroid(8, 3)}};

    std::vector<ChunkExtents> ext = {
        make_extents(0.0, 30.0, 0.0, 30.0),
        make_extents(30.0, 60.0, 30.0, 60.0),
    };

    auto out = stitch_chunks({c0, c1}, {m0, m1}, ext,
                             DiarizeChunkConfig{}, /*num_speakers=*/0);
    REQUIRE(out.diar.num_speakers == 2);
}

TEST_CASE("stitch: weighted-mean update preserves raw (non-unit) centroid format",
          "[diarize][stitch][t2-1]") {
    // Same direction, different magnitudes → cosine ≈ 1 → merge into one
    // global. After merge, the stored centroid must still be raw (norm ≠ 1).
    DiarizeResult c0 = make_chunk({{0.0, 5.0, 0}}, 1);
    DiarizeResult c1 = make_chunk({{0.0, 5.0, 0}}, 1);

    std::vector<float> cA = {3.0f, 0.0f, 0.0f, 0.0f};   // norm 3
    std::vector<float> cB = {7.0f, 0.0f, 0.0f, 0.0f};   // norm 7
    std::map<int, std::vector<float>> m0{{0, cA}};
    std::map<int, std::vector<float>> m1{{0, cB}};

    std::vector<ChunkExtents> ext = {
        make_extents(0.0, 30.0, 0.0, 30.0),
        make_extents(30.0, 60.0, 30.0, 60.0),
    };

    auto out = stitch_chunks({c0, c1}, {m0, m1}, ext,
                             DiarizeChunkConfig{}, /*num_speakers=*/0);
    REQUIRE(out.centroids.size() == 1);
    auto stored = out.centroids.begin()->second;
    float n = l2(stored);
    CHECK(std::fabs(n - 1.0f) > 0.05f);   // explicitly NOT a unit vector
    CHECK(stored[0] > 0.0f);
}

TEST_CASE("stitch: cosine-similarity threshold boundary (0.59 vs 0.61)",
          "[diarize][stitch][t2-1]") {
    DiarizeChunkConfig cfg;
    cfg.stitch_threshold = 0.6f;

    // Below threshold → distinct globals.
    {
        std::vector<float> a = angle_centroid_2d(0.0);
        std::vector<float> b = angle_centroid_2d(std::acos(0.59));
        DiarizeResult c0 = make_chunk({{0.0, 5.0, 0}}, 1);
        DiarizeResult c1 = make_chunk({{0.0, 5.0, 0}}, 1);
        std::vector<ChunkExtents> ext = {
            make_extents(0.0, 30.0, 0.0, 30.0),
            make_extents(30.0, 60.0, 30.0, 60.0),
        };
        auto out = stitch_chunks({c0, c1}, {{{0, a}}, {{0, b}}}, ext, cfg, 0);
        CHECK(out.diar.num_speakers == 2);
    }
    // Above threshold → merged.
    {
        std::vector<float> a = angle_centroid_2d(0.0);
        std::vector<float> b = angle_centroid_2d(std::acos(0.61));
        DiarizeResult c0 = make_chunk({{0.0, 5.0, 0}}, 1);
        DiarizeResult c1 = make_chunk({{0.0, 5.0, 0}}, 1);
        std::vector<ChunkExtents> ext = {
            make_extents(0.0, 30.0, 0.0, 30.0),
            make_extents(30.0, 60.0, 30.0, 60.0),
        };
        auto out = stitch_chunks({c0, c1}, {{{0, a}}, {{0, b}}}, ext, cfg, 0);
        CHECK(out.diar.num_speakers == 1);
    }
}

TEST_CASE("stitch: post-stitch count limit greedy-merges to N globals",
          "[diarize][stitch][t2-1]") {
    // 5 chunks each contributing one orthogonal-ish global (5 globals total).
    // Inject two near-duplicates among them so greedy-merge has obvious
    // highest-similarity pairs to fuse.
    int dim = 5;
    std::vector<std::vector<float>> raws = {
        axis_centroid(dim, 0, 1.0f),
        axis_centroid(dim, 0, 1.0f),   // near-duplicate of #0
        axis_centroid(dim, 1, 1.0f),
        axis_centroid(dim, 2, 1.0f),
        axis_centroid(dim, 3, 1.0f),
    };
    std::vector<DiarizeResult> chunks;
    std::vector<std::map<int, std::vector<float>>> cents;
    std::vector<ChunkExtents> ext;
    for (int i = 0; i < 5; ++i) {
        chunks.push_back(make_chunk({{0.0, 5.0, 0}}, 1));
        cents.push_back({{0, raws[i]}});
        ext.push_back(make_extents(i * 30.0, (i + 1) * 30.0,
                                   i * 30.0, (i + 1) * 30.0));
    }

    DiarizeChunkConfig cfg;
    cfg.stitch_threshold = 0.99f;  // force distinct globals from stitching
    auto out = stitch_chunks(chunks, cents, ext, cfg, /*num_speakers=*/3);

    // First 5 globals collapse to exactly 3 after greedy-merge.
    CHECK(out.diar.num_speakers == 3);
    CHECK(static_cast<int>(out.centroids.size()) == 3);
}

TEST_CASE("stitch: post-stitch merge uses sample-weighted mean of raw vectors",
          "[diarize][stitch][t2-1]") {
    // Two chunks with disjoint speakers; chunk 0 contributes 1000 samples
    // (chunk-local segment 0..1000/SR sec), chunk 1 contributes 10 samples.
    // Set stitch_threshold high so they don't merge during stitching, then
    // num_speakers=1 forces a post-stitch merge. The resulting centroid must
    // be (1000*A + 10*B) / 1010, NOT (A+B)/2.
    std::vector<float> A = {1.0f, 0.0f};
    std::vector<float> B = {0.0f, 1.0f};

    double dur_a_sec = 1000.0 / SAMPLE_RATE;
    double dur_b_sec = 10.0 / SAMPLE_RATE;
    DiarizeResult c0 = make_chunk({{0.0, dur_a_sec, 0}}, 1);
    DiarizeResult c1 = make_chunk({{0.0, dur_b_sec, 0}}, 1);

    std::vector<ChunkExtents> ext = {
        make_extents(0.0, 30.0, 0.0, 30.0),
        make_extents(30.0, 60.0, 30.0, 60.0),
    };

    DiarizeChunkConfig cfg;
    cfg.stitch_threshold = 0.99f;  // orthogonal so no merge during stitch

    auto out = stitch_chunks({c0, c1}, {{{0, A}}, {{0, B}}}, ext, cfg,
                             /*num_speakers=*/1);
    REQUIRE(out.centroids.size() == 1);
    auto merged = out.centroids.begin()->second;

    // expected: (1000*A + 10*B) / 1010
    std::vector<float> expected = {1000.0f / 1010.0f, 10.0f / 1010.0f};
    REQUIRE(merged.size() == 2);
    CHECK(std::fabs(merged[0] - expected[0]) < 1e-4f);
    CHECK(std::fabs(merged[1] - expected[1]) < 1e-4f);

    // Sanity: NOT (A+B)/2 = (0.5, 0.5).
    CHECK(std::fabs(merged[0] - 0.5f) > 0.4f);
}

TEST_CASE("stitch: post-merge ID compaction yields contiguous 0..N-1",
          "[diarize][stitch][t2-1]") {
    // Build a setup that creates 4 distinct globals {0,1,2,3} during stitch.
    // Then inject one near-duplicate-of-#1 in chunk 4 (post-merge target).
    // num_speakers=3 forces one merge; we verify final IDs are {0,1,2}
    // contiguous in the centroid map and the emitted segments use the
    // compacted IDs (no Speaker_04 ghost, M-2').
    int dim = 4;
    std::vector<std::vector<float>> raws = {
        axis_centroid(dim, 0),  // -> global 0
        axis_centroid(dim, 1),  // -> global 1
        axis_centroid(dim, 1),  // -> matches global 1 during stitch (count++)
        axis_centroid(dim, 2),  // -> global 2
        axis_centroid(dim, 3),  // -> global 3
    };
    // We need 4 distinct globals after stitching. Use stitch_threshold low
    // enough that the duplicate-of-1 actually does merge into 1 (so that
    // we have globals {0,1,2,3} not {0,1,1',2,3}).
    DiarizeChunkConfig cfg;
    cfg.stitch_threshold = 0.6f;

    std::vector<DiarizeResult> chunks;
    std::vector<std::map<int, std::vector<float>>> cents;
    std::vector<ChunkExtents> ext;
    for (size_t i = 0; i < raws.size(); ++i) {
        chunks.push_back(make_chunk({{0.0, 5.0, 0}}, 1));
        cents.push_back({{0, raws[i]}});
        ext.push_back(make_extents(i * 30.0, (i + 1) * 30.0,
                                   i * 30.0, (i + 1) * 30.0));
    }
    // Pre-condition: stitching produces 4 globals.
    auto pre = stitch_chunks(chunks, cents, ext, cfg, /*num_speakers=*/0);
    REQUIRE(pre.diar.num_speakers == 4);

    // num_speakers=3 forces one merge; result IDs must be {0,1,2} contiguous.
    auto out = stitch_chunks(chunks, cents, ext, cfg, /*num_speakers=*/3);
    REQUIRE(out.diar.num_speakers == 3);
    REQUIRE(out.centroids.size() == 3);
    CHECK(out.centroids.count(0) == 1);
    CHECK(out.centroids.count(1) == 1);
    CHECK(out.centroids.count(2) == 1);
    CHECK(out.centroids.count(3) == 0);  // no gap-leftover ID

    // Every emitted segment's speaker is in {0,1,2}.
    for (const auto& s : out.diar.segments) {
        CHECK(s.speaker >= 0);
        CHECK(s.speaker <= 2);
    }
}

TEST_CASE("diarize_chunked: core ownership emits each segment exactly once",
          "[diarize][stitch][t2-1]") {
    // Two chunks with cores tiling [0, 60) at boundary 30. Build a segment
    // whose midpoint lies in chunk-A core (mid=20): chunk[0] owns; chunk[1]
    // contributes the same segment from its overlap region but discards it
    // because mid < its core_start.
    auto cent = axis_centroid(4, 0);
    std::vector<ChunkExtents> ext = {
        make_extents(0.0, 35.0, 0.0, 30.0),    // pcm extends past core
        make_extents(25.0, 60.0, 30.0, 60.0),
    };
    DiarizeChunkConfig cfg;

    {
        // Mid = 20 → owned by chunk 0.
        // Chunk 0 sees seg locally as [15, 25] (offset 0).
        // Chunk 1 sees the same audio but locally [-10, 0] which is invalid,
        // so we just test the chunk-0-owned case here directly.
        DiarizeResult c0 = make_chunk({{15.0, 25.0, 0}}, 1);
        DiarizeResult c1 = make_chunk({}, 0);
        auto out = stitch_chunks({c0, c1}, {{{0, cent}}, {}}, ext, cfg, 0);
        REQUIRE(out.diar.segments.size() == 1);
        CHECK(out.diar.segments[0].start == 15.0);
        CHECK(out.diar.segments[0].end   == 25.0);
    }

    {
        // Mid = 40 → owned by chunk 1.
        DiarizeResult c0 = make_chunk({}, 0);
        DiarizeResult c1 = make_chunk({{10.0, 20.0, 0}}, 1);  // local: 10..20 → global 35..45
        auto out = stitch_chunks({c0, c1},
                                 {std::map<int, std::vector<float>>{},
                                  std::map<int, std::vector<float>>{{0, cent}}},
                                 ext, cfg, 0);
        REQUIRE(out.diar.segments.size() == 1);
        CHECK(out.diar.segments[0].start == 35.0);
        CHECK(out.diar.segments[0].end   == 45.0);
    }

    {
        // Midpoint exactly on the core boundary (30.0) → half-open lower-
        // index ownership: chunk 0 owns (interval is [core_start, core_end)).
        // For the lower-indexed chunk, mid == core_start (30.0) is in [0,30)
        // → false; for the upper chunk, mid == core_start (30.0) is in
        // [30, 60) → true. Plan line 402 says "tie-break to lower-indexed
        // chunk" via "<" upper bound. Mid==30 falls into chunk 1's core
        // [30, 60) under the upper-bound-half-open rule.
        DiarizeResult c0 = make_chunk({{29.0, 31.0, 0}}, 1);  // mid=30 in [0,30)? false
        DiarizeResult c1 = make_chunk({{4.0, 6.0, 0}}, 1);    // mid=30 in [30,60)? true
        auto out = stitch_chunks({c0, c1}, {{{0, cent}}, {{0, cent}}}, ext, cfg, 0);
        // Exactly one chunk owns this midpoint — never both, never neither.
        REQUIRE(out.diar.segments.size() == 1);
    }
}

TEST_CASE("diarize_chunked: emitted segments use global timestamps for chunks i>0",
          "[diarize][stitch][t2-1]") {
    // 3 chunks; assert every emitted segment from chunk[i>0] has start
    // greater than the chunk's core_start_sec (i.e. the offset rewrite
    // happened). Catches the regression where chunk-local times leak.
    auto cent = axis_centroid(4, 0);

    std::vector<ChunkExtents> ext = {
        make_extents(0.0, 35.0, 0.0, 30.0),
        make_extents(25.0, 65.0, 30.0, 60.0),
        make_extents(55.0, 90.0, 60.0, 90.0),
    };

    DiarizeResult c0 = make_chunk({{2.0, 4.0, 0}}, 1);     // global ~[2,4]
    DiarizeResult c1 = make_chunk({{10.0, 12.0, 0}}, 1);   // local 10..12 → global ~35..37
    DiarizeResult c2 = make_chunk({{10.0, 11.0, 0}}, 1);   // local 10..11 → global ~65..66

    auto out = stitch_chunks({c0, c1, c2},
                             {{{0, cent}}, {{0, cent}}, {{0, cent}}},
                             ext, DiarizeChunkConfig{}, 0);

    REQUIRE(out.diar.segments.size() == 3);
    // Segments are sorted by start; check the chunks-1+ ones meet their
    // respective core_start bounds.
    bool saw_chunk1 = false, saw_chunk2 = false;
    for (const auto& s : out.diar.segments) {
        if (s.start >= 30.0 && s.start < 60.0) {
            CHECK(s.start >= ext[1].core_start_sec);
            saw_chunk1 = true;
        }
        if (s.start >= 60.0) {
            CHECK(s.start >= ext[2].core_start_sec);
            saw_chunk2 = true;
        }
    }
    CHECK(saw_chunk1);
    CHECK(saw_chunk2);
}

TEST_CASE("diarize_chunked: boundary segment emitted at full extent (no trim)",
          "[diarize][stitch][t2-1]") {
    // Reproduces M-1' (rev 7 line 405). Two chunks; the same boundary
    // speech appears in both, but with slightly different VAD boundaries.
    // The full extent seen by chunk[0] ([t-Δ, t+Δ], midpoint in chunk[0].core)
    // must be emitted at full extent — NOT trimmed to the core boundary.
    auto cent = axis_centroid(4, 0);
    std::vector<ChunkExtents> ext = {
        make_extents(0.0, 35.0, 0.0, 30.0),
        make_extents(25.0, 60.0, 30.0, 60.0),
    };
    // Chunk 0 sees the segment as global [25.0, 28.0] → mid=26.5 ∈ [0,30) → owned.
    // Local times: 25.0..28.0 (offset = 0).
    DiarizeResult c0 = make_chunk({{25.0, 28.0, 0}}, 1);
    // Chunk 1 sees a tighter version of the same audio: global [26.0, 27.0]
    // → mid=26.5 ∈ [30,60)? false → not owned by chunk 1.
    // Local times: 1.0..2.0 (chunk 1 offset = 25).
    DiarizeResult c1 = make_chunk({{1.0, 2.0, 0}}, 1);

    auto out = stitch_chunks({c0, c1}, {{{0, cent}}, {{0, cent}}},
                             ext, DiarizeChunkConfig{}, 0);

    REQUIRE(out.diar.segments.size() == 1);
    // Full-extent emit means start == 25.0 and end == 28.0, NOT trim-to-core
    // 25.0..30.0 or anything tighter.
    CHECK(out.diar.segments[0].start == 25.0);
    CHECK(out.diar.segments[0].end   == 28.0);
}

TEST_CASE("diarize_chunked: invalid config throws RecmeetError",
          "[diarize][stitch][t2-1]") {
    // chunk_minutes=0.5 (= 30 s) <= overlap_seconds + 60 = 120 s → violation.
    DiarizeChunkConfig bad;
    bad.chunk_minutes = 0.5f;
    bad.overlap_seconds = 60.0f;

    // Provide a tiny audio buffer so we don't trip the empty-buffer check
    // first.
    std::vector<float> samples(SAMPLE_RATE, 0.0f);

    try {
        diarize_chunked(samples.data(), samples.size(),
                        /*num_speakers=*/0, /*threads=*/1, /*threshold=*/1.18f,
                        bad, nullptr);
        FAIL("expected RecmeetError for invalid chunk config");
    } catch (const RecmeetError& e) {
        std::string msg = e.what();
        // Must mention both "chunk" and "overlap" so the operator can fix the
        // offending knob (rev 7 line 363, line 479).
        CHECK_THAT(msg, Catch::Matchers::ContainsSubstring("chunk"));
        CHECK_THAT(msg, Catch::Matchers::ContainsSubstring("overlap"));
    }
}

#endif  // RECMEET_USE_SHERPA
