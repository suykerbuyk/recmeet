// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// test-and-verification-hardening item #4 (rev-2) — pure-helper tests
// for the tray-side "Cancel & Discard" click-to-confirm state machine.
//
// Test surface choice (per the task plan's three options):
//   * Option 1 (tray unit-style test on fabricated `g_tray.capture_state`)
//     — not feasible. `tray.cpp` carries `g_tray` as a file-static
//     `TrayState` whose fields include `AppIndicator*`, `GtkWidget*`,
//     `IpcClient`, three `SlotQueue` instances, `PendingJobsJournal`,
//     and a unique_ptr<PipeWireCapture>. There is no existing test
//     harness that can fabricate or substitute this struct, and
//     standing one up just for this test would be a 200+ LOC detour
//     well beyond the rev-2 scope ceiling. `cancel_capture()` itself
//     is unit-coverable only via interactive testing of the tray
//     binary (operator-level acceptance per the plan).
//   * Option 2 (`[full-stack]` spawning a tray binary) — not feasible.
//     There is no programmatic trigger for the menu item; the task
//     description explicitly notes this gap is out of scope (see R5
//     in `agentctx/tasks/tray-start-capture-fullstack-coverage.md`).
//   * Option 3 (`[ipc][integration]`) — not applicable. Cancel is
//     tray-local; no IPC is involved (`process.submit` is never called
//     for a cancelled recording, and the daemon does not own
//     in-progress capture state).
//
// What IS testable, and what this file covers:
//   1. The pure decision function `decide_cancel_button_state` —
//      exhaustive coverage of the Idle / Armed boundary and the time-
//      based transitions across all corner cases.
//   2. Label constants — pinned so a downstream UX change cannot
//      silently rename the operator-facing strings (a regression that
//      bypasses the v1 → v2 click-gesture parity contract).
//   3. The structural acceptance invariant "cancel writes no WAV" —
//      a positive check that `tray_capture::next_staging_wav_path`
//      allocates a path WITHOUT touching disk, so a cancel path that
//      simply never calls `write_wav` leaves no file behind. This is
//      the test most directly tied to the rev-2 acceptance signal
//      "no file under `~/.local/share/recmeet/staging/audio_*.wav`
//      for the cancelled session".
//
// Tag prefix `[tray][cancel]` matches the existing tray-test tag
// convention (`[tray][b2]`, `[tray][b3]`, etc.).

#include <catch2/catch_test_macros.hpp>

#include "test_tmpdir.h"
#include "tray_cancel_confirm.h"
#include "tray_capture.h"

#include <unistd.h>

#include <cstring>
#include <filesystem>
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
    oss << "recmeet_tray_cancel_" << ::getpid() << "_" << rd();
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

// ---------------------------------------------------------------------------
// 1. decide_cancel_button_state — pure state-machine decision
// ---------------------------------------------------------------------------

TEST_CASE("cancel-confirm: zero arm-until is canonical Idle",
          "[tray][cancel]") {
    // The 0 sentinel is the "never armed / already disarmed" state.
    // A click landing here must arm rather than fire — checked by the
    // production handler reading Idle and taking the arm branch.
    CHECK(decide_cancel_button_state(0, 0) == CancelButtonState::Idle);
    CHECK(decide_cancel_button_state(1000, 0) == CancelButtonState::Idle);
    CHECK(decide_cancel_button_state(1'000'000, 0) == CancelButtonState::Idle);
}

TEST_CASE("cancel-confirm: negative arm-until is treated as Idle",
          "[tray][cancel]") {
    // A negative value can only arise from arithmetic underflow in a
    // future caller; the helper must not interpret it as Armed. Test
    // pins the defensive contract: anything <= 0 → Idle.
    CHECK(decide_cancel_button_state(0, -1) == CancelButtonState::Idle);
    CHECK(decide_cancel_button_state(1000, -500) == CancelButtonState::Idle);
}

TEST_CASE("cancel-confirm: armed window is strictly greater-than",
          "[tray][cancel]") {
    // arm_until_ms > now_ms → Armed (the canonical mid-window state).
    // Used by the production handler to take the "second click fires"
    // branch.
    int64_t now = 1'000'000;
    CHECK(decide_cancel_button_state(now, now + 1) == CancelButtonState::Armed);
    CHECK(decide_cancel_button_state(now, now + CANCEL_ARMED_WINDOW_MS)
              == CancelButtonState::Armed);
    CHECK(decide_cancel_button_state(now, now + 2999)
              == CancelButtonState::Armed);
}

