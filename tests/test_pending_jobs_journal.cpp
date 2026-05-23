// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.5 — `pending_jobs.json` journal tests.
//
// Test #2: atomic-write under simulated crash. Per agent-3's
// recommendation in the plan (we do NOT introduce a fault-injection
// hook), we introspect the on-disk state DIRECTLY: after a `save()` we
// confirm (a) `pending_jobs.json` exists with the expected bytes and
// (b) the `.tmp` companion file is GONE (the atomic-write contract
// guarantees the rename runs to completion, not "tmp leaked alongside
// the final file"). The contract also guarantees that a concurrent
// reader sees either the prior bytes or the new bytes — never a
// partial line.
//
// Test #3: schema round-trip + append + remove on the seven-field
// `Entry` payload.

#include <catch2/catch_test_macros.hpp>

#include "pending_jobs_journal.h"
#include "test_tmpdir.h"
#include "util.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

using namespace recmeet;
namespace fs = std::filesystem;

namespace {

fs::path make_scratch() {
    std::random_device rd;
    std::ostringstream oss;
    oss << "recmeet_d5_journal_" << ::getpid() << "_" << rd();
    fs::path p = recmeet::test::tmp_path(oss.str());
    std::error_code ec;
    fs::create_directories(p, ec);
    REQUIRE_FALSE(ec);
    return p;
}

struct ScopedDir {
    fs::path path;
    ~ScopedDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

PendingJobsJournal::Entry sample_entry(const std::string& job_id,
                                       const std::string& meeting_id) {
    PendingJobsJournal::Entry e;
    e.endpoint         = "127.0.0.1:8765";
    e.meeting_id       = meeting_id;
    e.job_id           = job_id;
    e.staging_wav_path = "/tmp/staging/audio_" + job_id + ".wav";
    e.kind             = "submit";
    e.slot_kind        = "postprocess";
    e.submitted_at_unix = 1747146600 + std::stoll(job_id);
    return e;
}

} // anonymous namespace

TEST_CASE("D.5: journal atomic write — final file present, .tmp gone",
          "[d5][journal][atomic]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};
    fs::path journal_path = scratch / "pending_jobs.json";

    PendingJobsJournal j(journal_path);

    std::vector<PendingJobsJournal::Entry> entries = {
        sample_entry("1001", "12345678-0001-4000-8000-000000000001"),
        sample_entry("1002", "12345678-0002-4000-8000-000000000002"),
    };
    REQUIRE_NOTHROW(j.save(entries));

    // (a) final file exists and is non-empty.
    REQUIRE(fs::exists(journal_path));
    REQUIRE(fs::file_size(journal_path) > 0);

    // (b) .tmp companion is gone — the atomic-write contract requires
    // the rename to complete, leaving no `.tmp` artifact behind for the
    // next operator/start-up to trip over.
    fs::path tmp_path = journal_path;
    tmp_path += ".tmp";
    CHECK_FALSE(fs::exists(tmp_path));

    // (c) load() returns the same payload by-value.
    auto loaded = j.load();
    REQUIRE(loaded.size() == 2);
    CHECK(loaded[0].job_id == "1001");
    CHECK(loaded[1].job_id == "1002");

    // (d) overwrite — final file must still be valid, .tmp still gone.
    entries.clear();
    entries.push_back(sample_entry("2001", "abcdef01-0001-4000-8000-aaa000000001"));
    REQUIRE_NOTHROW(j.save(entries));
    CHECK_FALSE(fs::exists(tmp_path));
    loaded = j.load();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].job_id == "2001");
}

TEST_CASE("D.5: journal schema round-trip preserves all seven fields",
          "[d5][journal]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};
    PendingJobsJournal j(scratch / "pending_jobs.json");

    auto e = sample_entry("7", "deadbeef-cafe-4000-8000-000000000007");
    e.endpoint         = "tcp://[::1]:8765 \"quoted\"\\";  // exercise escaping
    e.staging_wav_path = "/tmp/path with spaces/audio.wav";
    j.save({e});

    auto out = j.load();
    REQUIRE(out.size() == 1);
    CHECK(out[0].endpoint          == e.endpoint);
    CHECK(out[0].meeting_id        == e.meeting_id);
    CHECK(out[0].job_id            == e.job_id);
    CHECK(out[0].staging_wav_path  == e.staging_wav_path);
    CHECK(out[0].kind              == e.kind);
    CHECK(out[0].slot_kind         == e.slot_kind);
    CHECK(out[0].submitted_at_unix == e.submitted_at_unix);
}

TEST_CASE("D.5: journal append + remove_by_job_id semantics",
          "[d5][journal]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};
    PendingJobsJournal j(scratch / "pending_jobs.json");

    // Append three entries one at a time.
    j.append(sample_entry("100", "12345678-0001-4000-8000-000000000100"));
    j.append(sample_entry("200", "12345678-0002-4000-8000-000000000200"));
    j.append(sample_entry("300", "12345678-0003-4000-8000-000000000300"));

    auto v = j.load();
    REQUIRE(v.size() == 3);
    CHECK(v[0].job_id == "100");
    CHECK(v[2].job_id == "300");

    // Remove the middle.
    j.remove_by_job_id("200");
    v = j.load();
    REQUIRE(v.size() == 2);
    CHECK(v[0].job_id == "100");
    CHECK(v[1].job_id == "300");

    // Remove a non-existent id — no-op, no throw, no save.
    j.remove_by_job_id("999");
    v = j.load();
    REQUIRE(v.size() == 2);

    // Empty id is also a no-op (defensive).
    j.remove_by_job_id("");
    v = j.load();
    REQUIRE(v.size() == 2);

    // Remove the rest.
    j.remove_by_job_id("100");
    j.remove_by_job_id("300");
    v = j.load();
    CHECK(v.empty());
}

TEST_CASE("D.5: journal load on missing file returns empty",
          "[d5][journal]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};
    PendingJobsJournal j(scratch / "does_not_exist.json");
    auto v = j.load();
    CHECK(v.empty());
}
