// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.4 — pure renderer tests for the tray menu status surfaces.
//
// Five tests, all tagged `[d4]`:
//   1. [d4][slot-row]          — per-slot status display covers the
//                                 (in-flight + phase + progress) matrix
//                                 across all three slots, including an
//                                 empty (idle) slot.
//   2. [d4][slot-row][queue]   — "(N queued)" suffix appears on backlog
//                                 depth > 0 (both with and without an
//                                 in-flight entry).
//   3. [d4][caption-inline]    — caption-inline rendering truncates to
//                                 the configured tail (default 80 chars),
//                                 preserves UTF-8 boundaries, and
//                                 suppresses on empty input.
//   4. [d4][reconnect-line]    — connection-state line with the jitter-
//                                 aware reconnect countdown decrements
//                                 by 1 each successive call (using a
//                                 monotonically-decreasing remaining
//                                 input — D.4 reads what D.3 chose, it
//                                 does NOT re-roll jitter).
//   5. [d4][server-row][multi] — per-server display uses list iteration
//                                 from day one (multi-server hook #5);
//                                 v1's `derive_single_server_view`
//                                 produces a length-1 list whose row
//                                 format matches the multi-server case.
//
// All tests are pure / hermetic (no IPC, no GTK). The renderer's
// `[d4]` suite runs in foreground; flake-loop 3x is part of the
// orchestrator's verification protocol.

#include <catch2/catch_test_macros.hpp>

#include "caption_format.h"
#include "slot_queue.h"
#include "tray_status.h"

#include <chrono>
#include <string>
#include <vector>

using namespace recmeet;

// ---------------------------------------------------------------------------
// Test 1 — per-slot status display
// ---------------------------------------------------------------------------

TEST_CASE("D.4: per-slot rows render phase + progress from C.14 fields, "
          "across the three typed-slot kinds",
          "[d4][slot-row]") {
    // Postprocess in-flight: phase + progress populated (the C.14
    // globals `current_phase` / `progress_percent` are the source of
    // truth — D.4 surfaces them, does not re-derive).
    {
        InFlightView pp;
        pp.job_id = 42;
        pp.phase = "transcribe";
        pp.progress_percent = 37;
        std::string row = render_slot_row(
            SlotKind::Postprocess, pp, /*queue_depth=*/0);
        // "Postprocess: Transcribe... 37%"
        CHECK(row == "Postprocess: Transcribe... 37%");
    }

    // Streaming in-flight: phase but no progress (-1 → progress omitted)
    {
        InFlightView st;
        st.job_id = 99;
        st.phase = "recording";
        st.progress_percent = -1;
        std::string row = render_slot_row(
            SlotKind::Streaming, st, /*queue_depth=*/0);
        CHECK(row == "Streaming: Recording...");
    }

    // Model download in-flight: zero progress is distinct from -1
    {
        InFlightView md;
        md.phase = "downloading";
        md.progress_percent = 0;
        std::string row = render_slot_row(
            SlotKind::ModelDownload, md, /*queue_depth=*/0);
        CHECK(row == "Model Download: Downloading... 0%");
    }

    // Empty third slot (no in-flight) — renders "Idle".
    {
        std::string row = render_slot_row(
            SlotKind::Streaming, std::nullopt, /*queue_depth=*/0);
        CHECK(row == "Streaming: Idle");
    }

    // In-flight with empty phase (admit-pre-first-event window) renders
    // the "Working..." fallback.
    {
        InFlightView w;
        w.phase.clear();
        w.progress_percent = -1;
        std::string row = render_slot_row(
            SlotKind::Postprocess, w, /*queue_depth=*/0);
        CHECK(row == "Postprocess: Working...");
    }
}

// ---------------------------------------------------------------------------
// Test 2 — queue-depth suffix
// ---------------------------------------------------------------------------

