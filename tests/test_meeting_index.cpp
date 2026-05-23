// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

// Phase C.11.2 — unit tests for MeetingIndex.

#include <catch2/catch_test_macros.hpp>

#include "meeting_index.h"
#include "pipeline.h" // save_meeting_context
#include "test_tmpdir.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace recmeet;

namespace {
fs::path tmp_dir(const std::string& tag) {
    auto base = recmeet::test::tmp_path(
        "recmeet-meeting-index-" + tag + "-" +
        std::to_string(::getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch().count()));
    fs::create_directories(base);
    return base;
}

// Helper: create a meeting dir under root with the given id, returning the path.
fs::path make_meeting(const fs::path& root, const std::string& dirname,
                      const std::string& id, const std::string& ctx = "ctx") {
    fs::path d = root / dirname;
    fs::create_directories(d);
    save_meeting_context(d, ctx, {}, "", id);
    return d;
}
} // namespace

// --- find / bind / unbind --------------------------------------------------

TEST_CASE("MeetingIndex: find on empty index returns nullopt",
          "[meeting-index][c11]") {
    MeetingIndex idx;
    CHECK_FALSE(idx.find("12345678-1234-4567-89ab-1234567890ab").has_value());
    CHECK(idx.size() == 0);
}

TEST_CASE("MeetingIndex: find empty meeting_id returns nullopt even if mapped",
          "[meeting-index][c11]") {
    MeetingIndex idx;
    // bind() of an empty id is a defensive no-op — exercise it.
    idx.bind("", fs::path("/tmp/x"));
    CHECK_FALSE(idx.find("").has_value());
    CHECK(idx.size() == 0);
}

TEST_CASE("MeetingIndex: bind then find returns the bound path",
          "[meeting-index][c11]") {
    MeetingIndex idx;
    const std::string id = "12345678-1234-4567-89ab-1234567890ab";
    idx.bind(id, fs::path("/tmp/meet-A"));
    auto p = idx.find(id);
    REQUIRE(p.has_value());
    CHECK(*p == fs::path("/tmp/meet-A"));
    CHECK(idx.size() == 1);
}

TEST_CASE("MeetingIndex: bind is idempotent on same path",
          "[meeting-index][c11]") {
    MeetingIndex idx;
    const std::string id = "abcdef01-2345-4678-9abc-def012345678";
    idx.bind(id, fs::path("/tmp/meet-A"));
    idx.bind(id, fs::path("/tmp/meet-A"));
    CHECK(idx.size() == 1);
    CHECK(idx.find(id) == fs::path("/tmp/meet-A"));
}

TEST_CASE("MeetingIndex: bind overwrites on different path "
          "(rare but the convergence-principle dedup needs this property)",
          "[meeting-index][c11]") {
    MeetingIndex idx;
    const std::string id = "abcdef01-2345-4678-9abc-def012345678";
    idx.bind(id, fs::path("/tmp/meet-A"));
    idx.bind(id, fs::path("/tmp/meet-B"));
    CHECK(idx.size() == 1);
    CHECK(idx.find(id) == fs::path("/tmp/meet-B"));
}

TEST_CASE("MeetingIndex: unbind removes the binding and returns true",
          "[meeting-index][c11]") {
    MeetingIndex idx;
    const std::string id = "12345678-1234-4567-89ab-1234567890ab";
    idx.bind(id, fs::path("/tmp/x"));
    CHECK(idx.unbind(id));
    CHECK_FALSE(idx.find(id).has_value());
    CHECK(idx.size() == 0);
}

TEST_CASE("MeetingIndex: unbind of unknown id returns false",
          "[meeting-index][c11]") {
    MeetingIndex idx;
    CHECK_FALSE(idx.unbind("12345678-1234-4567-89ab-1234567890ab"));
    CHECK_FALSE(idx.unbind(""));
}

// --- rebuild_from_disk -----------------------------------------------------

TEST_CASE("MeetingIndex: rebuild_from_disk on missing root yields empty index",
          "[meeting-index][c11]") {
    MeetingIndex idx;
    idx.bind("12345678-1234-4567-89ab-1234567890ab", fs::path("/old"));
    // The rebuild MUST clear the prior contents even when the new root is
    // missing — otherwise startup with an unconfigured meetings_root would
    // resurrect stale daemon-internal bindings (none today, but the invariant
    // belongs to the class).
    auto n = idx.rebuild_from_disk("/definitely/does/not/exist");
    CHECK(n == 0);
    CHECK(idx.size() == 0);
}

