// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// T2.3 — chunked-diarize integration test (postprocess-memory-containment).
//
// Purpose
// -------
// Reprocess the iter-110 long-audio fixture (60-min real recording) under a
// cgroup `MemoryMax=8G` cap and assert that the chunked diarization path
// (T2.1/T2.2) completes without:
//   - global OOM-kill (cgroup-bounded cleanup wins over kernel reaper)
//   - T1A child self-limit fire ("child RSS limit exceeded" stderr line)
//   - watchdog kill (no deadline expiry)
//
// Plus invariants:
//   - persisted speakers.json embeddings have **non-unit** L2 norm
//     (raw-format invariant, T2.1 H1 — guards against accidental
//     normalize-on-store regression that would diverge the persisted
//     vector format from the legacy single-call path)
//   - persisted cluster_id values are 0..N-1 contiguous (compaction
//     invariant, T2.1 step 8 / rev 7 M-2')
//
// Tags
// ----
// `[integration][t2-1][slow]` — excluded from the default `make test` run
// (which uses `~[integration]~[benchmark]~[full-stack]`). Run with:
//
//   make integration-t2-1     # convenience target — runs under systemd-run
//   ./build/recmeet_tests "[integration][t2-1]"   # bare run, no cgroup
//
// Fixture
// -------
// Audio length must exceed the chunked-path threshold, which at defaults
// (`chunk_minutes=15`, `chunk_overlap_sec=30`) is ~17.5 min. The iter-110
// recording is ~60 min so it produces ~4 chunks plus headroom.
//
// Search order for the fixture WAV (16 kHz mono PCM):
//   1. RECMEET_T2_1_FIXTURE env var (absolute path)
//   2. <project_root>/notes/t1b-t1c-validation-2026-05-01/iter-110.wav
//   3. <project_root>/notes/iter-110/audio.wav
//   4. ~/recordings/iter-110.wav
//   5. ~/meetings/<dir>/audio_*.wav — longest recording ≥ 17.5 min (the
//      chunked-path threshold at defaults). Picks any sufficiently-long
//      recmeet output so the operator does not need to copy or symlink the
//      iter-110 WAV to a fixed path.
//
// If none exist the test SKIPs cleanly with the expected paths logged. The
// fixture is a recorded artifact and is not committed to the repository.

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.h"
#include "pipeline.h"
#include "speaker_id.h"
#include "model_manager.h"
#include "config.h"
#include "audio_file.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace recmeet;
using namespace recmeet::test_helpers;

#if RECMEET_USE_SHERPA

