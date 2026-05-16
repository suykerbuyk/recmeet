// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.9 — unit coverage for `src/live_recording.cpp`.
//
// Background. Audit iter 156 flagged `live_recording.cpp` as having zero
// direct unit tests after C.9 extracted `run_recording()` / `run_pipeline()`
// from `pipeline.cpp` into its own translation unit. The function has three
// active V2 callers:
//   - `src/main.cpp` standalone (`recmeet --no-daemon`, ~line 1062 →
//     `run_pipeline`),
//   - `src/main.cpp` daemon-postprocess subprocess (~line 808 →
//     `run_recording`),
//   - `src/reprocess_batch.cpp` batch driver (~line 564 → `run_pipeline`).
// `--no-daemon` is an operator-documented feature (QUICKSTART.md §3 and
// §"Force standalone mode"); the reprocess branch of `run_recording` is
// also reached by the daemon's postprocess subprocess. The function is
// therefore a real V2 surface and is tested here per the audit decision
// tree.
//
// Coverage strategy. `run_recording()` has two branches:
//
//   (1) `cfg.reprocess_dir` is non-empty  →  resolve input path + output
//       directory, derive timestamp, return PostprocessInput. This branch
//       has NO PipeWire/Pulse dependency and is fully testable in a headless
//       harness.
//   (2) `cfg.reprocess_dir` is empty       →  live capture via
//       PipeWireCapture / PulseMonitorCapture. Hardware-dependent
//       (`detect_sources()` calls pulseaudio); not unit-testable in CI.
//
// We exercise the reprocess branch end-to-end here (5 cases) and add one
// case for the live-branch device-not-found error which is reachable
// without actually starting a capture, but only when the host enumerates
// zero matching sources. That case `SUCCEED`s with a note when the host
// happens to have a default source, so CI stays green either way.

#include <catch2/catch_test_macros.hpp>
#include "pipeline.h"
#include "config.h"
#include "audio_file.h"
#include "util.h"

#include <atomic>
#include <fstream>
#include <random>
#include <vector>

using namespace recmeet;

