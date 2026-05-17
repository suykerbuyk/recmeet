// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

// Phase C.13 — server-side resume_token store + GC sweep + --evict primitive.
//
// V2 introduces a server-issued "resume_token" as the first per-client
// persistent credential. The PSK gates connection; the resume_token gates
// *re-association* with prior server-side state (`client_id`, owned jobs)
// across a TCP reconnect. See docs/V2-STRATEGY.md "Session identity and
// lifecycle" for the full rationale and the iter-161 review addendum of
// agentctx/tasks/thin-client-recording-server.md for the locked design.
//
// What this class owns:
//   - The `resume_token → ResumeSession` map (in-memory only, MC-1).
//   - Token minting via `getrandom(2)` (256 bits of entropy, hex-encoded).
//   - TTL-driven eviction (`sweep_expired`) called by the daemon's GC
//     sweep thread on a configurable cadence.
//   - `--evict` primitive (`evict_by_prefix`) callable from the
//     `admin.evict` IPC verb.
//
// What this class deliberately does NOT own:
//   - Credentials / preferences (H-2): on disconnect those are dropped from
//     ClientState; on resume the client MUST re-send `session.init`. The
//     security budget is tighter (no 24 h api_key retention) at the cost of
//     one extra wire round-trip on every resume.
//   - The `client_id → fd` reverse map: that lives on IpcServer and tracks
//     LIVE connections. ResumeSession outlives connections; the two maps
//     are queried separately by the resume path.
//   - On-disk persistence: by design. Daemon restart invalidates all tokens.
//
// Logging discipline (L-1): callers MUST emit at most the first 8 hex chars
// of any resume_token (the `--evict`-eligible prefix); never the full token.
// Helpers below expose only the prefix for log lines.
//
// Constant-time-compare discipline (M-3): per-entry compare uses `ct_equals`
// (precedent: `src/ipc_server.cpp:33-44`). The `std::unordered_map` bucket
// walk leaks per-bucket comparison timing; this is accepted as below the
// practical-attack threshold for 256-bit tokens. Real CT lookup would
// require O(n) scan of the entire map on every resolve.

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace recmeet {

/// Per-session state retained across reconnect. Intentionally minimal
/// (H-2): no creds, no prefs, no owned-jobs duplication. `client_id` is
/// the routing key the resume path restores onto the new connection;
/// `last_seen_epoch` is the TTL clock, bumped on every inbound IPC
/// request from this session (M-4 — stamp lives at the per-handler
/// dispatch site in `src/ipc_server.cpp`).
struct ResumeSession {
    std::string client_id;
    int64_t     last_seen_epoch = 0;
};

/// Result of `evict_by_prefix`. The full token is returned (NOT just the
/// prefix) so the caller's response payload can disambiguate. Empty when
/// no match or > 1 match — caller distinguishes via `match_count`.
struct EvictResult {
    enum class Kind {
        Evicted,        // exact-one match — `token` + `client_id` populated
        NoMatch,        // count == 0
        Ambiguous,      // count > 1
        PrefixTooShort, // < 8 hex chars
    };
    Kind        kind = Kind::NoMatch;
    std::string token;        // full token of the evicted session (Evicted only)
    std::string client_id;    // owner of the evicted session (Evicted only)
    std::size_t match_count = 0;
};

class SessionManager {
public:
    /// Clock seam (L-3). Defaults to wall-clock seconds; tests inject a
    /// fake clock for deterministic TTL assertions without `sleep_for`.
    /// Pattern matched on `DiarizationCache` at `src/diarization_cache.h:91-93`.
    using Clock = std::function<int64_t()>;

    /// `ttl_seconds` is the session-binding TTL since `last_seen_epoch` —
    /// the consolidated knob (M-1) `[server] retain_terminal_hours * 3600`.
    /// Default Clock returns `std::chrono::system_clock::now()` in epoch s.
    explicit SessionManager(int64_t ttl_seconds, Clock clock = {});

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    /// Mint a fresh 64-char lowercase-hex resume_token from `getrandom(2)`
    /// and bind it to `client_id` with `last_seen_epoch = clock()`. Returns
    /// the new token. Never collides in practice (256-bit entropy from the
    /// kernel CSPRNG); on the astronomically improbable collision the
    /// existing binding is replaced and a warning is logged.
    std::string mint(const std::string& client_id);

    /// Look up `token` and return the bound `client_id` if present and not
    /// expired. Returns `std::nullopt` for unknown OR expired tokens so the
    /// caller (auth.token handler) treats both as the fresh-mint fallback
    /// path. Does NOT bump `last_seen_epoch` — the resolve is the moment
    /// of reconnect, and `bump_last_seen` is called separately by the
    /// per-handler dispatch (M-4). Per-entry compare uses `ct_equals`;
    /// hash-bucket walk timing leak is accepted (M-3).
    std::optional<std::string> resolve(const std::string& token) const;

    /// M-4 — bump `last_seen_epoch` for `token` to `clock()`. Called once
    /// per inbound IPC request from the per-handler dispatch site in
    /// `src/ipc_server.cpp:777`. Silent no-op on unknown token. Outbound
    /// server-emitted events (progress.job, caption, broadcast) do NOT bump.
    void bump_last_seen(const std::string& token);

    /// H-4 — evict by `prefix`. Returns one of `Kind::PrefixTooShort` (< 8
    /// hex chars), `Kind::NoMatch`, `Kind::Ambiguous`, or `Kind::Evicted`.
    /// The `admin.evict` IPC verb maps these to `InvalidParams` rejects
    /// (first three) or a success response (Evicted). The caller should
    /// separately ask the JobQueue / streaming manager / upload manager to
    /// fail the evicted session's owned jobs (the eviction primitive itself
    /// only drops the resume_token binding here — the orphan-job teardown
    /// path is shared with the GC sweep below).
    EvictResult evict_by_prefix(const std::string& prefix);

    /// Walk the map and drop every entry whose `(clock() - last_seen_epoch)
    /// > ttl_seconds`. Returns the list of evicted `(token, client_id)`
    /// pairs so the caller can run the orphan-job teardown on each.
    /// Intended to be called periodically by the GC sweep thread; safe to
    /// call from any thread (mutex-guarded).
    std::vector<std::pair<std::string, std::string>> sweep_expired();

    /// Number of bindings currently in the map. Testing surface.
    std::size_t size() const;

    /// L-1 — return the first 8 hex chars of `token` for log lines.
    /// Static helper so any caller (handler, sweep, log_warn) can emit
    /// the loggable form without remembering the rule.
    static std::string log_prefix(const std::string& token);

private:
    /// C.13 default Clock — `std::chrono::system_clock` epoch seconds.
    static int64_t default_clock();

    mutable std::mutex                              mu_;
    std::unordered_map<std::string, ResumeSession>  sessions_;
    int64_t                                         ttl_seconds_;
    Clock                                           clock_;
};

} // namespace recmeet
