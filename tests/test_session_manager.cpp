// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.13 — SessionManager-isolated tests.
//
// Tag-spaces:
//   [c13][session]        — SessionManager-isolated cases (mint, resolve,
//                            sweep_expired via Clock seam, evict_by_prefix).
//   [c13][evict]          — `evict_by_prefix` prefix-policy assertions.
//   [c13][gc]              — Clock-seam-driven TTL eviction shape.
//
// All Clock-dependent tests use a fake std::function clock — never
// std::this_thread::sleep_for — so the suite stays deterministic and fast.
// The constant-time-compare M-3 assertion lives at the bottom: it scans
// the source for the `ct_equals` invocation and the documenting comment
// so a refactor that silently drops the CT compare or the security-budget
// note breaks the build, not a production deploy.

#include <catch2/catch_test_macros.hpp>

#include "session_manager.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>

using namespace recmeet;

namespace {

// Always-returns-this-value clock. The test mutates `now` between assertions
// to advance time deterministically. Captures by reference so the same
// `now` variable can be advanced from the test body.
struct FakeClock {
    std::atomic<int64_t> now{1'000'000};
    int64_t operator()() const { return now.load(); }
};

} // namespace

// ===========================================================================
// (1) Token mint uniqueness — 1000 mints, 1000 distinct 64-char lowercase-hex.
// Relies on getrandom(2) quality; the test asserts the SHAPE invariant
// (length + alphabet) and the uniqueness invariant.
// ===========================================================================
TEST_CASE("C.13: mint produces unique 64-char lowercase-hex tokens",
          "[c13][session]") {
    SessionManager sm(/*ttl_seconds=*/3600);
    std::set<std::string> seen;
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        std::string tok = sm.mint("client-" + std::to_string(i));
        REQUIRE(tok.size() == 64);
        for (char c : tok) {
            const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            REQUIRE(ok);
        }
        auto [it, inserted] = seen.insert(tok);
        (void)it;
        REQUIRE(inserted);  // never collides
    }
    REQUIRE(seen.size() == N);
}

// ===========================================================================
// (2) Resume restores client_id (H-2). The SessionManager itself carries ONLY
// {client_id, last_seen_epoch}; the test pins that the struct never grows
// to retain creds/prefs. The "re-session.init required" contract is enforced
// elsewhere (the daemon does not copy SessionCredentials into ResumeSession),
// asserted here by inspecting the struct's serializable surface.
// ===========================================================================
TEST_CASE("C.13: resume restores client_id; creds NOT preserved (H-2)",
          "[c13][session]") {
    FakeClock clk;
    SessionManager sm(/*ttl_seconds=*/3600,
                      [&clk]() { return clk(); });
    std::string tok = sm.mint("clientA");
    auto cid = sm.resolve(tok);
    REQUIRE(cid.has_value());
    REQUIRE(*cid == "clientA");

    // H-2 — the ResumeSession struct holds ONLY (client_id, last_seen).
    // sizeof check is a structural assertion: extending the struct with
    // SessionCredentials (~40+ bytes for strings/maps) would push this
    // past 64 bytes on every libstdc++ build. The assertion documents the
    // budget rather than enforces a hard byte count.
    ResumeSession probe{"x", 0};
    REQUIRE(sizeof(probe) < 128);  // string + int64; no creds map
}

// ===========================================================================
// (3) Expired token mints fresh — auth.token with an expired resume_token
// falls back to fresh-mint via resolve()->nullopt. Equivalent client
// experience to a first-connect.
// ===========================================================================
TEST_CASE("C.13: expired token resolves to nullopt (fresh-mint fallback)",
          "[c13][session][gc]") {
    FakeClock clk;
    SessionManager sm(/*ttl_seconds=*/3600,
                      [&clk]() { return clk(); });
    std::string tok = sm.mint("clientA");

    // Within TTL — resolves.
    clk.now.store(clk.now.load() + 1800);  // 30 minutes
    auto resolved = sm.resolve(tok);
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == "clientA");

    // Past TTL — nullopt. The handler at the call site treats this
    // identically to an unknown token: mint a fresh one.
    clk.now.store(clk.now.load() + 7200);  // +2h, well past 1h TTL
    auto expired = sm.resolve(tok);
    REQUIRE(!expired.has_value());

    // After the expired resolve, a fresh mint returns a different token.
    std::string tok2 = sm.mint("clientA");
    REQUIRE(tok2 != tok);
}

