// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include "util.h"

#include <climits>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

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

    auto out = create_output_dir(base);
    CHECK(fs::exists(out.path));
    CHECK(fs::is_directory(out.path));

    // Directory name should match YYYY-MM-DD_HH-MM pattern
    std::string name = out.path.filename().string();
    CHECK(name.size() >= 16);
    CHECK(name[4] == '-');
    CHECK(name[7] == '-');
    CHECK(name[10] == '_');
    CHECK(name[13] == '-');

    // Timestamp should match directory name
    CHECK(out.timestamp == name);

    fs::remove_all(base);
}

TEST_CASE("create_output_dir: handles collision", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_collision";
    fs::create_directories(base);

    auto out1 = create_output_dir(base);
    // Create another — same minute, should get _2 suffix
    auto out2 = create_output_dir(base);

    CHECK(fs::exists(out1.path));
    CHECK(fs::exists(out2.path));
    CHECK(out1.path != out2.path);
    // Second directory should have _2 suffix
    CHECK(out2.path.filename().string().find("_2") != std::string::npos);
    // But timestamp should be clean (no collision suffix)
    CHECK(out2.timestamp == out1.timestamp);
    CHECK(out2.timestamp.size() == 16);  // YYYY-MM-DD_HH-MM, no suffix

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

// ---------------------------------------------------------------------------
// write_text_file tests
// ---------------------------------------------------------------------------

TEST_CASE("write_text_file: writes content to new file", "[util]") {
    auto dir = fs::temp_directory_path() / "recmeet_test_write";
    fs::create_directories(dir);
    fs::path file = dir / "output.txt";

    write_text_file(file, "hello world\n");

    std::ifstream in(file);
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    CHECK(content == "hello world\n");

    fs::remove_all(dir);
}

TEST_CASE("write_text_file: overwrites existing file", "[util]") {
    auto dir = fs::temp_directory_path() / "recmeet_test_write";
    fs::create_directories(dir);
    fs::path file = dir / "overwrite.txt";

    write_text_file(file, "first");
    write_text_file(file, "second");

    std::ifstream in(file);
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    CHECK(content == "second");

    fs::remove_all(dir);
}

TEST_CASE("write_text_file: throws for nonexistent directory", "[util]") {
    fs::path file = "/nonexistent/path/recmeet_test/output.txt";
    CHECK_THROWS_AS(write_text_file(file, "data"), RecmeetError);
}

TEST_CASE("write_text_file: throws for read-only directory", "[util]") {
    auto dir = fs::temp_directory_path() / "recmeet_test_readonly";
    fs::create_directories(dir);
    fs::path file = dir / "blocked.txt";

    // Make directory read-only
    chmod(dir.c_str(), 0555);

    CHECK_THROWS_AS(write_text_file(file, "data"), RecmeetError);

    // Restore permissions for cleanup
    chmod(dir.c_str(), 0755);
    fs::remove_all(dir);
}

TEST_CASE("write_text_file: handles empty content", "[util]") {
    auto dir = fs::temp_directory_path() / "recmeet_test_write";
    fs::create_directories(dir);
    fs::path file = dir / "empty.txt";

    write_text_file(file, "");

    CHECK(fs::exists(file));
    CHECK(fs::file_size(file) == 0);

    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// resolve_meeting_time tests
// ---------------------------------------------------------------------------

TEST_CASE("resolve_meeting_time: parses YYYY-MM-DD_HH-MM from directory name", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_rmt";
    fs::path dir = base / "2026-02-15_14-30";
    fs::create_directories(dir);
    fs::path audio = dir / "audio.wav";
    // Audio file doesn't need to exist for dir-name parsing
    std::ofstream(audio) << "fake";

    auto [date, time] = resolve_meeting_time(dir, audio);
    CHECK(date == "2026-02-15");
    CHECK(time == "14:30");

    fs::remove_all(base);
}

TEST_CASE("resolve_meeting_time: handles dir name with collision suffix", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_rmt2";
    fs::path dir = base / "2026-03-08_09-15_2";
    fs::create_directories(dir);
    fs::path audio = dir / "audio.wav";
    std::ofstream(audio) << "fake";

    auto [date, time] = resolve_meeting_time(dir, audio);
    CHECK(date == "2026-03-08");
    CHECK(time == "09:15");

    fs::remove_all(base);
}

TEST_CASE("resolve_meeting_time: falls back to audio mtime when dir name doesn't match", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_rmt3";
    fs::path dir = base / "random_dir_name";
    fs::create_directories(dir);
    fs::path audio = dir / "audio.wav";
    std::ofstream(audio) << "fake";

    auto [date, time] = resolve_meeting_time(dir, audio);
    // Should return something valid (from mtime) — at least non-empty
    CHECK(date.size() == 10);  // YYYY-MM-DD
    CHECK(time.size() == 5);   // HH:MM
    CHECK(date[4] == '-');
    CHECK(date[7] == '-');
    CHECK(time[2] == ':');

    fs::remove_all(base);
}

TEST_CASE("resolve_meeting_time: falls back to now when audio doesn't exist", "[util]") {
    fs::path dir = "/tmp/recmeet_test_nonexistent_dir";
    fs::path audio = dir / "audio.wav";

    auto [date, time] = resolve_meeting_time(dir, audio);
    // Should return current time — at least valid format
    CHECK(date.size() == 10);
    CHECK(time.size() == 5);
}

// ---------------------------------------------------------------------------
// find_audio_file tests
// ---------------------------------------------------------------------------

TEST_CASE("find_audio_file: finds timestamped audio file", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_find_audio_ts";
    fs::remove_all(base);
    fs::create_directories(base);
    std::ofstream(base / "audio_2026-02-21_09-41.wav") << "RIFF";

    auto result = find_audio_file(base);
    CHECK(result == base / "audio_2026-02-21_09-41.wav");

    fs::remove_all(base);
}

TEST_CASE("find_audio_file: finds legacy audio.wav", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_find_audio_legacy";
    fs::remove_all(base);
    fs::create_directories(base);
    std::ofstream(base / "audio.wav") << "RIFF";

    auto result = find_audio_file(base);
    CHECK(result == base / "audio.wav");

    fs::remove_all(base);
}

TEST_CASE("find_audio_file: prefers timestamped over legacy", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_find_audio_both";
    fs::remove_all(base);
    fs::create_directories(base);
    std::ofstream(base / "audio_2026-02-21_09-41.wav") << "RIFF";
    std::ofstream(base / "audio.wav") << "RIFF";

    auto result = find_audio_file(base);
    CHECK(result.filename().string().find(AUDIO_PREFIX) == 0);
    CHECK(result != base / "audio.wav");

    fs::remove_all(base);
}

TEST_CASE("find_audio_file: empty directory returns empty", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_find_audio_empty";
    fs::remove_all(base);
    fs::create_directories(base);

    auto result = find_audio_file(base);
    CHECK(result.empty());

    fs::remove_all(base);
}

TEST_CASE("find_audio_file: non-existent directory returns empty", "[util]") {
    auto result = find_audio_file("/tmp/recmeet_nonexistent_find_audio");
    CHECK(result.empty());
}

TEST_CASE("find_audio_file: directory with non-audio files only returns empty", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_find_audio_noaudio";
    fs::remove_all(base);
    fs::create_directories(base);
    std::ofstream(base / "transcript.txt") << "hello";
    std::ofstream(base / "summary.md") << "summary";

    auto result = find_audio_file(base);
    CHECK(result.empty());

    fs::remove_all(base);
}

TEST_CASE("find_audio_file: ignores audio_ files without .wav extension", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_find_audio_notwav";
    fs::remove_all(base);
    fs::create_directories(base);
    std::ofstream(base / "audio_2026-02-21_09-41.mp3") << "data";

    auto result = find_audio_file(base);
    CHECK(result.empty());

    fs::remove_all(base);
}

// ---------------------------------------------------------------------------
// resolve_meeting_time: filename-based timestamp parsing
// ---------------------------------------------------------------------------

TEST_CASE("resolve_meeting_time: extracts timestamp from audio filename (priority 1)", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_rmt_fname";
    fs::path dir = base / "2026-03-08_10-00";
    fs::create_directories(dir);
    fs::path audio = dir / "audio_2026-03-08_10-00.wav";
    std::ofstream(audio) << "RIFF";

    auto [date, time] = resolve_meeting_time(dir, audio);
    CHECK(date == "2026-03-08");
    CHECK(time == "10:00");

    fs::remove_all(base);
}

TEST_CASE("resolve_meeting_time: filename takes precedence over directory name", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_rmt_fname_prio";
    // Directory says 09:15, but filename says 14:30
    fs::path dir = base / "2026-03-08_09-15";
    fs::create_directories(dir);
    fs::path audio = dir / "audio_2026-02-21_14-30.wav";
    std::ofstream(audio) << "RIFF";

    auto [date, time] = resolve_meeting_time(dir, audio);
    // Filename wins
    CHECK(date == "2026-02-21");
    CHECK(time == "14:30");

    fs::remove_all(base);
}

TEST_CASE("resolve_meeting_time: legacy audio.wav falls through to directory name", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_rmt_legacy";
    fs::path dir = base / "2026-02-15_14-30";
    fs::create_directories(dir);
    fs::path audio = dir / "audio.wav";
    std::ofstream(audio) << "RIFF";

    auto [date, time] = resolve_meeting_time(dir, audio);
    // Falls through to directory name parsing
    CHECK(date == "2026-02-15");
    CHECK(time == "14:30");

    fs::remove_all(base);
}

TEST_CASE("resolve_meeting_time: rejects invalid dates in filename", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_rmt_invalid";
    fs::path dir = base / "2026-02-15_14-30";
    fs::create_directories(dir);
    // Month 13 is invalid
    fs::path audio = dir / "audio_2026-13-01_10-00.wav";
    std::ofstream(audio) << "RIFF";

    auto [date, time] = resolve_meeting_time(dir, audio);
    // Falls through to directory name
    CHECK(date == "2026-02-15");
    CHECK(time == "14:30");

    fs::remove_all(base);
}

TEST_CASE("resolve_meeting_time: rejects too-short filename stem", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_rmt_short";
    fs::path dir = base / "2026-02-15_14-30";
    fs::create_directories(dir);
    // "audio_short" has the prefix but stem is too short for timestamp
    fs::path audio = dir / "audio_short.wav";
    std::ofstream(audio) << "RIFF";

    auto [date, time] = resolve_meeting_time(dir, audio);
    // Falls through to directory name
    CHECK(date == "2026-02-15");
    CHECK(time == "14:30");

    fs::remove_all(base);
}

// ---------------------------------------------------------------------------
// create_output_dir: OutputDir struct tests
// ---------------------------------------------------------------------------

TEST_CASE("create_output_dir: returns clean timestamp without collision suffix", "[util]") {
    auto base = fs::temp_directory_path() / "recmeet_test_outdir_struct";
    fs::remove_all(base);
    fs::create_directories(base);

    auto out1 = create_output_dir(base);
    auto out2 = create_output_dir(base);

    // Both timestamps should be identical (clean, no collision suffix)
    CHECK(out1.timestamp == out2.timestamp);
    CHECK(out1.timestamp.size() == 16);  // YYYY-MM-DD_HH-MM, no suffix

    // But paths should differ
    CHECK(out1.path != out2.path);
    CHECK(out2.path.filename().string().find("_2") != std::string::npos);

    // Timestamp should be valid format
    CHECK(out1.timestamp[4] == '-');
    CHECK(out1.timestamp[7] == '-');
    CHECK(out1.timestamp[10] == '_');
    CHECK(out1.timestamp[13] == '-');

    fs::remove_all(base);
}

TEST_CASE("audio constants are consistent", "[util]") {
    CHECK(SAMPLE_RATE == 16000);
    CHECK(CHANNELS == 1);
    CHECK(SAMPLE_BITS == 16);
    CHECK(BYTES_PER_SAMPLE == 2);
    CHECK(BYTES_PER_SEC == 32000);  // 16000 * 1 * 2
}

// ---------------------------------------------------------------------------
// read_self_rss_kb tests
// ---------------------------------------------------------------------------

TEST_CASE("read_self_rss_kb: returns positive value for current process", "[util]") {
    long rss = read_self_rss_kb();
    // Catch2 + linked vendor libs guarantee a non-trivial RSS.
    CHECK(rss > 0);
    // Sanity bound — the test process should be well under 8 GB.
    CHECK(rss < 8L * 1024L * 1024L);
}

TEST_CASE("read_self_rss_kb: monotonically grows after large allocation", "[util]") {
    long before = read_self_rss_kb();
    REQUIRE(before > 0);
    // Allocate ~64 MB and touch every page so it actually faults in.
    constexpr size_t bytes = 64L * 1024L * 1024L;
    std::vector<unsigned char> buf(bytes);
    long page = sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < bytes; i += page) buf[i] = 1;
    long after = read_self_rss_kb();
    CHECK(after >= before);
    // Should observe at least ~32 MB of growth (slop for kernel timing).
    CHECK(after - before > 32L * 1024L);
}

// ---------------------------------------------------------------------------
// write_heartbeat_ndjson / write_rss_limit_msg tests
// ---------------------------------------------------------------------------

namespace {

std::string read_pipe_to_string(int fd) {
    std::string out;
    char buf[256];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) out.append(buf, n);
        else break;
    }
    return out;
}

}  // namespace

