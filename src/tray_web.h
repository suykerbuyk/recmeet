// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

// Phase E.6.2 — tray-bundled WebUI HTTP listener.
//
// The tray binds an embedded httplib::Server on
// 127.0.0.1:<kernel-picked-port>, serves the four static assets compiled
// into the binary (index.html, app.js, style.css, favicon.svg), and
// translates every /api/* endpoint into an IPC call against the daemon
// via the IpcClient handed in at startup. Replaces the standalone web
// binary that previously shipped as a separate process.
//
// Listener lifecycle is lazy: start_web_listener() is invoked on the
// first "Open Speaker Management" menu click and the listener stays up
// until the tray quits (stop_web_listener() in the on_quit slot). The
// listener runs on its own std::thread so httplib's blocking
// listen_after_bind() does not stall the GTK main loop.
//
// The listener binds loopback only and serves /api/* + static assets
// from the same origin; no CORS preflights or Access-Control-Allow-*
// headers are emitted (plan M-E6-4 — same-origin under loopback).

namespace recmeet {

class IpcClient;

// Start the WebUI HTTP listener on 127.0.0.1:<auto-picked-port>.
//
// Idempotent: re-entry returns the already-bound port without spawning a
// second listener thread. Returns the resolved port (>0) on success, -1
// on bind/listen error.
//
// `client` must outlive the returned listener — the tray (the sole
// caller) owns the IpcClient as a member of TrayState, so this is
// satisfied by construction; the listener captures a pointer to it.
int start_web_listener(IpcClient& client);

// Stop the WebUI HTTP listener, joining the listener thread. Safe to
// call when the listener has never been started or has already been
// stopped.
void stop_web_listener();

// Phase E.6.3 — accessor for the resolved listener port. Returns the
// kernel-picked port the listener is currently bound to (>0), or -1 if
// the listener has never been started or has been stopped. Used by the
// tray's headless-mode startup log so operators don't need `ss -ltnp`
// to discover the port.
int get_listener_port();

} // namespace recmeet
