// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "ndjson_parse.h"

using namespace recmeet;

// ---------------------------------------------------------------------------
// parse_ndjson_string
// ---------------------------------------------------------------------------

TEST_CASE("parse_ndjson_string: extracts simple string value", "[ndjson]") {
    std::string line = R"({"event":"phase","data":{"name":"transcribing"}})";
    CHECK(parse_ndjson_string(line, "event") == "phase");
    CHECK(parse_ndjson_string(line, "name") == "transcribing");
}

TEST_CASE("parse_ndjson_string: handles escaped quotes in value", "[ndjson]") {
    std::string line = R"({"path":"C:\\Users\\test\\\"file\".txt"})";
    CHECK(parse_ndjson_string(line, "path") == R"(C:\Users\test\"file".txt)");
}

TEST_CASE("parse_ndjson_string: returns empty for missing key", "[ndjson]") {
    std::string line = R"({"event":"phase"})";
    CHECK(parse_ndjson_string(line, "missing").empty());
}

TEST_CASE("parse_ndjson_string: returns empty for empty input", "[ndjson]") {
    CHECK(parse_ndjson_string("", "key").empty());
}

TEST_CASE("parse_ndjson_string: handles empty string value", "[ndjson]") {
    std::string line = R"({"name":""})";
    CHECK(parse_ndjson_string(line, "name").empty());
}

TEST_CASE("parse_ndjson_string: extracts job.complete fields", "[ndjson]") {
    std::string line = R"({"event":"job.complete","data":{"note_path":"/home/user/meetings/note.md","output_dir":"/home/user/meetings/2026-03-29"}})";
    CHECK(parse_ndjson_string(line, "event") == "job.complete");
    CHECK(parse_ndjson_string(line, "note_path") == "/home/user/meetings/note.md");
    CHECK(parse_ndjson_string(line, "output_dir") == "/home/user/meetings/2026-03-29");
}

TEST_CASE("parse_ndjson_string: handles escaped backslashes in paths", "[ndjson]") {
    std::string line = R"({"path":"/home/user/my \\meetings\\/output"})";
    CHECK(parse_ndjson_string(line, "path") == "/home/user/my \\meetings\\/output");
}

// ---------------------------------------------------------------------------
// parse_ndjson_int
// ---------------------------------------------------------------------------

TEST_CASE("parse_ndjson_int: extracts integer value", "[ndjson]") {
    std::string line = R"({"event":"progress","data":{"phase":"transcribing","percent":42}})";
    CHECK(parse_ndjson_int(line, "percent") == 42);
}

TEST_CASE("parse_ndjson_int: returns -1 for missing key", "[ndjson]") {
    std::string line = R"({"event":"phase"})";
    CHECK(parse_ndjson_int(line, "percent") == -1);
}

TEST_CASE("parse_ndjson_int: returns -1 for empty input", "[ndjson]") {
    CHECK(parse_ndjson_int("", "key") == -1);
}

TEST_CASE("parse_ndjson_int: handles zero", "[ndjson]") {
    std::string line = R"({"percent":0})";
    CHECK(parse_ndjson_int(line, "percent") == 0);
}

TEST_CASE("parse_ndjson_int: handles 100", "[ndjson]") {
    std::string line = R"({"percent":100})";
    CHECK(parse_ndjson_int(line, "percent") == 100);
}

TEST_CASE("parse_ndjson_int: handles negative value", "[ndjson]") {
    std::string line = R"({"value":-1})";
    CHECK(parse_ndjson_int(line, "value") == -1);
}

TEST_CASE("parse_ndjson_int: handles value with trailing content", "[ndjson]") {
    std::string line = R"({"percent":75,"other":"stuff"})";
    CHECK(parse_ndjson_int(line, "percent") == 75);
}

TEST_CASE("parse_ndjson_int: returns -1 for non-numeric value", "[ndjson]") {
    std::string line = R"({"percent":"abc"})";
    CHECK(parse_ndjson_int(line, "percent") == -1);
}

TEST_CASE("parse_ndjson_int: handles whitespace before value", "[ndjson]") {
    std::string line = R"({"percent": 50})";
    CHECK(parse_ndjson_int(line, "percent") == 50);
}

// ---------------------------------------------------------------------------
// Integration: full NDJSON line parsing
// ---------------------------------------------------------------------------

TEST_CASE("parse_ndjson: phase event round-trip", "[ndjson]") {
    std::string line = R"({"event":"phase","data":{"name":"diarizing"}})";
    CHECK(parse_ndjson_string(line, "event") == "phase");
    CHECK(parse_ndjson_string(line, "name") == "diarizing");
}

TEST_CASE("parse_ndjson: progress event round-trip", "[ndjson]") {
    std::string line = R"({"event":"progress","data":{"phase":"transcribing","percent":67}})";
    CHECK(parse_ndjson_string(line, "event") == "progress");
    CHECK(parse_ndjson_string(line, "phase") == "transcribing");
    CHECK(parse_ndjson_int(line, "percent") == 67);
}

TEST_CASE("parse_ndjson: job.complete event round-trip", "[ndjson]") {
    std::string line = R"({"event":"job.complete","data":{"note_path":"/tmp/note.md","output_dir":"/tmp/out"}})";
    CHECK(parse_ndjson_string(line, "event") == "job.complete");
    CHECK(parse_ndjson_string(line, "note_path") == "/tmp/note.md");
    CHECK(parse_ndjson_string(line, "output_dir") == "/tmp/out");
}
