// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase 2b — test fixture for exercising production daemon handlers
// in-process.
//
// Sets up the daemon's process-global state (`g_server_config`,
// `g_meeting_index`, `g_jobs`, `g_streaming`, `g_uploads`, `g_sessions`,
// `g_diar_cache`) pointing at a per-test temp directory, calls
// `register_daemon_handlers(server)`, and tears everything down on
// destruction. Use as RAII:
//
//   TEST_CASE("speakers.get returns 200", "[e6][speakers-meetings][ipc]") {
//       DaemonTestHarness harness;
//       harness.start();  // starts IPC server on a unix socket
//       IpcClient client(harness.socket_path());
//       REQUIRE(client.connect());
//       ...
//   }
//
// The harness MUST clean up `g_*` globals on destruction so successive
// tests don't bleed state. Production handlers (registered via
// `register_daemon_handlers`) read these globals directly, so they must
// be wired BEFORE `register_daemon_handlers(server)` runs and they must
// be torn down AFTER the IPC server has stopped (so any in-flight handler
// has finished touching them).
//
// Daemon workers (pp_worker_loop / model_dl_worker_loop /
// stream_worker_loop) are NOT started by this harness — Phase 2b only
// exercises the verb-dispatch path. Tests that want to drive a job
// through to completion belong in Phase 3 (daemon-spawning harness).
//
// Phase 2b extension — sink injection. Tests that need to observe upload
// progress or streaming caption events from the production handlers can
// inject their own sinks BEFORE start():
//
//   DaemonTestHarness harness;
//   UploadProgressSink ps;
//   ps.on_progress = [&](int64_t job_id, const std::string& client_id,
//                        int64_t bytes_received, int64_t audio_size) {
//       /* observe */
//   };
//   harness.set_upload_progress_sink(std::move(ps));
//   harness.set_upload_staging_root(my_tmp_dir);
//   harness.start();
//
// The setters must be called before start() — start() constructs
// `g_uploads` / `g_streaming` with the injected values, and the production
// session managers read the sinks on the poll thread from then on. Default
// behavior (no setter call) is identical to pre-extension: no-op sinks,
// upload staging_root == per-test tmp_dir, streaming meetings_root ==
// per-test meetings_dir.

#pragma once

#include "config.h"
#include "daemon_handlers.h"
#include "daemon_handlers_internal.h"
#include "diarization_cache.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"
#include "meeting_index.h"
#include "session_manager.h"
#include "speaker_id.h"
#include "streaming_session.h"
#include "test_tmpdir.h"
#include "upload_session.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace recmeet {
namespace test {

namespace fs = std::filesystem;

class DaemonTestHarness {
public:
    DaemonTestHarness()
        : tmp_dir_(make_unique_tmp_dir()),
          meetings_dir_(tmp_dir_ / "meetings"),
          speaker_db_dir_(tmp_dir_ / "speakers"),
          socket_path_(make_socket_path()),
          server_(std::make_unique<IpcServer>(socket_path_)) {
        fs::create_directories(meetings_dir_);
        fs::create_directories(speaker_db_dir_);

        // Wire the daemon-global state. Tests can override individual
        // ServerConfig fields by calling `mutate_config(...)` BEFORE
        // start().
        {
            ServerConfig cfg;
            cfg.meetings_root         = meetings_dir_;
            cfg.speaker_db            = speaker_db_dir_;
            cfg.allow_client_downloads = true;
            cfg.diarize               = true;
            cfg.vad                   = true;
            cfg.captions_enabled      = false;
            cfg.retain_terminal_hours = 24;
            cfg.diarization_cache_ttl_secs = 86400;
            g_server_config = cfg;
        }

        // JobQueue with default slot capacities (1/1/1).
        {
            JobQueue::SlotCapacities caps;
            caps.postprocess    = 1;
            caps.streaming      = 1;
            caps.model_download = 1;
            g_jobs = std::make_unique<JobQueue>(caps);
        }
        g_meeting_index = std::make_unique<MeetingIndex>();
        g_diar_cache    = std::make_unique<DiarizationCache>(
            g_server_config.diarization_cache_ttl_secs);
        g_sessions      = std::make_unique<SessionManager>(
            static_cast<int64_t>(g_server_config.retain_terminal_hours) * 3600);

        // Streaming + Upload managers are deferred to start() so tests can
        // inject custom sinks / staging roots via `set_streaming_caption_sink`
        // / `set_upload_progress_sink` / `set_upload_staging_root` between
        // construction and start(). Default sinks are no-op; default upload
        // staging_root is the per-harness tmp_dir; default streaming
        // meetings_root is meetings_dir_.

        // Wire the resume_token resolver + dispatch hook the same way
        // daemon.cpp's main() does. Tests that need PSK / resume-token
        // semantics drive these via the IpcClient handshake; tests that
        // don't simply ignore the token in the auth.ok frame.
        server_->set_resume_token_resolver(
            [this](const std::string& provided)
                -> std::pair<std::string, std::string> {
            if (g_sessions && !provided.empty()) {
                if (auto cid = g_sessions->resolve(provided); cid)
                    return {*cid, provided};
            }
            std::string cid   = server_->mint_client_id();
            std::string token = g_sessions ? g_sessions->mint(cid)
                                           : std::string();
            return {cid, token};
        });
        server_->set_request_dispatch_hook(
            [](const std::string& tok) {
                if (g_sessions) g_sessions->bump_last_seen(tok);
            });
    }

