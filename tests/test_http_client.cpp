// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "http_client.h"
#include "util.h"

#include <fstream>

using namespace recmeet;

static fs::path tmp_dir() {
    fs::path dir = fs::temp_directory_path() / "recmeet_test_http";
    fs::create_directories(dir);
    return dir;
}

TEST_CASE("http_get: reads local file via file:// URI", "[http_client]") {
    auto dir = tmp_dir();
    fs::path file = dir / "test_get.txt";

    {
        std::ofstream out(file);
        out << "hello from file";
    }

    std::string url = "file://" + file.string();
    std::string body = http_get(url);
    CHECK(body == "hello from file");

    fs::remove_all(dir);
}

TEST_CASE("http_get: throws for nonexistent file", "[http_client]") {
    CHECK_THROWS_AS(http_get("file:///nonexistent/path/to/file.txt"), RecmeetError);
}

TEST_CASE("http_get: reads empty file returns empty string", "[http_client]") {
    auto dir = tmp_dir();
    fs::path file = dir / "empty.txt";
    { std::ofstream out(file); }

    std::string url = "file://" + file.string();
    std::string body = http_get(url);
    CHECK(body.empty());

    fs::remove_all(dir);
}

TEST_CASE("http_get: reads file with special characters", "[http_client]") {
    auto dir = tmp_dir();
    fs::path file = dir / "special.txt";

    {
        std::ofstream out(file);
        out << "line1\nline2\ttab\n";
        out << "unicode: \xc3\xa9\xc3\xa0\xc3\xbc";  // eauu
    }

    std::string url = "file://" + file.string();
    std::string body = http_get(url);
    CHECK(body.find("line1\nline2\ttab\n") != std::string::npos);
    CHECK(body.find("\xc3\xa9\xc3\xa0\xc3\xbc") != std::string::npos);

    fs::remove_all(dir);
}

TEST_CASE("http_post_json: throws for unsupported protocol", "[http_client]") {
    // POST to file:// is not supported by libcurl
    CHECK_THROWS_AS(
        http_post_json("file:///tmp/test.txt", R"({"key":"val"})"),
        RecmeetError);
}
