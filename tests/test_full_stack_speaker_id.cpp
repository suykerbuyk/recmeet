// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 3 (test-and-verification-hardening) — `[full-stack][speaker-id]`:
// closes the cross-meeting voice-id gap. The debate [full-stack] test in
// test_full_stack.cpp deliberately sets cfg.speaker_id = false to avoid
// depending on enrollment state; this test fills that gap by exercising
// the full speakers.enroll → process.reprocess → identified-label-in-
// transcript loop over a real spawned daemon connected via Unix-domain
// socket.
//
// Shape:
//   1. Spawn `recmeet-daemon` over a Unix socket (per Phase 3 plan).
//   2. SKIP if `assets/biden_trump_debate_2020.wav` is absent, matching
//      the existing test_full_stack.cpp gate.
//   3. process.submit the debate audio with a client-minted meeting_id.
//      (Diarize + speaker_id ON; summarize OFF — we don't need API keys.)
//   4. Poll job.status until terminal; assert state == "done".
//   5. From the meeting dir on disk, load speakers.json and pick the
//      cluster with the largest duration_sec — that's the dominant
//      speaker in the debate (Biden / Trump / Wallace, in some order;
//      we don't assert which one).
//   6. speakers.enroll {name="DebatePersonAlpha", meeting_id, cluster_id}.
//      Name is deliberately ASCII-no-spaces (is_safe_dirname gate).
//   7. process.reprocess { meeting_id, identify=true, diarize=true,
//      summarize=false } and wait for terminal "done".
//   8. process.fetch the artifacts and read the new transcript.
//   9. ASSERT: the transcript contains the enrolled NAME (not just
//      "Speaker_01") — proving the cross-meeting voice-id round-trip.
//
// Why this matters: until enrollment + reidentify works end-to-end against
// the real daemon, the IPC contract for voice-id is only validated in unit
// tests that mock either side. A regression in cluster_id propagation,
// embedding persistence, or the L2-similarity threshold could silently
// reduce identified labels to anonymous "Speaker_NN" and no other test
// would catch it.
//
// Tag layout: [full-stack][speaker-id][slow]. `[slow]` because two
// postprocess runs on the 4-min debate fixture takes >30s on warm cache
// (whisper base + sherpa diarize + identify ≈ 25-40s per pass on this
// hardware; CI is slower). The `[slow]` tag matches the convention used
// by test_full_stack.cpp:505 ("reprocess-batch ... [slow]").

#include <catch2/catch_test_macros.hpp>

#include "full_stack_helpers.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "json_util.h"
#include "model_manager.h"
#include "speaker_id.h"
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

// Speaker-id requires sherpa-onnx; on a build with RECMEET_USE_SHERPA=OFF the
// MeetingSpeaker struct and load_meeting_speakers() are not declared. Gate the
// whole TU on the same build option as test_full_stack.cpp.
#if RECMEET_USE_SHERPA

namespace recmeet {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// Mint a canonical lowercase UUID v4 — required by is_valid_meeting_id()
// (util.cpp:418). We hand-roll instead of pulling in libuuid because the
// generator is trivial and the test would otherwise add a link-time
// dependency for one call.
std::string make_uuid_v4() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t a = dist(gen);
    uint64_t b = dist(gen);
    // Stamp version (4) into the 13th hex digit and variant (8,9,a,b) into
    // the 17th.
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

// Slurp a WAV file into a byte string for upload as `format=wav`. The daemon
// accepts WAV directly (upload_session.h:kSubmitFormatWav); the
// upload-session machinery uses libsndfile to convert to S16LE PCM on the
// daemon side.
std::string read_file_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.is_open());
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Send a byte buffer as a series of `0x01` upload frames, each ≤ chunk_bytes.
// The 16 MiB binary-frame cap (kDefaultMaxBinaryFrameBytes) requires
// splitting for any payload larger than that; we choose a smaller chunk
// to keep memory churn bounded on the test runner.
void send_upload_chunked(IpcClient& client, const std::string& bytes,
                         std::size_t chunk_bytes = 4 * 1024 * 1024) {
    std::size_t off = 0;
    while (off < bytes.size()) {
        std::size_t n = std::min(chunk_bytes, bytes.size() - off);
        REQUIRE(client.send_upload_chunk(bytes.substr(off, n)));
        off += n;
    }
}

// Poll job.status until the job reaches "done" / "failed" / "cancelled" or
// the deadline expires. Returns the terminal state ("" on timeout). Used
// for both the initial submit and the post-enroll reprocess.
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

