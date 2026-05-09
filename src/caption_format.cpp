// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "caption_format.h"

#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>

namespace recmeet {

namespace {

// ASCII whitespace check (matches \s for our purposes). Tab/newline count as
// sentence-boundary whitespace per the Phase 5.5 spec.
bool ascii_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

bool ascii_is_alpha(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

unsigned char ascii_to_lower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<unsigned char>(c - 'A' + 'a');
    return c;
}

unsigned char ascii_to_upper(unsigned char c) {
    if (c >= 'a' && c <= 'z') return static_cast<unsigned char>(c - 'a' + 'A');
    return c;
}

// Pango markup — escape the four entities that GTK's parser cares about.
std::string pango_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '\'': out += "&apos;"; break;
            case '"':  out += "&quot;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// normalize_caption — lowercase + sentence-boundary capitalization
// ---------------------------------------------------------------------------
//
// Algorithm:
//   1. Lowercase every ASCII alpha byte (UTF-8 multibyte sequences pass through
//      unchanged because their leading bytes are >= 0x80, outside our
//      lowercase range).
//   2. Walk a small state machine: capitalize the next ASCII alpha when we are
//      either at the start of the string OR have just observed a sequence of
//      `[.!?]` followed by one or more whitespace chars.
//
// Embedded periods like "FILE.TXT" do NOT trigger capitalization because the
// `.` is followed by an alpha (no whitespace) — matches the spec's regex
// `[.!?] +[A-Za-z]`.

std::string normalize_caption(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());

    // Two-flag state machine:
    //   capitalize_next : the next ASCII alpha byte should be uppercase.
    //                     Set initially (start-of-string) and after a
    //                     `[.!?] +` lookahead consumes a sentence
    //                     terminator + run of whitespace.
    //
    // UTF-8: ASCII alphabetics are <= 0x7F. Multi-byte sequences have
    // leading bytes >= 0xC0 and continuation bytes 0x80-0xBF, none of
    // which match `ascii_is_alpha`, so they pass through unchanged.
    bool capitalize_next = true;

