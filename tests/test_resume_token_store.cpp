// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.5 — `session.tokens.json` resume_token store tests.
//
// Test #4 in the D.5 plan: atomic-write, mode 0600 secrecy enforcement,
// multi-server key isolation. The class is plural-from-day-one (per
// multi-server hook #1) so the file format is
// `{"<server_address>": "<token>", ...}` not a bare token string — this
// test pins that contract.

#include <catch2/catch_test_macros.hpp>

#include "resume_token_store.h"
#include "util.h"

#include <sys/stat.h>
#include <unistd.h>

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
    oss << "/tmp/recmeet_d5_tokstore_" << ::getpid() << "_" << rd();
    fs::path p = oss.str();
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

mode_t file_mode_bits(const fs::path& p) {
    struct stat st;
    if (::stat(p.string().c_str(), &st) != 0) return 0;
    return st.st_mode & 07777;
}

} // anonymous namespace

TEST_CASE("D.5: resume_token store atomic write yields final file mode 0600",
          "[d5][token-store]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};
    fs::path path = scratch / "session.tokens.json";

    ResumeTokenStore store(path);
    store.put("127.0.0.1:8765", "tok-aaaaaaaa");
    REQUIRE(fs::exists(path));

    // The atomic-write contract guarantees mode 0600 on the final file
    // (the ResumeTokenStore explicitly passes mode=0600 to
    // atomic_write_file — see implementation).
    CHECK(file_mode_bits(path) == 0600);

    // `.tmp` companion must not be present.
    fs::path tmp = path;
    tmp += ".tmp";
    CHECK_FALSE(fs::exists(tmp));
}

TEST_CASE("D.5: resume_token store keyed by server_address (multi-server)",
          "[d5][token-store]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};

    ResumeTokenStore store(scratch / "session.tokens.json");
    store.put("alpha.example:8000", "tok-alpha-111");
    store.put("beta.example:9000",  "tok-beta-222");

    auto a = store.get("alpha.example:8000");
    auto b = store.get("beta.example:9000");
    auto c = store.get("gamma.example:1000");

    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a == "tok-alpha-111");
    CHECK(*b == "tok-beta-222");
    CHECK_FALSE(c.has_value());

    // Update one, leave the other untouched.
    store.put("alpha.example:8000", "tok-alpha-rotated");
    auto a2 = store.get("alpha.example:8000");
    REQUIRE(a2.has_value());
    CHECK(*a2 == "tok-alpha-rotated");
    auto b2 = store.get("beta.example:9000");
    REQUIRE(b2.has_value());
    CHECK(*b2 == "tok-beta-222");

    // Erase one.
    store.erase("alpha.example:8000");
    auto a3 = store.get("alpha.example:8000");
    CHECK_FALSE(a3.has_value());
    auto b3 = store.get("beta.example:9000");
    REQUIRE(b3.has_value());
    CHECK(*b3 == "tok-beta-222");
}

TEST_CASE("D.5: resume_token store survives process restart (reload from disk)",
          "[d5][token-store]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};
    fs::path path = scratch / "session.tokens.json";

    {
        ResumeTokenStore writer(path);
        writer.put("127.0.0.1:8765", "tok-persisted");
    }

    // Fresh store reads the same file.
    ResumeTokenStore reader(path);
    auto v = reader.get("127.0.0.1:8765");
    REQUIRE(v.has_value());
    CHECK(*v == "tok-persisted");
}

TEST_CASE("D.5: resume_token store file format is JSON object keyed by addr",
          "[d5][token-store]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};
    fs::path path = scratch / "session.tokens.json";

    ResumeTokenStore store(path);
    store.put("a:1", "tok-a");
    store.put("b:2", "tok-b");

    std::ifstream in(path);
    std::ostringstream ss; ss << in.rdbuf();
    std::string body = ss.str();
    // Plural-from-day-one shape contract: file MUST be a JSON object
    // (starts with `{`), NOT a bare string.
    REQUIRE(!body.empty());
    CHECK(body.front() == '{');
    CHECK(body.find("\"a:1\"") != std::string::npos);
    CHECK(body.find("\"tok-a\"") != std::string::npos);
    CHECK(body.find("\"b:2\"") != std::string::npos);
    CHECK(body.find("\"tok-b\"") != std::string::npos);
}

TEST_CASE("D.5: resume_token store handles missing file cleanly",
          "[d5][token-store]") {
    auto scratch = make_scratch();
    ScopedDir guard{scratch};
    ResumeTokenStore store(scratch / "missing.json");
    auto v = store.get("any-addr");
    CHECK_FALSE(v.has_value());
}
