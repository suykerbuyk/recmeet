// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.5 — `.pending` sidecar v2 schema round-trip tests.
//
// Test #1 in the D.5 test plan: write the full payload (6 scalars + the
// `context` block with subject / participants / notes / language /
// vocabulary), read it back, assert every field round-trips byte-for-
// byte. Test #7 pins the "sidecar-protected from eviction" contract
// (D.6 eviction sweep stub).

#include <catch2/catch_test_macros.hpp>

#include "test_tmpdir.h"
#include "tray_capture.h"
#include "util.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

using namespace recmeet;
namespace fs = std::filesystem;

namespace {

fs::path make_scratch() {
    std::random_device rd;
    std::ostringstream oss;
    oss << "recmeet_d5_sidecar_" << ::getpid() << "_" << rd();
    fs::path p = recmeet::test::tmp_path(oss.str());
    std::error_code ec;
    fs::create_directories(p, ec);
    REQUIRE_FALSE(ec);
    return p;
}

struct ScopedDir {
    fs::path path;
    ~ScopedDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // anonymous namespace

TEST_CASE("D.5: sidecar v2 round-trip recovers all scalars + context block",
          "[d5][sidecar]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    fs::path wav = scratch / "audio_2026-05-17_09-30.wav";
    std::ofstream(wav).close();

    tray_capture::PendingSidecarV2 in;
    in.meeting_id       = "12345678-1234-4567-89ab-1234567890ab";
    in.wav_path         = wav.string();
    in.timestamp        = "2026-05-17_09-30";
    in.mic_source       = "alsa_input.usb-microphone";
    in.captions_enabled = true;
    in.context.subject      = "Quarterly review with \"quotes\" and \\slashes";
    in.context.participants = {"Alice", "Bob", "Carol"};
    in.context.notes        = "Multi-line\nnotes\nblock";
    in.context.language     = "en";
    in.context.vocabulary   = {"recmeet", "thin-client", "session_id"};

    REQUIRE_NOTHROW(tray_capture::write_pending_sidecar_v2(in));

    fs::path sidecar = tray_capture::pending_sidecar_path(wav);
    REQUIRE(fs::exists(sidecar));

    auto out = tray_capture::read_pending_sidecar(sidecar);
    CHECK(out.meeting_id       == in.meeting_id);
    CHECK(out.wav_path         == in.wav_path);
    CHECK(out.timestamp        == in.timestamp);
    CHECK(out.mic_source       == in.mic_source);
    CHECK(out.captions_enabled == in.captions_enabled);
    CHECK(out.context.subject      == in.context.subject);
    CHECK(out.context.participants == in.context.participants);
    CHECK(out.context.notes        == in.context.notes);
    CHECK(out.context.language     == in.context.language);
    CHECK(out.context.vocabulary   == in.context.vocabulary);
}

TEST_CASE("D.5: sidecar v2 reader returns empty on missing file",
          "[d5][sidecar]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};
    auto out = tray_capture::read_pending_sidecar(scratch / "missing.pending");
    CHECK(out.meeting_id.empty());
    CHECK(out.wav_path.empty());
}

// D.5 plan test #7 — sidecar-protected-from-eviction contract.
// D.6's disk-budget eviction sweep skips any staging WAV with a sibling
// `.pending` sidecar. D.5 only exposes the probe; the assertion is that
// the probe agrees with the sidecar's on-disk presence.
TEST_CASE("D.5: is_sidecar_protected reports protected iff sidecar exists",
          "[d5][eviction-contract]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    fs::path wav = scratch / "audio_2026-05-17_10-00.wav";
    std::ofstream(wav).close();

    // No sidecar yet → not protected.
    CHECK_FALSE(tray_capture::is_sidecar_protected(wav));

    tray_capture::PendingSidecarV2 p;
    p.meeting_id = "abcdef01-2345-4678-89ab-cdef01234567";
    p.wav_path   = wav.string();
    p.timestamp  = "2026-05-17_10-00";
    p.mic_source = "default";
    REQUIRE_NOTHROW(tray_capture::write_pending_sidecar_v2(p));

    // Sidecar present → protected.
    CHECK(tray_capture::is_sidecar_protected(wav));

    // Remove the sidecar → protection clears.
    fs::path sidecar = tray_capture::pending_sidecar_path(wav);
    std::error_code ec;
    fs::remove(sidecar, ec);
    CHECK_FALSE(tray_capture::is_sidecar_protected(wav));
}
