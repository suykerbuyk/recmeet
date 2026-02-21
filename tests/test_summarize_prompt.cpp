#include <catch2/catch_test_macros.hpp>
#include "summarize.h"

using namespace recmeet;

TEST_CASE("build_user_prompt: contains all required section headings", "[summarize]") {
    std::string prompt = build_user_prompt("Some transcript text.");

    CHECK(prompt.find("### Overview") != std::string::npos);
    CHECK(prompt.find("### Key Points") != std::string::npos);
    CHECK(prompt.find("### Decisions") != std::string::npos);
    CHECK(prompt.find("### Action Items") != std::string::npos);
    CHECK(prompt.find("### Open Questions") != std::string::npos);
    CHECK(prompt.find("### Participants") != std::string::npos);
}

TEST_CASE("build_user_prompt: includes transcript text", "[summarize]") {
    std::string prompt = build_user_prompt("Hello world transcript.");

    CHECK(prompt.find("## Transcript") != std::string::npos);
    CHECK(prompt.find("Hello world transcript.") != std::string::npos);

    // Transcript should appear after the "## Transcript" heading
    auto heading_pos = prompt.find("## Transcript");
    auto text_pos = prompt.find("Hello world transcript.");
    CHECK(text_pos > heading_pos);
}

TEST_CASE("build_user_prompt: includes context when provided", "[summarize]") {
    std::string prompt = build_user_prompt("transcript", "Agenda: discuss Q1 goals");

    CHECK(prompt.find("## Pre-Meeting Context") != std::string::npos);
    CHECK(prompt.find("Agenda: discuss Q1 goals") != std::string::npos);
}

TEST_CASE("build_user_prompt: omits context section when empty", "[summarize]") {
    std::string prompt = build_user_prompt("transcript", "");
    CHECK(prompt.find("Pre-Meeting Context") == std::string::npos);

    std::string prompt2 = build_user_prompt("transcript");
    CHECK(prompt2.find("Pre-Meeting Context") == std::string::npos);
}

TEST_CASE("build_user_prompt: escapes correctly for JSON embedding", "[summarize]") {
    std::string prompt = build_user_prompt("test transcript\nwith newlines");
    std::string escaped = json_escape(prompt);

    // The escaped version should have no literal newlines
    CHECK(escaped.find('\n') == std::string::npos);
    // But should have escaped newline sequences
    CHECK(escaped.find("\\n") != std::string::npos);
}
