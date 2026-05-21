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

        // Streaming + Upload managers need a caption sink / progress sink
        // and a meetings_root. The sinks are no-op in tests — handlers
        // that exercise them are Phase 3 scope.
        StreamingCaptionSink stream_sink;
        g_streaming = std::make_unique<StreamingSessionManager>(
            *g_jobs, stream_sink, /*caption_model_dir=*/std::string(),
            g_meeting_index.get(), meetings_dir_);

        UploadProgressSink upload_sink;
        g_uploads = std::make_unique<UploadSessionManager>(
            *g_jobs, /*staging_root=*/fs::path{}, upload_sink,
            g_meeting_index.get(), meetings_dir_);

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

    /// Register the production handlers + start the IPC server's poll
    /// loop. Must be called exactly once. After this call no further
    /// `server().on(...)` calls or `mutate_config(...)` calls are safe
    /// from the test thread (the poll thread now reads them).
    void start() {
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
        fs::path p = fs::temp_directory_path() / oss.str();
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
};

} // namespace test
} // namespace recmeet
