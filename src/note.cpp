// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "note.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

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

} // anonymous namespace

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
    // Note filename: Meeting_YYYY-MM-DD_HH-MM[_Title_Words].md
    std::string safe_time = data.time;
    for (auto& c : safe_time)
        if (c == ':') c = '-';
    std::string filename = "Meeting_" + data.date + "_" + safe_time;
    if (!data.title.empty()) {
        std::string safe_title;
        for (char c : data.title) {
            if (c == ' ')
                safe_title += '_';
            else if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
                safe_title += c;
        }
        if (!safe_title.empty())
            filename += "_" + safe_title;
    }
    filename += ".md";
    fs::path note_path = data.output_dir / filename;

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
