#include <catch2/catch_test_macros.hpp>
#include "util.h"

using namespace recmeet;

TEST_CASE("StopToken: default state is not requested", "[util]") {
    StopToken token;
    CHECK_FALSE(token.stop_requested());
}

TEST_CASE("StopToken: request sets flag", "[util]") {
    StopToken token;
    token.request();
    CHECK(token.stop_requested());
}

TEST_CASE("StopToken: reset clears flag", "[util]") {
    StopToken token;
    token.request();
    REQUIRE(token.stop_requested());
    token.reset();
    CHECK_FALSE(token.stop_requested());
}

TEST_CASE("config_dir: returns path ending in recmeet", "[util]") {
    auto dir = config_dir();
    CHECK(dir.filename() == "recmeet");
    // Should be under .config or XDG_CONFIG_HOME
    CHECK(dir.string().find("config") != std::string::npos);
}

TEST_CASE("data_dir: returns path ending in recmeet", "[util]") {
    auto dir = data_dir();
    CHECK(dir.filename() == "recmeet");
}

TEST_CASE("models_dir: is subdirectory of data_dir", "[util]") {
    auto mdir = models_dir();
    auto ddir = data_dir();
    CHECK(mdir.filename() == "models");
    // models_dir should start with data_dir
    CHECK(mdir.string().find(ddir.string()) == 0);
}

TEST_CASE("create_output_dir: creates timestamped directory", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_output";
    fs::create_directories(base);

    auto dir = create_output_dir(base);
    CHECK(fs::exists(dir));
    CHECK(fs::is_directory(dir));

    // Directory name should match YYYY-MM-DD_HH-MM pattern
    std::string name = dir.filename().string();
    // At minimum: 4 digits - 2 digits - 2 digits _ 2 digits - 2 digits
    CHECK(name.size() >= 16);
    CHECK(name[4] == '-');
    CHECK(name[7] == '-');
    CHECK(name[10] == '_');
    CHECK(name[13] == '-');

    fs::remove_all(base);
}

TEST_CASE("create_output_dir: handles collision", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_collision";
    fs::create_directories(base);

    auto dir1 = create_output_dir(base);
    // Create another — same minute, should get _2 suffix
    auto dir2 = create_output_dir(base);

    CHECK(fs::exists(dir1));
    CHECK(fs::exists(dir2));
    CHECK(dir1 != dir2);
    // Second should have _2 suffix
    CHECK(dir2.filename().string().find("_2") != std::string::npos);

    fs::remove_all(base);
}

TEST_CASE("default_thread_count: returns at least 1", "[util]") {
    int n = default_thread_count();
    CHECK(n >= 1);
}

TEST_CASE("default_thread_count: returns hardware_concurrency - 1 when available", "[util]") {
    unsigned hw = std::thread::hardware_concurrency();
    int n = default_thread_count();
    if (hw > 1) {
        CHECK(n == static_cast<int>(hw - 1));
    } else {
        CHECK(n == 1);
    }
}

TEST_CASE("audio constants are consistent", "[util]") {
    CHECK(SAMPLE_RATE == 16000);
    CHECK(CHANNELS == 1);
    CHECK(SAMPLE_BITS == 16);
    CHECK(BYTES_PER_SAMPLE == 2);
    CHECK(BYTES_PER_SEC == 32000);  // 16000 * 1 * 2
}
