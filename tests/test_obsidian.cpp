// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "obsidian.h"

#include <fstream>
#include <sstream>

using namespace recmeet;

static fs::path tmp_dir() {
    fs::path dir = fs::temp_directory_path() / "recmeet_test_obsidian";
    fs::create_directories(dir);
    return dir;
}

static std::string read_file(const fs::path& path) {
    std::ifstream in(path);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

TEST_CASE("extract_action_items: parses bullet list under Action Items heading", "[obsidian]") {
    std::string summary =
        "### Overview\nSome overview.\n\n"
        "### Action Items\n"
        "- **Alice** — review the design doc (Friday)\n"
        "- **Bob** — set up CI pipeline\n"
        "- **Carol** — schedule follow-up meeting\n\n"
        "### Open Questions\n"
        "- What about budget?\n";

    auto items = extract_action_items(summary);
    REQUIRE(items.size() == 3);
    CHECK(items[0] == "**Alice** — review the design doc (Friday)");
    CHECK(items[1] == "**Bob** — set up CI pipeline");
    CHECK(items[2] == "**Carol** — schedule follow-up meeting");
}

TEST_CASE("extract_action_items: empty when no Action Items section", "[obsidian]") {
    std::string summary = "### Overview\nJust an overview.\n### Key Points\n- Point 1\n";
    auto items = extract_action_items(summary);
    CHECK(items.empty());
}

TEST_CASE("extract_action_items: handles ## heading variant", "[obsidian]") {
    std::string summary =
        "## Action Items\n"
        "- Do thing one\n"
        "- Do thing two\n";

    auto items = extract_action_items(summary);
    REQUIRE(items.size() == 2);
}

TEST_CASE("extract_action_items: stops at next heading", "[obsidian]") {
    std::string summary =
        "### Action Items\n"
        "- Item A\n"
        "### Participants\n"
        "- Alice\n"
        "- Bob\n";

    auto items = extract_action_items(summary);
    REQUIRE(items.size() == 1);
    CHECK(items[0] == "Item A");
}

TEST_CASE("write_obsidian_note: creates note with frontmatter and sections", "[obsidian]") {
    auto dir = tmp_dir();

    ObsidianConfig config;
    config.vault_path = dir;
    config.subfolder = "";  // flat directory for testing
    config.domain = "engineering";
    config.tags = {"weekly", "team-alpha"};

    MeetingData data;
    data.date = "2026-02-20";
    data.time = "14:30";
    data.summary_text =
        "### Overview\nDiscussed Q1 roadmap.\n\n"
        "### Action Items\n"
        "- **Alice** — update specs\n";
    data.transcript_text = "[00:00 - 00:05] Hello everyone.";
    data.context_text = "Agenda: Q1 planning";
    data.output_dir = "/tmp/meetings/2026-02-20_14-30";

    fs::path note = write_obsidian_note(config, data);
    REQUIRE(fs::exists(note));

    std::string content = read_file(note);

    SECTION("frontmatter") {
        CHECK(content.find("---\n") == 0);
        CHECK(content.find("date: 2026-02-20") != std::string::npos);
        CHECK(content.find("created: 2026-02-20") != std::string::npos);
        CHECK(content.find("time: \"14:30\"") != std::string::npos);
        CHECK(content.find("type: meeting") != std::string::npos);
        CHECK(content.find("domain: engineering") != std::string::npos);
        CHECK(content.find("status: processed") != std::string::npos);
        CHECK(content.find("  - meeting") != std::string::npos);
        CHECK(content.find("  - weekly") != std::string::npos);
        CHECK(content.find("  - team-alpha") != std::string::npos);
    }

    SECTION("context callout") {
        CHECK(content.find("> [!note] Pre-Meeting Context") != std::string::npos);
        CHECK(content.find("> Agenda: Q1 planning") != std::string::npos);
    }

    SECTION("summary callout") {
        CHECK(content.find("> [!summary] Meeting Summary") != std::string::npos);
        CHECK(content.find("> ### Overview") != std::string::npos);
    }

    SECTION("action items as checkboxes") {
        CHECK(content.find("- [ ] **Alice** — update specs") != std::string::npos);
    }

    SECTION("foldable transcript") {
        CHECK(content.find("> [!abstract]- Full Transcript") != std::string::npos);
        CHECK(content.find("> [00:00 - 00:05] Hello everyone.") != std::string::npos);
    }

    SECTION("raw files link") {
        CHECK(content.find("/tmp/meetings/2026-02-20_14-30") != std::string::npos);
    }

    // Cleanup
    fs::remove_all(dir);
}

TEST_CASE("write_obsidian_note: subfolder creates dated directories", "[obsidian]") {
    auto dir = tmp_dir();

    ObsidianConfig config;
    config.vault_path = dir;
    config.subfolder = "Meetings/%Y/";

    MeetingData data;
    data.date = "2026-02-21";
    data.time = "09:00";
    data.transcript_text = "[00:00 - 00:01] Test.";

    fs::path note = write_obsidian_note(config, data);
    REQUIRE(fs::exists(note));

    // The path should contain "Meetings/2026/" (current year)
    std::string note_str = note.string();
    CHECK(note_str.find("Meetings/") != std::string::npos);

    fs::remove_all(dir);
}

// --- Metadata extraction tests ---

TEST_CASE("extract_meeting_metadata: full metadata block", "[obsidian]") {
    std::string summary =
        "Title: Q1 Roadmap Planning Session\n"
        "Tags: roadmap, planning, q1-goals, engineering\n"
        "Description: The team reviewed Q1 priorities and assigned ownership.\n\n"
        "### Overview\n"
        "Some overview text.\n\n"
        "### Participants\n"
        "- Alice (host)\n"
        "- Bob (primary speaker, engineering lead)\n"
        "- Carol\n\n"
        "### Action Items\n"
        "- Do stuff\n";

    auto meta = extract_meeting_metadata(summary);

    CHECK(meta.title == "Q1 Roadmap Planning Session");
    CHECK(meta.description == "The team reviewed Q1 priorities and assigned ownership.");
    REQUIRE(meta.tags.size() == 4);
    CHECK(meta.tags[0] == "roadmap");
    CHECK(meta.tags[1] == "planning");
    CHECK(meta.tags[2] == "q1-goals");
    CHECK(meta.tags[3] == "engineering");
    REQUIRE(meta.participants.size() == 3);
    CHECK(meta.participants[0] == "Alice");
    CHECK(meta.participants[1] == "Bob");
    CHECK(meta.participants[2] == "Carol");
}

TEST_CASE("extract_meeting_metadata: missing metadata block", "[obsidian]") {
    std::string summary =
        "### Overview\nJust an overview.\n### Key Points\n- Point 1\n";

    auto meta = extract_meeting_metadata(summary);

    CHECK(meta.title.empty());
    CHECK(meta.description.empty());
    CHECK(meta.tags.empty());
    CHECK(meta.participants.empty());
}

TEST_CASE("extract_meeting_metadata: partial metadata (title only)", "[obsidian]") {
    std::string summary =
        "Title: Weekly Standup\n\n"
        "### Overview\nDiscussed blockers.\n";

    auto meta = extract_meeting_metadata(summary);

    CHECK(meta.title == "Weekly Standup");
    CHECK(meta.description.empty());
    CHECK(meta.tags.empty());
}

TEST_CASE("extract_meeting_metadata: tags trimmed and lowercased", "[obsidian]") {
    std::string summary =
        "Tags:  Frontend , BACKEND , DevOps-CI \n"
        "### Overview\nStuff.\n";

    auto meta = extract_meeting_metadata(summary);

    REQUIRE(meta.tags.size() == 3);
    CHECK(meta.tags[0] == "frontend");
    CHECK(meta.tags[1] == "backend");
    CHECK(meta.tags[2] == "devops-ci");
}

TEST_CASE("strip_metadata_block: removes metadata lines, preserves rest", "[obsidian]") {
    std::string summary =
        "Title: Some Meeting\n"
        "Tags: a, b\n"
        "Description: A meeting about things.\n"
        "\n"
        "### Overview\n"
        "The overview text.\n"
        "\n"
        "### Key Points\n"
        "- Point one\n";

    std::string stripped = strip_metadata_block(summary);

    CHECK(stripped.find("Title:") == std::string::npos);
    CHECK(stripped.find("Tags:") == std::string::npos);
    CHECK(stripped.find("Description:") == std::string::npos);
    CHECK(stripped.find("### Overview") != std::string::npos);
    CHECK(stripped.find("The overview text.") != std::string::npos);
    CHECK(stripped.find("### Key Points") != std::string::npos);
}

TEST_CASE("strip_metadata_block: no metadata returns content unchanged", "[obsidian]") {
    std::string summary =
        "### Overview\n"
        "Just an overview.\n";

    std::string stripped = strip_metadata_block(summary);

    CHECK(stripped.find("### Overview") != std::string::npos);
    CHECK(stripped.find("Just an overview.") != std::string::npos);
}

TEST_CASE("extract_action_items: stops before next heading with different level", "[obsidian]") {
    std::string summary =
        "### Action Items\n"
        "- Task one\n"
        "- Task two\n"
        "### Next Section\n"
        "- Not an action item\n"
        "- Also not\n";

    auto items = extract_action_items(summary);
    REQUIRE(items.size() == 2);
    CHECK(items[0] == "Task one");
    CHECK(items[1] == "Task two");
}

TEST_CASE("write_obsidian_note: extracts one-liner for frontmatter", "[obsidian]") {
    auto dir = tmp_dir();

    ObsidianConfig config;
    config.vault_path = dir;
    config.subfolder = "";

    MeetingData data;
    data.date = "2026-02-21";
    data.time = "11:00";
    data.summary_text =
        "### Overview\n"
        "The team discussed Q1 roadmap priorities.\n\n"
        "### Key Points\n"
        "- Point 1\n";
    data.transcript_text = "[00:00 - 00:05] Hello.";

    fs::path note = write_obsidian_note(config, data);
    REQUIRE(fs::exists(note));

    std::string content = read_file(note);
    // Frontmatter should contain a description field with the first sentence (fallback from Overview)
    CHECK(content.find("description: \"The team discussed Q1 roadmap priorities.\"") != std::string::npos);

    fs::remove_all(dir);
}

TEST_CASE("write_obsidian_note: enriched frontmatter with all fields", "[obsidian]") {
    auto dir = tmp_dir();

    ObsidianConfig config;
    config.vault_path = dir;
    config.subfolder = "";
    config.domain = "engineering";
    config.tags = {"weekly"};

    MeetingData data;
    data.date = "2026-02-24";
    data.time = "10:00";
    data.summary_text = "### Overview\nOverview text.\n";
    data.transcript_text = "[00:00 - 00:05] Hello.";
    data.output_dir = "/tmp/meetings/2026-02-24_10-00";

    // Enriched fields
    data.title = "Q1 Roadmap Review";
    data.description = "Reviewed Q1 priorities and assigned tasks.";
    data.ai_tags = {"roadmap", "planning", "weekly"};  // "weekly" dupes with config
    data.participants = {"Alice", "Bob"};
    data.duration_seconds = 3725;  // 1:02:05
    data.whisper_model = "large-v3";

    fs::path note = write_obsidian_note(config, data);
    REQUIRE(fs::exists(note));

    std::string content = read_file(note);

    CHECK(content.find("title: \"Q1 Roadmap Review\"") != std::string::npos);
    CHECK(content.find("created: 2026-02-24") != std::string::npos);
    CHECK(content.find("time: \"10:00\"") != std::string::npos);
    CHECK(content.find("description: \"Reviewed Q1 priorities and assigned tasks.\"") != std::string::npos);
    // Tags: meeting + ai_tags(roadmap, planning, weekly) + config(weekly) — deduplicated
    CHECK(content.find("  - meeting") != std::string::npos);
    CHECK(content.find("  - roadmap") != std::string::npos);
    CHECK(content.find("  - planning") != std::string::npos);
    CHECK(content.find("  - weekly") != std::string::npos);
    // Participants as wikilinks
    CHECK(content.find("  - \"[[Alice]]\"") != std::string::npos);
    CHECK(content.find("  - \"[[Bob]]\"") != std::string::npos);
    // Duration formatted as H:MM:SS
    CHECK(content.find("duration: \"1:02:05\"") != std::string::npos);
    CHECK(content.find("source: \"/tmp/meetings/2026-02-24_10-00\"") != std::string::npos);
    CHECK(content.find("whisper_model: large-v3") != std::string::npos);

    fs::remove_all(dir);
}

TEST_CASE("write_obsidian_note: empty metadata falls back gracefully", "[obsidian]") {
    auto dir = tmp_dir();

    ObsidianConfig config;
    config.vault_path = dir;
    config.subfolder = "";

    MeetingData data;
    data.date = "2026-02-24";
    data.time = "09:00";
    data.summary_text =
        "### Overview\nThe team met to discuss progress.\n\n"
        "### Key Points\n- A point\n";
    data.transcript_text = "[00:00 - 00:05] Hi.";

    fs::path note = write_obsidian_note(config, data);
    REQUIRE(fs::exists(note));

    std::string content = read_file(note);

    // No title field when empty
    CHECK(content.find("title:") == std::string::npos);
    // Description falls back to one-liner from Overview
    CHECK(content.find("description: \"The team met to discuss progress.\"") != std::string::npos);
    // No participants/duration/whisper_model when empty
    CHECK(content.find("participants:") == std::string::npos);
    CHECK(content.find("duration:") == std::string::npos);
    CHECK(content.find("whisper_model:") == std::string::npos);

    fs::remove_all(dir);
}

TEST_CASE("write_obsidian_note: handles empty summary gracefully", "[obsidian]") {
    auto dir = tmp_dir();

    ObsidianConfig config;
    config.vault_path = dir;
    config.subfolder = "";

    MeetingData data;
    data.date = "2026-02-20";
    data.time = "10:00";
    data.transcript_text = "[00:00 - 00:10] Just a test.";

    fs::path note = write_obsidian_note(config, data);
    REQUIRE(fs::exists(note));

    std::string content = read_file(note);
    // No summary callout when summary is empty
    CHECK(content.find("> [!summary]") == std::string::npos);
    // But transcript should still be there
    CHECK(content.find("> [!abstract]- Full Transcript") != std::string::npos);

    fs::remove_all(dir);
}