TEST_CASE("D.4: queue depth surfaces as `(N queued)` suffix on the row",
          "[d4][slot-row][queue]") {
    // (a) Push two entries into the postprocess SlotQueue — one becomes
    //     the in-flight entry, the second joins the backlog (depth = 1).
    SlotQueues q;
    JobEntry e1;
    e1.meeting_id = "mtg-1";
    e1.kind = "submit";
    REQUIRE(q.postprocess.admit(e1));
    CHECK(q.postprocess.is_in_flight());
    CHECK(q.postprocess.backlog_size() == 0);

    JobEntry e2;
    e2.meeting_id = "mtg-2";
    e2.kind = "submit";
    REQUIRE_FALSE(q.postprocess.admit(e2));
    CHECK(q.postprocess.backlog_size() == 1);

    JobEntry e3;
    e3.meeting_id = "mtg-3";
    e3.kind = "submit";
    REQUIRE_FALSE(q.postprocess.admit(e3));
    CHECK(q.postprocess.backlog_size() == 2);

    // (b) Render with the live in-flight view + the real backlog count.
    InFlightView v;
    v.phase = "diarize";
    v.progress_percent = 50;
    std::string row = render_slot_row(
        SlotKind::Postprocess, v, q.postprocess.backlog_size());
    CHECK(row == "Postprocess: Diarize... 50% (2 queued)");

    // (c) Idle slot with a non-zero queue depth (renderer-completeness):
    //     "Streaming: Idle (3 queued)". The production tray cannot reach
    //     this state today, but the renderer must handle it for the
    //     post-D.4 backlog-dispatcher seam.
    std::string row_idle = render_slot_row(
        SlotKind::Streaming, std::nullopt, /*queue_depth=*/3);
    CHECK(row_idle == "Streaming: Idle (3 queued)");
}

// ---------------------------------------------------------------------------
// Test 3 — caption-inline truncation + render-row gating
// ---------------------------------------------------------------------------

TEST_CASE("D.4: caption-inline truncates to a tray-friendly tail, "
          "preserving UTF-8 boundaries",
          "[d4][caption-inline]") {
    // ASCII shorter than the cap: pass through verbatim.
    {
        std::string out = format_caption_inline("hello world", /*max=*/80);
        CHECK(out == "hello world");
    }

    // ASCII longer than the cap: returns the last <max> characters.
    {
        std::string long_text;
        long_text.reserve(200);
        for (int i = 0; i < 200; ++i) long_text.push_back('a' + (i % 26));
        std::string out = format_caption_inline(long_text, /*max=*/10);
        REQUIRE(out.size() == 10);
        CHECK(out == long_text.substr(long_text.size() - 10));
    }

    // Empty input: empty output. The render-row helper suppresses on
    // empty so the menu row never lands when there is nothing to show.
    {
        CHECK(format_caption_inline("", /*max=*/80).empty());
        CHECK(render_caption_inline_row("", /*max=*/80).empty());
    }

    // UTF-8 boundary: a tail that would split a 2-byte sequence walks
    // forward to the next starter so the rendered string is valid UTF-8.
    // The Greek small letter alpha "α" is two bytes (0xCE 0xB1).
    {
        std::string utf;
        utf += "abcd";          // 4 ASCII bytes
        utf += "\xce\xb1";      // α (2 bytes, single codepoint)
        utf += "\xce\xb2";      // β (2 bytes, single codepoint)
        // Total: 8 bytes, 6 codepoints.
        REQUIRE(utf.size() == 8);

        // max=3 → would slice into the middle of α. The walker advances
        // past the continuation byte so we land at β's starter (1 codepoint,
        // 2 bytes).
        std::string out = format_caption_inline(utf, /*max=*/3);
        CHECK(out == "\xce\xb2");  // pure β, no split sequence
    }

    // render_caption_inline_row composes "Caption: <truncated>".
    {
        std::string r = render_caption_inline_row("hello", /*max=*/80);
        CHECK(r == "Caption: hello");
    }
}

// ---------------------------------------------------------------------------
// Test 4 — reconnect countdown line
// ---------------------------------------------------------------------------

TEST_CASE("D.4: connection-state line surfaces the jitter-aware reconnect "
          "countdown that D.3 already chose (no re-roll)",
          "[d4][reconnect-line]") {
    // Connected: terse "Status: Connected" — per-slot rows carry the rest.
    CHECK(render_reconnect_status_line(
              /*connected=*/true, /*armed=*/false, 0)
          == "Status: Connected");
    // Sanity: stale countdown / armed inputs are ignored once connected.
    CHECK(render_reconnect_status_line(
              /*connected=*/true, /*armed=*/true, 7)
          == "Status: Connected");

    // Disconnected + not armed (D.3 Unix-out-of-scope path): no
    // countdown, no "reconnecting..." (because nothing IS reconnecting).
    CHECK(render_reconnect_status_line(
              /*connected=*/false, /*armed=*/false, 0)
          == "Status: Disconnected");
    CHECK(render_reconnect_status_line(
              /*connected=*/false, /*armed=*/false, 99)
          == "Status: Disconnected");

    // Disconnected + armed + countdown <= 0: "reconnecting..."
    // (mid-attempt window between timer-fire and connect result).
    CHECK(render_reconnect_status_line(
              /*connected=*/false, /*armed=*/true, 0)
          == "Status: Disconnected \xe2\x80\x94 reconnecting...");
    CHECK(render_reconnect_status_line(
              /*connected=*/false, /*armed=*/true, -3)
          == "Status: Disconnected \xe2\x80\x94 reconnecting...");

    // Disconnected + armed + countdown > 0: live "reconnect in <N>s".
    // The value is the input — D.4 surfaces what D.3 already chose.
    // Simulate a monotonically decrementing count (the 1 Hz tick walks
    // it down) to assert the rendered string reflects the live input.
    for (int n = 12; n >= 1; --n) {
        std::string label = render_reconnect_status_line(
            /*connected=*/false, /*armed=*/true, n);
        std::string expected =
            "Status: Disconnected \xe2\x80\x94 reconnect in "
            + std::to_string(n) + "s";
        CHECK(label == expected);
    }
}

