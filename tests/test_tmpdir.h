#pragma once
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>

namespace recmeet::test {

// Per-process test root. Computed on first call, created on disk, reused for
// the lifetime of the process. Override via RECMEET_TEST_ROOT env var (CI,
// debugging, controlled cleanup).
//
// Default form: <fs::temp_directory_path()>/recmeet/<pid>_<epoch_ms>
//
// PID + millisecond timestamp is sufficient: collision requires same PID +
// same wall-clock millisecond, which means two concurrent processes from the
// same parent forking in the same ms — and create_directories is idempotent
// for the already-exists case (returns false, does not throw).
//
// Errors propagate: if create_directories cannot make the root (perm denied,
// no space, RECMEET_TEST_ROOT pointed at an unwriteable path), filesystem_error
// is thrown out of the magic-static initializer and the process aborts with
// a clear diagnostic. This is intentional — silently swallowing the error
// converts a single clear failure into N opaque write failures later.
inline const std::filesystem::path& test_root() {
    static const std::filesystem::path root = []() {
        std::filesystem::path r;
        if (const char* env = std::getenv("RECMEET_TEST_ROOT")) {
            r = env;
        } else {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            r = std::filesystem::temp_directory_path() / "recmeet"
                / (std::to_string(::getpid()) + "_" + std::to_string(ms));
        }
        std::filesystem::create_directories(r);  // throws on real failure
        return r;
    }();
    return root;
}

// Build a path under the test root. The stem is appended verbatim — caller
// keeps full control over creation, cleanup, and error handling. Idiom:
//
//   auto out = recmeet::test::tmp_path("my_stem");
//   fs::remove_all(out);
//   fs::create_directories(out);
//   ... write files under `out` ...
//   fs::remove_all(out);          // optional intra-process cleanup
//
// The root itself is created automatically; the stem dir is not.
inline std::filesystem::path tmp_path(std::string_view stem) {
    return test_root() / stem;
}

}  // namespace recmeet::test
