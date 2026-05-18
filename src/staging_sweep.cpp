// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "staging_sweep.h"
#include "log.h"

#include <sys/stat.h>

#include <algorithm>
#include <system_error>

namespace recmeet {

namespace {

// Mirror of `tray_capture::is_sidecar_protected` — inlined here so the
// sweep helper can live in `recmeet_ipc` (one layer below
// `recmeet_capture`, the library that owns `tray_capture`). The check
// is identical: a `.pending` sibling next to the WAV → save-for-later
// per D.5 → PROTECTED. Replicating one fs::exists call is cheaper than
// inverting the library dep chain.
bool sidecar_present(const fs::path& wav_path) {
    fs::path sidecar = wav_path;
    sidecar.replace_extension(".pending");
    std::error_code ec;
    return fs::exists(sidecar, ec) && !ec;
}

} // namespace

std::vector<StagingEntry>
scan_staging_dir(const fs::path& staging_dir,
                 const std::unordered_set<std::string>& protected_staging_paths) {
    std::vector<StagingEntry> out;

    std::error_code ec;
    if (!fs::exists(staging_dir, ec) || ec || !fs::is_directory(staging_dir, ec)) {
        return out;
    }

    fs::directory_iterator it(staging_dir, ec);
    if (ec) {
        log_warn("[d6] staging_sweep: cannot read %s — %s",
                 staging_dir.string().c_str(), ec.message().c_str());
        return out;
    }

    for (const auto& de : it) {
        std::error_code de_ec;
        if (!de.is_regular_file(de_ec) || de_ec) continue;
        if (de.path().extension() != ".wav") continue;

        struct stat st{};
        if (::stat(de.path().c_str(), &st) != 0) {
            log_warn("[d6] staging_sweep: stat failed for %s",
                     de.path().string().c_str());
            continue;
        }

        StagingEntry e;
        e.wav_path           = de.path();
        e.size_bytes         = static_cast<std::uintmax_t>(st.st_size);
        e.mtime              = st.st_mtime;
        e.sidecar_protected  = sidecar_present(de.path());
        e.journal_protected  =
            protected_staging_paths.find(de.path().string()) !=
                protected_staging_paths.end();
        out.push_back(std::move(e));
    }
    return out;
}

std::uintmax_t current_staging_bytes(const fs::path& staging_dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    if (!fs::exists(staging_dir, ec) || ec) return 0;

    fs::directory_iterator it(staging_dir, ec);
    if (ec) return 0;
    for (const auto& de : it) {
        std::error_code de_ec;
        if (!de.is_regular_file(de_ec) || de_ec) continue;
        if (de.path().extension() != ".wav") continue;
        struct stat st{};
        if (::stat(de.path().c_str(), &st) != 0) continue;
        total += static_cast<std::uintmax_t>(st.st_size);
    }
    return total;
}

std::vector<StagingEntry>
plan_evictions(const std::vector<StagingEntry>& entries,
               std::uintmax_t max_bytes,
               std::uintmax_t extra_pending_bytes) {
    // Compute overall total (protected + safe + extra).
    std::uintmax_t total = extra_pending_bytes;
    for (const auto& e : entries) total += e.size_bytes;

    if (total <= max_bytes) return {};

    // Build safe-to-evict subset, sort by (mtime asc, path asc).
    std::vector<StagingEntry> safe;
    safe.reserve(entries.size());
    for (const auto& e : entries) {
        if (e.sidecar_protected || e.journal_protected) continue;
        safe.push_back(e);
    }
    std::sort(safe.begin(), safe.end(),
              [](const StagingEntry& a, const StagingEntry& b) {
                  if (a.mtime != b.mtime) return a.mtime < b.mtime;
                  return a.wav_path.string() < b.wav_path.string();
              });

    // Greedy: peel oldest until total <= max_bytes OR safe list exhausted.
    std::vector<StagingEntry> plan;
    std::uintmax_t running = total;
    for (const auto& e : safe) {
        if (running <= max_bytes) break;
        plan.push_back(e);
        running -= e.size_bytes;
    }
    return plan;
}

std::vector<EvictionRecord>
apply_evictions(const std::vector<StagingEntry>& to_evict) {
    std::vector<EvictionRecord> done;
    done.reserve(to_evict.size());
    for (const auto& e : to_evict) {
        std::error_code ec;
        fs::remove(e.wav_path, ec);
        if (ec) {
            log_warn("[d6] staging_sweep: unlink failed for %s — %s",
                     e.wav_path.string().c_str(), ec.message().c_str());
            continue;
        }
        EvictionRecord r;
        r.wav_path   = e.wav_path;
        r.size_bytes = e.size_bytes;
        r.reason     = "over staging_max_bytes";
        done.push_back(std::move(r));
    }
    return done;
}

std::vector<EvictionRecord>
sweep_staging(const fs::path& staging_dir,
              const std::unordered_set<std::string>& protected_staging_paths,
              std::uintmax_t max_bytes,
              std::uintmax_t extra_pending_bytes) {
    auto entries = scan_staging_dir(staging_dir, protected_staging_paths);
    auto plan    = plan_evictions(entries, max_bytes, extra_pending_bytes);

    if (plan.empty()) {
        // Silent on the "nothing to evict" path per plan checklist item #8.
        // Surface would-evict-but-protected entries at debug for observability.
        std::uintmax_t total = extra_pending_bytes;
        for (const auto& e : entries) total += e.size_bytes;
        if (total > max_bytes) {
            for (const auto& e : entries) {
                if (!(e.sidecar_protected || e.journal_protected)) continue;
                log_debug("[d6] staging_sweep: would-evict-but-protected "
                          "path=%s size=%ju reason=%s",
                          e.wav_path.string().c_str(),
                          e.size_bytes,
                          e.sidecar_protected ? "sidecar" : "journal");
            }
            log_warn("[d6] staging_sweep: cannot reach budget — total=%ju "
                     "max=%ju (all evictable entries already swept; "
                     "protected entries hold remainder)",
                     total, max_bytes);
        }
        return {};
    }

    auto done = apply_evictions(plan);
    for (const auto& r : done) {
        log_info("[d6] staging_sweep: evicted path=%s size=%ju reason=%s",
                 r.wav_path.string().c_str(), r.size_bytes, r.reason.c_str());
    }
    return done;
}

} // namespace recmeet
