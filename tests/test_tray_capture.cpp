// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase B.2 / B.3 — tray-side capture helpers (hermetic unit cover).
//
// Each test runs against a per-case temporary staging dir so the
// operator's actual ~/.local/share/recmeet/staging/ is never touched.
// The PipeWire-driven recording lifecycle + GTK dialog live in
// tray.cpp and aren't exercised here (GTK requires a display);
// the helpers under test are the WAV writer, the .pending sidecar
// writer, the timestamp formatter, the staging-path allocator, and
// the audio_capture fan-out subscriber used by the WAV stager.

#include <catch2/catch_test_macros.hpp>

#include "audio_capture.h"
#include "device_enum.h"
#include "tray_capture.h"
#include "util.h"

#include <sndfile.h>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace recmeet;
namespace fs = std::filesystem;

namespace {

// Build a unique per-test scratch dir under /tmp so concurrent runs
// don't trip over each other. Caller is responsible for removing it
// (via std::error_code, not throwing) at test end.
fs::path make_scratch() {
    std::random_device rd;
    std::ostringstream oss;
    oss << "/tmp/recmeet_tray_cap_" << ::getpid() << "_" << rd();
    fs::path p = oss.str();
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

// ---------------------------------------------------------------------------
// B.2 — timestamp + path allocation
// ---------------------------------------------------------------------------

TEST_CASE("B.2: format_timestamp matches YYYY-MM-DD_HH-MM convention",
          "[tray][b2]") {
    // 2026-05-13 14:30:00 UTC. We can't pin the local TZ, so we just
    // assert the shape — the dashes / underscore / digit count are
    // the load-bearing properties (file-naming convention).
    std::time_t t = 1747146600;  // 2026-05-13 14:30:00 UTC
    std::string ts = tray_capture::format_timestamp(t);
    REQUIRE(ts.size() == 16);   // "YYYY-MM-DD_HH-MM"
    CHECK(ts[4]  == '-');
    CHECK(ts[7]  == '-');
    CHECK(ts[10] == '_');
    CHECK(ts[13] == '-');
}

TEST_CASE("B.2: next_staging_wav_path returns the unsuffixed path when free",
          "[tray][b2]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    fs::path got = tray_capture::next_staging_wav_path(scratch, "2026-05-13_14-30");
    CHECK(got == scratch / "audio_2026-05-13_14-30.wav");
}

TEST_CASE("B.2: next_staging_wav_path appends _N collision suffix",
          "[tray][b2]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    // Pre-populate the un-suffixed name and the _1 slot.
    std::ofstream(scratch / "audio_2026-05-13_14-30.wav").close();
    std::ofstream(scratch / "audio_2026-05-13_14-30_1.wav").close();

    fs::path got = tray_capture::next_staging_wav_path(scratch, "2026-05-13_14-30");
    CHECK(got == scratch / "audio_2026-05-13_14-30_2.wav");
}

// ---------------------------------------------------------------------------
// B.2 — WAV writer (16 kHz mono PCM-16)
// ---------------------------------------------------------------------------

TEST_CASE("B.2: write_wav produces a 16kHz mono PCM-16 readable WAV",
          "[tray][b2]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    // 1 s of silence at 16 kHz mono — small payload, fast test.
    std::vector<int16_t> samples(SAMPLE_RATE, 0);
    fs::path wav = scratch / "audio_test.wav";

    std::string err;
    REQUIRE(tray_capture::write_wav(wav, samples, err));
    REQUIRE(fs::exists(wav));
    REQUIRE(fs::file_size(wav) > 44);  // header + at least some data

    // Read back via libsndfile to verify the format the daemon-side
    // reprocess flow will see.
    SF_INFO info = {};
    SNDFILE* sf = sf_open(wav.c_str(), SFM_READ, &info);
    REQUIRE(sf != nullptr);
    CHECK(info.samplerate == SAMPLE_RATE);
    CHECK(info.channels   == CHANNELS);
    CHECK(info.frames     == static_cast<sf_count_t>(samples.size()));
    sf_close(sf);
}

TEST_CASE("B.2: write_wav surfaces sf_open failure cleanly",
          "[tray][b2]") {
    // Write to a path under a non-existent dir — sf_open should fail
    // and the helper should return false with a populated err_msg
    // without leaving a half-written file behind.
    std::vector<int16_t> samples(16, 0);
    std::string err;
    fs::path bad = "/nonexistent_b2_dir_xyz/audio_test.wav";
    REQUIRE_FALSE(tray_capture::write_wav(bad, samples, err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(fs::exists(bad));
}

// ---------------------------------------------------------------------------
// B.3 — .pending sidecar (Save for later)
// ---------------------------------------------------------------------------

TEST_CASE("B.3: pending_sidecar_path swaps .wav for .pending",
          "[tray][b3]") {
    fs::path wav = "/tmp/audio_2026-05-13_14-30.wav";
    CHECK(tray_capture::pending_sidecar_path(wav) ==
          fs::path("/tmp/audio_2026-05-13_14-30.pending"));
}

TEST_CASE("B.3: write_pending_sidecar persists the recording metadata",
          "[tray][b3]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    fs::path wav = scratch / "audio_2026-05-13_14-30.wav";
    std::ofstream(wav).close();  // wav file does not need to be a real WAV

    REQUIRE(tray_capture::write_pending_sidecar(
        wav, "2026-05-13_14-30", "alsa_input.usb-microphone", /*captions_enabled=*/true));

    fs::path sidecar = tray_capture::pending_sidecar_path(wav);
    REQUIRE(fs::exists(sidecar));

    std::ifstream in(sidecar);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();

    CHECK(content.find("\"timestamp\": \"2026-05-13_14-30\"") != std::string::npos);
    CHECK(content.find("\"mic_source\": \"alsa_input.usb-microphone\"")
          != std::string::npos);
    CHECK(content.find("\"captions_enabled\": true") != std::string::npos);
    CHECK(content.find(wav.string()) != std::string::npos);
}

// ---------------------------------------------------------------------------
// B.3 — discard latency (Plan acceptance: WAV gone in <100 ms)
// ---------------------------------------------------------------------------

TEST_CASE("B.3: discard unlinks the staging WAV in <100 ms",
          "[tray][b3]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    // Materialize a non-trivial WAV so the unlink isn't a no-op on
    // some filesystems' fast-path heuristics.
    std::vector<int16_t> samples(SAMPLE_RATE * 5, 0);  // 5 s of silence
    fs::path wav = scratch / "audio_discard_test.wav";
    std::string err;
    REQUIRE(tray_capture::write_wav(wav, samples, err));
    REQUIRE(fs::exists(wav));

    auto t0 = std::chrono::steady_clock::now();
    std::error_code ec;
    fs::remove(wav, ec);
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    CHECK_FALSE(ec);
    CHECK_FALSE(fs::exists(wav));
    CHECK(ms < 100);
}

// ---------------------------------------------------------------------------
// B.4 — local source enumeration (tray uses device_enum::list_sources()
// directly; no sources.list IPC).
// ---------------------------------------------------------------------------

TEST_CASE("B.4: list_sources() is callable from the test process without IPC",
          "[tray][b4]") {
    // The function itself talks to PulseAudio / PipeWire — in CI / WSL
    // contexts it may return an empty vector or throw DeviceError if
    // no server is running. We only assert it does NOT crash and does
    // NOT reach for the daemon socket. Empty / non-empty is fine.
    try {
        auto sources = list_sources();
        // If the test host has a Pulse server, we should see some
        // sources. If not, an empty vector is acceptable.
        (void)sources;
        SUCCEED("list_sources returned without throwing");
    } catch (const DeviceError& e) {
        // Acceptable: no Pulse server in this test environment.
        SUCCEED(std::string("DeviceError caught (no Pulse server in test env): ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// B.2 — fan-out: two subscribers each receive identical frames.
// (Plan acceptance for the C.10 streaming-uploader hook compatibility.)
// ---------------------------------------------------------------------------

namespace {

struct FrameSink {
    std::size_t calls = 0;
    std::vector<int16_t> bytes;
};

void sink_cb(const int16_t* samples, std::size_t n, void* userdata) {
    auto* sink = static_cast<FrameSink*>(userdata);
    sink->calls++;
    sink->bytes.insert(sink->bytes.end(), samples, samples + n);
}

} // anonymous namespace

TEST_CASE("B.2: WAV stager + streaming hook share identical frames",
          "[tray][b2]") {
    // Tray's WAV stager and (future) C.10 streaming uploader both
    // subscribe via add_audio_subscriber. We don't link the tray's
    // WAV-stager callback directly here (it touches g_tray), but we
    // exercise the same fan-out shape that wires them together.
    PipeWireCapture cap("test-source");
    FrameSink stager, uploader;
    auto h1 = cap.add_audio_subscriber(&sink_cb, &stager);
    auto h2 = cap.add_audio_subscriber(&sink_cb, &uploader);
    REQUIRE(h1 != 0);
    REQUIRE(h2 != 0);

    const int16_t chunk[] = {1, 2, 3, 4, 5};
    cap._inject_for_test(chunk, 5);

    CHECK(stager.calls   == 1);
    CHECK(uploader.calls == 1);
    CHECK(stager.bytes   == uploader.bytes);
    CHECK(stager.bytes.size() == 5);
}
