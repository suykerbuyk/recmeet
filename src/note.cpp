// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "note.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

namespace recmeet {

namespace {

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string yaml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default:   out += c;
        }
    }
    return out;
}

bool starts_with(const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
}

// Regex-escape every character in `s` that has a special meaning in
// std::regex (ECMAScript). Defensive — `YYYY-MM-DD_HH-MM` carries no
// metachars today (only digits + `-`/`_`), but future schema changes
// could introduce one and the helper's regex would silently misbehave.
std::string regex_escape(const std::string& s) {
    static const std::regex kSpecials(R"([.^$|()\[\]{}*+?\\])");
    return std::regex_replace(s, kSpecials, R"(\$&)");
}

} // anonymous namespace

namespace note_internal {

namespace {

// Internal record for a legacy (pre-`.XX`) note that needs migration.
struct LegacyEntry {
    fs::path path;
    fs::file_time_type mtime;
    std::string suffix;   // captured `_safe_title` group; empty if none
};

} // anonymous namespace

int next_attempt_and_migrate(const fs::path& note_parent,
                             const std::string& timestamp_prefix) {
    // Caller (`write_meeting_note`) has already ensured `note_parent`
    // exists. A `directory_iterator` failure here is a real filesystem
    // error (permissions/ENOSPC on the dir itself) — propagate.
    std::error_code dir_ec;
    if (!fs::is_directory(note_parent, dir_ec) || dir_ec) {
        throw RecmeetError("note: cannot scan " + note_parent.string()
                           + ": " + dir_ec.message());
    }

    const std::string esc = regex_escape(timestamp_prefix);
    // New form: Meeting_<ts>.<digits>[_<safe_title>].md
    //   digit group is captured numerically (so `.10` > `.2`).
    const std::regex new_form_re(
        "^Meeting_" + esc + R"(\.(\d+)(_.*)?\.md$)");
    // Legacy form: Meeting_<ts>[_<safe_title>].md
    //   The optional `(_.*)?` group REQUIRES a leading `_`, so it will
    //   not consume a `.NN` suffix (which starts with `.`); the literal
    //   trailing `\.md$` then can't match `.NN.md` either. New-form
    //   files are excluded by elimination, not by an explicit guard.
    const std::regex legacy_re(
        "^Meeting_" + esc + R"((_.*)?\.md$)");

    int max_seen = -1;
    std::vector<LegacyEntry> legacies;

    std::error_code iter_ec;
    for (const auto& entry : fs::directory_iterator(note_parent, iter_ec)) {
        if (iter_ec) {
            throw RecmeetError("note: directory_iterator failure in "
                               + note_parent.string()
                               + ": " + iter_ec.message());
        }
        if (!entry.is_regular_file()) continue;
        const std::string fname = entry.path().filename().string();

        std::smatch m;
        if (std::regex_match(fname, m, new_form_re)) {
            // Parse the digit group numerically — `.10` outranks `.2`.
            try {
                int n = std::stoi(m[1].str());
                if (n > max_seen) max_seen = n;
            } catch (const std::exception&) {
                // Unparseable digits are impossible for `\d+` matches,
                // but be defensive: skip the entry.
            }
            continue;
        }
        if (std::regex_match(fname, m, legacy_re)) {
            LegacyEntry le;
            le.path = entry.path();
            std::error_code mt_ec;
            le.mtime = fs::last_write_time(entry.path(), mt_ec);
            if (mt_ec) {
                // Treat unreadable mtime as the oldest possible time so
                // the file sorts first and migration still proceeds.
                le.mtime = fs::file_time_type::min();
                log_warn("note: mtime read failed for %s: %s",
                         entry.path().c_str(), mt_ec.message().c_str());
            }
            le.suffix = m[1].matched ? m[1].str() : std::string{};
            legacies.push_back(std::move(le));
        }
    }

    // Sort legacies by (mtime ASC, filename ASC) — deterministic across
    // hosts even when mtimes collide (cp -a, syncthing replication, or
    // back-to-back writes within filesystem mtime granularity).
    std::sort(legacies.begin(), legacies.end(),
              [](const LegacyEntry& a, const LegacyEntry& b) {
                  if (a.mtime != b.mtime) return a.mtime < b.mtime;
                  return a.path.filename().string()
                       < b.path.filename().string();
              });

    // Migration loop. The `++max_seen` happens BEFORE the rename so a
    // failed rename still consumes the slot — never-gap-fill invariant
    // protects against the partial-migration collision documented in
    // the task plan's Phase 1 step 2.
    for (auto& le : legacies) {
        int candidate = ++max_seen;
        char num_buf[8];
        std::snprintf(num_buf, sizeof(num_buf), "%02d", candidate);
        fs::path target = note_parent / ("Meeting_" + timestamp_prefix
                                         + "." + num_buf
                                         + le.suffix + ".md");
        std::error_code rn_ec;
        fs::rename(le.path, target, rn_ec);
        if (rn_ec) {
            // "target exists" / "source missing" = a race healed by
            // another caller. Anything else = real fs error. Either
            // way, we do NOT decrement `max_seen` — the slot is burnt
            // (matching the never-gap-fill rule). A real ENOSPC will
            // simply leave an orphan that the next write self-heals.
            log_warn("note: migration rename failed for %s -> .%s: %s",
                     le.path.c_str(), num_buf, rn_ec.message().c_str());
        }
    }

    return max_seen + 1;
}

} // namespace note_internal

