// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "obsidian.h"
#include "log.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace recmeet {

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

fs::path write_obsidian_note(const ObsidianConfig& config, const MeetingData& data) {
    // Build subfolder path using strftime
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);

    char subfolder_buf[256];
    strftime(subfolder_buf, sizeof(subfolder_buf), config.subfolder.c_str(), &tm);

    fs::path note_dir = config.vault_path / subfolder_buf;
    fs::create_directories(note_dir);

    // Note filename: Meeting_YYYY-MM-DD_HH-MM.md
    std::string safe_time = data.time;
    for (auto& c : safe_time)
        if (c == ':') c = '-';
    std::string filename = "Meeting_" + data.date + "_" + safe_time + ".md";
    fs::path note_path = note_dir / filename;

    std::ofstream out(note_path);
    if (!out)
        throw RecmeetError("Cannot write Obsidian note: " + note_path.string());

    // Build one-line summary from first sentence of summary
    std::string one_liner;
    if (!data.summary_text.empty()) {
        // Find the first section after "Overview", grab first sentence
        auto overview_pos = data.summary_text.find("### Overview");
        if (overview_pos != std::string::npos) {
            auto text_start = data.summary_text.find('\n', overview_pos);
            if (text_start != std::string::npos) {
                text_start++;
                // Skip blank lines
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

    // Build tags string
    std::ostringstream tags_oss;
    tags_oss << "[meeting";
    for (const auto& tag : config.tags)
        tags_oss << ", " << tag;
    tags_oss << "]";

    // --- YAML Frontmatter ---
    out << "---\n"
        << "date: " << data.date << "\n"
        << "time: " << data.time << "\n"
        << "type: meeting\n"
        << "domain: " << config.domain << "\n"
        << "status: processed\n"
        << "tags: " << tags_oss.str() << "\n";
    if (!one_liner.empty())
        out << "summary: \"" << one_liner << "\"\n";
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
    log_info("Obsidian note: %s", note_path.c_str());
    return note_path;
}

} // namespace recmeet
