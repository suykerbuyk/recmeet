// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.4 — pure renderer helpers for the tray menu's status surfaces.
//
// Carved out of `tray.cpp` so the menu-row text generation can be unit-
// tested without booting GTK. The tray glues these helpers into
// `build_menu()` (status line + per-server row + per-slot rows + caption-
// inline row). Three orthogonal concerns live here:
//
//   1. `render_reconnect_status_line` — connection state line with a
//      jitter-aware reconnect countdown in seconds. The countdown value
//      is whatever D.3's `schedule_reconnect_attempt()` chose — D.4 does
//      NOT re-roll jitter; it surfaces what D.3 already armed.
//
//   2. `render_slot_row` — one row per typed-slot kind (postprocess,
//      streaming, model_download). Carries the in-flight job's phase +
//      progress (sourced from the tray's per-slot maps
//      `current_phase_by_slot` / `progress_percent_by_slot`, keyed by
//      SlotKind — the D.4-follow-up replacement for the original single
//      shared pair which collapsed concurrent-slot UI state) plus a
//      "(N queued)" suffix when the backlog depth is non-zero.
//
//   3. `render_server_row` + `derive_single_server_view` — per-server
//      queue-depth display. The renderer iterates a `std::vector<ServerView>`
//      from day one (multi-server hook #5); v1 has a length-1 list. The
//      derive helper builds the v1 single-entry list locally without
//      touching `Config` — Phase E owns the schema split (plan lines
//      416-425).
//
//   4. `format_caption_inline` — truncate the latest streaming-caption
//      text to a tray-friendly length. The tray reads the existing
//      `CaptionRenderState::latest_text()` source (no duplicate caption
//      event subscription) and renders the last-N-chars view inline in
//      the menu, IN ADDITION TO the C.10a overlay.
//
// All helpers are GTK-free and side-effect-free; the test suite
// (`[d4]`-tagged) drives them with hand-constructed inputs.

#pragma once

#include "slot_queue.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace recmeet {

/// View of the in-flight job for one slot, as seen by the tray's
/// renderer. The tray populates this from `slot_queues.<kind>.in_flight()`
/// plus the per-slot maps `current_phase_by_slot` /
/// `progress_percent_by_slot` (D.4 follow-up — the pre-fix design used
/// a single shared pair of globals; per-slot keys ensure concurrent
/// in-flight jobs in different slots render their own state). The view
/// is a value type so tests can construct it directly without depending
/// on `SlotQueue` or `TrayState` internals.
struct InFlightView {
    /// Server-assigned job_id (post-dispatch). 0 → unknown (e.g. the
    /// admit-pre-dispatch window). Not rendered today; reserved for D.6
    /// notifications.
    int64_t job_id = 0;
    /// Phase string (e.g. "transcribe", "diarize"). Empty when the job
    /// is admitted but has not yet emitted any phase event.
    std::string phase;
    /// Progress percent. `-1` → no progress emitted yet; renderer shows
    /// phase only. `>=0` → renderer appends ` <p>%`.
    int progress_percent = -1;
};

/// One server entry in the per-server queue-depth display. Multi-server
/// hook #5: the renderer always iterates a list of these, even though
/// v1's list has length 1. `queued_total` is the SUM of backlog sizes
/// across all slots that this server owns — the operator-facing "what
/// is queued up against this daemon" metric. The slot-level breakdown
/// lives in the per-slot rows above; this row is the per-server
/// aggregate.
struct ServerView {
    /// Display name (e.g. "server-1"). Multi-server config (Phase E.2)
    /// will surface the operator-chosen name; v1 derives a stable
    /// placeholder.
    std::string name;
    /// Wire address (Unix socket path or `host:port`). Surfaced after
    /// the name so the operator can disambiguate two identically-named
    /// entries pointing at different endpoints (a v2 corner case the
    /// v1 single-entry path also tolerates).
    std::string address;
    /// Aggregate queue depth (sum of per-slot backlog sizes for this
    /// server). v1 has one server, so this equals the total tray-side
    /// backlog.
    std::size_t queued_total = 0;
};

