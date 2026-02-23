// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "pipeline.h"

#include <fstream>

using namespace recmeet;

static fs::path tmp_dir() {
    fs::path dir = fs::temp_directory_path() / "recmeet_test_pipeline";
    fs::create_directories(dir);
    return dir;
}

TEST_CASE("read_context_file: reads existing file", "[pipeline]") {
    auto dir = tmp_dir();
    fs::path file = dir / "context.txt";

    {
        std::ofstream out(file);
        out << "Agenda:\n- Item 1\n- Item 2\n";
    }

    std::string content = read_context_file(file);
    CHECK(content == "Agenda:\n- Item 1\n- Item 2\n");

    fs::remove_all(dir);
}

TEST_CASE("read_context_file: returns empty for missing file", "[pipeline]") {
    std::string content = read_context_file("/nonexistent/path/context.txt");
    CHECK(content.empty());
}

TEST_CASE("read_context_file: returns empty for empty path", "[pipeline]") {
    std::string content = read_context_file(fs::path{});
    CHECK(content.empty());
}

TEST_CASE("read_context_file: returns empty for empty file", "[pipeline]") {
    auto dir = tmp_dir();
    fs::path file = dir / "empty.txt";
    { std::ofstream out(file); }

    std::string content = read_context_file(file);
    CHECK(content.empty());

    fs::remove_all(dir);
}
