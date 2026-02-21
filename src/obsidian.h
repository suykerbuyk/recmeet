#pragma once

#include "util.h"
#include "transcribe.h"

#include <string>
#include <vector>

namespace recmeet {

struct ObsidianConfig {
    fs::path vault_path;           // e.g. ~/obsidian/ObsMeetings/
    std::string subfolder = "Meetings/%Y/%m/"; // strftime format
    std::string domain = "general";
    std::vector<std::string> tags;
};

struct MeetingData {
    std::string date;              // YYYY-MM-DD
    std::string time;              // HH:MM
    std::string summary_text;      // Markdown summary (from API/LLM)
    std::string transcript_text;   // Full timestamped transcript
    std::string context_text;      // Pre-meeting notes (optional)
    fs::path output_dir;           // Path to raw files
    std::vector<std::string> action_items; // Extracted from summary
};

/// Write an Obsidian-compatible meeting note to the vault.
/// Returns the path to the created note.
fs::path write_obsidian_note(const ObsidianConfig& config, const MeetingData& data);

/// Extract action items from a summary (lines starting with "- " under "Action Items").
std::vector<std::string> extract_action_items(const std::string& summary);

} // namespace recmeet