TEST_CASE("write_heartbeat_ndjson: emits expected NDJSON format with rss_kb", "[util]") {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);

    size_t written = write_heartbeat_ndjson(fds[1], 12345);
    CHECK(written > 0);
    ::close(fds[1]);

    std::string line = read_pipe_to_string(fds[0]);
    ::close(fds[0]);

    CHECK(line == "{\"event\":\"heartbeat\",\"data\":{\"rss_kb\":12345}}\n");
}

TEST_CASE("write_heartbeat_ndjson: handles zero rss_kb (read failure)", "[util]") {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);

    size_t written = write_heartbeat_ndjson(fds[1], 0);
    CHECK(written > 0);
    ::close(fds[1]);

    std::string line = read_pipe_to_string(fds[0]);
    ::close(fds[0]);

    CHECK(line == "{\"event\":\"heartbeat\",\"data\":{\"rss_kb\":0}}\n");
}

TEST_CASE("write_rss_limit_msg: writes distinctive marker line", "[util]") {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);

    write_rss_limit_msg(fds[1]);
    ::close(fds[1]);

    std::string line = read_pipe_to_string(fds[0]);
    ::close(fds[0]);

    // Must contain the distinctive substring the daemon will surface to the user.
    CHECK(line.find("child RSS limit exceeded") != std::string::npos);
    // Must end with a newline so it's captured as a single stderr line.
    CHECK(!line.empty());
    CHECK(line.back() == '\n');
}

