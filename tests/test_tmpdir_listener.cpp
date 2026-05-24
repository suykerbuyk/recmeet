#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdlib>
#include <filesystem>

#include "test_tmpdir.h"

namespace {

class TmpRootCleanup : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(const Catch::TestRunStats& stats) override {
        // Keep artifacts when anything failed — they're the debugging
        // surface for the failure. Also keep when RECMEET_TEST_KEEP=1.
        if (std::getenv("RECMEET_TEST_KEEP")) return;
        if (stats.aborting) return;
        if (!stats.totals.assertions.allOk()) return;
        if (!stats.totals.testCases.allOk()) return;
        std::error_code ec;
        std::filesystem::remove_all(recmeet::test::test_root(), ec);
        // Ignore ec — best-effort cleanup; OS-level /tmp purge handles stragglers.
    }
};

}  // namespace

CATCH_REGISTER_LISTENER(TmpRootCleanup)
