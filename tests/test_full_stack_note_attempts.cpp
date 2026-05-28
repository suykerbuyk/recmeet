// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// `[full-stack][note-attempts]` — daemon-spawning integration coverage for
// the `.XX` attempt-counter feature shipped in this commit series.
//
// Closes the seam between the helper unit tests (tests/test_note.cpp
// `[note][note-attempts]` — exercise next_attempt_and_migrate() in
// isolation) and the IPC handler unit tests (tests/test_speakers_meetings_ipc.cpp
// `[e6][ipc][speakers-meetings][note-attempts]` — drive meetings.read_note
// against an in-process DaemonTestHarness). Neither of those tests proves
// the END-TO-END contract: that a real recmeet-server subprocess fed a
// real WAV via process.submit + upload chunks
//   (a) writes a `.00`-suffixed Meeting note via the production pipeline,
//       and
//   (b) writes a `.01`-suffixed Meeting note when the same recording is
//       reprocessed via process.reprocess, with the `.00` left in place.
//
// This is the strongest end-to-end proof available for the feature:
// the production note-write site (src/pipeline.cpp:964 ->
// src/note.cpp::write_meeting_note -> note_internal::next_attempt_and_migrate)
// executes in a forked subprocess, under the real subprocess-postprocessing
// flow, with on-disk state seeded by a real process.submit + chunked upload.
// An in-process [full-stack] fixture would not exercise the subprocess +
// IPC marshalling boundary; feedback_subagent_silent_scope_downgrade calls
// out exactly that kind of fallback. The plan's STOP-and-report trigger
// (e) makes the daemon-spawning harness non-negotiable — we use
// SpawnedDaemon from tests/full_stack_helpers.h (Phase 3 of
// test-and-verification-hardening, iter 215; reference pattern:
// tests/test_full_stack_webui.cpp).
//
// Scope note (iter 228 — what this test deliberately does NOT assert):
//
//   * meetings.read_note over real IPC. The Phase 2 IPC tests in
//     tests/test_speakers_meetings_ipc.cpp cover the handler's
//     highest-attempt-wins + legacy-mtime-tiebreak ordering against
//     the production handler via DaemonTestHarness. Driving it
//     end-to-end here was attempted and surfaced a pre-existing
//     architecture mismatch: the production pipeline routes notes to
//     `<meeting_dir>/<YYYY>/<MM>/Meeting_*.md` (because pipeline.cpp:953
//     sets `md.note_dir = input.out_dir` as a fallback, and the writer's
//     YYYY/MM branch fires for any non-empty note_dir), while the
//     meetings.read_note handler iterates `meeting_path` non-recursively
//     and therefore never sees the note at all. That is a real bug
//     orthogonal to this commit series — opening that scope here would
//     conflate two independent concerns. The mismatch is surfaced in
//     the orchestrator report so the operator can decide the followup.
//
//   * process.fetch returning N `.md` artifacts. Same root cause: the
//     fetch enumerator (`enumerate_artifacts` in src/fetch_artifacts.cpp)
//     is intentionally non-recursive and excludes the YYYY/MM subtree,
//     so the daemon's own pipeline output is never reachable via fetch
//     in the V1 layout. Plan rev-4 decision 10 cited fetch as
//     pre-verified by source inspection of `enumerate_artifacts` — but
//     the inspection missed that no `.md` actually lands in `out_dir`
//     under the current pipeline. Same architectural conversation as
//     above; same out-of-scope verdict.

#include <catch2/catch_test_macros.hpp>

#include "full_stack_helpers.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "json_util.h"
#include "model_manager.h"
#include "test_helpers.h"
#include "test_tmpdir.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace recmeet {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// Canonical lowercase UUID v4 — required by is_valid_meeting_id() on the
// daemon-side handlers (process.submit, process.reprocess, meetings.read_note).
// Same shape as tests/test_full_stack_webui.cpp::make_uuid_v4.
std::string make_uuid_v4() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t a = dist(gen);
    uint64_t b = dist(gen);
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>((a >> 32) & 0xFFFFFFFFULL),
                  static_cast<unsigned>((a >> 16) & 0xFFFFULL),
                  static_cast<unsigned>(a & 0xFFFFULL),
                  static_cast<unsigned>((b >> 48) & 0xFFFFULL),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return std::string(buf, 36);
}

std::string read_file_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.is_open());
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

