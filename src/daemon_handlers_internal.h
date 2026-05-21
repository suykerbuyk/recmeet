// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase 2a — daemon-internal surface shared by daemon.cpp and
// daemon_handlers.cpp.
//
// This is NOT a public header. It is the linkage glue that lets the
// extracted `register_daemon_handlers()` (in daemon_handlers.cpp) reach the
// daemon's process-global state (job queue, meeting index, server config,
// per-handler helper routines) without exporting those internals to the
// rest of the codebase. Only daemon.cpp and daemon_handlers.cpp may
// include this file.
//
// Symbols whose definitions live in daemon.cpp are dropped from `static`
// linkage to file-extern; their declarations live here. Adding a new
// global / helper used by both translation units? Declare it here and
// drop `static` at the definition site.

#pragma once

#include "config.h"
#include "diarization_cache.h"
#include "ipc_protocol.h"
#include "job_queue.h"
#include "meeting_index.h"
#include "session_manager.h"
#include "streaming_session.h"
#include "upload_session.h"
#include "util.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <sys/types.h>

namespace recmeet {
class IpcServer;
} // namespace recmeet

// ---------------------------------------------------------------------------
// Daemon-wide state. Definitions in src/daemon.cpp. The daemon translation
// unit declares these without `static` so they have external linkage; the
// daemon binary is a single executable target so the names cannot collide
// with anything else in the link line.
// ---------------------------------------------------------------------------

extern std::unique_ptr<recmeet::JobQueue>                  g_jobs;
extern std::unique_ptr<recmeet::StreamingSessionManager>   g_streaming;
extern std::unique_ptr<recmeet::UploadSessionManager>      g_uploads;
extern std::unique_ptr<recmeet::MeetingIndex>              g_meeting_index;
extern std::unique_ptr<recmeet::DiarizationCache>          g_diar_cache;
extern std::unique_ptr<recmeet::SessionManager>            g_sessions;

extern recmeet::ServerConfig                               g_server_config;
extern std::mutex                                          g_server_config_mu;

extern recmeet::StopToken                                  g_pp_stop;
extern std::atomic<pid_t>                                  g_pp_child_pid;

// `speakers.batch_reidentify` async bookkeeping.
extern std::atomic<bool>                                   g_batch_reidentify_running;

// ---------------------------------------------------------------------------
// Daemon-wide helper routines. Definitions in src/daemon.cpp.
// ---------------------------------------------------------------------------

void fill_state_fields(recmeet::JsonMap& data);
void broadcast_state_inline(recmeet::IpcServer& server,
                            const std::string& error = "");
bool is_safe_dirname(const std::string& name);

// `teardown_orphan_jobs` (admin.evict + GC sweep shared body). Returns the
// list of job_ids that were marked Cancelled.
std::vector<int64_t> teardown_orphan_jobs(const std::string& token_prefix8,
                                          const std::string& client_id);
