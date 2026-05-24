// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Catch2 progress listener with tag-aware heartbeat + announce.
//
// Operator's primary invocation pattern is `./build/recmeet_tests "[full-stack]"`,
// which would otherwise sit silent for 30+ minutes — exactly the silence this
// listener exists to close. Short-tag suites (unit tests) stay quiet by default;
// long-tagged tests (see kLongRunningTags) auto-enable per-test announce +
// heartbeat regardless of env. Env vars (RECMEET_TEST_ANNOUNCE,
// RECMEET_TEST_HEARTBEAT) act as a global override.

#include <catch2/catch_test_case_info.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "util.h"  // recmeet::read_self_rss_kb — already exists, returns 0 on failure

namespace {

// Tags that auto-enable the heartbeat + per-test announce, regardless of env.
// Operators adding new long-running tags should update this list.
constexpr std::string_view kLongRunningTags[] = {
    "[benchmark]", "[full-stack]", "[slow]", "[stress]", "[verylong]",
};

bool case_is_long_running(const Catch::TestCaseInfo& info) {
    const std::string tags = info.tagsAsString();
    for (auto t : kLongRunningTags) {
        if (tags.find(t) != std::string::npos) return true;
    }
    return false;
}

bool env_truthy(const char* name) {
    const char* s = std::getenv(name);
    return s && *s && std::string{s} != "0";
}

std::chrono::seconds default_interval() {
    if (const char* s = std::getenv("RECMEET_TEST_HEARTBEAT_SECS"); s && *s) {
        int v = std::atoi(s);
        if (v > 0 && v < 3600) return std::chrono::seconds{v};
    }
    return std::chrono::seconds{30};
}

class ProgressListener : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(const Catch::TestCaseInfo& info) override {
        // Reset per-case state BEFORE std::thread construction in start_heartbeat.
        // Thread construction is a C++ memory-model synchronization point —
        // writes here happen-before reads in the heartbeat thread, so no
        // explicit acquire/release is needed for current_name_ / case_start_.
        current_name_ = info.name;
        case_start_   = std::chrono::steady_clock::now();
        const bool long_running = case_is_long_running(info);
        announce_this_case_  = announce_global_  || long_running;
        heartbeat_this_case_ = heartbeat_global_ || long_running;

        if (announce_this_case_) {
            std::string line = "[test] starting \"" + info.name +
                               "\" tags=" + info.tagsAsString() + "\n";
            std::cerr << line;
        }
        if (heartbeat_this_case_) start_heartbeat();
    }

    void testCaseEnded(const Catch::TestCaseStats& stats) override {
        stop_heartbeat();
        if (!announce_this_case_) return;
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - case_start_)
                        .count();
        std::string line = std::string{"[test] "} +
                           (stats.totals.assertions.allOk() ? "passed" : "FAILED") +
                           " in " + std::to_string(secs) + "s — \"" +
                           current_name_ + "\"\n";
        std::cerr << line;
    }

  private:
    void start_heartbeat() {
        stop_flag_.store(false);
        // std::thread construction is a C++ memory-model synchronization point.
        // Writes to case_start_ / current_name_ in testCaseStarting
        // happen-before reads in the lambda below; no explicit
        // acquire/release needed.
        heartbeat_ = std::thread([this] {
            auto next = case_start_ + interval_;
            while (!stop_flag_.load()) {
                std::unique_lock lk(cv_mu_);
                if (cv_.wait_until(lk, next,
                                   [this] { return stop_flag_.load(); })) {
                    return;
                }
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::steady_clock::now() - case_start_)
                                   .count();
                const long rss_kb = recmeet::read_self_rss_kb();  // 0 on failure
                // Pre-format into one line so emission is one syscall —
                // concurrent PhaseEcho writes from the test thread cannot
                // interleave mid-line.
                std::string rss_field =
                    (rss_kb > 0) ? "rss=" + std::to_string(rss_kb / 1024) + " MiB"
                                 : "rss=?";
                std::string line = "[heartbeat] T+" + std::to_string(elapsed) +
                                   "s " + rss_field + " current=\"" +
                                   current_name_ + "\"\n";
                std::cerr << line;
                next += interval_;
            }
        });
    }

    void stop_heartbeat() {
        if (!heartbeat_.joinable()) return;
        {
            std::lock_guard lk(cv_mu_);
            stop_flag_.store(true);
        }
        cv_.notify_all();
        heartbeat_.join();
    }

    std::chrono::steady_clock::time_point case_start_{};
    std::string current_name_{};
    std::chrono::seconds interval_{default_interval()};
    bool announce_global_{env_truthy("RECMEET_TEST_ANNOUNCE")};
    bool heartbeat_global_{env_truthy("RECMEET_TEST_HEARTBEAT")};
    bool announce_this_case_{false};
    bool heartbeat_this_case_{false};
    std::atomic<bool> stop_flag_{false};
    std::mutex cv_mu_{};
    std::condition_variable cv_{};
    std::thread heartbeat_{};
};

}  // namespace

CATCH_REGISTER_LISTENER(ProgressListener)