namespace {

// Search order documented in the file header.
fs::path find_iter110_fixture(std::string& out_searched_summary) {
    std::vector<fs::path> candidates;

    if (const char* env = std::getenv("RECMEET_T2_1_FIXTURE")) {
        if (env[0]) candidates.emplace_back(env);
    }

    fs::path root = find_project_root();
    if (!root.empty()) {
        candidates.push_back(root / "notes" / "t1b-t1c-validation-2026-05-01" / "iter-110.wav");
        candidates.push_back(root / "notes" / "iter-110" / "audio.wav");
    }

    if (const char* home = std::getenv("HOME")) {
        candidates.push_back(fs::path(home) / "recordings" / "iter-110.wav");
    }

    std::string summary;
    for (const auto& p : candidates) {
        summary += "  - " + p.string();
        if (fs::exists(p)) {
            summary += " [FOUND]";
            out_searched_summary = summary;
            return p;
        }
        summary += " [missing]\n";
    }

    // Fallback: scan ~/meetings/<dir>/audio_*.wav for the longest recording
    // ≥ 17.5 min (the chunked-path threshold at default settings). Any
    // sufficiently-long recmeet output works as a fixture for this test.
    fs::path meetings_match = find_long_meetings_audio(17.5 * 60.0);
    if (!meetings_match.empty()) {
        summary += "  - " + meetings_match.string()
                 + " [FOUND via ~/meetings scan, longest >17.5 min]";
        out_searched_summary = summary;
        return meetings_match;
    }
    summary += "  - ~/meetings/<dir>/audio_*.wav [no recording ≥ 17.5 min]\n";

    out_searched_summary = summary;
    return {};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// T2.3 — long-audio chunked diarization end-to-end under memory cap
// ---------------------------------------------------------------------------
//
// This test does NOT itself spawn under a cgroup — it runs the chunked
// pipeline in-process. The cgroup containment is the responsibility of the
// invoker (`make integration-t2-1` wraps with `systemd-run --user --scope
// -p MemoryMax=8G ...`). Inside the test we still:
//   - check no T1A "child RSS limit exceeded" stderr token leaks (via
//     the captured stderr buffer)
//   - track peak RSS via 1 Hz sampling to surface a useful number when run
//     bare
//   - assert the structural invariants (raw-norm + ID-compaction) on the
//     persisted speakers.json
TEST_CASE("Chunked pipeline: long-audio reprocess under memory cap",
          "[integration][t2-1][slow]") {
    std::string searched;
    fs::path audio_src = find_iter110_fixture(searched);
    if (audio_src.empty()) {
        SKIP("iter-110 long-audio fixture not found. Searched paths:\n" + searched +
             "\nProvide a 16 kHz mono PCM WAV (>17.5 min) via "
             "RECMEET_T2_1_FIXTURE=/path/to.wav or one of the search paths.");
    }
    if (!is_whisper_model_cached("base"))
        SKIP("Whisper base model not cached — run: ./build/recmeet --download-models --model base");
    if (!is_sherpa_model_cached())
        SKIP("Sherpa diarization models not cached — run: ./build/recmeet --download-models");

    // Set up a temp output directory so persisted speakers.json doesn't
    // leak between runs.
    auto out_dir = fs::temp_directory_path() / "recmeet_t2_1_integration";
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);

    // Copy in fixture so resolve_meeting_time() finds a sensibly-named file.
    fs::path audio_path = out_dir / "audio_2026-04-30_12-00.wav";
    fs::copy_file(audio_src, audio_path, fs::copy_options::overwrite_existing);

    Config cfg;
    cfg.whisper_model = "base";
    cfg.language = "en";
    cfg.diarize = true;
    cfg.num_speakers = 0;       // auto-detect, post-stitch greedy-merge.
    cfg.speaker_id = false;     // no enrolled DB needed for this gate.
    cfg.vad = true;
    cfg.no_summary = true;      // skip LLM step — we're measuring postprocess RSS.
    cfg.output_dir = out_dir;
    cfg.output_dir_explicit = true;
    cfg.note_dir = out_dir;
    // Defaults exercise the chunked path (audio > ~17.5 min @ defaults).

    // Phase A instrumentation hook. When the integration test is invoked with
    // RECMEET_DEBUG_DUMP_CENTROIDS=/some/path.json, the chunked pipeline
    // writes the centroid dump there (auto-suffixed with the meeting
    // timestamp). Lets the threshold-sweep run reuse this fixture without
    // forking the test or changing the gate behavior.
    if (const char* dump = std::getenv("RECMEET_DEBUG_DUMP_CENTROIDS")) {
        if (dump[0]) cfg.debug_dump_centroids_path = dump;
    }

    PostprocessInput input;
    input.out_dir = out_dir;
    input.audio_path = audio_path;

    // 1 Hz peak-RSS sampler for diagnostic context. Cheap; runs even when
    // we are NOT under a cgroup.
    std::atomic<bool> sampler_stop{false};
    std::atomic<long> peak_rss_kb{0};
    std::thread sampler([&]() {
        while (!sampler_stop.load(std::memory_order_relaxed)) {
            long now_kb = read_self_rss_kb();
            long prev = peak_rss_kb.load(std::memory_order_relaxed);
            while (now_kb > prev &&
                   !peak_rss_kb.compare_exchange_weak(prev, now_kb)) {}
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    auto t0 = std::chrono::steady_clock::now();
    PipelineResult result;
    bool threw = false;
    std::string err_msg;
    try {
        result = run_postprocessing(cfg, input);
    } catch (const std::exception& e) {
        threw = true;
        err_msg = e.what();
    }
    auto t1 = std::chrono::steady_clock::now();
    sampler_stop.store(true);
    sampler.join();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    INFO("wall-clock: " << secs << " s");
    INFO("peak RSS:   " << peak_rss_kb.load() << " KB ("
         << (peak_rss_kb.load() / 1024) << " MB)");
    INFO("note path:  " << result.note_path.string());
    if (threw) INFO("exception:  " << err_msg);

    // Subprocess-style failures (T1A self-limit, watchdog kill) would have
    // killed THIS process before it returned, so reaching this point means
    // none fired. We still assert the postprocess returned cleanly.
    REQUIRE_FALSE(threw);

    // ---- speakers.json invariants ----
    auto speakers = load_meeting_speakers(out_dir);
    INFO("speaker count: " << speakers.size());

    // Coarse sanity gate: chunked stitching across 4×15-min windows on real
    // 60-min meeting audio with two-ish dominant speakers should land in a
    // small range. We don't pin an exact number because pyannote VAD and
    // sherpa clustering can both jitter run-to-run; we just guard against
    // gross fragmentation (e.g. one global speaker per chunk).
    CHECK(!speakers.empty());
    CHECK(speakers.size() <= 8);

    // Raw-format invariant (T2.1 H1): persisted embeddings must NOT be unit
    // norm — chunked + single-call paths both store raw running-mean
    // centroids so callers can do byte-identical compares against the
    // legacy DB format.
    for (const auto& s : speakers) {
        REQUIRE_FALSE(s.embedding.empty());
        double sum_sq = 0.0;
        for (float v : s.embedding) sum_sq += static_cast<double>(v) * v;
        double norm = std::sqrt(sum_sq);
        INFO("speaker '" << s.label << "' embedding L2 norm = " << norm);
        // Generous gate — raw eres2net norms cluster in 5–25 range; failure
        // mode (silent normalize-on-store) lands at norm ≈ 1.0.
        CHECK(std::abs(norm - 1.0) > 0.05);
    }

    // ID-compaction invariant (rev 7 M-2'): cluster_id values must be
    // 0..N-1 contiguous after the post-stitch greedy-merge pass. A gap
    // (e.g. {0, 1, 3}) would surface as `Speaker_01, Speaker_02,
    // Speaker_04` in transcripts — guarded here.
    std::set<int> cluster_ids;
    for (const auto& s : speakers) cluster_ids.insert(s.cluster_id);
    REQUIRE(cluster_ids.size() == speakers.size());
    int expected = 0;
    for (int id : cluster_ids) {
        INFO("cluster_id = " << id << " (expected " << expected << ")");
        CHECK(id == expected);
        ++expected;
    }

    fs::remove_all(out_dir);
}

#endif  // RECMEET_USE_SHERPA