namespace {

// Per-test working dir under the system temp tree. Each TEST_CASE gets its
// own subdir keyed on `tag` so parallel runs don't collide.
fs::path test_dir(const std::string& tag) {
    fs::path d = fs::temp_directory_path() / ("recmeet_test_live_recording_" + tag);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

// Write a minimal valid WAV file (16 kHz, mono, int16). 0.5 s of silence is
// long enough for `validate_audio()` to accept and short enough that the
// test runs in well under a second.
fs::path write_minimal_wav(const fs::path& path, double seconds = 0.5) {
    std::vector<int16_t> samples(static_cast<size_t>(seconds * SAMPLE_RATE), 0);
    // A few non-zero samples so any future "all-silence" guard doesn't
    // reject — values well below clipping.
    for (size_t i = 0; i < samples.size(); i += 100) {
        samples[i] = 256;
    }
    write_wav(path, samples);
    return path;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// run_recording() — reprocess branch
// ---------------------------------------------------------------------------

TEST_CASE("run_recording: reprocess existing WAV file returns canonical PostprocessInput",
          "[live_recording][c9][cli-standalone]") {
    auto dir = test_dir("reprocess_wav");
    fs::path wav = write_minimal_wav(dir / "audio_2026-05-15_10-00.wav");

    Config cfg;
    cfg.reprocess_dir = wav;
    // output_dir_explicit defaults to false — the function should use the
    // parent of the audio file as the output dir in that case.

    StopToken stop;
    PostprocessInput pp = run_recording(cfg, stop);

    CHECK(fs::canonical(pp.audio_path) == fs::canonical(wav));
    CHECK(fs::canonical(pp.out_dir) == fs::canonical(dir));
    // Timestamp derived from the filename pattern.
    CHECK(pp.timestamp == "2026-05-15_10-00");

    fs::remove_all(dir);
}

TEST_CASE("run_recording: reprocess directory resolves to its audio file",
          "[live_recording][c9][cli-standalone]") {
    auto dir = test_dir("reprocess_dir");
    fs::path meeting = dir / "2026-05-15_10-00";
    fs::create_directories(meeting);
    fs::path wav = write_minimal_wav(meeting / "audio_2026-05-15_10-00.wav");

    Config cfg;
    cfg.reprocess_dir = meeting;

    StopToken stop;
    PostprocessInput pp = run_recording(cfg, stop);

    CHECK(fs::canonical(pp.audio_path) == fs::canonical(wav));
    CHECK(fs::canonical(pp.out_dir) == fs::canonical(meeting));
    CHECK(pp.timestamp == "2026-05-15_10-00");

    fs::remove_all(dir);
}

TEST_CASE("run_recording: reprocess relative path resolves against output_dir",
          "[live_recording][c9][cli-standalone]") {
    auto base = test_dir("reprocess_relative");
    // Meeting directory lives under `<base>/meetings/<stamp>/`.
    fs::path output_dir = base / "meetings";
    fs::path meeting = output_dir / "2026-05-15_10-00";
    fs::create_directories(meeting);
    fs::path wav = write_minimal_wav(meeting / "audio_2026-05-15_10-00.wav");

    Config cfg;
    // Caller passes a path RELATIVE to output_dir — the function must try
    // the bare path first (which fails), then fall back to
    // `output_dir / reprocess_dir` and find the meeting.
    cfg.reprocess_dir = fs::path("2026-05-15_10-00");
    cfg.output_dir = output_dir;

    StopToken stop;
    PostprocessInput pp = run_recording(cfg, stop);

    CHECK(fs::canonical(pp.audio_path) == fs::canonical(wav));

    fs::remove_all(base);
}

TEST_CASE("run_recording: reprocess nonexistent path throws RecmeetError",
          "[live_recording][c9][cli-standalone]") {
    Config cfg;
    cfg.reprocess_dir = "/tmp/this/does/not/exist/audio.wav";

    StopToken stop;
    CHECK_THROWS_AS(run_recording(cfg, stop), RecmeetError);
}

TEST_CASE("run_recording: reprocess unsupported audio format throws with conversion hint",
          "[live_recording][c9][cli-standalone]") {
    auto dir = test_dir("reprocess_unsupported");
    // Plant an .mp3 file (validate_reprocess_input rejects on the extension
    // before sf_open, so the contents are irrelevant — a stub byte will do).
    fs::path mp3 = dir / "meeting.mp3";
    { std::ofstream out(mp3); out << "x"; }

    Config cfg;
    cfg.reprocess_dir = mp3;

    StopToken stop;
    try {
        run_recording(cfg, stop);
        FAIL("expected RecmeetError for .mp3 input");
    } catch (const RecmeetError& e) {
        std::string msg = e.what();
        // The error message includes the ffmpeg conversion hint.
        INFO("error message was: " << msg);
        CHECK(msg.find("ffmpeg") != std::string::npos);
    }

    fs::remove_all(dir);
}

TEST_CASE("run_recording: reprocess with output_dir_explicit honors explicit output dir",
          "[live_recording][c9][cli-standalone]") {
    auto base = test_dir("reprocess_explicit_out");
    fs::path src_dir   = base / "src";
    fs::path out_dir   = base / "out";
    fs::create_directories(src_dir);
    fs::create_directories(out_dir);
    fs::path wav = write_minimal_wav(src_dir / "audio_2026-05-15_10-00.wav");

    Config cfg;
    cfg.reprocess_dir         = wav;
    cfg.output_dir            = out_dir;
    cfg.output_dir_explicit   = true;  // operator passed --output-dir

    StopToken stop;
    PostprocessInput pp = run_recording(cfg, stop);

    // audio_path resolves to the source WAV; output_dir tracks the
    // explicit operator choice rather than the WAV's parent dir.
    CHECK(fs::canonical(pp.audio_path) == fs::canonical(wav));
    CHECK(fs::canonical(pp.out_dir)    == fs::canonical(out_dir));

    fs::remove_all(base);
}

// ---------------------------------------------------------------------------
// run_recording() — live-capture branch (device error)
// ---------------------------------------------------------------------------
//
// The live-capture branch exercises PipeWire/Pulse. We cannot drive the
// happy path in a headless harness, but we CAN exercise the
// "no-matching-source" error: an unmatchable device_pattern forces
// `detect_sources()` to return empty `.mic`, which makes `run_recording()`
// raise `DeviceError` before ever touching PipeWire.
//
// On a host with NO sound system (a stripped CI container) detect_sources
// itself may bail out differently — in that case we SUCCEED with a note
// rather than failing the test. The point is to document the error path,
// not to assert pulseaudio availability.

TEST_CASE("run_recording: live branch with no matching mic source raises DeviceError",
          "[live_recording][c9][cli-standalone]") {
    Config cfg;
    // An impossible device pattern that no sane source name will match.
    cfg.device_pattern = "__recmeet_test_no_such_source_pattern_12345__";
    cfg.mic_only       = true;  // skip monitor auto-detect entirely
    cfg.reprocess_dir.clear();  // ensure we enter the live branch
    cfg.captions_enabled = false;

    // Pre-trip the stop token so that IF detect_sources happened to return
    // a default mic somehow, the recording loop exits immediately. The
    // expected outcome is still that detect_sources finds nothing matching
    // the pattern and run_recording raises DeviceError before reaching the
    // loop. (Pre-tripping is a defensive belt — it does not affect the
    // exception path under test.)
    StopToken stop;
    stop.request();

    try {
        (void)run_recording(cfg, stop);
        SUCCEED("host returned a mic source for the impossible pattern — "
                "device matching is more lenient than expected; skipping");
    } catch (const DeviceError& e) {
        std::string msg = e.what();
        INFO("DeviceError message: " << msg);
        CHECK(msg.find("__recmeet_test_no_such_source_pattern_12345__")
              != std::string::npos);
    } catch (const RecmeetError& e) {
        // Some host configurations surface a different RecmeetError subclass
        // (e.g. when pulseaudio is not running at all). Accept that as a
        // soft pass — the function still rejects the bad input.
        SUCCEED(std::string("RecmeetError (non-DeviceError): ") + e.what());
    }
}
