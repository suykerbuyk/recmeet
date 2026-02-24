// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "log.h"
#include "util.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace recmeet;
namespace fs = std::filesystem;

TEST_CASE("parse_log_level: valid strings", "[log]") {
    CHECK(parse_log_level("none") == LogLevel::NONE);
    CHECK(parse_log_level("NONE") == LogLevel::NONE);
    CHECK(parse_log_level("error") == LogLevel::ERROR);
    CHECK(parse_log_level("ERROR") == LogLevel::ERROR);
    CHECK(parse_log_level("warn") == LogLevel::WARN);
    CHECK(parse_log_level("WARN") == LogLevel::WARN);
    CHECK(parse_log_level("info") == LogLevel::INFO);
    CHECK(parse_log_level("INFO") == LogLevel::INFO);
}

TEST_CASE("parse_log_level: unrecognized returns NONE", "[log]") {
    CHECK(parse_log_level("debug") == LogLevel::NONE);
    CHECK(parse_log_level("") == LogLevel::NONE);
    CHECK(parse_log_level("verbose") == LogLevel::NONE);
}

TEST_CASE("log_level_name: round-trip", "[log]") {
    CHECK(std::string(log_level_name(LogLevel::NONE)) == "NONE");
    CHECK(std::string(log_level_name(LogLevel::ERROR)) == "ERROR");
    CHECK(std::string(log_level_name(LogLevel::WARN)) == "WARN");
    CHECK(std::string(log_level_name(LogLevel::INFO)) == "INFO");
}

TEST_CASE("log_init with NONE: no file created", "[log]") {
    fs::path tmp = fs::temp_directory_path() / "recmeet_test_log_none";
    fs::remove_all(tmp);
    log_init(LogLevel::NONE, tmp);
    log_info("this should not appear");
    log_shutdown();
    // Directory should not even be created when level is NONE
    CHECK_FALSE(fs::exists(tmp));
}

TEST_CASE("log_init with INFO: creates log file", "[log]") {
    fs::path tmp = fs::temp_directory_path() / "recmeet_test_log_info";
    fs::remove_all(tmp);
    log_init(LogLevel::INFO, tmp);
    log_info("test message %d", 42);
    log_warn("warning %s", "hello");
    log_error("error!");
    log_shutdown();

    // Find the log file
    bool found = false;
    for (const auto& entry : fs::directory_iterator(tmp)) {
        if (entry.path().extension() == ".log") {
            found = true;
            std::ifstream in(entry.path());
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            CHECK(content.find("[INFO] test message 42") != std::string::npos);
            CHECK(content.find("[WARN] warning hello") != std::string::npos);
            CHECK(content.find("[ERROR] error!") != std::string::npos);
        }
    }
    CHECK(found);
    fs::remove_all(tmp);
}

TEST_CASE("log_init with ERROR: filters INFO and WARN", "[log]") {
    fs::path tmp = fs::temp_directory_path() / "recmeet_test_log_error";
    fs::remove_all(tmp);
    log_init(LogLevel::ERROR, tmp);
    log_info("should not appear");
    log_warn("should not appear");
    log_error("should appear");
    log_shutdown();

    for (const auto& entry : fs::directory_iterator(tmp)) {
        if (entry.path().extension() == ".log") {
            std::ifstream in(entry.path());
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            CHECK(content.find("should not appear") == std::string::npos);
            CHECK(content.find("[ERROR] should appear") != std::string::npos);
        }
    }
    fs::remove_all(tmp);
}
