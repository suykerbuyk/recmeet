// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "tray_status.h"

#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>

namespace recmeet {

namespace {

const char* slot_display_name(SlotKind k) {
    switch (k) {
        case SlotKind::Postprocess:   return "Postprocess";
        case SlotKind::Streaming:     return "Streaming";
        case SlotKind::ModelDownload: return "Model Download";
    }
    return "Postprocess";  // unreachable; keeps -Wreturn-type quiet
}

} // namespace

std::string render_reconnect_status_line(bool connected,
                                         bool reconnect_armed,
                                         int seconds_until_reconnect) {
    if (connected) return "Status: Connected";
    if (!reconnect_armed) {
        // No reconnect is scheduled — most commonly the D.3 Unix-out-of-
        // scope guard rejected the schedule; the operator must restart
        // the tray. The status line stays terse; the diagnosis lives in
        // the daemon log.
        return "Status: Disconnected";
    }
    if (seconds_until_reconnect <= 0) {
        return "Status: Disconnected \xe2\x80\x94 reconnecting...";
    }
    std::ostringstream oss;
    oss << "Status: Disconnected \xe2\x80\x94 reconnect in "
        << seconds_until_reconnect << "s";
    return oss.str();
}

std::string capitalize_first(std::string s) {
    if (s.empty()) return s;
    unsigned char first = static_cast<unsigned char>(s[0]);
    if (first < 0x80 && std::isalpha(first)) {
        s[0] = static_cast<char>(std::toupper(first));
    }
    return s;
}

std::string render_slot_row(SlotKind slot,
                            const std::optional<InFlightView>& in_flight,
                            std::size_t queue_depth) {
    std::ostringstream oss;
    oss << slot_display_name(slot) << ": ";

    if (!in_flight.has_value()) {
        oss << "Idle";
    } else {
        const auto& f = *in_flight;
        if (f.phase.empty()) {
            oss << "Working...";
        } else {
            oss << capitalize_first(f.phase) << "...";
            if (f.progress_percent >= 0) {
                oss << " " << f.progress_percent << "%";
            }
        }
    }

    if (queue_depth > 0) {
        oss << " (" << queue_depth << " queued)";
    }
    return oss.str();
}

std::string render_server_row(const ServerView& sv) {
    std::ostringstream oss;
    oss << sv.name;
    if (!sv.address.empty()) {
        oss << " (" << sv.address << ")";
    }
    oss << ": ";
    if (sv.queued_total == 0) {
        oss << "Idle";
    } else {
        oss << sv.queued_total << " queued";
    }
    return oss.str();
}

std::vector<ServerView> derive_single_server_view(const std::string& address,
                                                  std::size_t queued_total) {
    ServerView sv;
    sv.name = "server-1";
    sv.address = address.empty() ? std::string("default") : address;
    sv.queued_total = queued_total;
    std::vector<ServerView> v;
    v.push_back(std::move(sv));
    return v;
}

std::string format_caption_inline(std::string_view text,
                                  std::size_t max_chars) {
    if (text.empty() || max_chars == 0) return std::string();
    if (text.size() <= max_chars) return std::string(text);

    // Walk back from `text.size() - max_chars` to the next UTF-8 starter
    // byte so we never split a multi-byte sequence in half. A starter
    // byte is any byte whose top two bits are NOT `10`; continuation
    // bytes are `10xxxxxx`.
    std::size_t start = text.size() - max_chars;
    while (start < text.size() &&
           (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
        ++start;
        // Defensive: if every remaining byte is a continuation (malformed
        // input), bail out and return an empty tail rather than the full
        // string — the menu can render an empty caption row safely.
        if (start >= text.size()) return std::string();
    }
    return std::string(text.substr(start));
}

std::string render_caption_inline_row(std::string_view inline_text,
                                      std::size_t max_chars) {
    std::string truncated = format_caption_inline(inline_text, max_chars);
    if (truncated.empty()) return std::string();
    return "Caption: " + truncated;
}

} // namespace recmeet
