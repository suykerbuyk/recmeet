// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "meeting_index.h"

#include "log.h"
#include "pipeline.h" // load_meeting_id

#include <system_error>

namespace recmeet {

std::optional<fs::path> MeetingIndex::find(const std::string& meeting_id) const {
    if (meeting_id.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = by_id_.find(meeting_id);
    if (it == by_id_.end()) return std::nullopt;
    return it->second;
}

void MeetingIndex::bind(const std::string& meeting_id, const fs::path& dir) {
    if (meeting_id.empty()) return; // defensive — empty is the "no id" sentinel
    std::lock_guard<std::mutex> lock(mu_);
    auto it = by_id_.find(meeting_id);
    if (it == by_id_.end()) {
        by_id_.emplace(meeting_id, dir);
        return;
    }
    if (it->second == dir) return; // idempotent — no-op on identical rebind
    log_warn("MeetingIndex: rebinding meeting_id %s from %s to %s — "
             "convergence-principle overwrite (last-bind-wins)",
             meeting_id.c_str(), it->second.string().c_str(),
             dir.string().c_str());
    it->second = dir;
}

bool MeetingIndex::unbind(const std::string& meeting_id) {
    if (meeting_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    return by_id_.erase(meeting_id) > 0;
}

std::size_t MeetingIndex::rebuild_from_disk(const fs::path& meetings_root) {
    std::unordered_map<std::string, fs::path> fresh;

    std::error_code ec;
    if (!fs::is_directory(meetings_root, ec)) {
        // Missing meetings_root is the empty-server case — not an error.
        std::lock_guard<std::mutex> lock(mu_);
        by_id_.clear();
        return 0;
    }

    for (const auto& entry : fs::directory_iterator(meetings_root, ec)) {
        if (ec) {
            log_warn("MeetingIndex: directory_iterator error at %s: %s",
                     meetings_root.string().c_str(), ec.message().c_str());
            break;
        }
        if (!entry.is_directory(ec) || ec) continue;

        const fs::path& dir = entry.path();
        std::string id = load_meeting_id(dir); // returns "" for missing / malformed
        if (id.empty()) continue;

        // Last-write-wins is fine here — a duplicate id across two dirs
        // would indicate operator-side directory cloning, which v1 does not
        // attempt to repair beyond logging. Pre-C.11 dirs (no meeting_id
        // in context.json) skip cleanly via the empty-id check above.
        auto [it, inserted] = fresh.emplace(id, dir);
        if (!inserted) {
            log_warn("MeetingIndex: duplicate meeting_id %s found at %s "
                     "(prior binding %s) — last entry wins",
                     id.c_str(), dir.string().c_str(),
                     it->second.string().c_str());
            it->second = dir;
        }
    }

    std::size_t n = fresh.size();
    {
        std::lock_guard<std::mutex> lock(mu_);
        by_id_ = std::move(fresh);
    }
    log_info("MeetingIndex: rebuilt from %s (%zu binding%s)",
             meetings_root.string().c_str(), n, n == 1 ? "" : "s");
    return n;
}

std::size_t MeetingIndex::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return by_id_.size();
}

} // namespace recmeet
