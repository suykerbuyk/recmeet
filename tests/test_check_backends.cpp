// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase E.4.1 — `recmeet-daemon --check-backends` is a diagnostic CLI flag
// that prints the `ggml: active backend: <name>` banner from
// `src/backend_info.cpp` and exits 0 BEFORE any pid-lock, socket bind, or
// worker thread. These tests fork+exec the actual installed daemon binary
// at `build/recmeet-daemon`, capture combined stdout+stderr via popen, and
// assert on banner shape so an operator can rely on the output for
// scripted GPU-vs-CPU detection on a deployed host.
//
// Tag: `[e4-1]`.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <regex>
#include <string>

namespace fs = std::filesystem;

namespace {

// Resolve the daemon binary next to the test runner. CTest's working dir
// is the CMake build directory, so `./recmeet-daemon` works; but be
// defensive and use the absolute path so `ctest --test-dir <other>`
// invocations don't break.
std::string daemon_binary_path() {
    // The test binary lives at <build>/recmeet_tests, the daemon at
    // <build>/recmeet-daemon. /proc/self/exe walks us there regardless of
    // CWD.
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec || self.empty()) return "./recmeet-daemon";
    return (self.parent_path() / "recmeet-daemon").string();
}

// Run `<daemon> --check-backends`, return combined stdout+stderr plus the
// child exit code parsed from the trailing `EXIT:<n>` sentinel. Uses popen
// so we don't have to hand-roll fork+exec+pipe plumbing for a single
// well-defined command. `2>&1` is essential — the daemon dups stdout over
// stderr in the --check-backends path, but a healthy implementation lands
// banner text on stdout exclusively. The redirect ensures we still see
// any pre-dup2 stderr noise (e.g. a missed init failure) instead of
// silently losing it.
struct CheckBackendsRun {
    int  exit_code = -1;
    std::string output;
};

CheckBackendsRun run_check_backends() {
    CheckBackendsRun out;
    const std::string cmd =
        daemon_binary_path() + " --check-backends 2>&1; echo EXIT:$?";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);

    char buf[1024];
    while (::fgets(buf, sizeof(buf), pipe)) {
        out.output += buf;
    }
    // pclose returns shell exit; the actual daemon exit code is in the
    // EXIT: sentinel we appended so the test layer is robust to popen
    // shell-level surprises.
    ::pclose(pipe);

    auto pos = out.output.rfind("EXIT:");
    REQUIRE(pos != std::string::npos);
    out.exit_code = std::atoi(out.output.c_str() + pos + 5);
    // Trim the sentinel from the captured text so format assertions
    // operate on the banner alone.
    out.output.erase(pos);
    // popen leaves a stray newline before EXIT — strip trailing whitespace
    // for tidy regex matching.
    while (!out.output.empty() &&
           (out.output.back() == '\n' || out.output.back() == '\r')) {
        // Keep one trailing \n so the "ends with newline" assertion is
        // meaningful, but drop any extras.
        if (out.output.size() >= 2 &&
            (out.output[out.output.size() - 2] == '\n' ||
             out.output[out.output.size() - 2] == '\r')) {
            out.output.pop_back();
        } else {
            break;
        }
    }
    return out;
}

} // namespace

TEST_CASE("check_backends_exits_zero_and_emits_banner", "[e4-1]") {
    auto run = run_check_backends();

    INFO("daemon output:\n" << run.output);
    REQUIRE(run.exit_code == 0);

    // The banner line shape is `ggml: active backend: <token> ...`. The
    // regex is intentionally permissive on suffix (CPU shows thread count;
    // GPU paths show description + memory) but rigid on the
    // "active backend:" anchor token a downstream script would grep.
    std::regex banner_re(R"(active backend:\s*\S+)");
    REQUIRE(std::regex_search(run.output, banner_re));
}

TEST_CASE("check_backends_output_format", "[e4-1]") {
    auto run = run_check_backends();
    INFO("daemon output:\n" << run.output);
    REQUIRE(run.exit_code == 0);

    // Bound the output: registry + active line + optional warn ≈ < 1 KB
    // realistically. 2 KB ceiling keeps the contract loose enough for
    // verbose GPU descriptions while still flagging runaway noise.
    REQUIRE(run.output.size() <= 2048);

    // Banner is line-terminated — operators piping to head/grep should
    // see a clean final newline.
    REQUIRE_FALSE(run.output.empty());
    REQUIRE(run.output.back() == '\n');

    // Extract the active-backend token and assert it's one of the names
    // the ggml registry actually exposes. backend_info.cpp falls through
    // to the registry name for the GPU/IGPU path, so the set of legal
    // tokens is the union of `CPU` (always shown that way for the CPU
    // fallback) and the optional non-CPU registry names.
    std::smatch m;
    std::regex token_re(R"(active backend:\s*([A-Za-z0-9_]+))");
    REQUIRE(std::regex_search(run.output, m, token_re));
    const std::string token = m[1].str();
    INFO("active backend token: " << token);

    const bool known =
        token == "CPU"    ||
        token == "Vulkan" ||
        token == "HIP"    ||
        token == "ROCm"   ||  // some ggml builds expose ROCm as the HIP reg name
        token == "CUDA"   ||
        token == "Metal"  ||
        token == "GPU"    ||  // backend_info defaults to "GPU" if reg_name lookup fails
        token == "SYCL"   ||  // tolerated for completeness with upstream ggml backends
        token == "OpenCL" ||
        token == "Kompute";
    REQUIRE(known);
}
