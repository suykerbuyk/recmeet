#include <catch2/catch_test_macros.hpp>
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
        CHECK(content.find("time: 14:30") != std::string::npos);
        CHECK(content.find("type: meeting") != std::string::npos);
        CHECK(content.find("domain: engineering") != std::string::npos);
        CHECK(content.find("status: processed") != std::string::npos);
        CHECK(content.find("tags: [meeting, weekly, team-alpha]") != std::string::npos);
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