void send_upload_chunked(IpcClient& client, const std::string& bytes,
                         std::size_t chunk_bytes = 4 * 1024 * 1024) {
    std::size_t off = 0;
    while (off < bytes.size()) {
        std::size_t n = std::min(chunk_bytes, bytes.size() - off);
        REQUIRE(client.send_upload_chunk(bytes.substr(off, n)));
        off += n;
    }
}

std::string wait_for_terminal(IpcClient& client, int64_t job_id,
                              std::chrono::seconds budget) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        JsonMap p;
        p["job_id"] = job_id;
        IpcResponse resp;
        IpcError err;
        if (!client.call("job.status", p, resp, err, /*timeout_ms=*/5000)) {
            FAIL("job.status failed: " << err.message);
        }
        std::string state = json_val_as_string(resp.result["state"]);
        if (state == "done" || state == "failed" || state == "cancelled")
            return state;
        std::this_thread::sleep_for(500ms);
    }
    return {};
}

// Pin meetings_root + disable summary; same pattern as
// tests/test_full_stack_webui.cpp::write_daemon_yaml minus speaker_id.
// We intentionally leave diarization + speaker_id at defaults — the
// note-attempts contract is filename-shape only, not transcript content.
void write_daemon_yaml(const fs::path& xdg_config_dir,
                       const fs::path& meetings_root) {
    fs::path cfg_dir = xdg_config_dir / "recmeet-server";
    fs::create_directories(cfg_dir);
    std::ofstream cfg(cfg_dir / "daemon.yaml");
    REQUIRE(cfg.is_open());
    cfg << "# recmeet full-stack note-attempts test config\n"
        << "summary:\n"
        << "  disabled: true\n"
        << "server:\n"
        << "  meetings_root: \"" << meetings_root.string() << "\"\n";
}

} // anonymous namespace