/// Render the connection-state line for the tray menu's top status row.
///
/// `connected=true` returns "Status: Connected" — the per-slot rows below
/// carry the actual liveness data, so the status line stays terse.
///
/// `connected=false, reconnect_armed=false` returns "Status: Disconnected"
/// — no countdown to surface because no reconnect is scheduled (e.g. the
/// D.3 Unix-out-of-scope guard rejected the schedule; the operator must
/// restart the tray).
///
/// `connected=false, reconnect_armed=true, seconds_until_reconnect <= 0`
/// returns "Status: Disconnected — reconnecting..." (mid-attempt: the
/// timer just fired and the connect call is in progress).
///
/// `connected=false, reconnect_armed=true, seconds_until_reconnect > 0`
/// returns "Status: Disconnected — reconnect in <N>s". The value
/// reflects D.3's chosen jittered delay; D.4 reads it, does NOT
/// re-compute jitter.
std::string render_reconnect_status_line(bool connected,
                                         bool reconnect_armed,
                                         int seconds_until_reconnect);

/// Capitalize the first ASCII letter of `s` in place. Helper for phase
/// label rendering — the daemon emits lower-case phase names; the menu
/// shows "Transcribe..." not "transcribe...". Idempotent.
std::string capitalize_first(std::string s);

/// Render one slot row. The label format is stable:
///
///   "<SlotName>: Idle"                              (no in-flight, no queue)
///   "<SlotName>: Idle (N queued)"                   (no in-flight, queue>0)
///   "<SlotName>: <Phase>..."                        (in-flight, no progress)
///   "<SlotName>: <Phase>... <p>%"                   (in-flight, progress>=0)
///   "<SlotName>: <Phase>... <p>% (N queued)"        (in-flight + queue>0)
///   "<SlotName>: Working..."                        (in-flight, empty phase)
///
/// SlotName is the plain-English label ("Postprocess", "Streaming",
/// "Model Download"). The phase is `capitalize_first()`-ed so the
/// daemon's lower-case strings render cleanly.
///
/// `queue_depth` is `slot_queues.<kind>.backlog_size()` from D.1.
/// `in_flight` is `nullopt` iff the slot is fully idle (no in-flight,
/// nothing pending). Today's tray sees in-flight without backlog (the
/// production UI has no "queue up another submission" affordance yet)
/// and idle-with-backlog is unreachable, but the renderer handles every
/// combination so the testable matrix is complete.
std::string render_slot_row(SlotKind slot,
                            const std::optional<InFlightView>& in_flight,
                            std::size_t queue_depth);

/// Render one per-server row. Format:
///
///   "<name> (<address>): Idle"           (queued_total == 0)
///   "<name> (<address>): <N> queued"     (queued_total > 0)
///
/// The address is shown in parentheses so the operator can spot which
/// daemon a queued backlog belongs to in a future multi-server world.
std::string render_server_row(const ServerView& sv);

/// Derive the v1 single-server view list from the tray's known address
/// + aggregate backlog. Pure helper so the renderer's iteration shape
/// (`for (auto& sv : servers) ...`) is correct from day one — multi-
/// server is purely additive (Phase E.2 swaps this for `Config::servers`).
///
/// `address` is the wire address (`g_tray.daemon_addr` if non-empty,
/// otherwise a Unix-socket placeholder). Empty `address` → "(default)"
/// as the rendered placeholder. The name is always "server-1" in v1.
std::vector<ServerView> derive_single_server_view(const std::string& address,
                                                  std::size_t queued_total);

/// Truncate the most-recent streaming caption to `max_chars` for inline
/// display in the tray menu. The overlay (C.10a) carries the full
/// rolling buffer; the menu surface gets the tail (right-most window)
/// because that is the freshest text. UTF-8 safe: we never split a
/// multi-byte sequence; if `text.size() <= max_chars` the input is
/// returned verbatim, otherwise we walk back from `text.size() -
/// max_chars` to the previous UTF-8 starter byte (top two bits != 10).
///
/// Empty input returns empty string — caller suppresses the inline row
/// entirely in that case (no caption to surface).
std::string format_caption_inline(std::string_view text,
                                  std::size_t max_chars = 80);

/// Render the caption-inline row for the streaming-slot. The full menu
/// label format is "Caption: <truncated>". The renderer is separate
/// from `format_caption_inline` so a future inline label change (e.g.
/// localization) is a one-spot fix.
///
/// Empty `inline_text` returns empty string; caller suppresses the row.
std::string render_caption_inline_row(std::string_view inline_text,
                                      std::size_t max_chars = 80);

} // namespace recmeet
