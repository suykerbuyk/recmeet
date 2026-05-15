// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.4 — process.fetch (artifact download) tests.
//
// Three layers, mirroring the C.2/C.3 patterns:
//   * Unit tests for the artifact-enumeration filter (no socket, no JobQueue).
//     Exercise the should_include_artifact() decision against a synthetic
//     directory and assert the policy from fetch_artifacts.h.
//   * Validation tests that drive a process.fetch handler against a real
//     IpcServer + JobQueue, asserting each error path produces the right
//     IpcErrorCode (PermissionDenied, JobNotReady, InvalidParams,
//     InternalError) without bringing up the actual postprocess subprocess.
//   * End-to-end wire tests: client A submits a process.fetch over a Unix
//     socket, the server-side handler reads the staged note + captions, sends
//     them back as `0x02` BinaryArtifact frames, and the client demultiplexes
//     and writes them to a local directory. Byte-exactness, ordering, and
//     count-mismatch error handling are all asserted.
//
// Thread hygiene (orchestrator rule 5): every std::thread spawned here is
// owned by a RAII guard (`ServerGuard`, `JqGuard`) that joins on destruction,
// so a REQUIRE between thread construction and `.join()` cannot trigger
// std::terminate().
//
// IMPORTANT — the process.fetch handler in src/daemon.cpp depends on the
// global `g_jobs` symbol. These tests do NOT exercise that handler directly
// (we'd have to spin up the daemon process). Instead each test registers
// an equivalent handler on the test's IpcServer, capturing the local
// JobQueue by reference. The handler body MUST stay in sync with
// daemon.cpp's `process.fetch` — both shapes are documented at the test
// helper `register_process_fetch_handler()` below.

#include <catch2/catch_test_macros.hpp>

#include "fetch_artifacts.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <unistd.h>

using namespace recmeet;
namespace fs = std::filesystem;