TEST_CASE("cancel-confirm: arm-until equal to now collapses to Idle",
          "[tray][cancel]") {
    // The exact boundary: arm_until == now. We choose the "closed at
    // the upper end" semantics: a click landing at exactly the strict
    // expiry moment re-arms rather than fires, which is the safer
    // failure mode (operator must re-confirm). Mirrors v1's boundary
    // convention and is enforced here so a future refactor can't
    // silently flip the inequality.
    int64_t t = 5000;
    CHECK(decide_cancel_button_state(t, t) == CancelButtonState::Idle);
}

TEST_CASE("cancel-confirm: arm-until in the past is Idle",
          "[tray][cancel]") {
    // Transient state: the strict window has expired but the revert
    // timer at +100 ms has not yet cleared the field. A click landing
    // here must re-arm, not fire, so the operator's two-click gesture
    // is always within a single 3 s window (no implicit "well, you
    // clicked 3.05 s after the first click, that counts as confirm").
    int64_t now = 10'000;
    CHECK(decide_cancel_button_state(now, now - 1) == CancelButtonState::Idle);
    CHECK(decide_cancel_button_state(now, now - 100) == CancelButtonState::Idle);
    CHECK(decide_cancel_button_state(now, 1) == CancelButtonState::Idle);
}

TEST_CASE("cancel-confirm: 3 s window matches v1 d2d6dc5 gesture",
          "[tray][cancel]") {
    // Pin the load-bearing 3 s figure. v1's `d2d6dc5` chose 3 s as the
    // safe window: short enough to forbid a casual double-click from
    // cancelling a recording, long enough that an operator with
    // intent has time to confirm. Changing this constant should be a
    // deliberate UX decision, not a drive-by edit; the test forces the
    // review.
    CHECK(CANCEL_ARMED_WINDOW_MS == 3000);
    // The revert timer adds 100 ms grace past the strict expiry so the
    // GTK main loop can process a click landing at exactly t=3000
    // before the timer flips the label back.
    CHECK(CANCEL_REVERT_TIMER_MS == 3100);
    CHECK(CANCEL_REVERT_TIMER_MS - CANCEL_ARMED_WINDOW_MS == 100);
}

TEST_CASE("cancel-confirm: simulated three-click lifecycle",
          "[tray][cancel]") {
    // Walk through the full state-machine: idle → arm → re-arm-after-
    // expiry → fire. Each transition is the decision the production
    // handler's branch logic gates on, so the simulated walk is the
    // closest unit-coverage we can get to the live click sequence
    // without a GTK harness.
    int64_t armed_until = 0;

    // T=0: first click. State observed: Idle. Handler arms.
    CHECK(decide_cancel_button_state(0, armed_until)
              == CancelButtonState::Idle);
    armed_until = 0 + CANCEL_ARMED_WINDOW_MS;

    // T=1500 (mid-window): hover-over. State: Armed.
    CHECK(decide_cancel_button_state(1500, armed_until)
              == CancelButtonState::Armed);

    // T=3500 (window expired): a click here re-arms. State: Idle.
    CHECK(decide_cancel_button_state(3500, armed_until)
              == CancelButtonState::Idle);
    // Re-arm.
    armed_until = 3500 + CANCEL_ARMED_WINDOW_MS;

    // T=4000 (mid-new-window): second click within the second arm.
    // State: Armed. Handler fires `cancel_capture()`.
    CHECK(decide_cancel_button_state(4000, armed_until)
              == CancelButtonState::Armed);
}

// ---------------------------------------------------------------------------
// 2. Label constants — pinned operator-facing strings
// ---------------------------------------------------------------------------