TEST_CASE("V2 full-stack note-attempts: submit writes .00, reprocess writes .01",
          "[full-stack][note-attempts][slow]") {
    // --------------------------------------------------------------------
    // 0. Fixture / model / binary gates. SKIP semantics match the other
    //    `[full-stack]` tests — a host without the assets/models/binary
    //    reports a SKIP rather than a FAIL.
    // --------------------------------------------------------------------
    fs::path root = test_helpers::find_project_root();
    if (root.empty()) SKIP("Project root with assets/ not found");
    fs::path audio_src = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_src)) SKIP("Debate audio asset not found");
    if (!is_whisper_model_cached("base"))
        SKIP("Whisper base model not cached");

    fs::path daemon_bin = full_stack::find_daemon_binary();

    // --------------------------------------------------------------------
    // 1. Per-test work directory layout.
    // --------------------------------------------------------------------
    fs::path workdir = recmeet::test::tmp_path("recmeet_full_stack_note_attempts");
    fs::remove_all(workdir);
    fs::create_directories(workdir);

    fs::path xdg_config = workdir / "config";
    fs::path meetings = workdir / "meetings";
    fs::path sock_dir = workdir / "sock";
    fs::create_directories(meetings);
    fs::create_directories(sock_dir);

    write_daemon_yaml(xdg_config, meetings);

    // --------------------------------------------------------------------
    // 2. Spawn the daemon over a Unix socket.
    // --------------------------------------------------------------------
    fs::path sock_path = sock_dir / "daemon.sock";
    full_stack::SpawnedDaemon daemon(
        daemon_bin,
        full_stack::SpawnedDaemon::Transport::Unix,
        /*tcp_addr=*/std::string{},
        /*psk=*/std::string{},
        xdg_config,
        sock_path);
    REQUIRE(daemon.pid() > 0);

    // --------------------------------------------------------------------
    // 3. IPC client + session init. We deliberately leave `note_dir`
    //    UNSET in session prefs so the pipeline falls back to writing
    //    the note INSIDE the per-meeting `<output_dir>/<ts>/` directory
    //    (note.cpp:160 — `data.note_dir.empty() ? data.output_dir`).
    //    Why this matters: `process.fetch` enumerates artifacts non-
    //    recursively in `Job::input.out_dir` (= the meeting dir);
    //    setting `note_dir` would push notes to
    //    `<note_dir>/<YYYY>/<MM>/Meeting_*.md`, which is outside
    //    `out_dir` and would never be returned by fetch. The decision-10
    //    contract (fetch returns ALL `.md` attempts for the meeting) is
    //    therefore only observable in the no-note_dir layout — and that
    //    is the layout the V2 thin-client uses (the client owns its
    //    own note_dir / Obsidian vault path post-fetch).
    // --------------------------------------------------------------------
    IpcClient client(sock_path.string());
    REQUIRE(client.connect());
    CHECK_FALSE(client.is_remote());
    REQUIRE(client.protocol_version() == IPC_PROTOCOL_VERSION);

    {
        JsonMap creds;
        JsonMap prefs;
        prefs["output_dir"] = meetings.string();
        // intentionally no prefs["note_dir"] — see comment above.
        prefs["whisper_model"] = std::string("base");
        prefs["language"] = std::string("en");

        IpcResponse resp;
        IpcError err;
        REQUIRE(client.session_init(creds, prefs, resp, err));
        CHECK(json_val_as_bool(resp.result["ok"]) == true);
    }

    // --------------------------------------------------------------------
    // 4. First write — process.submit + chunked upload + wait. This
    //    triggers a transcribe pass and writes the first note (.00).
    // --------------------------------------------------------------------
    const std::string audio_bytes = read_file_bytes(audio_src);
    REQUIRE(!audio_bytes.empty());
    const std::string meeting_id = make_uuid_v4();

    int64_t submit_job_id = 0;
    {
        JsonMap p;
        p["audio_size"] = static_cast<int64_t>(audio_bytes.size());
        p["format"] = std::string("wav");
        p["sample_rate"] = static_cast<int64_t>(16000);
        p["channels"] = static_cast<int64_t>(1);
        p["mode"] = std::string("transcribe");
        p["meeting_id"] = meeting_id;

        IpcResponse resp;
        IpcError err;
        REQUIRE(client.call("process.submit", p, resp, err,
                            /*timeout_ms=*/5000));
        REQUIRE_FALSE(json_val_as_string(resp.result["upload_token"]).empty());
        submit_job_id = json_val_as_int(resp.result["job_id"]);
        REQUIRE(submit_job_id > 0);
    }
    send_upload_chunked(client, audio_bytes);

    const std::string submit_state = wait_for_terminal(client, submit_job_id, 300s);
    INFO("Submit job state: " << submit_state);
    REQUIRE(submit_state == "done");

    // Locate the meeting directory the daemon created. Same scan as
    // test_full_stack_webui.cpp: YYYY-MM-DD_HH-MM. We need it to verify
    // the on-disk filename shape independently of the IPC return.
    fs::path meeting_dir;
    auto looks_like_meeting_ts = [](const std::string& s) {
        if (s.size() != 16) return false;
        for (int i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15})
            if (s[i] < '0' || s[i] > '9') return false;
        return s[4] == '-' && s[7] == '-' && s[10] == '_' && s[13] == '-';
    };
    for (const auto& e : fs::directory_iterator(meetings)) {
        if (!e.is_directory()) continue;
        const std::string nm = e.path().filename().string();
        if (!nm.empty() && nm[0] == '.') continue;
        if (!looks_like_meeting_ts(nm)) continue;
        meeting_dir = e.path();
        break;
    }
    REQUIRE(!meeting_dir.empty());
    const std::string ts = meeting_dir.filename().string();
    INFO("Meeting timestamp: " << ts);

    // Note layout: the V1/V2 pipeline routes every note through
    // `<note_parent>/YYYY/MM/Meeting_<ts>.NN[_<title>].md` because
    // `pipeline.cpp::run_postprocessing()` sets
    // `md.note_dir = input.out_dir` as the fallback when `cfg.note_dir`
    // is empty (pipeline.cpp:949-954), and the writer's YYYY/MM-subdir
    // branch (note.cpp:300-305) fires for any non-empty `note_dir`
    // regardless of whether it equals `output_dir`. The note therefore
    // lands at `<meeting_dir>/<YYYY>/<MM>/Meeting_<ts>.NN*.md`.
    //
    // This is the layout the directory scan must target.
    const std::string year = ts.substr(0, 4);
    const std::string month = ts.substr(5, 2);
    const fs::path note_layout_dir = meeting_dir / year / month;

    // After the first write, the only `Meeting_*.md` on disk must be
    // `Meeting_<ts>.00[_<title>].md` — confirms the writer composed the
    // `.00` suffix via next_attempt_and_migrate().
    auto find_meeting_notes = [&note_layout_dir](const std::string& ts) {
        std::vector<std::string> notes;
        const std::string prefix = "Meeting_" + ts;
        std::error_code ec;
        if (!fs::is_directory(note_layout_dir, ec)) return notes;
        for (const auto& e : fs::directory_iterator(note_layout_dir)) {
            if (!e.is_regular_file()) continue;
            const std::string nm = e.path().filename().string();
            if (nm.size() < prefix.size() + 3) continue;
            if (nm.compare(0, prefix.size(), prefix) != 0) continue;
            if (nm.compare(nm.size() - 3, 3, ".md") != 0) continue;
            notes.push_back(nm);
        }
        std::sort(notes.begin(), notes.end());
        return notes;
    };
    {
        auto notes = find_meeting_notes(ts);
        REQUIRE(notes.size() == 1);
        const std::string& nm = notes.front();
        // Filename must start with `Meeting_<ts>.00` (the optional title
        // suffix comes after the `.00` — we don't pin it because the AI
        // metadata is summary-derived and we've disabled summary).
        const std::string expected_prefix = "Meeting_" + ts + ".00";
        INFO("First-write note filename: " << nm);
        CHECK(nm.compare(0, expected_prefix.size(), expected_prefix) == 0);
    }

    // --------------------------------------------------------------------
    // 5. Second write — process.reprocess on the same meeting. The
    //    helper must see the `.00` on disk and pick `.01` for the new
    //    note. summary off (keeps the test deterministic + fast);
    //    transcribe + diarize ON to actually re-run the pipeline.
    // --------------------------------------------------------------------
    int64_t reprocess_job_id = 0;
    {
        JsonMap p;
        p["meeting_id"] = meeting_id;
        p["transcribe"] = true;
        p["diarize"] = true;
        p["summarize"] = false;

        IpcResponse resp;
        IpcError err;
        REQUIRE(client.call("process.reprocess", p, resp, err,
                            /*timeout_ms=*/5000));
        reprocess_job_id = json_val_as_int(resp.result["job_id"]);
        REQUIRE(reprocess_job_id > 0);
        REQUIRE(reprocess_job_id != submit_job_id);
    }

    const std::string reprocess_state =
        wait_for_terminal(client, reprocess_job_id, 300s);
    INFO("Reprocess job state: " << reprocess_state);
    REQUIRE(reprocess_state == "done");

    // After the reprocess, exactly TWO notes on disk: `.00` and `.01`.
    // The .00 from the first write must be untouched (no rename); the
    // .01 is the freshly-written reprocess output.
    //
    // This is the end-to-end proof:
    //   * The daemon's forked postprocess subprocess called the
    //     production write_meeting_note() -> next_attempt_and_migrate()
    //     pair (note.cpp:333 -> note_internal::next_attempt_and_migrate).
    //   * The helper saw the existing `.00` on disk via its directory
    //     scan and returned 1, so the writer composed `.01`.
    //   * No rename happened on the `.00` (the migration loop only
    //     touches LEGACY un-numbered files; `.00` is already new-form).
    // A bug in any of those layers (helper scan, writer composition,
    // pipeline note_dir wiring, subprocess hand-off) breaks this
    // assertion in a way the unit-only tests cannot.
    std::string note00_name, note01_name;
    {
        auto notes = find_meeting_notes(ts);
        REQUIRE(notes.size() == 2);
        for (const auto& nm : notes) {
            INFO("Post-reprocess note: " << nm);
            const std::string expected_00 = "Meeting_" + ts + ".00";
            const std::string expected_01 = "Meeting_" + ts + ".01";
            if (nm.compare(0, expected_00.size(), expected_00) == 0) {
                note00_name = nm;
            } else if (nm.compare(0, expected_01.size(), expected_01) == 0) {
                note01_name = nm;
            }
        }
        CHECK_FALSE(note00_name.empty());
        CHECK_FALSE(note01_name.empty());
    }

    // --------------------------------------------------------------------
    // 6. Cleanup. Close our IPC connection BEFORE the daemon dtor runs
    //    (matches the pattern in test_full_stack_webui.cpp).
    // --------------------------------------------------------------------
    client.close_connection();

    // Keep workdir around when RECMEET_NOTE_ATTEMPTS_KEEP=1 in env (for
    // operator debugging — useful when the on-disk asserts fire).
    const char* keep = std::getenv("RECMEET_NOTE_ATTEMPTS_KEEP");
    if (!keep || keep[0] == '\0') {
        std::error_code ec;
        fs::remove_all(workdir, ec);
    } else {
        INFO("Workdir preserved at: " << workdir.string());
    }
}

} // namespace recmeet
