// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "log.h"

#include <filesystem>
#include <fstream>

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
    CHECK(parse_log_level("debug") == LogLevel::DEBUG);
    CHECK(parse_log_level("DEBUG") == LogLevel::DEBUG);
}

TEST_CASE("parse_log_level: unrecognized returns NONE", "[log]") {
    CHECK(parse_log_level("") == LogLevel::NONE);
    CHECK(parse_log_level("verbose") == LogLevel::NONE);
    CHECK(parse_log_level("trace") == LogLevel::NONE);
}

TEST_CASE("log_level_name: round-trip", "[log]") {
    CHECK(std::string(log_level_name(LogLevel::NONE)) == "NONE");
    CHECK(std::string(log_level_name(LogLevel::ERROR)) == "ERROR");
    CHECK(std::string(log_level_name(LogLevel::WARN)) == "WARN");
    CHECK(std::string(log_level_name(LogLevel::INFO)) == "INFO");
    CHECK(std::string(log_level_name(LogLevel::DEBUG)) == "DEBUG");
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

TEST_CASE("log_init with INFO: creates hourly log file", "[log]") {
    fs::path tmp = fs::temp_directory_path() / "recmeet_test_log_info";
    fs::remove_all(tmp);
    log_init(LogLevel::INFO, tmp);
    log_info("test message %d", 42);
    log_warn("warning %s", "hello");
    log_error("error!");
    log_debug("should not appear at INFO level");
    log_shutdown();

    // Find the log file
    bool found = false;
    for (const auto& entry : fs::directory_iterator(tmp)) {
        if (entry.path().extension() == ".log") {
            found = true;
            std::ifstream in(entry.path());
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            // Format: YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [tid=N] message
            CHECK(content.find("[INFO]") != std::string::npos);
            CHECK(content.find("test message 42") != std::string::npos);
            CHECK(content.find("[WARN]") != std::string::npos);
            CHECK(content.find("warning hello") != std::string::npos);
            CHECK(content.find("[ERROR]") != std::string::npos);
            CHECK(content.find("error!") != std::string::npos);
            CHECK(content.find("[tid=") != std::string::npos);
            // DEBUG should not appear at INFO level
            CHECK(content.find("should not appear") == std::string::npos);
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
            CHECK(content.find("[ERROR]") != std::string::npos);
            CHECK(content.find("should appear") != std::string::npos);
        }
    }
    fs::remove_all(tmp);
}

TEST_CASE("log_init with DEBUG: all levels appear", "[log]") {
    fs::path tmp = fs::temp_directory_path() / "recmeet_test_log_debug";
    fs::remove_all(tmp);
    log_init(LogLevel::DEBUG, tmp);
    log_debug("debug msg");
    log_info("info msg");
    log_warn("warn msg");
    log_error("error msg");
    log_shutdown();

    for (const auto& entry : fs::directory_iterator(tmp)) {
        if (entry.path().extension() == ".log") {
            std::ifstream in(entry.path());
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            CHECK(content.find("[DEBUG]") != std::string::npos);
            CHECK(content.find("debug msg") != std::string::npos);
            CHECK(content.find("[INFO]") != std::string::npos);
            CHECK(content.find("[WARN]") != std::string::npos);
            CHECK(content.find("[ERROR]") != std::string::npos);
        }
    }
    fs::remove_all(tmp);
}

TEST_CASE("log format: millisecond timestamps", "[log]") {
    fs::path tmp = fs::temp_directory_path() / "recmeet_test_log_millis";
    fs::remove_all(tmp);
    log_init(LogLevel::INFO, tmp);
    log_info("timestamp check");
    log_shutdown();

    for (const auto& entry : fs::directory_iterator(tmp)) {
        if (entry.path().extension() == ".log") {
            std::ifstream in(entry.path());
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            // Check for millis: pattern like "HH:MM:SS.NNN"
            // Just verify the dot+3digits pattern exists before [INFO]
            CHECK(content.find(".[") == std::string::npos);  // no ".["
            // The format is SS.mmm [LEVEL] — look for a 3-digit group after a dot
            auto pos = content.find("[INFO]");
            REQUIRE(pos != std::string::npos);
            // Millis appear before the level tag: "...SS.123 [INFO]..."
            REQUIRE(pos >= 5);
            CHECK(content[pos - 1] == ' ');
            CHECK(std::isdigit(content[pos - 2]));
            CHECK(std::isdigit(content[pos - 3]));
            CHECK(std::isdigit(content[pos - 4]));
            CHECK(content[pos - 5] == '.');
        }
    }
    fs::remove_all(tmp);
}

TEST_CASE("hourly log file naming", "[log]") {
    fs::path tmp = fs::temp_directory_path() / "recmeet_test_log_hourly";
    fs::remove_all(tmp);
    log_init(LogLevel::INFO, tmp);
    log_info("hourly check");
    log_shutdown();

    bool found = false;
    for (const auto& entry : fs::directory_iterator(tmp)) {
        auto name = entry.path().filename().string();
        // Hourly format: recmeet-YYYY-MM-DD-HH.log (25 chars)
        if (name.substr(0, 8) == "recmeet-" && name.size() == 25 && name.substr(21) == ".log") {
            found = true;
            // Verify the hour part is two digits (positions 19-20)
            CHECK(std::isdigit(name[20]));
            CHECK(std::isdigit(name[19]));
            CHECK(name[18] == '-');
        }
    }
    CHECK(found);
    fs::remove_all(tmp);
}

TEST_CASE("log rotation: purges old files by filename", "[log]") {
    fs::path tmp = fs::temp_directory_path() / "recmeet_test_log_rotation";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // Create fake old log files (24+ hours old, well outside 4-hour retention)
    std::ofstream(tmp / "recmeet-2020-01-01-00.log") << "old\n";
    std::ofstream(tmp / "recmeet-2020-01-01-01.log") << "old\n";
    // Create a non-matching file that should be left alone
    std::ofstream(tmp / "other.txt") << "keep\n";

    // Init logging — this triggers purge on startup
    log_init(LogLevel::INFO, tmp, 4);
    log_info("after purge");
    log_shutdown();

    CHECK_FALSE(fs::exists(tmp / "recmeet-2020-01-01-00.log"));
    CHECK_FALSE(fs::exists(tmp / "recmeet-2020-01-01-01.log"));
    CHECK(fs::exists(tmp / "other.txt"));

    fs::remove_all(tmp);
}
