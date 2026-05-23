// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include "test_tmpdir.h"

namespace {

class TmpRootCleanup : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(const Catch::TestRunStats& stats) override {
        if (std::getenv("RECMEET_TEST_KEEP")) return;
        if (stats.aborting) return;
        if (!stats.totals.assertions.allOk()) return;
        if (!stats.totals.testCases.allOk()) return;
        std::error_code ec;
        std::filesystem::remove_all(recmeet::test::test_root(), ec);
    }
};

}  // namespace

CATCH_REGISTER_LISTENER(TmpRootCleanup)