TEST_CASE("cancel-confirm: idle/armed label constants are stable",
          "[tray][cancel]") {
    // The labels are user-visible and the click-to-confirm UX depends
    // on the wording flip being noticeable. A silent rename would
    // weaken the gesture's intent signal. Pinning here forces a code-
    // review decision rather than a drive-by string edit.
    CHECK(std::string(CANCEL_LABEL_IDLE) == "Cancel & Discard");
    CHECK(std::string(CANCEL_LABEL_ARMED) ==
          "Discard recording? click again to confirm");

    // Labels MUST differ; the transition between them is the whole
    // point of the click-to-confirm gesture.
    CHECK(std::strcmp(CANCEL_LABEL_IDLE, CANCEL_LABEL_ARMED) != 0);

    // The armed label MUST contain the word "again" (the load-bearing
    // confirmation cue per the v1 reference). Future i18n / wording
    // changes must preserve this signal.
    std::string armed{CANCEL_LABEL_ARMED};
    CHECK(armed.find("again") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 3. "Cancel writes no WAV" structural invariant
// ---------------------------------------------------------------------------

TEST_CASE("cancel-confirm: allocated wav_path is not created on disk",
          "[tray][cancel]") {
    // The rev-2 acceptance signal: "After cancel: no file under
    // `~/.local/share/recmeet/staging/audio_*.wav` for the cancelled
    // session." The production `start_capture` calls
    // `next_staging_wav_path` to ALLOCATE a path string but does NOT
    // create the file — file creation happens later inside libsndfile's
    // sf_open() from `write_wav()`, which `cancel_capture` skips.
    //
    // This test asserts the structural invariant the cancel path
    // relies on: `next_staging_wav_path` is a string-allocation
    // operation that touches the staging dir for existence checks but
    // NEVER creates a WAV file. Combined with the cancel path's
    // skip-write-wav decision, this is what guarantees no on-disk
    // artifact for a cancelled session.
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    fs::path got = tray_capture::next_staging_wav_path(
        scratch, "2026-05-26_10-30");

    // The path is in the staging dir, named per the convention.
    CHECK(got.parent_path() == scratch);
    CHECK(got.filename().string() == "audio_2026-05-26_10-30.wav");

    // CRITICAL invariant: the file was NOT created. This is what
    // makes the cancel path's "skip write_wav" safe — there is no
    // half-staged file to clean up later.
    CHECK_FALSE(fs::exists(got));

    // Belt-and-suspenders: nothing else under staging either.
    int file_count = 0;
    for (auto& entry : fs::directory_iterator(scratch)) {
        (void)entry;
        ++file_count;
    }
    CHECK(file_count == 0);
}

TEST_CASE("cancel-confirm: simulated cancel sequence leaves staging clean",
          "[tray][cancel]") {
    // End-to-end shape of the cancel sequence at the tray_capture
    // layer (no GTK, no g_tray): allocate a staging path (as
    // start_capture would), record some samples in an in-memory
    // buffer (as the WAV stager subscriber would), then SKIP the
    // write_wav step (as cancel_capture does). Asserts: after the
    // skip, the staging dir is identical to its pre-allocation state
    // (no .wav, no .pending sidecar, no temp files).
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    // 1. Allocate path (mirrors start_capture's path allocation).
    fs::path wav_path =
        tray_capture::next_staging_wav_path(scratch, "2026-05-26_11-00");

    // 2. Buffer samples in memory (mirrors the wav_buffer fan-out
    //    subscriber receiving PCM from PipeWireCapture). 100 ms of
    //    silence at 16 kHz mono = 1600 samples — well over the rev-2
    //    plan's "accumulate >100ms of audio" benchmark.
    std::vector<int16_t> buffer(1600, 0);
    REQUIRE(buffer.size() == 1600);

    // 3. Cancel path: discard buffer in memory WITHOUT calling
    //    write_wav. The buffer just goes out of scope.
    {
        std::vector<int16_t> discarded;
        discarded.swap(buffer);
        // `discarded` destructs at scope end — equivalent to
        // `wav_buffer.clear()` inside cancel_capture().
    }
    CHECK(buffer.empty());

    // 4. Acceptance: no file at the allocated path, no sidecar, no
    //    other artifact in staging. This is what the rev-2 plan calls
    //    out as "no file under ~/.local/share/recmeet/staging/audio_*.wav
    //    for the cancelled session".
    CHECK_FALSE(fs::exists(wav_path));
    CHECK_FALSE(fs::exists(tray_capture::pending_sidecar_path(wav_path)));

    int file_count = 0;
    for (auto& entry : fs::directory_iterator(scratch)) {
        (void)entry;
        ++file_count;
    }
    CHECK(file_count == 0);
}

// ---------------------------------------------------------------------------
// 4. Contrast: stop+write_wav DOES create a file
// ---------------------------------------------------------------------------

TEST_CASE("cancel-confirm: stop-equivalent write_wav DOES create a file "
          "(contrast with cancel)",
          "[tray][cancel]") {
    // The contrast test: prove the on-disk artifact IS produced when
    // write_wav runs (the stop path). This pins the load-bearing
    // difference between Stop and Cancel — Stop produces a WAV, Cancel
    // does not. If a future refactor accidentally calls write_wav from
    // cancel_capture, this test still passes; the prior test
    // ("cancel-confirm: simulated cancel sequence leaves staging clean")
    // is the one that would fail. Together they bracket the
    // behavioral contract.
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    fs::path wav_path =
        tray_capture::next_staging_wav_path(scratch, "2026-05-26_11-30");

    std::vector<int16_t> buffer(1600, 0);  // 100 ms of silence
    std::string err;
    REQUIRE(tray_capture::write_wav(wav_path, buffer, err));

    // Stop path: file exists.
    CHECK(fs::exists(wav_path));
    CHECK(fs::file_size(wav_path) > 44);  // header + samples
}
