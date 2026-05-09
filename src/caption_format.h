// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 5.5 — render-time caption normalization + the lightweight render-state
// machine used by both the tray overlay and the CLI stderr renderer.
//
// The streaming Zipformer model (en-2023-06-26) emits ALL-CAPS ASCII without
// punctuation. The IPC `caption` event payload carries that raw text verbatim
// (Phase 3 — daemon-side never normalizes); render-side clients pass it
// through `normalize_caption()` for human-readable display. `.vtt` writing
// (Phase 6) optionally applies the same normalization.
//
// Pure string manipulation — no GTK / sherpa / IPC dependencies, so this
// header lives in `recmeet_ipc` (linked by both tray and CLI/daemon).

#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <string>
#include <string_view>

namespace recmeet {

/// Normalize raw engine caption text for human display. The default
/// streaming Zipformer (en-2023-06-26) emits ALL-CAPS without punctuation;
/// this helper lowercases everything, then capitalizes the first letter
/// and the first letter after sentence-boundary punctuation `[.!?]\s+`.
///
/// Input is treated as UTF-8; the implementation operates on ASCII for
/// the case-conversion logic (the model's tokenizer emits English ASCII
/// alphabetics + spaces). Non-ASCII bytes pass through unchanged.
///
/// Pure function; no allocation beyond the result string. Safe to call
/// from any thread.
std::string normalize_caption(std::string_view raw);

/// Format a caption line for the CLI `--show-captions` stderr renderer.
/// Returns the rendered text (without ANSI escapes — the wire-level
/// `\r` / `\033[K` are added by the stderr writer). The shape is stable
/// for unit testing:
///
///   "[captions] (partial) hello world"
///   "[captions] (final)   Hello world."
///
/// `apply_normalize` gates `normalize_caption()`. When false, the raw
/// engine text is rendered verbatim — useful for transcript fidelity
/// debugging.
std::string format_caption_for_cli(std::string_view raw,
                                   bool is_partial,
                                   bool apply_normalize);

/// Format a `caption.degraded` line for the CLI stderr renderer.
std::string format_caption_degraded_for_cli(std::string_view reason);

// ---------------------------------------------------------------------------
// CaptionRenderState — pure state machine used by the tray overlay (and any
// other client that wants to render a rolling caption buffer). GUI-free so
// it's directly unit-testable.
//
// Wire it up like this:
//
//   CaptionRenderState st;
//   on_caption_event(text, is_partial)  -> st.update(text, is_partial)
//   on_degraded_event(reason)           -> st.degraded(reason)
//   periodic tick (e.g. 500ms)          -> if (st.tick(now)) hide_overlay()
//   render                              -> label.set_markup(st.to_label_markup())
// ---------------------------------------------------------------------------

class CaptionRenderState {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit CaptionRenderState(std::size_t max_lines = 3,
                                std::chrono::milliseconds auto_hide_after =
                                    std::chrono::seconds(5));

    /// Append or replace a caption line. Partial captions replace the most
    /// recent partial line (or append a new partial slot if the previous
    /// line is finalized). Finalized captions promote the trailing partial
    /// (if any) to a final line and start a fresh partial slot on the next
    /// `update`. The visible history is capped at `max_lines` finals;
    /// the partial does not count against the cap.
    void update(std::string_view raw_text, bool is_partial,
                bool apply_normalize = true,
                TimePoint now = Clock::now());

    /// Record a degradation reason. `clear=false` keeps the marker until
    /// the next `update`; `clear=true` resets it.
    void degraded(std::string_view reason, TimePoint now = Clock::now());

    /// Observe the current time. Returns true when the overlay should be
    /// hidden — i.e. `auto_hide_after` has elapsed since the last update
    /// AND the trailing line is finalized (we don't auto-hide on top of
    /// a live partial).
    bool tick(TimePoint now) const;

    /// Render the current state as Pango markup for `gtk_label_set_markup`.
    /// Final lines are normal weight; partials are italicized; a degraded
    /// banner is appended for `degraded_visible_for_` after the marker is
    /// set.
    std::string to_label_markup() const;

    /// True when there is at least one line (final or partial) to display.
    bool has_content() const;

    /// Forget all caption history. Does NOT clear the degraded marker;
    /// call `degraded("", now, true)` for that.
    void clear();

    // Test introspection.
    std::size_t line_count() const { return lines_.size(); }
    bool degraded_active(TimePoint now) const;
    TimePoint last_update() const { return last_update_; }

private:
    struct Line {
        std::string text;
        bool is_partial;
    };

    std::deque<Line> lines_;
    std::size_t max_lines_;
    std::chrono::milliseconds auto_hide_after_;

    TimePoint last_update_{};
    bool ever_updated_ = false;

    std::string degraded_reason_;
    TimePoint degraded_at_{};
    std::chrono::milliseconds degraded_visible_for_{2000};
};

} // namespace recmeet
