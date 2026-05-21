// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase 2a — production IPC handler registration surface.
//
// `register_daemon_handlers(server)` registers every production verb the
// daemon answers (`status.get`, `config.reload`, the session.* family, the
// process.* family, the speakers.* family, the meetings.* family, the
// models.* family, `admin.evict`, and friends). The daemon's main()
// startup path calls this once; Phase 2b (a follow-on dispatch) makes the
// IPC integration tests call it instead of stubbing each verb themselves
// so the production code path is the one under test.
//
// Internals (the static helper functions, the file-static lambdas that
// were inline in daemon.cpp, the cross-file globals it touches) are
// expressed via `daemon_handlers_internal.h` and are NOT part of this
// public surface. Keep this header to exactly one declaration.

#pragma once

namespace recmeet {
class IpcServer;
} // namespace recmeet

void register_daemon_handlers(recmeet::IpcServer& server);
