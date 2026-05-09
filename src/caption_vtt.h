// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase 6 — `.vtt` sidecar persistence for live captions.
//
// `VttWriter` appends finalized caption cues to a WebVTT sidecar file living
// alongside the meeting's audio (typically `<meeting_dir>/captions.vtt`).
// Design constraints:
//
//   - Append-only. Finals append; partials are silently dropped (defense in
//     depth on top of the daemon's existing partial filter).
//   - Lazy file creation. The header `WEBVTT\n\n` is written on the first
//     finalized append, so silent sessions produce no `.vtt` at all.
//   - Single `write(2)` per cue. On Linux, `O_APPEND` writes up to one
//     filesystem block (~4 KiB on btrfs/ext4) are atomic against concurrent
//     writers — well above WebVTT's ~100 B per cue. We do NOT call `fsync`;
//     the file is non-authoritative and the FS writeback window (≤30 s) is
//     acceptable. On crash mid-recording the file is valid up to the last
//     fully-flushed cue (parsers tolerate trailing garbage).
//   - No cue identifiers. Each cue block is self-contained; the writer
//     never has to renumber.
//   - No UTF-8 validation. Raw bytes pass through to `write()`.
//
// Pure I/O — no sherpa-onnx dependency. Lives in `recmeet_core` next to
// `caption_engine.{h,cpp}`.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace recmeet {

class VttWriter {
public:
    /// Construct without opening — `path` is where the sidecar will live.
    /// File is NOT created until the first append() call (silent sessions
    /// produce no `.vtt` at all). When `normalize_display` is true (the
    /// default), text is passed through `normalize_caption()` before being
    /// formatted into a cue. Callers that want raw ALL-CAPS engine output
    /// pass `false`.
    explicit VttWriter(std::filesystem::path path, bool normalize_display = true);
    ~VttWriter();

    VttWriter(const VttWriter&) = delete;
    VttWriter& operator=(const VttWriter&) = delete;

    /// Append a finalized caption cue. Lazily opens the file and writes
    /// the `WEBVTT\n\n` header on first call. Subsequent calls append cue
    /// blocks. Caller must NOT call this with partial captions; partials
    /// are silently dropped (defense in depth — daemon already filters).
    /// Returns false on I/O error; sets last_error().
    bool append(std::int64_t start_ms, std::int64_t end_ms,
                std::string_view text, bool is_partial);

    /// Optional explicit close — destructor closes too.
    void close();

    bool is_open() const;
    std::string last_error() const;

    /// Test seam — true once the WEBVTT header has been written.
    bool _header_written_for_test() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Format an int64 millisecond timestamp as WebVTT `HH:MM:SS.mmm`. Pure.
/// Negative values clamp to 0 (the engine's timestamp is monotonic and
/// non-negative; the clamp is defense in depth).
std::string format_vtt_timestamp(std::int64_t ms);

/// Format a single WebVTT cue as a self-contained block (no cue id):
///     HH:MM:SS.mmm --> HH:MM:SS.mmm\n
///     <text>\n
///     \n
///
/// The trailing blank line is part of the cue terminator — every cue ends
/// with two `\n`s. If `text` contains the literal `-->` substring it is
/// replaced with `--&gt;` so a stray arrow never confuses a WebVTT parser.
/// `text` is otherwise byte-transparent (no UTF-8 validation).
std::string format_vtt_cue(std::int64_t start_ms, std::int64_t end_ms,
                           std::string_view text);

/// Pair-tracking helper used by the daemon's caption fan-out adapter to
/// turn the engine's single `timestamp_ms` per result into a (start, end)
/// pair for WebVTT. The previous final's timestamp becomes the next cue's
/// start; the current final's timestamp becomes the cue's end. The first
/// final's start is 0. Pure — no globals, no allocation.
struct VttCueTimer {
    std::int64_t prev_final_ms = 0;

    /// Returns (start_ms, end_ms) for the cue ending at `this_final_ms`,
    /// and updates the running watermark. If `this_final_ms` regresses
    /// below the previous watermark (shouldn't happen — the engine emits
    /// monotonic timestamps), the start clamps so end >= start.
    std::pair<std::int64_t, std::int64_t> next(std::int64_t this_final_ms) {
        std::int64_t start = prev_final_ms;
        if (this_final_ms < start) start = this_final_ms;
        prev_final_ms = this_final_ms;
        return {start, this_final_ms};
    }
};

} // namespace recmeet