std::vector<std::string> extract_action_items(const std::string& summary) {
    std::vector<std::string> items;
    bool in_action_section = false;
    std::istringstream stream(summary);
    std::string line;

    while (std::getline(stream, line)) {
        // Detect section headers
        if (line.find("### Action Items") != std::string::npos ||
            line.find("## Action Items") != std::string::npos) {
            in_action_section = true;
            continue;
        }
        if (in_action_section && line.size() >= 2 && line[0] == '#') {
            break; // Next section
        }
        if (in_action_section && line.size() >= 2 && line[0] == '-' && line[1] == ' ') {
            items.push_back(line.substr(2));
        }
    }
    return items;
}

MeetingMetadata extract_meeting_metadata(const std::string& summary) {
    MeetingMetadata meta;
    std::istringstream stream(summary);
    std::string line;
    bool in_participants = false;

    while (std::getline(stream, line)) {
        if (starts_with(line, "Title: ")) {
            meta.title = trim(line.substr(7));
            in_participants = false;
        } else if (starts_with(line, "Tags: ")) {
            std::string tags_str = line.substr(6);
            std::istringstream ts(tags_str);
            std::string tag;
            while (std::getline(ts, tag, ',')) {
                tag = trim(tag);
                std::transform(tag.begin(), tag.end(), tag.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (!tag.empty())
                    meta.tags.push_back(tag);
            }
            in_participants = false;
        } else if (starts_with(line, "Description: ")) {
            meta.description = trim(line.substr(13));
            in_participants = false;
        } else if (line.find("### Participants") != std::string::npos ||
                   line.find("## Participants") != std::string::npos) {
            in_participants = true;
        } else if (in_participants && line.size() >= 2 && line[0] == '#') {
            in_participants = false;
        } else if (in_participants && line.size() >= 2 && line[0] == '-' && line[1] == ' ') {
            std::string name = line.substr(2);
            // Strip trailing role description in parentheses
            auto paren = name.rfind('(');
            if (paren != std::string::npos)
                name = name.substr(0, paren);
            name = trim(name);
            if (!name.empty())
                meta.participants.push_back(name);
        }
    }

    return meta;
}

std::string strip_metadata_block(const std::string& summary) {
    std::istringstream stream(summary);
    std::string line;
    std::ostringstream out;
    bool found_heading = false;

    while (std::getline(stream, line)) {
        if (starts_with(line, "Title: ") || starts_with(line, "Tags: ") ||
            starts_with(line, "Description: ")) {
            continue; // skip metadata lines
        }
        // Skip blank lines before the first heading
        if (!found_heading && trim(line).empty())
            continue;
        if (line.size() >= 1 && line[0] == '#')
            found_heading = true;
        if (found_heading) {
            out << line << "\n";
        }
    }

    // Remove trailing newline
    std::string result = out.str();
    while (!result.empty() && result.back() == '\n')
        result.pop_back();
    return result;
}

fs::path write_meeting_note(const NoteConfig& config, const MeetingData& data) {
    // Note filename: Meeting_YYYY-MM-DD_HH-MM.<NN>[_Title_Words].md
    // The `.NN` attempt counter (zero-padded to 2 digits, unbounded
    // upward) is inserted between the timestamp and the optional title
    // by `next_attempt_and_migrate()`. The counter sorts oldest-to-
    // newest within a timestamp and migrates legacy un-numbered notes
    // on the first call for that timestamp. See note.h::note_internal.
    std::string safe_time = data.time;
    for (auto& c : safe_time)
        if (c == ':') c = '-';
    const std::string timestamp_prefix = data.date + "_" + safe_time;

    std::string safe_title;
    if (!data.title.empty()) {
        for (char c : data.title) {
            if (c == ' ')
                safe_title += '_';
            else if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
                safe_title += c;
        }
    }

    fs::path note_parent = data.note_dir.empty() ? data.output_dir : data.note_dir;
    // Add year/month subdirectories when using a separate note_dir.
    // The helper requires the directory to exist; resolve + create BEFORE
    // the scan-and-migrate call.
    if (!data.note_dir.empty() && data.date.size() >= 7) {
        std::string year = data.date.substr(0, 4);
        std::string month = data.date.substr(5, 2);
        note_parent = note_parent / year / month;
        fs::create_directories(note_parent);
    }

    const int attempt =
        note_internal::next_attempt_and_migrate(note_parent, timestamp_prefix);

    char attempt_buf[8];
    std::snprintf(attempt_buf, sizeof(attempt_buf), "%02d", attempt);
    std::string filename = "Meeting_" + timestamp_prefix + "." + attempt_buf;
    if (!safe_title.empty())
        filename += "_" + safe_title;
    filename += ".md";

    fs::path note_path = note_parent / filename;

    std::ofstream out(note_path);
    if (!out)
        throw RecmeetError("Cannot write meeting note: " + note_path.string());

    // Build one-line summary fallback from Overview section
    std::string one_liner;
    if (!data.summary_text.empty()) {
        auto overview_pos = data.summary_text.find("### Overview");
        if (overview_pos != std::string::npos) {
            auto text_start = data.summary_text.find('\n', overview_pos);
            if (text_start != std::string::npos) {
                text_start++;
                while (text_start < data.summary_text.size() && data.summary_text[text_start] == '\n')
                    text_start++;
                auto text_end = data.summary_text.find('\n', text_start);
                if (text_end != std::string::npos)
                    one_liner = data.summary_text.substr(text_start, text_end - text_start);
                else
                    one_liner = data.summary_text.substr(text_start);
            }
        }
    }

    // Description: use AI description, fall back to one-liner from Overview
    std::string description = data.description.empty() ? one_liner : data.description;

    // Deduplicate tags: AI tags + "meeting" + config tags
    std::set<std::string> tag_set;
    std::vector<std::string> tags_ordered;
    auto add_tag = [&](const std::string& t) {
        if (tag_set.insert(t).second)
            tags_ordered.push_back(t);
    };
    add_tag("meeting");
    for (const auto& t : data.ai_tags) add_tag(t);
    for (const auto& t : config.tags) add_tag(t);

    // Format duration
    std::string duration_str;
    if (data.duration_seconds > 0) {
        int h = data.duration_seconds / 3600;
        int m = (data.duration_seconds % 3600) / 60;
        int s = data.duration_seconds % 60;
        char buf[16];
        if (h > 0)
            snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
        else
            snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
        duration_str = buf;
    }

    // --- YAML Frontmatter ---
    out << "---\n";
    if (!data.title.empty())
        out << "title: \"" << yaml_escape(data.title) << "\"\n";
    out << "date: " << data.date << "\n"
        << "created: " << data.date << "\n"
        << "time: \"" << data.time << "\"\n"
        << "type: meeting\n"
        << "domain: " << config.domain << "\n"
        << "status: processed\n";
    if (!description.empty())
        out << "description: \"" << yaml_escape(description) << "\"\n";
    out << "tags:\n";
    for (const auto& tag : tags_ordered)
        out << "  - " << tag << "\n";
    if (!data.participants.empty()) {
        out << "participants:\n";
        for (const auto& p : data.participants)
            out << "  - \"[[" << yaml_escape(p) << "]]\"\n";
    }
    if (!duration_str.empty())
        out << "duration: \"" << duration_str << "\"\n";
    if (!data.output_dir.empty())
        out << "source: \"" << yaml_escape(data.output_dir.string()) << "\"\n";
    if (!data.whisper_model.empty())
        out << "whisper_model: " << data.whisper_model << "\n";
    out << "---\n\n";

    // --- Context (if provided) ---
    if (!data.context_text.empty()) {
        out << "> [!note] Pre-Meeting Context\n";
        std::istringstream ctx_stream(data.context_text);
        std::string line;
        while (std::getline(ctx_stream, line))
            out << "> " << line << "\n";
        out << "\n";
    }

    // --- Summary ---
    if (!data.summary_text.empty()) {
        out << "> [!summary] Meeting Summary\n";
        std::istringstream sum_stream(data.summary_text);
        std::string line;
        while (std::getline(sum_stream, line))
            out << "> " << line << "\n";
        out << "\n";
    }

    // --- Action Items as checkboxes ---
    auto actions = data.action_items.empty()
        ? extract_action_items(data.summary_text) : data.action_items;
    if (!actions.empty()) {
        out << "## Action Items\n\n";
        for (const auto& item : actions)
            out << "- [ ] " << item << "\n";
        out << "\n";
    }

    // --- Transcript (foldable) ---
    if (!data.transcript_text.empty()) {
        out << "> [!abstract]- Full Transcript\n";
        std::istringstream tx_stream(data.transcript_text);
        std::string line;
        while (std::getline(tx_stream, line))
            out << "> " << line << "\n";
        out << "\n";
    }

    // --- Raw files link ---
    if (!data.output_dir.empty()) {
        out << "---\n"
            << "*Raw files: `" << data.output_dir.string() << "`*\n";
    }

    out.close();
    log_info("Meeting note: %s", note_path.c_str());
    return note_path;
}

} // namespace recmeet