TEST_CASE("MeetingIndex: rebuild_from_disk picks up valid meeting dirs",
          "[meeting-index][c11]") {
    auto root = tmp_dir("rebuild-happy");
    auto p1 = make_meeting(root, "2026-05-16_10-00",
                           "12345678-1234-4567-89ab-1234567890ab", "ctx1");
    auto p2 = make_meeting(root, "2026-05-16_11-00",
                           "abcdef01-2345-4678-9abc-def012345678", "ctx2");
    auto p3 = make_meeting(root, "2026-05-16_12-00",
                           "00000000-0000-4000-8000-000000000001", "ctx3");

    MeetingIndex idx;
    auto n = idx.rebuild_from_disk(root);
    CHECK(n == 3);
    CHECK(idx.size() == 3);
    CHECK(idx.find("12345678-1234-4567-89ab-1234567890ab") == p1);
    CHECK(idx.find("abcdef01-2345-4678-9abc-def012345678") == p2);
    CHECK(idx.find("00000000-0000-4000-8000-000000000001") == p3);

    fs::remove_all(root);
}

TEST_CASE("MeetingIndex: rebuild_from_disk skips v1-shaped meeting dirs "
          "(no meeting_id in context.json)",
          "[meeting-index][c11]") {
    auto root = tmp_dir("rebuild-v1-skip");

    // v1-shaped dir — context.json present, no meeting_id field.
    fs::path v1 = root / "2026-05-16_09-00";
    fs::create_directories(v1);
    save_meeting_context(v1, "v1 context"); // no id

    // v2-shaped dir — has meeting_id.
    auto v2 = make_meeting(root, "2026-05-16_10-00",
                           "12345678-1234-4567-89ab-1234567890ab", "v2 ctx");

    MeetingIndex idx;
    auto n = idx.rebuild_from_disk(root);
    CHECK(n == 1);
    CHECK(idx.size() == 1);
    CHECK(idx.find("12345678-1234-4567-89ab-1234567890ab") == v2);

    fs::remove_all(root);
}

TEST_CASE("MeetingIndex: rebuild_from_disk skips dirs without context.json",
          "[meeting-index][c11]") {
    auto root = tmp_dir("rebuild-no-ctx");

    // Empty dir.
    fs::create_directories(root / "abandoned");

    // Dir with random files but no context.json.
    fs::create_directories(root / "partial");
    std::ofstream(root / "partial" / "audio.wav") << "fake wav";

    // One valid dir as the positive control.
    auto good = make_meeting(root, "2026-05-16_10-00",
                             "12345678-1234-4567-89ab-1234567890ab");

    MeetingIndex idx;
    auto n = idx.rebuild_from_disk(root);
    CHECK(n == 1);
    CHECK(idx.find("12345678-1234-4567-89ab-1234567890ab") == good);

    fs::remove_all(root);
}

TEST_CASE("MeetingIndex: rebuild_from_disk skips dirs with malformed meeting_id "
          "(matches load_meeting_id's defensive return)",
          "[meeting-index][c11]") {
    auto root = tmp_dir("rebuild-malformed");

    fs::path bad = root / "2026-05-16_09-00";
    fs::create_directories(bad);
    {
        std::ofstream out(bad / "context.json");
        out << "{\"context\":\"\",\"context_file\":\"\","
               "\"meeting_id\":\"NOT-A-VALID-UUID\"}";
    }

    auto good = make_meeting(root, "2026-05-16_10-00",
                             "12345678-1234-4567-89ab-1234567890ab");

    MeetingIndex idx;
    auto n = idx.rebuild_from_disk(root);
    CHECK(n == 1);
    CHECK(idx.find("12345678-1234-4567-89ab-1234567890ab") == good);

    fs::remove_all(root);
}

