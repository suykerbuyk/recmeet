// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "session_manager.h"

#include "log.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include <sys/random.h>  // getrandom(2)

namespace recmeet {

namespace {

/// Per-entry constant-time compare (M-3). Hand-rolled XOR-accumulator with
/// no early-exit; folds length difference into the accumulator. Mirrors
/// `ct_equals` at `src/ipc_server.cpp:33-44` so the precedent and the
/// resume_token compare share the same primitive. Used only on tokens that
/// already passed length validation; the length-fold is belt-and-braces.
bool ct_equals_token(const std::string& a, const std::string& b) {
    const std::size_t n = std::max(a.size(), b.size());
    unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

/// 256 bits of kernel entropy → 64-char lowercase-hex string. Drops back
/// to a tight retry loop on EINTR (getrandom is interruptible by signals
/// when reading > 256 bytes; with a 32-byte buffer this should be rare
/// but we handle it for completeness). Throws std::runtime_error on a
/// hard failure (CSPRNG should never refuse 32 bytes — if it does, the
/// caller is better off failing the connection than silently weakening).
std::string random_hex_32() {
    unsigned char buf[32];
    std::size_t got = 0;
    while (got < sizeof(buf)) {
        ssize_t r = ::getrandom(buf + got, sizeof(buf) - got, 0);
        if (r > 0) {
            got += static_cast<std::size_t>(r);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        throw std::runtime_error(std::string("getrandom(2) failed: ") +
                                 std::strerror(errno));
    }
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (std::size_t i = 0; i < 32; ++i) {
        out[2 * i]     = hex[(buf[i] >> 4) & 0x0f];
        out[2 * i + 1] = hex[ buf[i]       & 0x0f];
    }
    return out;
}

} // namespace

int64_t SessionManager::default_clock() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch())
        .count();
}

SessionManager::SessionManager(int64_t ttl_seconds, Clock clock)
    : ttl_seconds_(ttl_seconds),
      clock_(clock ? std::move(clock) : Clock(&SessionManager::default_clock)) {}

std::string SessionManager::mint(const std::string& client_id) {
    std::string token = random_hex_32();
    std::lock_guard<std::mutex> lock(mu_);
    auto [it, inserted] = sessions_.emplace(
        token, ResumeSession{client_id, clock_()});
    if (!inserted) {
        // Astronomically improbable on a 256-bit token from getrandom — but
        // if it happens, replacing the prior binding is safer than throwing
        // (the prior client would have to re-`session.init` anyway on its
        // next resume; the new client gets a working token).
        log_warn("SessionManager: token collision on mint (prefix=%s) — "
                 "replacing prior binding",
                 log_prefix(token).c_str());
        it->second = ResumeSession{client_id, clock_()};
    }
    return token;
}

std::optional<std::string>
SessionManager::resolve(const std::string& token) const {
    if (token.size() != 64) return std::nullopt;  // wrong shape, no need to ct-compare
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return std::nullopt;
    // Hash table found a bucket; do an explicit ct_equals on the key to
    // pin the per-entry timing invariant (the bucket walk's leakage is the
    // accepted M-3 gap, not the leaf compare's).
    if (!ct_equals_token(it->first, token)) return std::nullopt;
    // TTL check — expired entries return nullopt so the caller falls into
    // the fresh-mint path. The actual eviction happens in `sweep_expired`
    // (lazy-on-lookup is a fallback for the case where the sweep hasn't
    // run yet).
    if (ttl_seconds_ > 0 &&
        clock_() - it->second.last_seen_epoch > ttl_seconds_) {
        return std::nullopt;
    }
    return it->second.client_id;
}

void SessionManager::bump_last_seen(const std::string& token) {
    if (token.size() != 64) return;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return;
    it->second.last_seen_epoch = clock_();
}

EvictResult SessionManager::evict_by_prefix(const std::string& prefix) {
    EvictResult r;
    if (prefix.size() < 8) {
        r.kind = EvictResult::Kind::PrefixTooShort;
        return r;
    }
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> matches;
    for (const auto& [tok, _] : sessions_) {
        if (tok.size() >= prefix.size() &&
            std::equal(prefix.begin(), prefix.end(), tok.begin())) {
            matches.push_back(tok);
            if (matches.size() > 1) break;  // ambiguous — short-circuit
        }
    }
    r.match_count = matches.size();
    if (matches.empty()) {
        r.kind = EvictResult::Kind::NoMatch;
        return r;
    }
    if (matches.size() > 1) {
        r.kind = EvictResult::Kind::Ambiguous;
        return r;
    }
    const std::string& victim = matches.front();
    auto it = sessions_.find(victim);
    r.kind      = EvictResult::Kind::Evicted;
    r.token     = victim;
    r.client_id = it->second.client_id;
    sessions_.erase(it);
    log_info("SessionManager: evicted session prefix=%s client=%s",
             log_prefix(victim).c_str(), r.client_id.c_str());
    return r;
}

std::vector<std::pair<std::string, std::string>>
SessionManager::sweep_expired() {
    std::vector<std::pair<std::string, std::string>> evicted;
    if (ttl_seconds_ <= 0) return evicted;  // 0 = never expire (tests / dev)
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t now = clock_();
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (now - it->second.last_seen_epoch > ttl_seconds_) {
            evicted.emplace_back(it->first, it->second.client_id);
            log_info("SessionManager: TTL-evicted session prefix=%s "
                     "client=%s age=%ld",
                     log_prefix(it->first).c_str(),
                     it->second.client_id.c_str(),
                     (long)(now - it->second.last_seen_epoch));
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
    return evicted;
}

std::size_t SessionManager::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return sessions_.size();
}

std::string SessionManager::log_prefix(const std::string& token) {
    if (token.size() < 8) return token;  // malformed input — emit as-is
    return token.substr(0, 8);
}

} // namespace recmeet