    ~DaemonTestHarness() {
        // Tear down in reverse-of-construction order so any in-flight
        // handler-thread reference to a global lands on a still-valid
        // object. The IpcServer destructor joins the poll thread, which
        // guarantees no handler is mid-dispatch when we proceed.
        if (server_) {
            server_->stop();
            if (run_thread_.joinable()) run_thread_.join();
            server_.reset();
        }
        g_uploads.reset();
        g_streaming.reset();
        g_sessions.reset();
        g_diar_cache.reset();
        g_meeting_index.reset();
        g_jobs.reset();
        g_server_config = ServerConfig{};
        g_pp_child_pid.store(-1);
        g_batch_reidentify_running.store(false);
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
        unlink(socket_path_.c_str());
    }

    DaemonTestHarness(const DaemonTestHarness&)            = delete;
    DaemonTestHarness& operator=(const DaemonTestHarness&) = delete;

    /// Inject a `StreamingCaptionSink` to be used when start() constructs
    /// `g_streaming`. Must be called BEFORE start() — the production
    /// `StreamingSessionManager` ctor takes the sink by const-ref and the
    /// session manager stores a copy internally, so changing it after start()
    /// would be a race against the IPC poll thread / engine worker thread.
    /// Default (no setter call) is a no-op sink.
    void set_streaming_caption_sink(StreamingCaptionSink sink) {
        stream_sink_   = std::move(sink);
        stream_sink_set_ = true;
    }

    /// Inject an `UploadProgressSink` to be used when start() constructs
    /// `g_uploads`. Must be called BEFORE start() — the production
    /// `UploadSessionManager` stores the sink by value and the IPC poll
    /// thread reads it on every `feed_chunk()`. Default (no setter call)
    /// is a no-op sink.
    void set_upload_progress_sink(UploadProgressSink sink) {
        upload_sink_   = std::move(sink);
        upload_sink_set_ = true;
    }

    /// Override the staging root the `UploadSessionManager` is constructed
    /// with. Default is the harness's per-test tmp_dir(). Must be called
    /// BEFORE start(). The path must remain valid for the harness's
    /// lifetime (the manager stores it by value).
    void set_upload_staging_root(fs::path root) {
        upload_staging_root_       = std::move(root);
        upload_staging_root_set_   = true;
    }

