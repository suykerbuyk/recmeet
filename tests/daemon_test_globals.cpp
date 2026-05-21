// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase 2b — definitions for the daemon-global state that
// `daemon_handlers.cpp` references via `daemon_handlers_internal.h`.
//
// In production these globals live in `src/daemon.cpp` (the daemon binary's
// `main()` translation unit). The test binary does NOT link daemon.cpp (it
// has its own `main()` and pulls in subprocess machinery the tests must
// not exercise), so the externs in `daemon_handlers_internal.h` would
// otherwise be unresolved when we add `daemon_handlers.cpp` to the test
// target. This TU provides exactly those definitions and the small set of
// daemon-side helper functions that handlers reach for
// (`fill_state_fields`, `broadcast_state_inline`, `is_safe_dirname`,
// `teardown_orphan_jobs`).
//
// The behavior of every helper here is byte-equivalent to its daemon.cpp
// counterpart for the in-process test path:
//   - `fill_state_fields`        — same composite-state derivation.
//   - `broadcast_state_inline`   — same event shape; same broadcast site.
//   - `is_safe_dirname`          — verbatim copy of daemon.cpp:220-227.
//   - `teardown_orphan_jobs`     — same cancellation routing as daemon.cpp;
//                                  the WAV-archive step is omitted (tests
//                                  do not write archive dirs) and SIGTERM
//                                  is never sent (no live child process).
//                                  Either omission only matters on the
//                                  Postprocess-Running path, which tests
//                                  do not exercise — the production path
//                                  is still the daemon.cpp version.
//
// This file is exclusively for the test binary. `RECMEET_TESTING` gates
// its inclusion in CMakeLists.txt; production targets never see these
// definitions.

#include "daemon_handlers_internal.h"

#include "ipc_protocol.h"
#include "ipc_server.h"
#include "job_queue.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <sys/types.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Daemon-global state (mirrors src/daemon.cpp:75-149,233).
// ---------------------------------------------------------------------------

std::unique_ptr<recmeet::JobQueue>                g_jobs;
std::unique_ptr<recmeet::StreamingSessionManager> g_streaming;
std::unique_ptr<recmeet::UploadSessionManager>    g_uploads;
std::unique_ptr<recmeet::MeetingIndex>            g_meeting_index;
std::unique_ptr<recmeet::DiarizationCache>        g_diar_cache;
std::unique_ptr<recmeet::SessionManager>          g_sessions;

recmeet::ServerConfig g_server_config;
std::mutex            g_server_config_mu;

recmeet::StopToken    g_pp_stop;
std::atomic<pid_t>    g_pp_child_pid{-1};

std::atomic<bool>     g_batch_reidentify_running{false};

// ---------------------------------------------------------------------------
// Daemon-side helper functions (mirrors src/daemon.cpp:163-227,608-694).
// The composite-state derivation is identical; the orphan teardown is
// simplified because the test path does not exercise the Running-with-
// child-pid branch.
// ---------------------------------------------------------------------------

namespace {

bool jq_postprocessing() {
    return g_jobs && g_jobs->slot_busy(recmeet::JobKind::Postprocess);
}
bool jq_downloading() {
    return g_jobs && g_jobs->slot_busy(recmeet::JobKind::ModelDownload);
}
bool jq_streaming() {
    return g_jobs && g_jobs->slot_busy(recmeet::JobKind::Streaming);
}

std::string composite_state_name() {
    bool pp = jq_postprocessing();
    bool dl = jq_downloading();
    bool st = jq_streaming();
    if (pp) return "postprocessing";
    if (st) return "streaming";
    if (dl) return "downloading";
    return "idle";
}

} // namespace

void fill_state_fields(recmeet::JsonMap& data) {
    data["state"]          = composite_state_name();
    data["postprocessing"] = jq_postprocessing();
    data["downloading"]    = jq_downloading();
    data["streaming"]      = jq_streaming();
}

void broadcast_state_inline(recmeet::IpcServer& server, const std::string& error) {
    recmeet::IpcEvent ev;
    ev.event = "state.changed";
    fill_state_fields(ev.data);
    if (!error.empty()) ev.data["error"] = error;
    server.broadcast(ev);
}

bool is_safe_dirname(const std::string& name) {
    if (name.empty()) return false;
    if (name == "." || name == "..") return false;
    if (name.find('/')  != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find('\0') != std::string::npos) return false;
    return true;
}

// Test-side teardown_orphan_jobs:
//   - Same cancellation routing as daemon.cpp's body (Postprocess /
//     Streaming / ModelDownload non-terminal states → cancel via the
//     appropriate JobQueue / streaming-manager path).
//   - Skips the daemon's archive_orphan_wav step (tests do not assert
//     archive dir layout; the meetings_root is per-test and torn down by
//     the harness on destruction).
//   - Skips the SIGTERM-to-child branch (no pp_worker child runs in tests).
std::vector<int64_t>
teardown_orphan_jobs(const std::string& /*token_prefix8*/,
                     const std::string& client_id) {
    std::vector<int64_t> canceled;
    if (!g_jobs) return canceled;

    auto owned = g_jobs->list_by_client(client_id);
    for (const auto& job : owned) {
        if (job.state == recmeet::JobState::Done ||
            job.state == recmeet::JobState::Failed ||
            job.state == recmeet::JobState::Cancelled) {
            continue;
        }
        switch (job.kind) {
        case recmeet::JobKind::Postprocess: {
            if (job.state == recmeet::JobState::WaitingForUpload) {
                if (g_uploads) g_uploads->cancel_by_job_id(job.job_id);
                g_jobs->cancel(job.job_id);
            } else {
                g_jobs->cancel(job.job_id);
            }
            canceled.push_back(job.job_id);
            break;
        }
        case recmeet::JobKind::Streaming: {
            if (g_streaming) g_streaming->cancel_by_job_id(job.job_id);
            canceled.push_back(job.job_id);
            break;
        }
        case recmeet::JobKind::ModelDownload:
            g_jobs->cancel(job.job_id);
            canceled.push_back(job.job_id);
            break;
        case recmeet::JobKind::_count:
            break;
        }
    }
    return canceled;
}