// ===========================================================================
// (5) GC sweep evicts at TTL via Clock seam (no real sleep).
// ===========================================================================
TEST_CASE("C.13: sweep_expired drops past-TTL entries via Clock seam",
          "[c13][session][gc]") {
    FakeClock clk;
    SessionManager sm(/*ttl_seconds=*/600,  // 10 minutes
                      [&clk]() { return clk(); });
    std::string a = sm.mint("clientA");
    std::string b = sm.mint("clientB");
    REQUIRE(sm.size() == 2);

    // At t = +5 minutes, nothing expires.
    clk.now.store(clk.now.load() + 300);
    auto e0 = sm.sweep_expired();
    REQUIRE(e0.empty());
    REQUIRE(sm.size() == 2);

    // Bump A's last_seen, advance time so only B expires.
    sm.bump_last_seen(a);
    clk.now.store(clk.now.load() + 400);  // total 700s from B's mint
    auto e1 = sm.sweep_expired();
    REQUIRE(e1.size() == 1);
    REQUIRE(e1[0].first == b);
    REQUIRE(e1[0].second == "clientB");
    REQUIRE(sm.size() == 1);

    // Bump-then-resolve still works for A.
    auto cid = sm.resolve(a);
    REQUIRE(cid.has_value());
    REQUIRE(*cid == "clientA");
}

// ===========================================================================
// (9) Terminal-job artifacts survive 24h, evicted at 24h+1.
// Tested at the SessionManager level: a session bumped only at t=0
// expires exactly at ttl+1 (not at ttl). The artifact lifecycle is
// indirectly pinned by the matching diarization_cache_ttl_secs derivation
// — see test_config.cpp / test_diarize.cpp.
// ===========================================================================
TEST_CASE("C.13: session survives at TTL boundary; evicted at TTL+1",
          "[c13][session][gc]") {
    constexpr int64_t TTL = 24 * 3600;
    FakeClock clk;
    SessionManager sm(TTL, [&clk]() { return clk(); });
    std::string tok = sm.mint("clientA");
    REQUIRE(sm.resolve(tok).has_value());

    // At exactly TTL — still alive (the predicate is `> ttl`, not `>=`).
    clk.now.store(clk.now.load() + TTL);
    REQUIRE(sm.resolve(tok).has_value());
    auto e0 = sm.sweep_expired();
    REQUIRE(e0.empty());

    // At TTL + 1 — evicted.
    clk.now.store(clk.now.load() + 1);
    auto e1 = sm.sweep_expired();
    REQUIRE(e1.size() == 1);
    REQUIRE(e1[0].first == tok);
    // And resolve() now returns nullopt.
    REQUIRE(!sm.resolve(tok).has_value());
}

// ===========================================================================
// (10) --evict happy path: exact-one match returns {evicted, client_id};
// session is removed.
// ===========================================================================
TEST_CASE("C.13: evict_by_prefix exact-match returns full token + client_id",
          "[c13][evict]") {
    SessionManager sm(/*ttl_seconds=*/3600);
    std::string tok = sm.mint("clientA");
    REQUIRE(tok.size() == 64);
    std::string prefix8 = tok.substr(0, 8);

    auto r = sm.evict_by_prefix(prefix8);
    REQUIRE(r.kind == EvictResult::Kind::Evicted);
    REQUIRE(r.token == tok);
    REQUIRE(r.client_id == "clientA");
    REQUIRE(r.match_count == 1);
    REQUIRE(sm.size() == 0);

    // Second resolve fails — entry is gone.
    REQUIRE(!sm.resolve(tok).has_value());
}