    /// Register the production handlers + start the IPC server's poll
    /// loop. Must be called exactly once. After this call no further
    /// `server().on(...)` calls or `mutate_config(...)` calls are safe
    /// from the test thread (the poll thread now reads them). Sink
    /// injection via `set_*_sink(...)` MUST happen before this call.
    void start() {
        // Build the streaming + upload managers now, using any test-
        // injected sinks / staging root. Default sinks are no-op so legacy
        // tests that don't call the setters behave exactly as before.
        StreamingCaptionSink stream_sink = stream_sink_;
        g_streaming = std::make_unique<StreamingSessionManager>(
            *g_jobs, stream_sink, /*caption_model_dir=*/std::string(),
            g_meeting_index.get(), meetings_dir_);

        UploadProgressSink upload_sink = upload_sink_;
        fs::path staging_root = upload_staging_root_set_
            ? upload_staging_root_
            : tmp_dir_;
        g_uploads = std::make_unique<UploadSessionManager>(
            *g_jobs, staging_root, std::move(upload_sink),
            g_meeting_index.get(), meetings_dir_);

        // Phase 2b ext — wire the daemon's on_client_disconnect inline
        // (it lives in daemon.cpp's main(), not in register_daemon_handlers,
        // so production tests need to install it explicitly). Mirrors
        // src/daemon.cpp:2029-2047 — abort any in-flight streaming /
        // upload session owned by the disconnected client.
        server_->on_client_disconnect([](const std::string& client_id) {
            if (g_streaming) g_streaming->on_client_disconnect(client_id);
            if (g_uploads)   g_uploads->on_client_disconnect(client_id);
        });

        // Phase 2b ext — wire the binary-frame router (also lives in
        // daemon.cpp's main, not in register_daemon_handlers). Mirrors
        // src/daemon.cpp:1987-2023 — routes `0x03` to g_streaming and
        // `0x01` to g_uploads.
        server_->on_binary_frame(
            [](const std::string& client_id, FrameType type,
               const std::string& payload) -> bool {
                if (type == FrameType::StreamAudio) {
                    if (!g_streaming) return false;
                    std::string token = g_streaming->token_for_client(client_id);
                    if (token.empty()) return false;
                    return g_streaming->feed_audio(token, payload);
                }
                if (type == FrameType::BinaryUpload) {
                    if (!g_uploads) return false;
                    return g_uploads->feed_chunk(client_id, payload);
                }
                return true;   // other binary frame types discarded silently.
            });

        register_daemon_handlers(*server_);
        if (!server_->start()) {
            throw std::runtime_error(
                "DaemonTestHarness: IpcServer::start() failed on "
                + socket_path_);
        }
        run_thread_ = std::thread([this]() { server_->run(); });
        // Give the poll thread a beat to enter run() before clients
        // start connecting.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    /// Test-time mutator for the daemon's ServerConfig. Mutate BEFORE
    /// start() — handlers snapshot the config under g_server_config_mu
    /// at request time.
    template <typename Fn>
    void mutate_config(Fn&& fn) {
        std::lock_guard<std::mutex> lk(g_server_config_mu);
        std::forward<Fn>(fn)(g_server_config);
    }

    const std::string& socket_path() const { return socket_path_; }
    IpcServer&         server()              { return *server_; }
    const fs::path&    tmp_dir()        const { return tmp_dir_; }
    const fs::path&    meetings_dir()   const { return meetings_dir_; }
    const fs::path&    speaker_db_dir() const { return speaker_db_dir_; }

    /// Allocate a per-test IpcClient connected to this harness's socket.
    std::unique_ptr<IpcClient> make_client() {
        auto c = std::make_unique<IpcClient>(socket_path_);
        if (!c->connect()) {
            throw std::runtime_error(
                "DaemonTestHarness: IpcClient::connect() failed");
        }
        return c;
    }

    /// Seed a speaker profile on disk via the production helper. Returns
    /// the resulting SpeakerProfile so the test can read it back.
    SpeakerProfile seed_speaker(const std::string& name,
                                std::size_t enrollments = 1,
                                std::size_t dim = 4) {
        SpeakerProfile p;
        p.name = name;
        p.created = p.updated = "2026-01-01T00:00:00Z";
        for (std::size_t i = 0; i < enrollments; ++i) {
            std::vector<float> emb(dim, static_cast<float>(i + 1));
            p.embeddings.push_back(std::move(emb));
        }
        save_speaker(speaker_db_dir_, p);
        return p;
    }

private:
    static fs::path make_unique_tmp_dir() {
        std::mt19937_64 rng(static_cast<uint64_t>(::getpid()) ^
                            static_cast<uint64_t>(
                                std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
        std::ostringstream oss;
        oss << "recmeet_phase2b_" << ::getpid() << "_" << rng();
        fs::path p = recmeet::test::tmp_path(oss.str());
        std::error_code ec;
        fs::remove_all(p, ec);
        fs::create_directories(p);
        return p;
    }

    std::string make_socket_path() {
        std::ostringstream oss;
        oss << (tmp_dir_ / "ipc.sock").string();
        return oss.str();
    }

    fs::path                   tmp_dir_;
    fs::path                   meetings_dir_;
    fs::path                   speaker_db_dir_;
    std::string                socket_path_;
    std::unique_ptr<IpcServer> server_;
    std::thread                run_thread_;

    // Injection points for the streaming caption sink, upload progress
    // sink, and upload staging root. start() consumes these when
    // constructing `g_streaming` / `g_uploads`. Defaults are no-op sinks
    // and tmp_dir_ for staging.
    StreamingCaptionSink stream_sink_{};
    bool                 stream_sink_set_ = false;
    UploadProgressSink   upload_sink_{};
    bool                 upload_sink_set_ = false;
    fs::path             upload_staging_root_{};
    bool                 upload_staging_root_set_ = false;
};

} // namespace test
} // namespace recmeet