// After a postprocess run, job.status returns the meeting_id the daemon
// resolved (or echoed back). Returns "" on a non-terminal job — caller
// must have already waited for terminal.
std::string get_meeting_id(IpcClient& client, int64_t job_id) {
    JsonMap p;
    p["job_id"] = job_id;
    IpcResponse resp;
    IpcError err;
    if (!client.call("job.status", p, resp, err, /*timeout_ms=*/5000))
        return {};
    return json_val_as_string(resp.result["meeting_id"]);
}

// Write a daemon.yaml that pins meetings_root + speaker_db to test-owned
// dirs so concurrent test runs (and the operator's real `~/meetings/`)
// stay untouched. We write `daemon.yaml` directly to skip the legacy-
// migration shim; the daemon's `load_server_config` reads this file
// straight from $XDG_CONFIG_HOME/recmeet-server/daemon.yaml (v2 split-paths).
void write_daemon_yaml(const fs::path& xdg_config_dir,
                       const fs::path& meetings_root,
                       const fs::path& speaker_db) {
    fs::path cfg_dir = xdg_config_dir / "recmeet-server";
    fs::create_directories(cfg_dir);
    std::ofstream cfg(cfg_dir / "daemon.yaml");
    REQUIRE(cfg.is_open());
    cfg << "# recmeet full-stack speaker-id test config\n"
        << "summary:\n"
        << "  disabled: true\n"
        << "server:\n"
        << "  meetings_root: \"" << meetings_root.string() << "\"\n"
        << "speaker_id:\n"
        << "  enabled: true\n"
        << "  database: \"" << speaker_db.string() << "\"\n";
}

} // anonymous namespace