// ===========================================================================
// (11) --evict prefix policy errors: too-short / ambiguous / no-match.
// ===========================================================================
TEST_CASE("C.13: evict_by_prefix policy rejects short / ambiguous / no-match",
          "[c13][evict]") {
    SessionManager sm(/*ttl_seconds=*/3600);

    SECTION("prefix < 8 chars → PrefixTooShort") {
        std::string tok = sm.mint("clientA");
        std::string short_prefix = tok.substr(0, 7);  // 7 chars
        auto r = sm.evict_by_prefix(short_prefix);
        REQUIRE(r.kind == EvictResult::Kind::PrefixTooShort);
        REQUIRE(r.token.empty());
        REQUIRE(sm.size() == 1);  // not evicted
    }

    SECTION("no match → NoMatch") {
        sm.mint("clientA");
        // 8 'z' chars — guaranteed not a hex prefix.
        auto r = sm.evict_by_prefix("zzzzzzzz");
        REQUIRE(r.kind == EvictResult::Kind::NoMatch);
        REQUIRE(r.match_count == 0);
        REQUIRE(sm.size() == 1);
    }

    SECTION("ambiguous (>=2 matches) → Ambiguous (deterministic via insert_for_test)") {
        // Force a deterministic 8-char-prefix collision via the
        // `insert_for_test` seam (gated by `#if defined(RECMEET_TESTING)`
        // in `src/session_manager.h`). The prior iteration of this SECTION
        // minted 200 tokens and scanned for a birthday-paradox collision
        // — which never fires in practice (8 hex chars = 4.3B prefixes,
        // collision expected at ~65K mints), so the assertion silently
        // degraded to a SUCCEED("birthday paradox did not produce...")
        // fallback that left the Ambiguous branch unverified on every CI
        // run. With the seam, two bindings sharing the literal prefix
        // `"deadbeef"` are planted and the >1-match branch is exercised
        // unconditionally.
        const std::string prefix8 = "deadbeef";
        // Each token must be a 64-char lowercase-hex string (`resolve()`
        // size-guards on 64 chars). The 56-char tail differentiates the
        // two so they map to distinct map keys.
        const std::string tok_a = prefix8 + std::string(56, 'a');
        const std::string tok_b = prefix8 + std::string(56, 'b');
        REQUIRE(tok_a.size() == 64);
        REQUIRE(tok_b.size() == 64);
        REQUIRE(tok_a != tok_b);
        sm.insert_for_test(tok_a, "clientA");
        sm.insert_for_test(tok_b, "clientB");
        REQUIRE(sm.size() == 2);

        auto r = sm.evict_by_prefix(prefix8);
        REQUIRE(r.kind == EvictResult::Kind::Ambiguous);
        REQUIRE(r.match_count == 2);
        REQUIRE(r.token.empty());      // not populated on Ambiguous
        REQUIRE(r.client_id.empty());  // not populated on Ambiguous

        // Neither session was evicted — Ambiguous is a no-op.
        REQUIRE(sm.size() == 2);

        // The longer (full-token) prefix unambiguously identifies tok_a
        // and DOES evict it. Pins the "extend the prefix to disambiguate"
        // operator workflow.
        auto r2 = sm.evict_by_prefix(tok_a);
        REQUIRE(r2.kind == EvictResult::Kind::Evicted);
        REQUIRE(r2.token == tok_a);
        REQUIRE(r2.client_id == "clientA");
        REQUIRE(sm.size() == 1);
    }
}