    for (std::size_t i = 0; i < raw.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(raw[i]);

        if (ascii_is_alpha(c)) {
            unsigned char emit = capitalize_next ? ascii_to_upper(c)
                                                  : ascii_to_lower(c);
            out.push_back(static_cast<char>(emit));
            capitalize_next = false;
            continue;
        }

        // Non-alpha byte — emit verbatim.
        out.push_back(static_cast<char>(c));

        // Sentence-boundary detection: a terminator followed by one or
        // more whitespace chars arms the next-alpha capitalization.
        // Without trailing whitespace (e.g. "FILE.TXT") we DO NOT arm —
        // matches the spec's regex `[.!?] +[A-Za-z]`.
        if (c == '.' || c == '!' || c == '?') {
            std::size_t j = i + 1;
            if (j < raw.size()
                && ascii_is_space(static_cast<unsigned char>(raw[j]))) {
                while (j < raw.size()
                       && ascii_is_space(static_cast<unsigned char>(raw[j]))) {
                    out.push_back(raw[j]);
                    ++j;
                }
                i = j - 1;  // resume at last consumed ws; loop ++i moves past
                capitalize_next = true;
            }
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// format_caption_for_cli / format_caption_degraded_for_cli
// ---------------------------------------------------------------------------

std::string format_caption_for_cli(std::string_view raw,
                                   bool is_partial,
                                   bool apply_normalize) {
    std::string text = apply_normalize ? normalize_caption(raw) : std::string(raw);
    std::string indicator = is_partial ? "(partial)" : "(final)  ";
    return "[captions] " + indicator + " " + text;
}

std::string format_caption_degraded_for_cli(std::string_view reason) {
    return std::string("[captions] degraded: ") + std::string(reason);
}

// ---------------------------------------------------------------------------
// CaptionRenderState
// ---------------------------------------------------------------------------

CaptionRenderState::CaptionRenderState(std::size_t max_lines,
                                       std::chrono::milliseconds auto_hide_after)
    : max_lines_(max_lines == 0 ? 1 : max_lines),
      auto_hide_after_(auto_hide_after) {}

void CaptionRenderState::update(std::string_view raw_text, bool is_partial,
                                bool apply_normalize, TimePoint now) {
    std::string text = apply_normalize ? normalize_caption(raw_text)
                                       : std::string(raw_text);

    last_update_ = now;
    ever_updated_ = true;

    // Replace-trailing-partial-or-append rules:
    //   - is_partial:
    //       trailing line partial    -> replace it with the new partial
    //       trailing line final or   -> append new partial line
    //         no lines yet
    //   - !is_partial (final):
    //       trailing line partial    -> promote: replace its text with the
    //                                   final, mark non-partial
    //       trailing line final or   -> append new final line
    //         no lines yet
    //
    // After the operation, trim the FINAL-only history to `max_lines_`.
    // The trailing partial doesn't count against the cap.

    if (!lines_.empty() && lines_.back().is_partial) {
        lines_.back().text = std::move(text);
        lines_.back().is_partial = is_partial;
    } else {
        lines_.push_back(Line{std::move(text), is_partial});
    }

    // Trim. Count finals in the deque; if > max_lines_ finals, pop_front
    // until we are at the cap. (Partials are by construction only at the
    // back, so popping from the front never disturbs the partial slot.)
    std::size_t finals = 0;
    for (const auto& l : lines_) if (!l.is_partial) ++finals;
    while (finals > max_lines_) {
        if (!lines_.front().is_partial) {
            lines_.pop_front();
            --finals;
        } else {
            // Should be unreachable — partials only at back — but break
            // to avoid an infinite loop on a malformed deque.
            break;
        }
    }
}

void CaptionRenderState::degraded(std::string_view reason, TimePoint now) {
    degraded_reason_ = std::string(reason);
    degraded_at_ = now;
}

bool CaptionRenderState::degraded_active(TimePoint now) const {
    if (degraded_reason_.empty()) return false;
    return (now - degraded_at_) < degraded_visible_for_;
}

bool CaptionRenderState::tick(TimePoint now) const {
    if (!ever_updated_) return false;
    if (lines_.empty()) return false;
    // Don't auto-hide on top of a live partial — the user is mid-sentence.
    if (lines_.back().is_partial) return false;
    return (now - last_update_) >= auto_hide_after_;
}

bool CaptionRenderState::has_content() const {
    return !lines_.empty();
}

void CaptionRenderState::clear() {
    lines_.clear();
    // Keep ever_updated_ so tick() stays well-defined; the empty-lines
    // check above short-circuits before tick() returns true.
}

std::string CaptionRenderState::to_label_markup() const {
    std::ostringstream oss;
    bool first = true;
    for (const auto& line : lines_) {
        if (!first) oss << "\n";
        first = false;
        std::string escaped = pango_escape(line.text);
        if (line.is_partial) {
            oss << "<i>" << escaped << "</i>";
        } else {
            oss << escaped;
        }
    }

    // Degraded banner — appended after the most recent line. We use a
    // small-italic span with a distinct color hint. The window-level
    // border treatment is the tray's job; this is just the textual
    // marker so the renderer state machine is self-contained.
    if (!degraded_reason_.empty()) {
        // Active window check uses a fresh `now`; callers refresh markup
        // on each tick(), so degraded_active() is consulted at render
        // time. To keep `to_label_markup()` const-time-pure we always
        // emit the banner if the reason is set; the caller is expected
        // to also check `degraded_active(now)` and clear the reason
        // (via degraded("", now)) once the visible window has elapsed.
        // The state machine here favors a static include so unit tests
        // can verify markup shape deterministically.
        if (!first) oss << "\n";
        oss << "<small><i>(captions falling behind: "
            << pango_escape(degraded_reason_)
            << ")</i></small>";
    }

    return oss.str();
}

} // namespace recmeet
