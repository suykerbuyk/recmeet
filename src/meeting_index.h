// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

// Phase C.11 — in-memory `meeting_id → meeting_dir_path` index.
//
// The convergence-principle dedup contract (docs/V2-STRATEGY.md
// "Meeting identity and the client-server audio contract") needs to
// answer one question on every `process.submit`: "have I seen this
// meeting_id before?" — and if yes, "which meeting directory does it
// live in?" This class owns that lookup.
//
// Storage is in-memory only. There is no on-disk index file in v1 —
// the source of truth is each meeting directory's `context.json` (per
// C.11.3, the `meeting_id` field is round-tripped there). On daemon
// startup the index is empty; `rebuild_from_disk(meetings_root)` walks
// the directory tree once, reads each `context.json` via
// `load_meeting_id()` (pipeline.h), and binds every valid id it finds.
// Cost is amortized against startup, never against an IPC request.
//
// Thread-safety: every public method is mutex-guarded. Lookups are
// frequent (one per `process.submit`); writes are rare (one per new
// meeting allocation + the startup rebuild). A single `std::mutex` is
// plenty for v1 — the contention window is microseconds and the index
// has at most O(meetings on disk) entries.

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace recmeet {

namespace fs = std::filesystem;

class MeetingIndex {
public:
    MeetingIndex() = default;

    MeetingIndex(const MeetingIndex&) = delete;
    MeetingIndex& operator=(const MeetingIndex&) = delete;

    /// Look up the directory path bound to `meeting_id`. Returns
    /// `std::nullopt` for an unknown or empty id. The returned path
    /// reflects what was last `bind()`'d (or last `rebuild_from_disk`'d)
    /// — the index does NOT re-stat the filesystem on every find. The
    /// upload finalize path (C.11.4) handles "indexed-but-dir-vanished"
    /// as a normal allocate path; the index stays a pure map.
    std::optional<fs::path> find(const std::string& meeting_id) const;

    /// Bind `meeting_id` to `dir`. Idempotent: a no-op when the binding
    /// already exists with the same path; an overwrite otherwise. The
    /// caller is responsible for validating `meeting_id` via
    /// `is_valid_meeting_id` (util.h) — `bind()` itself stores whatever
    /// it is handed, because by the time we are inside the daemon's
    /// handler the request has already been wire-validated.
    void bind(const std::string& meeting_id, const fs::path& dir);

    /// Drop the binding for `meeting_id`. Returns true when a binding
    /// existed and was removed, false otherwise. Unused by C.11.4 itself
    /// (the convergence-principle dedup never deletes — only overwrites
    /// existing entries or allocates new ones), but exposed for
    /// completeness and for the operator-side `--evict` paths that may
    /// land in later phases.
    bool unbind(const std::string& meeting_id);

    /// Walk `meetings_root` and rebuild the index from each meeting
    /// directory's `context.json`. Skips:
    ///   - non-directory entries at the top level (stray files),
    ///   - directories with no readable `context.json`,
    ///   - context files whose `meeting_id` field is absent or fails
    ///     `is_valid_meeting_id()` (covered by `load_meeting_id`'s
    ///     defensive return).
    ///
    /// **Replaces** the existing index contents under the lock — the
    /// rebuild is treated as the new source of truth. The intended
    /// caller is the daemon's startup path (before `server.on()`
    /// handlers are wired), so there is no "merge with in-flight
    /// bindings" mode in v1.
    ///
    /// Returns the number of bindings populated. A missing or
    /// non-directory `meetings_root` is not an error — returns 0 with
    /// an empty index.
    std::size_t rebuild_from_disk(const fs::path& meetings_root);

    /// Number of bindings currently in the index. Testing surface.
    std::size_t size() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, fs::path> by_id_;
};

} // namespace recmeet