TEST_CASE("V2 full-stack speaker-id: enroll → reprocess → named label in transcript",
          "[full-stack][speaker-id][slow]") {
    // --------------------------------------------------------------------
    // 0. Fixture / model gates — same as test_full_stack.cpp:160-171.
    // --------------------------------------------------------------------
    fs::path root = test_helpers::find_project_root();
    if (root.empty()) SKIP("Project root with assets/ not found");
    fs::path audio_src = root / "assets" / "biden_trump_debate_2020.wav";
    if (!fs::exists(audio_src)) SKIP("Debate audio asset not found");
    if (!is_whisper_model_cached("base"))
        SKIP("Whisper base model not cached");
    if (!is_sherpa_model_cached())
        SKIP("Sherpa diarization models not cached");
    if (!is_vad_model_cached())
        SKIP("VAD model not cached");

    // --------------------------------------------------------------------
    // 1. Stage a per-test work directory layout.
    // --------------------------------------------------------------------
    fs::path workdir = recmeet::test::tmp_path(
        "recmeet_full_stack_speaker_id");
    fs::remove_all(workdir);
    fs::create_directories(workdir);

    fs::path xdg_config  = workdir / "config";
    fs::path meetings    = workdir / "meetings";
    fs::path speaker_db  = workdir / "speakers_db";
    fs::path fetch_dst   = workdir / "fetched";
    fs::path sock_dir    = workdir / "sock";
    fs::create_directories(meetings);
    fs::create_directories(speaker_db);
    fs::create_directories(fetch_dst);
    fs::create_directories(sock_dir);

    write_daemon_yaml(xdg_config, meetings, speaker_db);

    // --------------------------------------------------------------------
    // 2. Spawn the daemon over a Unix domain socket.
    // --------------------------------------------------------------------
    fs::path sock_path = sock_dir / "daemon.sock";
    fs::path daemon_bin = full_stack::find_daemon_binary();

    full_stack::SpawnedDaemon daemon(
        daemon_bin,
        full_stack::SpawnedDaemon::Transport::Unix,
        /*tcp_addr=*/std::string{},
        /*psk=*/std::string{},
        xdg_config,
        sock_path);
    CHECK(daemon.pid() > 0);
    CHECK(daemon.socket_path() == sock_path);

    // --------------------------------------------------------------------
    // 3. Connect IpcClient over the Unix socket and run session.init.
    // --------------------------------------------------------------------
    IpcClient client(sock_path.string());
    REQUIRE(client.connect());
    CHECK_FALSE(client.is_remote());
    CHECK_FALSE(client.client_id().empty());
    CHECK(client.protocol_version() == IPC_PROTOCOL_VERSION);

    {
        JsonMap creds;
        JsonMap prefs;
        prefs["output_dir"]    = meetings.string();
        prefs["note_dir"]      = meetings.string();
        prefs["whisper_model"] = std::string("base");
        prefs["language"]      = std::string("en");

        IpcResponse resp;
        IpcError err;
        REQUIRE(client.session_init(creds, prefs, resp, err));
        CHECK(json_val_as_bool(resp.result["ok"]) == true);
    }

    // --------------------------------------------------------------------
    // 4. process.submit the debate audio with a client-minted meeting_id.
    // --------------------------------------------------------------------
    const std::string audio_bytes = read_file_bytes(audio_src);
    REQUIRE(!audio_bytes.empty());
    const std::string meeting_id_1 = make_uuid_v4();

    int64_t job_id_1 = 0;
    {
        JsonMap p;
        p["audio_size"]  = static_cast<int64_t>(audio_bytes.size());
        p["format"]      = std::string("wav");
        p["sample_rate"] = static_cast<int64_t>(16000);
        p["channels"]    = static_cast<int64_t>(1);
        p["mode"]        = std::string("transcribe");
        p["meeting_id"]  = meeting_id_1;

        IpcResponse resp;
        IpcError err;
        REQUIRE(client.call("process.submit", p, resp, err, /*timeout_ms=*/5000));

        const std::string upload_token =
            json_val_as_string(resp.result["upload_token"]);
        REQUIRE_FALSE(upload_token.empty());
        job_id_1 = json_val_as_int(resp.result["job_id"]);
        REQUIRE(job_id_1 > 0);
    }
    send_upload_chunked(client, audio_bytes);

    // 5-minute budget — whisper + diarize + identify on the 4-min debate
    // takes 20-45s on this hardware, 90-180s on CI cold start. 300s is
    // safely beyond both with margin for slow disk / contended cores.
    const std::string state_1 = wait_for_terminal(client, job_id_1, 300s);
    INFO("First job state: " << state_1);
    REQUIRE(state_1 == "done");

    // --------------------------------------------------------------------
    // 5. Read speakers.json from the meeting dir; pick the longest cluster.
    // --------------------------------------------------------------------
    // job.status now reports the daemon-resolved meeting_id (which should
    // equal the client-minted one we passed at submit).
    const std::string meeting_id_resolved = get_meeting_id(client, job_id_1);
    INFO("Resolved meeting_id: " << meeting_id_resolved);
    REQUIRE(meeting_id_resolved == meeting_id_1);

    // Find the meeting dir under meetings/. The upload session manager
    // allocates a canonical `YYYY-MM-DD_HH-MM` dir name; meanwhile the
    // note writer creates a `<YYYY>/<MM>/` bucket sibling. Match the
    // canonical regex to avoid picking up the year-bucket.
    fs::path meeting_dir;
    auto looks_like_meeting_ts = [](const std::string& s) {
        // YYYY-MM-DD_HH-MM = 16 chars; cheap structural match is enough
        // (test scope; no need for a full regex).
        if (s.size() != 16) return false;
        for (int i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15}) {
            if (s[i] < '0' || s[i] > '9') return false;
        }
        return s[4] == '-' && s[7] == '-' && s[10] == '_' && s[13] == '-';
    };
    for (const auto& e : fs::directory_iterator(meetings)) {
        if (!e.is_directory()) continue;
        const std::string name = e.path().filename().string();
        if (!name.empty() && name[0] == '.') continue;  // skip .orphan-*
        if (!looks_like_meeting_ts(name)) continue;     // skip YYYY/ bucket
        meeting_dir = e.path();
        break;
    }
    REQUIRE(!meeting_dir.empty());
    INFO("Meeting dir: " << meeting_dir.string());

    auto speakers = load_meeting_speakers(meeting_dir);
    REQUIRE(speakers.size() >= 2);

    // Pick the longest cluster with an embedding and ≥ 5s of audio
    // (speakers.enroll's quality gate at daemon_handlers.cpp:1801-1806
    // rejects shorter clusters).
    const MeetingSpeaker* longest = nullptr;
    for (const auto& s : speakers) {
        if (s.embedding.empty()) continue;
        if (s.duration_sec < 5.0f) continue;
        if (!longest || s.duration_sec > longest->duration_sec)
            longest = &s;
    }
    REQUIRE(longest != nullptr);
    const int chosen_cluster = longest->cluster_id;
    INFO("Chosen cluster_id=" << chosen_cluster
         << " duration_sec=" << longest->duration_sec
         << " original_label=" << longest->label);

    // --------------------------------------------------------------------
    // 6. speakers.enroll a known name against that cluster.
    // --------------------------------------------------------------------
    // Name must pass is_safe_dirname — alphanumeric / underscore / hyphen
    // / dot only. "DebatePersonAlpha" is conservative and distinct from any
    // pipeline-generated "Speaker_NN" fallback label, so a positive match
    // below is unambiguous.
    const std::string enroll_name = "DebatePersonAlpha";
    {
        JsonMap p;
        p["name"]       = enroll_name;
        p["meeting_id"] = meeting_id_1;
        p["cluster_id"] = static_cast<int64_t>(chosen_cluster);

        IpcResponse resp;
        IpcError err;
        REQUIRE(client.call("speakers.enroll", p, resp, err,
                            /*timeout_ms=*/10000));
        CHECK(json_val_as_bool(resp.result["ok"]) == true);
        INFO("Enroll duration_sec="
             << json_val_as_double(resp.result["duration_sec"])
             << " confidence="
             << json_val_as_double(resp.result["confidence"]));
    }

    // --------------------------------------------------------------------
    // 7. process.reprocess with identify=true, summarize=false.
    // --------------------------------------------------------------------
    // The re-identify pass runs against the now-populated speaker DB; the
    // cluster previously labeled "Speaker_NN" should now match the enrolled
    // profile and emit "DebatePersonAlpha" into the transcript.
    int64_t job_id_2 = 0;
    {
        JsonMap p;
        p["meeting_id"] = meeting_id_1;
        p["identify"]   = true;
        p["diarize"]    = true;
        p["transcribe"] = true;
        p["summarize"]  = false;

        IpcResponse resp;
        IpcError err;
        REQUIRE(client.call("process.reprocess", p, resp, err,
                            /*timeout_ms=*/5000));
        job_id_2 = json_val_as_int(resp.result["job_id"]);
        REQUIRE(job_id_2 > 0);
        REQUIRE(job_id_2 != job_id_1);
    }

    const std::string state_2 = wait_for_terminal(client, job_id_2, 300s);
    INFO("Reprocess job state: " << state_2);
    REQUIRE(state_2 == "done");

    // --------------------------------------------------------------------
    // 8. Locate the freshly-written note + transcript artifacts and assert
    //    the enrolled name appears.
    // --------------------------------------------------------------------
    // After a reprocess, the transcript-bearing artifacts live in
    // <meeting_dir> (flat layout, per pipeline.cpp). The Meeting_*.md note
    // file aggregates the transcript and is the canonical place the named
    // label surfaces. We assert directly against the on-disk note rather
    // than process.fetch'ing — the disk view is the ground truth for
    // speaker-id correctness; the IPC fetch is exercised separately by
    // [e2e][thin-client][tcp].
    //
    // The non-recursive directory walk in fetch_artifacts skips the
    // <year>/<month>/Meeting_*.md, so we scan the entire note_dir tree
    // (which here equals the meetings root — they're set to the same path
    // in session.init prefs). The pipeline writes notes under
    // `<note_dir>/<YYYY>/<MM>/Meeting_<ts>*.md`. The reprocess run produces
    // a NEW note alongside the original — we want the most recent, which
    // is the one with the named label.
    std::vector<fs::path> candidate_notes;
    for (const auto& e : fs::recursive_directory_iterator(meetings)) {
        if (!e.is_regular_file()) continue;
        const std::string name = e.path().filename().string();
        if (name.rfind("Meeting_", 0) == 0 && e.path().extension() == ".md") {
            candidate_notes.push_back(e.path());
        }
    }
    REQUIRE_FALSE(candidate_notes.empty());

    bool name_found = false;
    std::string match_evidence;
    for (const auto& p : candidate_notes) {
        std::ifstream in(p);
        if (!in) continue;
        std::ostringstream buf;
        buf << in.rdbuf();
        const std::string body = buf.str();
        if (body.find(enroll_name) != std::string::npos) {
            name_found = true;
            match_evidence = p.string();
            break;
        }
    }

    // Also reload speakers.json — after reprocess the cluster's label
    // field should be the enrolled name with identified=true. This is the
    // proximate signal; the transcript assertion above is the user-visible
    // payoff.
    auto speakers_after = load_meeting_speakers(meeting_dir);
    bool speakers_json_has_name = false;
    for (const auto& s : speakers_after) {
        if (s.label == enroll_name && s.identified) {
            speakers_json_has_name = true;
            break;
        }
    }

    INFO("Candidate notes scanned: " << candidate_notes.size());
    INFO("Name match evidence path: " << match_evidence);
    INFO("speakers.json identified=" << speakers_json_has_name);

    // The cross-meeting voice-id closure: AT LEAST ONE of these must hold.
    // Both should hold on a healthy pipeline; either alone is sufficient
    // evidence of the named-label round-trip.
    CHECK((name_found || speakers_json_has_name));

    // The stronger assertion (transcript-visible) is the Phase 3 plan's
    // explicit ask — surface it as a separate CHECK so a regression to
    // "speakers.json updated but note not regenerated with names" is
    // visible to the test reader.
    CHECK(name_found);

    // --------------------------------------------------------------------
    // 9. Cleanup — close client first; SpawnedDaemon dtor SIGTERMs + waits.
    // --------------------------------------------------------------------
    client.close_connection();

    // Best-effort workdir cleanup. Leaving on failure helps debugging.
    if (name_found && speakers_json_has_name) {
        std::error_code ec;
        fs::remove_all(workdir, ec);
    }
}

} // namespace recmeet

#endif  // RECMEET_USE_SHERPA
