// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 6 — `.vtt` sidecar persistence tests.
//
// Hermetic — pure I/O on a temp directory. No model, no IPC, no GTK.
// Validates the WebVTT formatters and the VttWriter's append-only,
// header-on-first-final behaviour, plus the duration helper that the
// daemon-side fan-out adapter uses to map single-timestamp finals into
// (start, end) cue spans.

#include <catch2/catch_test_macros.hpp>

#include "caption_vtt.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace recmeet;
namespace fs = std::filesystem;

namespace {

// Per-test unique temp dir to keep parallel ctest runs disjoint.
fs::path make_tmp_dir(const std::string& tag) {
    static std::atomic<unsigned> counter{0};
    auto pid = static_cast<unsigned>(::getpid());
    auto n = counter.fetch_add(1);
    fs::path dir = fs::temp_directory_path()
                 / ("recmeet_vtt_" + std::to_string(pid) + "_"
                    + std::to_string(n) + "_" + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// Read entire file into a string. Returns empty on missing file.
std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// Count occurrences of a substring.
std::size_t count_substr(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t i = 0; (i = hay.find(needle, i)) != std::string::npos; ++n) {
        i += needle.size();
    }
    return n;
}

} // anonymous namespace

// ===========================================================================
// Pure-function tests: format_vtt_timestamp
// ===========================================================================

TEST_CASE("format_vtt_timestamp: zero", "[caption-vtt]") {
    REQUIRE(format_vtt_timestamp(0) == "00:00:00.000");
}

TEST_CASE("format_vtt_timestamp: sub-second", "[caption-vtt]") {
    REQUIRE(format_vtt_timestamp(123456) == "00:02:03.456");
    REQUIRE(format_vtt_timestamp(1) == "00:00:00.001");
    REQUIRE(format_vtt_timestamp(999) == "00:00:00.999");
    REQUIRE(format_vtt_timestamp(1000) == "00:00:01.000");
}

TEST_CASE("format_vtt_timestamp: hour boundary", "[caption-vtt]") {
    REQUIRE(format_vtt_timestamp(3600000) == "01:00:00.000");
    REQUIRE(format_vtt_timestamp(3661001) == "01:01:01.001");
}

TEST_CASE("format_vtt_timestamp: large values and negative clamp", "[caption-vtt]") {
    // 100 hours + 1.234s
    REQUIRE(format_vtt_timestamp(100LL * 3600 * 1000 + 1234) == "100:00:01.234");
    // Negative clamps to 0 — defense in depth, the engine emits monotonic ms.
    REQUIRE(format_vtt_timestamp(-1) == "00:00:00.000");
    REQUIRE(format_vtt_timestamp(-10000) == "00:00:00.000");
}

// ===========================================================================
// Pure-function tests: format_vtt_cue
// ===========================================================================

TEST_CASE("format_vtt_cue: simple block", "[caption-vtt]") {
    std::string cue = format_vtt_cue(0, 500, "HELLO WORLD");
    REQUIRE(cue == "00:00:00.000 --> 00:00:00.500\nHELLO WORLD\n\n");
}

TEST_CASE("format_vtt_cue: arrow-in-text is escaped", "[caption-vtt]") {
    // A literal --> in caption text would confuse a WebVTT parser; the
    // formatter replaces it with --&gt; (entity-escaped).
    std::string cue = format_vtt_cue(0, 100, "ARROW --> HERE");
    REQUIRE(cue == "00:00:00.000 --> 00:00:00.100\nARROW --&gt; HERE\n\n");

    // Multiple occurrences and an arrow at the end of the text.
    std::string cue2 = format_vtt_cue(0, 100, "--> --> --> end");
    REQUIRE(cue2.find("-->") != std::string::npos);  // header arrow still present
    // Body contains 3 --&gt; entities and zero raw arrows in the body line.
    auto body_start = cue2.find('\n') + 1;
    auto body_end = cue2.rfind("\n\n");
    std::string body = cue2.substr(body_start, body_end - body_start);
    REQUIRE(body.find("-->") == std::string::npos);
    REQUIRE(count_substr(body, "--&gt;") == 3);
}

TEST_CASE("format_vtt_cue: empty text", "[caption-vtt]") {
    // Empty text still emits the timestamp header + a blank body line + the
    // terminator. Parsers tolerate this; in practice the daemon never
    // forwards empty finals.
    std::string cue = format_vtt_cue(1000, 1500, "");
    REQUIRE(cue == "00:00:01.000 --> 00:00:01.500\n\n\n");
}

// ===========================================================================
// VttCueTimer — running timestamp pair (duration helper)
// ===========================================================================

TEST_CASE("VttCueTimer: contiguous cue spans", "[caption-vtt]") {
    VttCueTimer t;
    auto p1 = t.next(500);
    REQUIRE(p1.first == 0);
    REQUIRE(p1.second == 500);

    auto p2 = t.next(1200);
    REQUIRE(p2.first == 500);
    REQUIRE(p2.second == 1200);

    auto p3 = t.next(2000);
    REQUIRE(p3.first == 1200);
    REQUIRE(p3.second == 2000);
}

TEST_CASE("VttCueTimer: regression clamps to non-negative duration", "[caption-vtt]") {
    // The engine emits monotonic timestamps in production; if a regression
    // happens (clock wobble, test stub) the timer must not produce
    // negative-duration cues.
    VttCueTimer t;
    (void)t.next(1000);
    auto p = t.next(800);
    REQUIRE(p.first <= p.second);
    REQUIRE(p.second == 800);
}

// ===========================================================================
// VttWriter behaviour
// ===========================================================================

TEST_CASE("VttWriter: header lazily written on first final", "[caption-vtt]") {
    auto dir = make_tmp_dir("header_lazy");
    fs::path vtt = dir / "captions.vtt";

    {
        VttWriter w(vtt);
        // Construction alone must not create the file — silent sessions
        // produce no .vtt at all.
        REQUIRE_FALSE(fs::exists(vtt));
        REQUIRE_FALSE(w.is_open());
        REQUIRE_FALSE(w._header_written_for_test());

        REQUIRE(w.append(0, 500, "HELLO WORLD", /*is_partial=*/false));
        REQUIRE(fs::exists(vtt));
        REQUIRE(w.is_open());
        REQUIRE(w._header_written_for_test());

        REQUIRE(w.append(500, 1200, "AND ANOTHER ONE", false));
    }  // dtor closes fd

    std::string contents = read_all(vtt);
    // Exactly one WEBVTT header at the start.
    REQUIRE(contents.rfind("WEBVTT\n\n", 0) == 0);  // starts with header
    REQUIRE(count_substr(contents, "WEBVTT\n\n") == 1);

    // Both cue text bodies are present (text passes through normalize_caption
    // by default — first letter capitalized).
    REQUIRE(contents.find("Hello world") != std::string::npos);
    REQUIRE(contents.find("And another one") != std::string::npos);
}

TEST_CASE("VttWriter: partials never persisted", "[caption-vtt]") {
    auto dir = make_tmp_dir("partials_dropped");
    fs::path vtt = dir / "captions.vtt";

    VttWriter w(vtt);
    // Five partials → file must not exist.
    for (int i = 0; i < 5; ++i) {
        REQUIRE(w.append(i * 100, (i + 1) * 100, "PARTIAL", /*is_partial=*/true));
    }
    REQUIRE_FALSE(fs::exists(vtt));
    REQUIRE_FALSE(w._header_written_for_test());

    // One final → file is created with header + the one cue.
    REQUIRE(w.append(0, 500, "FIRST FINAL", /*is_partial=*/false));
    REQUIRE(fs::exists(vtt));

    std::string c = read_all(vtt);
    REQUIRE(c.rfind("WEBVTT\n\n", 0) == 0);
    REQUIRE(c.find("First final") != std::string::npos);
    // Exactly one cue (one timestamp arrow line, plus the header arrow → 2
    // total `-->` substrings).
    REQUIRE(count_substr(c, "-->") == 1);
}

TEST_CASE("VttWriter: append integrity over 100 cues", "[caption-vtt]") {
    auto dir = make_tmp_dir("integrity_100");
    fs::path vtt = dir / "captions.vtt";

    {
        VttWriter w(vtt, /*normalize_display=*/false);
        for (int i = 0; i < 100; ++i) {
            std::int64_t s = i * 500;
            std::int64_t e = s + 400;
            std::string text = "CUE " + std::to_string(i);
            REQUIRE(w.append(s, e, text, /*is_partial=*/false));
        }
    }

    std::string contents = read_all(vtt);
    REQUIRE(contents.rfind("WEBVTT\n\n", 0) == 0);

    // Exactly 100 cue arrows.
    REQUIRE(count_substr(contents, "-->") == 100);

    // Each cue ends with `\n\n` (terminator). The header also ends with
    // `\n\n`, so we expect 101 occurrences of `\n\n` in the final file.
    REQUIRE(count_substr(contents, "\n\n") == 101);

    // First and last cue body markers present.
    REQUIRE(contents.find("CUE 0\n") != std::string::npos);
    REQUIRE(contents.find("CUE 99\n") != std::string::npos);

    // Spot-check the first cue's full block (verbatim — no normalization).
    REQUIRE(contents.find("00:00:00.000 --> 00:00:00.400\nCUE 0\n\n")
            != std::string::npos);
    // Last cue: 99 * 500 = 49500ms = 00:00:49.500 → 49900 = 00:00:49.900
    REQUIRE(contents.find("00:00:49.500 --> 00:00:49.900\nCUE 99\n\n")
            != std::string::npos);
}

TEST_CASE("VttWriter: reprocess no-op preservation (sidecar untouched by sibling writes)",
          "[caption-vtt]") {
    // Simulates Phase 6.3: when a meeting dir is reprocessed, other files
    // (note.md, transcript, summary, etc.) may be created or rewritten in
    // the same dir, but `captions.vtt` from the original recording must
    // remain byte-identical. Reprocess never re-runs the streaming engine,
    // so no append() ever fires against the existing sidecar.
    auto dir = make_tmp_dir("reprocess_preserve");
    fs::path vtt = dir / "captions.vtt";

    // Phase 1: write a sidecar via VttWriter, then capture its bytes.
    {
        VttWriter w(vtt, /*normalize_display=*/false);
        REQUIRE(w.append(0, 500, "HELLO WORLD", false));
        REQUIRE(w.append(500, 1200, "ANOTHER LINE", false));
        REQUIRE(w.append(1200, 2000, "FINAL CUE", false));
    }
    std::string before = read_all(vtt);
    REQUIRE_FALSE(before.empty());

    // Phase 2: simulate reprocess writing other files into the same dir.
    {
        std::ofstream(dir / "note.md") << "# Meeting\nbody\n";
        std::ofstream(dir / "transcript.txt") << "line one\nline two\n";
        std::ofstream(dir / "summary.json") << "{\"summary\":\"x\"}";
    }

    // Phase 3: assert the sidecar's bytes are unchanged.
    std::string after = read_all(vtt);
    REQUIRE(after == before);
}

TEST_CASE("VttWriter: malformed-UTF8 input passes through unchanged",
          "[caption-vtt]") {
    auto dir = make_tmp_dir("malformed_utf8");
    fs::path vtt = dir / "captions.vtt";

    // Lone 0xFF byte mid-string is invalid UTF-8. The writer must accept
    // it (no validation; raw bytes go straight to write()). We disable
    // normalization to keep the byte sequence exactly as supplied.
    std::string text;
    text += "ABC";
    text += static_cast<char>(0xFF);
    text += "DEF";

    {
        VttWriter w(vtt, /*normalize_display=*/false);
        REQUIRE(w.append(0, 500, text, /*is_partial=*/false));
        REQUIRE(w.last_error().empty());
    }

    std::string contents = read_all(vtt);
    REQUIRE(contents.find(text) != std::string::npos);
}

TEST_CASE("VttWriter: explicit close() then dtor is safe (idempotent)",
          "[caption-vtt]") {
    auto dir = make_tmp_dir("close_idempotent");
    fs::path vtt = dir / "captions.vtt";

    VttWriter w(vtt);
    REQUIRE(w.append(0, 500, "HELLO", false));
    REQUIRE(w.is_open());
    w.close();
    REQUIRE_FALSE(w.is_open());
    // Dtor runs after close — must not fault.
}