// ---------------------------------------------------------------------------
// Test 5 — per-server row with multi-server hook list iteration
// ---------------------------------------------------------------------------

TEST_CASE("D.4: per-server display iterates a list from day one — v1 has "
          "length 1; the renderer shape is multi-server-additive",
          "[d4][server-row][multi]") {
    // v1 derive: single entry, empty address falls back to "default".
    {
        auto servers = derive_single_server_view(/*address=*/"", 0);
        REQUIRE(servers.size() == 1);
        CHECK(servers[0].name == "server-1");
        CHECK(servers[0].address == "default");
        CHECK(servers[0].queued_total == 0);
        std::string row = render_server_row(servers[0]);
        CHECK(row == "server-1 (default): Idle");
    }

    // v1 derive: non-empty address + non-zero queued.
    {
        auto servers = derive_single_server_view(
            "recmeet-host.local:7777", /*queued_total=*/4);
        REQUIRE(servers.size() == 1);
        CHECK(servers[0].address == "recmeet-host.local:7777");
        std::string row = render_server_row(servers[0]);
        CHECK(row == "server-1 (recmeet-host.local:7777): 4 queued");
    }

    // Multi-server shape (v2 forward-compat): build a length-2 vector
    // by hand and iterate it the same way `build_menu()` does. This
    // proves the renderer + iteration shape generalize unchanged when
    // Phase E.2 lands `Config::servers` as a list.
    {
        std::vector<ServerView> servers;
        ServerView a;
        a.name = "primary";
        a.address = "10.0.0.1:7777";
        a.queued_total = 1;
        ServerView b;
        b.name = "secondary";
        b.address = "10.0.0.2:7777";
        b.queued_total = 0;
        servers.push_back(a);
        servers.push_back(b);

        std::vector<std::string> rendered;
        for (const auto& sv : servers) {
            rendered.push_back(render_server_row(sv));
        }
        REQUIRE(rendered.size() == 2);
        CHECK(rendered[0] == "primary (10.0.0.1:7777): 1 queued");
        CHECK(rendered[1] == "secondary (10.0.0.2:7777): Idle");
    }
}

// ---------------------------------------------------------------------------
// Bonus assertion — `CaptionRenderState::latest_text` is the inline-row
// source. The accessor exists so the menu reads the overlay's freshest
// text WITHOUT subscribing to the caption event a second time. Verifies
// the contract D.4's `build_menu()` relies on (no duplicate subscription).
// ---------------------------------------------------------------------------

TEST_CASE("D.4: CaptionRenderState::latest_text returns the most recent "
          "line — caption-inline reads the same buffer the overlay uses",
          "[d4][caption-inline][overlay-source]") {
    CaptionRenderState st;
    CHECK(st.latest_text().empty());

    // A partial caption lands as the trailing line.
    st.update("HELLO WORLD", /*is_partial=*/true);
    CHECK(st.latest_text() == "Hello world");

    // A final overwrites the trailing partial in place.
    st.update("HELLO WORLD.", /*is_partial=*/false);
    CHECK(st.latest_text() == "Hello world.");

    // A new partial appends — latest now reflects the new partial.
    st.update("AND HOW ARE YOU", /*is_partial=*/true);
    CHECK(st.latest_text() == "And how are you");

    // format_caption_inline applied to a typical render-state output —
    // the tail-window truncation works end-to-end.
    std::string truncated = format_caption_inline(st.latest_text(), 80);
    CHECK(truncated == "And how are you");
}
