// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 6 — `.vtt` sidecar persistence. See caption_vtt.h.

#include "caption_vtt.h"

#include "caption_format.h"
#include "log.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace recmeet {

namespace {

// Single-pass `-->` -> `--&gt;` escape. WebVTT parsers see `-->` as the
// timestamp arrow; embedding one in cue text confuses the parse. Realistic
// engine output won't contain it but defense in depth is cheap.
std::string escape_vtt_arrow(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    static constexpr std::string_view kArrow{"-->"};
    std::size_t i = 0;
    while (i < in.size()) {
        if (in.size() - i >= kArrow.size()
            && in.compare(i, kArrow.size(), kArrow) == 0) {
            out.append("--&gt;");
            i += kArrow.size();
        } else {
            out.push_back(in[i]);
            ++i;
        }
    }
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Pure formatters
// ---------------------------------------------------------------------------

std::string format_vtt_timestamp(std::int64_t ms) {
    if (ms < 0) ms = 0;
    std::int64_t total_seconds = ms / 1000;
    std::int64_t millis = ms % 1000;
    std::int64_t hours = total_seconds / 3600;
    std::int64_t minutes = (total_seconds % 3600) / 60;
    std::int64_t seconds = total_seconds % 60;

    // HH:MM:SS.mmm — `HH` is at least two digits but unbounded above (a
    // multi-day session would still render correctly with %02lld widening).
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld.%03lld",
                  static_cast<long long>(hours),
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds),
                  static_cast<long long>(millis));
    return std::string(buf);
}

std::string format_vtt_cue(std::int64_t start_ms, std::int64_t end_ms,
                           std::string_view text) {
    std::string body;
    body.reserve(text.size() + 64);
    body.append(format_vtt_timestamp(start_ms));
    body.append(" --> ");
    body.append(format_vtt_timestamp(end_ms));
    body.push_back('\n');
    body.append(escape_vtt_arrow(text));
    body.push_back('\n');
    body.push_back('\n');
    return body;
}

// ---------------------------------------------------------------------------
// VttWriter — owns an O_APPEND fd and a "header written" flag
// ---------------------------------------------------------------------------

struct VttWriter::Impl {
    std::filesystem::path path;
    bool normalize_display = true;
    int fd = -1;
    bool header_written = false;
    std::string last_error;

    explicit Impl(std::filesystem::path p, bool normalize)
        : path(std::move(p)), normalize_display(normalize) {}

    ~Impl() { close_fd(); }

    void close_fd() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    // Open the file (O_APPEND | O_CREAT | O_WRONLY) and write the WEBVTT
    // header in a single write(2). Returns true on success; on failure sets
    // last_error and leaves fd in its prior state (closed if it was closed).
    bool ensure_open_and_header_written() {
        if (header_written) return true;
        if (fd < 0) {
            fd = ::open(path.c_str(),
                        O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC,
                        0644);
            if (fd < 0) {
                last_error = std::string("open() failed: ") + std::strerror(errno);
                return false;
            }
        }
        // Write the header. A short write on an 8-byte payload to a regular
        // file is essentially impossible, but loop for paranoia.
        static constexpr std::string_view kHeader{"WEBVTT\n\n"};
        const char* p = kHeader.data();
        std::size_t remaining = kHeader.size();
        while (remaining > 0) {
            ssize_t n = ::write(fd, p, remaining);
            if (n < 0) {
                if (errno == EINTR) continue;
                last_error = std::string("write(header) failed: ")
                             + std::strerror(errno);
                close_fd();
                return false;
            }
            p += n;
            remaining -= static_cast<std::size_t>(n);
        }
        header_written = true;
        return true;
    }
};

VttWriter::VttWriter(std::filesystem::path path, bool normalize_display)
    : impl_(std::make_unique<Impl>(std::move(path), normalize_display)) {}

VttWriter::~VttWriter() = default;

bool VttWriter::append(std::int64_t start_ms, std::int64_t end_ms,
                       std::string_view text, bool is_partial) {
    if (!impl_) return false;
    // Defense in depth: partials are never persisted. The daemon already
    // filters before reaching the writer in production; this guard makes
    // the writer self-contained and unit-testable.
    if (is_partial) return true;

    if (!impl_->ensure_open_and_header_written()) {
        // last_error already populated.
        return false;
    }

    // Format the cue. `format_vtt_cue` handles `-->` escaping and the
    // trailing blank-line terminator; we only need to apply normalization
    // here (the caller passes raw engine text for V1).
    std::string normalized;
    std::string_view body = text;
    if (impl_->normalize_display) {
        normalized = normalize_caption(text);
        body = normalized;
    }
    std::string cue = format_vtt_cue(start_ms, end_ms, body);

    // Single write(2). On Linux, O_APPEND writes up to one filesystem block
    // are atomic against concurrent writers; a typical cue (~100 B) fits
    // well within that. We still loop on EINTR / short writes for paranoia.
    const char* p = cue.data();
    std::size_t remaining = cue.size();
    while (remaining > 0) {
        ssize_t n = ::write(impl_->fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            impl_->last_error = std::string("write(cue) failed: ")
                                + std::strerror(errno);
            return false;
        }
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
}

void VttWriter::close() {
    if (impl_) impl_->close_fd();
}

bool VttWriter::is_open() const {
    return impl_ && impl_->fd >= 0;
}

std::string VttWriter::last_error() const {
    return impl_ ? impl_->last_error : std::string{};
}

bool VttWriter::_header_written_for_test() const {
    return impl_ && impl_->header_written;
}

} // namespace recmeet