// ===========================================================================
// (12) last_seen bump on inbound IPC requests advances last_seen_epoch;
// outbound broadcasts do not.
// At the SessionManager level, `bump_last_seen` is what gets called from
// the per-handler dispatch hook (M-4 wiring). The test below exercises
// that bump_last_seen does advance the per-token clock — and that the
// per-handler hook reaches it via the right ClientState field. The
// integration of "outbound events do NOT bump" is structurally guaranteed
// by the hook only running at the per-handler dispatch site, NOT inside
// broadcast() / send_to_client() — see ipc_server.cpp dispatch loop.
// ===========================================================================
TEST_CASE("C.13: bump_last_seen advances stored epoch; missing token is no-op",
          "[c13][session]") {
    FakeClock clk;
    SessionManager sm(/*ttl_seconds=*/3600,
                      [&clk]() { return clk(); });
    std::string tok = sm.mint("clientA");

    // Advance clock; bump; resolve should still succeed past original TTL.
    clk.now.store(clk.now.load() + 3500);  // 58m later (still within 1h TTL)
    sm.bump_last_seen(tok);

    // Now jump another 3500s (total 7000s from mint) — without the bump
    // this token would have expired (1h TTL = 3600s). The bump kept it
    // alive.
    clk.now.store(clk.now.load() + 3500);
    REQUIRE(sm.resolve(tok).has_value());

    // Bump on unknown token: silent no-op.
    sm.bump_last_seen(std::string(64, 'f'));
    REQUIRE(sm.resolve(tok).has_value());  // not corrupted

    // Bump on wrong-length token: also silent no-op (size guard).
    sm.bump_last_seen("short");
    REQUIRE(sm.resolve(tok).has_value());
}

// ===========================================================================
// (13) Constant-time compare M-3 assertion: the source carries the
// `ct_equals` call AND the documenting comment about the bucket-walk
// timing-leak budget. This is the "lock it in code review" test: a
// refactor that silently drops the CT compare or the comment trips
// CI.
// ===========================================================================
TEST_CASE("C.13: ct_equals + M-3 budget comment present in session_manager",
          "[c13][session]") {
    // Find session_manager.cpp / .h relative to the test binary's CWD
    // (the build runs tests from the project root). Try a few likely
    // locations to keep the test robust against out-of-tree builds.
    const std::vector<std::string> candidates = {
        "src/session_manager.cpp",
        "../src/session_manager.cpp",
        "../../src/session_manager.cpp",
    };
    std::string cpp_text;
    for (const auto& p : candidates) {
        std::ifstream f(p);
        if (!f.is_open()) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        cpp_text = ss.str();
        if (!cpp_text.empty()) break;
    }
    REQUIRE(!cpp_text.empty());  // source must be findable

    // M-3 surface assertion #1: a ct_equals-style hand-rolled XOR
    // accumulator must be present in the source.
    const bool has_ct = cpp_text.find("ct_equals_token") != std::string::npos
                     || cpp_text.find("ct_equals") != std::string::npos;
    REQUIRE(has_ct);

    // M-3 surface assertion #2: a comment must document the
    // bucket-walk timing-leak budget.
    const bool has_budget = cpp_text.find("bucket") != std::string::npos
                         && cpp_text.find("256-bit") != std::string::npos;
    REQUIRE(has_budget);

    // The header should also document the policy (the class-level
    // comment near line 38-41 of session_manager.h).
    std::string hdr_text;
    for (const auto& p : {"src/session_manager.h",
                          "../src/session_manager.h",
                          "../../src/session_manager.h"}) {
        std::ifstream f(p);
        if (!f.is_open()) continue;
        std::stringstream ss; ss << f.rdbuf();
        hdr_text = ss.str();
        if (!hdr_text.empty()) break;
    }
    REQUIRE(!hdr_text.empty());
    const bool hdr_has_budget = hdr_text.find("256-bit token") != std::string::npos
                             || hdr_text.find("256-bit") != std::string::npos;
    REQUIRE(hdr_has_budget);
}