// -------------------------------------------------------------------------
// T1C.2: parse_memory_property_line — drives the daemon's MemoryHigh
// restore path. Must round-trip both numeric byte counts and the literal
// "infinity" sentinel.
// -------------------------------------------------------------------------

TEST_CASE("parse_memory_property_line: numeric bytes", "[util][t1c]") {
    CHECK(parse_memory_property_line("MemoryHigh=10737418240\n") == 10737418240L);
    CHECK(parse_memory_property_line("MemoryMax=15032385536\n") == 15032385536L);
    CHECK(parse_memory_property_line("MemoryHigh=0\n") == 0L);
    // No trailing newline (some systemd builds omit it).
    CHECK(parse_memory_property_line("MemoryHigh=10737418240") == 10737418240L);
}

TEST_CASE("parse_memory_property_line: infinity sentinel", "[util][t1c]") {
    CHECK(parse_memory_property_line("MemoryHigh=infinity\n") == LONG_MAX);
    CHECK(parse_memory_property_line("MemoryMax=infinity") == LONG_MAX);
    // The "infinity" token is matched as a prefix; trailing whitespace
    // or null fill from a fixed-size read buffer is acceptable.
    CHECK(parse_memory_property_line("MemoryHigh=infinity ") == LONG_MAX);
}

TEST_CASE("parse_memory_property_line: malformed input", "[util][t1c]") {
    CHECK(parse_memory_property_line(nullptr) == -1);
    CHECK(parse_memory_property_line("") == -1);
    CHECK(parse_memory_property_line("MemoryHigh") == -1);   // no '='
    CHECK(parse_memory_property_line("MemoryHigh=") == -1);  // empty value
    CHECK(parse_memory_property_line("garbage") == -1);
}