TEST_CASE("MeetingIndex: rebuild_from_disk ignores stray top-level files",
          "[meeting-index][c11]") {
    auto root = tmp_dir("rebuild-stray");

    // A stray file at the top level (operator's stray dotfile, .DS_Store, etc.).
    std::ofstream(root / "README.md") << "hi";

    auto good = make_meeting(root, "2026-05-16_10-00",
                             "12345678-1234-4567-89ab-1234567890ab");

    MeetingIndex idx;
    auto n = idx.rebuild_from_disk(root);
    CHECK(n == 1);
    CHECK(idx.find("12345678-1234-4567-89ab-1234567890ab") == good);

    fs::remove_all(root);
}

TEST_CASE("MeetingIndex: rebuild_from_disk replaces prior contents atomically",
          "[meeting-index][c11]") {
    MeetingIndex idx;
    // Seed with a stale binding.
    idx.bind("ffffffff-ffff-4fff-bfff-ffffffffffff", fs::path("/old/path"));

    auto root = tmp_dir("rebuild-replace");
    auto good = make_meeting(root, "2026-05-16_10-00",
                             "12345678-1234-4567-89ab-1234567890ab");

    idx.rebuild_from_disk(root);
    // Stale binding is gone.
    CHECK_FALSE(idx.find("ffffffff-ffff-4fff-bfff-ffffffffffff").has_value());
    // Fresh binding is present.
    CHECK(idx.find("12345678-1234-4567-89ab-1234567890ab") == good);
    CHECK(idx.size() == 1);

    fs::remove_all(root);
}

TEST_CASE("MeetingIndex: rebuild round-trips through "
          "save_meeting_context's per-timestamp filename",
          "[meeting-index][c11]") {
    auto root = tmp_dir("rebuild-per-ts");

    fs::path d = root / "2026-05-16_14-30";
    fs::create_directories(d);
    save_meeting_context(d, "ctx", {}, "2026-05-16_14-30",
                         "12345678-1234-4567-89ab-1234567890ab");
    // Sanity — per-instance filename (not legacy).
    REQUIRE(fs::exists(d / "context_2026-05-16_14-30.json"));

    MeetingIndex idx;
    auto n = idx.rebuild_from_disk(root);
    CHECK(n == 1);
    CHECK(idx.find("12345678-1234-4567-89ab-1234567890ab") == d);

    fs::remove_all(root);
}

// --- concurrency -----------------------------------------------------------

TEST_CASE("MeetingIndex: concurrent find/bind/unbind under load is race-free",
          "[meeting-index][c11]") {
    MeetingIndex idx;

    // Pre-seed a set of valid ids the workers will hammer.
    std::vector<std::string> ids;
    for (int i = 0; i < 16; ++i) {
        char buf[37];
        std::snprintf(buf, sizeof(buf),
                      "%08x-1234-4567-89ab-1234567890ab",
                      0xabcd0000u + i);
        ids.emplace_back(buf);
        idx.bind(ids.back(), fs::path("/tmp/seed-" + std::to_string(i)));
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};

    auto reader = [&]() {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> pick(0, ids.size() - 1);
        try {
            while (!stop.load(std::memory_order_relaxed)) {
                (void)idx.find(ids[pick(rng)]);
            }
        } catch (...) {
            failed.store(true);
        }
    };
    auto writer = [&]() {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> pick(0, ids.size() - 1);
        try {
            while (!stop.load(std::memory_order_relaxed)) {
                size_t i = pick(rng);
                idx.bind(ids[i], fs::path("/tmp/w-" + std::to_string(i)));
                (void)idx.unbind(ids[i]);
                idx.bind(ids[i], fs::path("/tmp/r-" + std::to_string(i)));
            }
        } catch (...) {
            failed.store(true);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) threads.emplace_back(reader);
    for (int i = 0; i < 2; ++i) threads.emplace_back(writer);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true, std::memory_order_relaxed);
    // Per repo norm (feedback memory: verification rigor): join threads on the
    // exception path too — wrapping in try/catch on the workers above means a
    // crash sets `failed` rather than throwing across the join, but we still
    // unconditionally join to avoid std::terminate via ~thread.
    for (auto& t : threads) t.join();

    CHECK_FALSE(failed.load());
    // We don't assert on the final contents — bind/unbind are racing — but the
    // index must remain internally consistent (size() returns a number, all
    // surviving bindings resolve via find()).
    size_t n = idx.size();
    CHECK(n <= ids.size());
}