namespace {

// Ignore SIGPIPE so a test that drops a connection mid-handler does not
// kill the test process.
struct SigpipeIgnorer {
    SigpipeIgnorer() { signal(SIGPIPE, SIG_IGN); }
} s_ignore_sigpipe;

const char* FETCH_SOCK = "/tmp/recmeet_test_fetch.sock";

// ---------------------------------------------------------------------------
// ServerGuard / JqGuard — RAII thread owners. Same idea as test_routed_events.
// ---------------------------------------------------------------------------

struct ServerGuard {
    IpcServer& server;
    std::thread thr;
    explicit ServerGuard(IpcServer& s) : server(s) {
        thr = std::thread([this]() { server.run(); });
    }
    ~ServerGuard() {
        server.stop();
        if (thr.joinable()) thr.join();
    }
};

// JqShutdownGuard — shuts the JobQueue down on destruction so any test that
// leaves a job parked in `Queued` (with no dequeueing worker) does not leak
// a still-runnable slot when the test scope unwinds. Distinct from the
// pp-worker pattern in test_routed_events.cpp: these C.4 tests stage jobs
// synchronously (enqueue → dequeue → finish in-test) instead of relying on
// a background drain, so a worker thread would race the test's own
// dequeue calls and consume jobs the test wanted to leave parked. The
// shutdown call wakes any future dequeue with std::nullopt — none today,
// but the guard mirrors the C.3 pattern for forward compat.
struct JqShutdownGuard {
    JobQueue& q;
    explicit JqShutdownGuard(JobQueue& q_) : q(q_) {}
    ~JqShutdownGuard() { q.shutdown(); }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// A clean per-test temp dir.
fs::path test_temp_dir(const std::string& tag) {
    fs::path d = fs::temp_directory_path() / ("recmeet_fetch_test_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

// Read an entire file into a string. Caller has already verified existence.
std::string read_file_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string buf((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    return buf;
}

void write_file(const fs::path& p, const std::string& contents) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << contents;
}

// Manually drive a job through enqueue -> dequeue -> finish so its registry
// entry ends up in `JobState::Done` with the desired `out_dir`. This skips
// the actual postprocess subprocess; the artifacts are pre-staged in the
// caller's fixture dir.
//
// Returns the assigned job_id.
int64_t stage_done_job(JobQueue& q,
                       const std::string& client_id,
                       const fs::path& out_dir) {
    Job j;
    j.input.out_dir = out_dir;
    int64_t id = q.enqueue(std::move(j), JobKind::Postprocess, client_id);
    // Drive it through Running -> Done so client_for_job() + status() return
    // a Done verdict. dequeue() flips to Running; finish() flips to Done.
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    REQUIRE(dq->job_id == id);
    q.finish(id, /*ok=*/true);
    return id;
}

// Stage a job that stays in Running. Returns the id; caller is responsible
// for finishing it (or letting JqGuard's worker drain on shutdown).
int64_t stage_running_job(JobQueue& q,
                          const std::string& client_id,
                          const fs::path& out_dir) {
    Job j;
    j.input.out_dir = out_dir;
    int64_t id = q.enqueue(std::move(j), JobKind::Postprocess, client_id);
    auto dq = q.dequeue(JobKind::Postprocess);
    REQUIRE(dq.has_value());
    REQUIRE(dq->job_id == id);
    // Job is now Running; we do NOT call finish().
    return id;
}

// Register a `process.fetch` handler on `server` whose body mirrors the one
// in src/daemon.cpp. Captures `q` (the JobQueue) and `server` by reference;
// `frame_cap` lets a test exercise the oversized-artifact reject branch.
//
// Keep this in sync with daemon.cpp's `process.fetch` registration — both
// hard-code the same wire shape and validation chain (see the comment block
// in daemon.cpp). If the production handler grows new validation, mirror it
// here so the tests cover the same surface.
void register_process_fetch_handler(IpcServer& server, JobQueue& q) {
    server.on("process.fetch",
              [&server, &q](const IpcRequest& req, IpcResponse& resp,
                            IpcError& err) {
        auto jit = req.params.find("job_id");
        if (jit == req.params.end()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.fetch: missing 'job_id'";
            return false;
        }
        const int64_t job_id = json_val_as_int(jit->second, 0);
        if (job_id <= 0) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.fetch: 'job_id' must be a positive integer";
            return false;
        }
        auto snap = q.status(job_id);
        if (!snap.has_value()) {
            err.code = static_cast<int>(IpcErrorCode::InvalidParams);
            err.message = "process.fetch: unknown job_id "
                        + std::to_string(job_id);
            return false;
        }
        const Job& job = *snap;
        auto owner = q.client_for_job(job_id);
        if (!owner.has_value() || *owner != req.client_id) {
            err.code = static_cast<int>(IpcErrorCode::PermissionDenied);
            err.message = "process.fetch: job_id "
                        + std::to_string(job_id)
                        + " is not owned by this client";
            return false;
        }
        if (job.state != JobState::Done) {
            err.code = static_cast<int>(IpcErrorCode::JobNotReady);
            err.message = std::string("process.fetch: fetch is only valid for "
                                      "Done jobs; current state=")
                        + job_state_name(job.state);
            return false;
        }
        const fs::path& out_dir = job.input.out_dir;
        if (out_dir.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "process.fetch: job has no out_dir on record";
            return false;
        }
        std::string enum_err;
        auto arts = enumerate_artifacts(out_dir, &enum_err);
        if (!enum_err.empty()) {
            err.code = static_cast<int>(IpcErrorCode::InternalError);
            err.message = "process.fetch: " + enum_err;
            return false;
        }
        const size_t frame_cap = server.max_binary_frame_bytes();
        for (const auto& a : arts) {
            if (static_cast<size_t>(a.size) > frame_cap) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "process.fetch: artifact '" + a.name
                            + "' exceeds max_binary_frame_bytes";
                return false;
            }
        }
        std::vector<std::string> bodies;
        bodies.reserve(arts.size());
        int64_t total_size = 0;
        for (const auto& a : arts) {
            std::ifstream in(a.path, std::ios::binary);
            if (!in) {
                err.code = static_cast<int>(IpcErrorCode::InternalError);
                err.message = "process.fetch: cannot open '" + a.name + "'";
                return false;
            }
            std::string body((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            total_size += static_cast<int64_t>(body.size());
            bodies.push_back(std::move(body));
        }
        std::string arr = "[";
        for (size_t i = 0; i < arts.size(); ++i) {
            if (i > 0) arr += ",";
            arr += "{\"name\":\"" + arts[i].name + "\""
                +  ",\"size\":" + std::to_string(arts[i].size)
                +  ",\"content_type\":\"" + arts[i].content_type + "\"}";
        }
        arr += "]";
        resp.result["job_id"]     = job_id;
        resp.result["artifacts"]  = arr;
        resp.result["total_size"] = total_size;

        // Post the binary fan-out so the frames enqueue AFTER the response
        // on the same per-fd outbound queue. Captures the client_id and the
        // bodies by shared_ptr so a slow drain on the client does not stall
        // the handler.
        std::string client_id = req.client_id;
        auto bodies_p = std::make_shared<std::vector<std::string>>(std::move(bodies));
        auto arts_p   = std::make_shared<std::vector<ArtifactInfo>>(std::move(arts));
        server.post([&server, client_id, bodies_p, arts_p]() {
            for (size_t i = 0; i < bodies_p->size(); ++i) {
                server.send_binary_to_client(client_id,
                                             FrameType::BinaryArtifact,
                                             (*bodies_p)[i],
                                             MessageClass::Response);
            }
        });
        return true;
    });
}

} // anonymous namespace

// ===========================================================================
// FILTER POLICY UNIT TESTS — assert the artifact allowlist matches the
// policy documented in src/fetch_artifacts.h.
// ===========================================================================

TEST_CASE("fetch_artifacts: should_include_artifact policy", "[c4][fetch]") {
    // INCLUDED — the allowlist.
    CHECK(should_include_artifact("Meeting_2026-05-15_10-30.md", true));
    CHECK(should_include_artifact("note.md", true));
    CHECK(should_include_artifact("Some_Title.MD", true));   // case-insensitive
    CHECK(should_include_artifact("captions.vtt", true));
    CHECK(should_include_artifact("captions.VTT", true));

    // EXCLUDED — directories.
    CHECK_FALSE(should_include_artifact("subdir", /*is_regular_file=*/false));

    // EXCLUDED — hidden / dotfiles.
    CHECK_FALSE(should_include_artifact(".hidden", true));
    CHECK_FALSE(should_include_artifact(".foo.md", true));

    // EXCLUDED — audio (upload staging + legacy capture paths).
    CHECK_FALSE(should_include_artifact("audio.wav", true));
    CHECK_FALSE(should_include_artifact("audio.flac", true));
    CHECK_FALSE(should_include_artifact("audio.mp3", true));
    CHECK_FALSE(should_include_artifact("audio.m4a", true));
    CHECK_FALSE(should_include_artifact("audio.ogg", true));
    CHECK_FALSE(should_include_artifact("audio_2026-05-15_10-30.wav", true));
    CHECK_FALSE(should_include_artifact("mic.wav", true));
    CHECK_FALSE(should_include_artifact("monitor.wav", true));

    // EXCLUDED — speakers DB (server-resident per v2 architecture).
    CHECK_FALSE(should_include_artifact("speakers.json", true));
    CHECK_FALSE(should_include_artifact("speakers_2026-05-15.json", true));

    // EXCLUDED — context inputs.
    CHECK_FALSE(should_include_artifact("context.json", true));
    CHECK_FALSE(should_include_artifact("context_2026-05-15.json", true));

    // EXCLUDED — unknown extensions (allowlist is narrow on purpose).
    CHECK_FALSE(should_include_artifact("debug.log", true));
    CHECK_FALSE(should_include_artifact("transcript.txt", true));
    CHECK_FALSE(should_include_artifact("summary.json", true));
}

TEST_CASE("fetch_artifacts: enumerate_artifacts filters per policy and sorts",
          "[c4][fetch]") {
    fs::path d = test_temp_dir("enumerate_policy");
    write_file(d / "Meeting_2026-05-15_10-30.md", "note body");
    write_file(d / "captions.vtt", "WEBVTT\n\n00:00.000 --> 00:01.000\nhi\n");
    write_file(d / "audio.wav", "RIFFwav-bytes");
    write_file(d / "speakers.json", "[]");
    write_file(d / "context.json", "{}");
    write_file(d / ".hidden_stuff", "secret");
    write_file(d / "Meeting_2026-05-15_11-00.md", "another note");

    auto arts = enumerate_artifacts(d);
    REQUIRE(arts.size() == 3);
    // Lexicographic by basename.
    CHECK(arts[0].name == "Meeting_2026-05-15_10-30.md");
    CHECK(arts[1].name == "Meeting_2026-05-15_11-00.md");
    CHECK(arts[2].name == "captions.vtt");

    CHECK(arts[0].content_type == "text/markdown");
    CHECK(arts[2].content_type == "text/vtt");

    // Sizes match what we wrote.
    CHECK(arts[0].size == static_cast<int64_t>(std::string("note body").size()));
}

TEST_CASE("fetch_artifacts: enumerate_artifacts on missing out_dir reports error",
          "[c4][fetch]") {
    fs::path nonexistent = test_temp_dir("missing") / "does-not-exist";
    std::string e;
    auto arts = enumerate_artifacts(nonexistent, &e);
    CHECK(arts.empty());
    CHECK(!e.empty());
    CHECK(e.find("does not exist") != std::string::npos);
}

TEST_CASE("fetch_artifacts: enumerate_artifacts on empty dir returns empty list",
          "[c4][fetch]") {
    fs::path d = test_temp_dir("empty");
    std::string e;
    auto arts = enumerate_artifacts(d, &e);
    CHECK(arts.empty());
    CHECK(e.empty());  // empty dir is not an error
}

// ===========================================================================
// WIRE TESTS — drive process.fetch over a real IpcServer + IpcClient.
// ===========================================================================

TEST_CASE("process.fetch: happy path round-trip (note + captions)",
          "[c4][fetch]") {
    ::unlink(FETCH_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(FETCH_SOCK);
    register_process_fetch_handler(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(FETCH_SOCK);
    REQUIRE(client.connect());
    REQUIRE(!client.client_id().empty());

    // Pre-stage the artifact dir AS THIS CLIENT'S Done job.
    fs::path out_dir = test_temp_dir("happy");
    const std::string note_body =
        "# Meeting note\n\nThis is the note body for C.4.\n";
    const std::string vtt_body =
        "WEBVTT\n\n00:00:00.000 --> 00:00:02.000\nhello\n";
    write_file(out_dir / "Meeting_2026-05-15_10-30.md", note_body);
    write_file(out_dir / "captions.vtt", vtt_body);
    // Also stage files we expect to be filtered out.
    write_file(out_dir / "audio.wav", "RIFFxxxx");
    write_file(out_dir / ".hidden", "shh");

    int64_t job_id = stage_done_job(q, client.client_id(), out_dir);

    // Fetch.
    fs::path dst = test_temp_dir("happy_dst");
    IpcError err;
    auto written = client.fetch_artifacts(job_id, dst, err, /*timeout_ms=*/5000);
    INFO("err code=" << err.code << " message=" << err.message);
    REQUIRE(err.message.empty());
    REQUIRE(written.size() == 2);

    // Order matches the artifacts[] array (sorted by name).
    CHECK(written[0].filename() == "Meeting_2026-05-15_10-30.md");
    CHECK(written[1].filename() == "captions.vtt");
    CHECK(read_file_bytes(written[0]) == note_body);
    CHECK(read_file_bytes(written[1]) == vtt_body);

    // Audio + dotfile were excluded.
    CHECK_FALSE(fs::exists(dst / "audio.wav"));
    CHECK_FALSE(fs::exists(dst / ".hidden"));

    client.close_connection();
    ::unlink(FETCH_SOCK);
}

TEST_CASE("process.fetch: rejects fetch on Queued job (not Done)",
          "[c4][fetch]") {
    ::unlink(FETCH_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(FETCH_SOCK);
    register_process_fetch_handler(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(FETCH_SOCK);
    REQUIRE(client.connect());

    fs::path out_dir = test_temp_dir("not_done_queued");
    write_file(out_dir / "note.md", "n");

    // Queued (never dequeued — no worker drains the queue in this test).
    Job j;
    j.input.out_dir = out_dir;
    int64_t queued_id = q.enqueue(std::move(j), JobKind::Postprocess,
                                  client.client_id());

    IpcError err;
    auto written = client.fetch_artifacts(queued_id, test_temp_dir("nd_dst"),
                                          err, 2000);
    CHECK(written.empty());
    CHECK(err.code == static_cast<int>(IpcErrorCode::JobNotReady));
    CHECK(err.message.find("Done") != std::string::npos);
    CHECK(err.message.find("queued") != std::string::npos);

    client.close_connection();
    ::unlink(FETCH_SOCK);
}

TEST_CASE("process.fetch: rejects fetch on Running job", "[c4][fetch]") {
    ::unlink(FETCH_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(FETCH_SOCK);
    register_process_fetch_handler(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(FETCH_SOCK);
    REQUIRE(client.connect());

    // Stage one job in Running (dequeued, never finished).
    int64_t running_id = stage_running_job(q, client.client_id(),
                                           test_temp_dir("running_dir"));

    IpcError err;
    auto written = client.fetch_artifacts(running_id, test_temp_dir("rn_dst"),
                                          err, 2000);
    CHECK(written.empty());
    CHECK(err.code == static_cast<int>(IpcErrorCode::JobNotReady));
    CHECK(err.message.find("running") != std::string::npos);

    // Release the slot so JqShutdownGuard tears down cleanly.
    q.finish(running_id, true);

    client.close_connection();
    ::unlink(FETCH_SOCK);
}

TEST_CASE("process.fetch: rejects foreign job with PermissionDenied",
          "[c4][fetch]") {
    ::unlink(FETCH_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(FETCH_SOCK);
    register_process_fetch_handler(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient a(FETCH_SOCK), b(FETCH_SOCK);
    REQUIRE(a.connect());
    REQUIRE(b.connect());
    REQUIRE(a.client_id() != b.client_id());

    fs::path out_dir = test_temp_dir("foreign");
    write_file(out_dir / "note.md", "A's note");
    int64_t a_job = stage_done_job(q, a.client_id(), out_dir);

    // B tries to fetch A's job.
    IpcError err;
    auto written = b.fetch_artifacts(a_job, test_temp_dir("foreign_dst"),
                                     err, 2000);
    CHECK(written.empty());
    CHECK(err.code == static_cast<int>(IpcErrorCode::PermissionDenied));
    CHECK(err.message.find("not owned") != std::string::npos);

    // A still gets its own artifacts back.
    IpcError ok_err;
    auto ok_written = a.fetch_artifacts(a_job, test_temp_dir("foreign_ok_dst"),
                                        ok_err, 2000);
    CHECK(ok_err.message.empty());
    CHECK(ok_written.size() == 1);

    a.close_connection();
    b.close_connection();
    ::unlink(FETCH_SOCK);
}

TEST_CASE("process.fetch: unknown job_id reports InvalidParams",
          "[c4][fetch]") {
    ::unlink(FETCH_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(FETCH_SOCK);
    register_process_fetch_handler(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(FETCH_SOCK);
    REQUIRE(client.connect());

    IpcError err;
    auto written = client.fetch_artifacts(/*job_id=*/99999,
                                          test_temp_dir("unknown_dst"),
                                          err, 2000);
    CHECK(written.empty());
    CHECK(err.code == static_cast<int>(IpcErrorCode::InvalidParams));
    CHECK(err.message.find("unknown") != std::string::npos);

    // Zero / negative job_id also rejected (separate branch in handler).
    IpcError err0;
    CHECK(client.fetch_artifacts(0, test_temp_dir("z_dst"), err0, 2000).empty());
    CHECK(err0.code == static_cast<int>(IpcErrorCode::InvalidParams));

    client.close_connection();
    ::unlink(FETCH_SOCK);
}

TEST_CASE("process.fetch: missing out_dir reports InternalError",
          "[c4][fetch]") {
    ::unlink(FETCH_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(FETCH_SOCK);
    register_process_fetch_handler(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(FETCH_SOCK);
    REQUIRE(client.connect());

    fs::path out_dir = test_temp_dir("vanish");
    write_file(out_dir / "note.md", "transient note");
    int64_t job_id = stage_done_job(q, client.client_id(), out_dir);

    // Operator deleted the meeting dir between job completion and fetch.
    std::error_code ec;
    fs::remove_all(out_dir, ec);
    REQUIRE(!fs::exists(out_dir));

    IpcError err;
    auto written = client.fetch_artifacts(job_id, test_temp_dir("vanish_dst"),
                                          err, 2000);
    CHECK(written.empty());
    CHECK(err.code == static_cast<int>(IpcErrorCode::InternalError));
    CHECK(err.message.find("does not exist") != std::string::npos);

    client.close_connection();
    ::unlink(FETCH_SOCK);
}

TEST_CASE("process.fetch: audio WAV in out_dir is not delivered",
          "[c4][fetch]") {
    ::unlink(FETCH_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(FETCH_SOCK);
    register_process_fetch_handler(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(FETCH_SOCK);
    REQUIRE(client.connect());

    fs::path out_dir = test_temp_dir("audio_filter");
    write_file(out_dir / "Meeting_test.md", "note");
    // Various staging-WAV / record.start-WAV / format-mp3 names — all filtered.
    write_file(out_dir / "audio.wav", "wav-bytes");
    write_file(out_dir / "audio_2026-05-15_10-30.wav", "wav-bytes");
    write_file(out_dir / "mic.wav", "mic-bytes");
    write_file(out_dir / "monitor.wav", "mon-bytes");
    write_file(out_dir / "audio.mp3", "mp3-bytes");
    int64_t job_id = stage_done_job(q, client.client_id(), out_dir);

    fs::path dst = test_temp_dir("audio_filter_dst");
    IpcError err;
    auto written = client.fetch_artifacts(job_id, dst, err, 5000);
    INFO("err: " << err.message);
    REQUIRE(err.message.empty());
    REQUIRE(written.size() == 1);
    CHECK(written[0].filename() == "Meeting_test.md");
    CHECK_FALSE(fs::exists(dst / "audio.wav"));
    CHECK_FALSE(fs::exists(dst / "mic.wav"));

    client.close_connection();
    ::unlink(FETCH_SOCK);
}

TEST_CASE("process.fetch: hidden files are not delivered", "[c4][fetch]") {
    ::unlink(FETCH_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(FETCH_SOCK);
    register_process_fetch_handler(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(FETCH_SOCK);
    REQUIRE(client.connect());

    fs::path out_dir = test_temp_dir("hidden_filter");
    write_file(out_dir / "note.md", "n");
    write_file(out_dir / ".hidden", "should not ship");
    write_file(out_dir / ".secret.md", "even though .md");  // still hidden
    int64_t job_id = stage_done_job(q, client.client_id(), out_dir);

    fs::path dst = test_temp_dir("hidden_dst");
    IpcError err;
    auto written = client.fetch_artifacts(job_id, dst, err, 5000);
    REQUIRE(err.message.empty());
    REQUIRE(written.size() == 1);
    CHECK(written[0].filename() == "note.md");
    CHECK_FALSE(fs::exists(dst / ".hidden"));
    CHECK_FALSE(fs::exists(dst / ".secret.md"));

    client.close_connection();
    ::unlink(FETCH_SOCK);
}

TEST_CASE("process.fetch: multiple artifacts arrive in artifacts[] order",
          "[c4][fetch]") {
    ::unlink(FETCH_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(FETCH_SOCK);
    register_process_fetch_handler(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(FETCH_SOCK);
    REQUIRE(client.connect());

    fs::path out_dir = test_temp_dir("order");
    // Distinct contents to detect any swap. Names chosen so the
    // lexicographic order is well-defined and not the file-creation order.
    write_file(out_dir / "z_note.md", "third");
    write_file(out_dir / "a_note.md", "first");
    write_file(out_dir / "m_note.md", "second");
    write_file(out_dir / "captions.vtt", "captions content");

    int64_t job_id = stage_done_job(q, client.client_id(), out_dir);

    fs::path dst = test_temp_dir("order_dst");
    IpcError err;
    auto written = client.fetch_artifacts(job_id, dst, err, 5000);
    INFO("err: " << err.message);
    REQUIRE(err.message.empty());
    REQUIRE(written.size() == 4);

    // Lexicographic by basename.
    CHECK(written[0].filename() == "a_note.md");
    CHECK(written[1].filename() == "captions.vtt");
    CHECK(written[2].filename() == "m_note.md");
    CHECK(written[3].filename() == "z_note.md");

    // Per-artifact byte-exactness (the load-bearing assertion that
    // ordering on the wire matches metadata).
    CHECK(read_file_bytes(written[0]) == "first");
    CHECK(read_file_bytes(written[1]) == "captions content");
    CHECK(read_file_bytes(written[2]) == "second");
    CHECK(read_file_bytes(written[3]) == "third");

    client.close_connection();
    ::unlink(FETCH_SOCK);
}

TEST_CASE("process.fetch: single artifact (no captions sidecar)",
          "[c4][fetch]") {
    ::unlink(FETCH_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(FETCH_SOCK);
    register_process_fetch_handler(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(FETCH_SOCK);
    REQUIRE(client.connect());

    fs::path out_dir = test_temp_dir("single");
    const std::string body = "Solo note body";
    write_file(out_dir / "note.md", body);
    int64_t job_id = stage_done_job(q, client.client_id(), out_dir);

    fs::path dst = test_temp_dir("single_dst");
    IpcError err;
    auto written = client.fetch_artifacts(job_id, dst, err, 5000);
    REQUIRE(err.message.empty());
    REQUIRE(written.size() == 1);
    CHECK(read_file_bytes(written[0]) == body);

    client.close_connection();
    ::unlink(FETCH_SOCK);
}

TEST_CASE("process.fetch: empty artifact set (audio-only out_dir)",
          "[c4][fetch]") {
    ::unlink(FETCH_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(FETCH_SOCK);
    register_process_fetch_handler(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(FETCH_SOCK);
    REQUIRE(client.connect());

    fs::path out_dir = test_temp_dir("empty_set");
    // Only the staging WAV — should be filtered out. The fetch must still
    // succeed cleanly with an empty list (no binary frames follow the
    // metadata response, so the client's pump must not stall).
    write_file(out_dir / "audio.wav", "wav-bytes");
    int64_t job_id = stage_done_job(q, client.client_id(), out_dir);

    fs::path dst = test_temp_dir("empty_set_dst");
    IpcError err;
    auto written = client.fetch_artifacts(job_id, dst, err, 5000);
    INFO("err: " << err.message);
    REQUIRE(err.message.empty());
    CHECK(written.empty());

    client.close_connection();
    ::unlink(FETCH_SOCK);
}

TEST_CASE("process.fetch: byte-exact for binary-ish content",
          "[c4][fetch]") {
    // The note format is markdown today, but the wire is "raw bytes" — verify
    // no transformation happens (no newline normalization, no encoding).
    ::unlink(FETCH_SOCK);

    JobQueue q;
    JqShutdownGuard jqg(q);
    IpcServer server(FETCH_SOCK);
    register_process_fetch_handler(server, q);
    REQUIRE(server.start());
    ServerGuard sg(server);

    IpcClient client(FETCH_SOCK);
    REQUIRE(client.connect());

    fs::path out_dir = test_temp_dir("binary_exact");
    // Embed CR, LF, NUL-like (use a controlled value), tab, UTF-8.
    std::string body;
    body.reserve(512);
    for (int i = 0; i < 256; ++i)
        body += static_cast<char>(static_cast<unsigned char>(i));
    body += "\r\n\t   end-of-note ✓";
    write_file(out_dir / "binary.md", body);
    int64_t job_id = stage_done_job(q, client.client_id(), out_dir);

    fs::path dst = test_temp_dir("binary_exact_dst");
    IpcError err;
    auto written = client.fetch_artifacts(job_id, dst, err, 5000);
    REQUIRE(err.message.empty());
    REQUIRE(written.size() == 1);
    CHECK(read_file_bytes(written[0]) == body);

    client.close_connection();
    ::unlink(FETCH_SOCK);
}
