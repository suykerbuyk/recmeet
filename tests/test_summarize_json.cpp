#include <catch2/catch_test_macros.hpp>
#include "summarize.h"

using namespace recmeet;

TEST_CASE("json_escape: plain string unchanged", "[json]") {
    CHECK(json_escape("hello world") == "hello world");
}

TEST_CASE("json_escape: escapes double quotes", "[json]") {
    CHECK(json_escape("say \"hello\"") == "say \\\"hello\\\"");
}

TEST_CASE("json_escape: escapes backslashes", "[json]") {
    CHECK(json_escape("path\\to\\file") == "path\\\\to\\\\file");
}

TEST_CASE("json_escape: escapes newlines and tabs", "[json]") {
    CHECK(json_escape("line1\nline2") == "line1\\nline2");
    CHECK(json_escape("col1\tcol2") == "col1\\tcol2");
    CHECK(json_escape("cr\rhere") == "cr\\rhere");
}

TEST_CASE("json_escape: combined special characters", "[json]") {
    std::string input = "He said \"hello\"\nand then\t\"goodbye\\\"";
    std::string expected = "He said \\\"hello\\\"\\nand then\\t\\\"goodbye\\\\\\\"";
    CHECK(json_escape(input) == expected);
}

TEST_CASE("json_escape: empty string", "[json]") {
    CHECK(json_escape("") == "");
}

TEST_CASE("json_extract_string: simple key extraction", "[json]") {
    std::string json = R"({"name": "Alice", "age": "30"})";
    CHECK(json_extract_string(json, "name") == "Alice");
    CHECK(json_extract_string(json, "age") == "30");
}

TEST_CASE("json_extract_string: handles escaped characters in values", "[json]") {
    std::string json = R"({"content": "line1\nline2\ttab"})";
    CHECK(json_extract_string(json, "content") == "line1\nline2\ttab");
}

TEST_CASE("json_extract_string: handles escaped quotes in values", "[json]") {
    std::string json = R"({"text": "she said \"hi\""})";
    CHECK(json_extract_string(json, "text") == "she said \"hi\"");
}

TEST_CASE("json_extract_string: returns empty for missing key", "[json]") {
    std::string json = R"({"name": "Alice"})";
    CHECK(json_extract_string(json, "missing") == "");
}

TEST_CASE("json_extract_string: handles escaped backslashes", "[json]") {
    std::string json = R"({"path": "C:\\Users\\file"})";
    CHECK(json_extract_string(json, "path") == "C:\\Users\\file");
}

TEST_CASE("json_extract_string: OpenAI-style response", "[json]") {
    // Simulated response from chat completion API
    std::string json = R"({
        "choices": [{
            "message": {
                "role": "assistant",
                "content": "### Overview\nThis was a planning meeting."
            }
        }]
    })";
    std::string content = json_extract_string(json, "content");
    CHECK(content == "### Overview\nThis was a planning meeting.");
}

TEST_CASE("is_chat_model: accepts chat models", "[summarize]") {
    CHECK(is_chat_model("grok-3"));
    CHECK(is_chat_model("grok-3-mini"));
    CHECK(is_chat_model("gpt-4o"));
    CHECK(is_chat_model("gpt-4o-mini"));
    CHECK(is_chat_model("claude-sonnet-4-6"));
    CHECK(is_chat_model("o1-preview"));
}

TEST_CASE("is_chat_model: rejects non-chat models", "[summarize]") {
    CHECK_FALSE(is_chat_model("text-embedding-3-large"));
    CHECK_FALSE(is_chat_model("text-embedding-ada-002"));
    CHECK_FALSE(is_chat_model("tts-1"));
    CHECK_FALSE(is_chat_model("tts-1-hd"));
    CHECK_FALSE(is_chat_model("whisper-1"));
    CHECK_FALSE(is_chat_model("dall-e-3"));
    CHECK_FALSE(is_chat_model("dall-e-2"));
    CHECK_FALSE(is_chat_model("text-moderation-latest"));
    CHECK_FALSE(is_chat_model("gpt-4o-audio-preview"));
    CHECK_FALSE(is_chat_model("gpt-4o-realtime-preview"));
}
