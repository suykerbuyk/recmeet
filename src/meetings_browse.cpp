// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "meetings_browse.h"

#include "pipeline.h"   // load_meeting_id
#include "util.h"       // find_audio_file, find_speakers_file

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <system_error>

namespace recmeet {

namespace {

// ISO 8601 UTC ("YYYY-MM-DDTHH:MM:SSZ") from a filesystem mtime. Matches
// the speaker_id.cpp::iso_now() pattern but works on an arbitrary
// file_time_type instead of "now". Returns empty on any conversion error.
std::string mtime_to_iso(const fs::file_time_type& ft) {
    using namespace std::chrono;
    // Convert to system_clock so we can hand to gmtime — fs::file_time_type
    // is implementation-defined (typically clock-distinct) so we go through
    // a duration offset against system_clock::now() / file_time_type::clock::now().
    auto sys_now = system_clock::now();
    auto file_now = fs::file_time_type::clock::now();
    auto sct = time_point_cast<system_clock::duration>(
        ft - file_now + sys_now);
    auto t = system_clock::to_time_t(sct);
    char buf[32];
    std::tm tm_buf{};
    if (gmtime_r(&t, &tm_buf) == nullptr) return {};
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf) == 0)
        return {};
    return buf;
}

bool has_meeting_note(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) return false;
        if (!entry.is_regular_file()) continue;
        const std::string fname = entry.path().filename().string();
        // Meeting_<timestamp>[_Title].md — match the prefix+suffix only;
        // the daemon's note.cpp emits this exact convention.
        constexpr const char* PREFIX = "Meeting_";
        constexpr const char* SUFFIX = ".md";
        const std::size_t pre_n = std::strlen(PREFIX);
        const std::size_t suf_n = std::strlen(SUFFIX);
        if (fname.size() < pre_n + suf_n) continue;
        if (fname.compare(0, pre_n, PREFIX) != 0) continue;
        if (fname.compare(fname.size() - suf_n, suf_n, SUFFIX) != 0) continue;
        return true;
    }
    return false;
}

} // namespace

std::vector<MeetingInfo> discover_meetings(const fs::path& meetings_root) {
    std::vector<MeetingInfo> out;
    std::error_code ec;
    if (!fs::is_directory(meetings_root, ec)) return out;

    // Pair the MeetingInfo with the raw mtime so we can sort numerically
    // (cheap) and emit only the formatted ISO string into the returned
    // value.
    struct Entry {
        MeetingInfo info;
        fs::file_time_type mtime;
    };
    std::vector<Entry> entries;

    for (const auto& dent : fs::directory_iterator(meetings_root, ec)) {
        if (ec) break;
        if (!dent.is_directory()) continue;

        const fs::path& dir = dent.path();
        MeetingInfo mi;
        mi.name = dir.filename().string();

        // meeting_id: read from context.json; load_meeting_id returns empty
        // when the file is missing, the field is absent (v1 context.json),
        // or the stored value fails is_valid_meeting_id().
        std::string mid = load_meeting_id(dir);
        if (!mid.empty()) mi.meeting_id = mid;

        mi.has_audio = !find_audio_file(dir).empty();
        mi.has_speakers = !find_speakers_file(dir).empty();
        mi.has_summary = has_meeting_note(dir);

        fs::file_time_type ft{};
        std::error_code mtec;
        ft = fs::last_write_time(dir, mtec);
        if (mtec) {
            mi.mtime_iso.clear();
        } else {
            mi.mtime_iso = mtime_to_iso(ft);
        }

        entries.push_back({std::move(mi), ft});
    }

    // Sort by mtime descending (most recent first).
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) {
                  return a.mtime > b.mtime;
              });

    out.reserve(entries.size());
    for (auto& e : entries) out.push_back(std::move(e.info));
    return out;
}

} // namespace recmeet
