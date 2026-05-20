// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase E.3 + E.4 — binary slimming assertions.
//
// Three tests, all tagged `[binary-slim]`:
//   1. tray_excludes_ml_deps      — `ldd recmeet-tray` must not list any of
//                                   {libonnxruntime, libsherpa-onnx,
//                                    libwhisper, libllama, libggml}, and
//                                   `nm -u` must not contain undefined
//                                   symbols prefixed Ort/whisper_/llama_/
//                                   sherpa_. Catches accidental link of
//                                   `recmeet_core` into the tray, which
//                                   would pull the full ML stack in
//                                   transitively.
//   2. daemon_excludes_pulse_gtk  — `ldd recmeet-daemon` must not list any
//                                   of {libpulse*, libgtk-3, libgdk-3}
//                                   (libgdk_pixbuf is a separate image-
//                                   decoding library, explicitly allowed),
//                                   and `nm -u` must not contain undefined
//                                   symbols prefixed pa_/gtk_/gdk_ (with
//                                   gdk_pixbuf_ allowed). Catches a
//                                   resurfaced PulseAudio capture path
//                                   (removed in Phase B) or any GTK widget-
//                                   toolkit link on the daemon side.
//   3. binaries_have_expected_core_deps — positive direction sanity check:
//                                   tray must show libgtk-3, daemon must
//                                   show libonnxruntime. Catches a
//                                   degenerate ldd / wrong-path probe that
//                                   would otherwise silently pass tests 1
//                                   and 2.
//
// Build-dir resolution order:
//   1. RECMEET_BUILD_DIR env var (set by `ctest` from CMake if needed)
//   2. RECMEET_BUILD_DIR_DEFAULT compile-time macro (CMake-injected
//      CMAKE_BINARY_DIR, absolute path to the configured build tree)
//   3. "./build" fallback (covers manual `./recmeet_tests` invocation
//      from the source root)

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#ifndef RECMEET_BUILD_DIR_DEFAULT
#define RECMEET_BUILD_DIR_DEFAULT "./build"
#endif

namespace {

std::string build_dir() {
    if (const char* env = std::getenv("RECMEET_BUILD_DIR"); env && *env) {
        return env;
    }
    return RECMEET_BUILD_DIR_DEFAULT;
}

// Run `cmd`, return combined-stdout+stderr capture. popen() with "r" on
// "cmd 2>&1" gives us the full output stream; we read until EOF to avoid
// partial captures on long ldd lists.
std::string run_capture(const std::string& cmd) {
    const std::string full = cmd + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) {
        return {};
    }
    std::string out;
    std::array<char, 4096> buf{};
    while (std::size_t n = std::fread(buf.data(), 1, buf.size(), pipe)) {
        out.append(buf.data(), n);
    }
    pclose(pipe);
    return out;
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("tray excludes ML deps", "[binary-slim][e3]") {
    const std::string tray = build_dir() + "/recmeet-tray";
    const std::string ldd_out = run_capture("ldd '" + tray + "'");
    REQUIRE_FALSE(ldd_out.empty());

    // ldd: no ML transitive deps.
    REQUIRE_FALSE(contains(ldd_out, "libonnxruntime"));
    REQUIRE_FALSE(contains(ldd_out, "libsherpa-onnx"));
    REQUIRE_FALSE(contains(ldd_out, "libwhisper"));
    REQUIRE_FALSE(contains(ldd_out, "libllama"));
    REQUIRE_FALSE(contains(ldd_out, "libggml"));

    // nm -u: no undefined ML-symbol references. Filter to symbol names
    // (last whitespace-separated field) so we don't false-positive on
    // file paths or addresses.
    const std::string nm_out = run_capture("nm -u '" + tray + "'");
    std::size_t pos = 0;
    while (pos < nm_out.size()) {
        const std::size_t eol = nm_out.find('\n', pos);
        const std::string_view line(nm_out.data() + pos,
                                    (eol == std::string::npos ? nm_out.size() : eol) - pos);
        const std::size_t sp = line.find_last_of(" \t");
        const std::string_view sym = (sp == std::string_view::npos)
                                         ? line
                                         : line.substr(sp + 1);
        if (!sym.empty()) {
            INFO("undefined symbol: " << std::string(sym));
            REQUIRE_FALSE(sym.rfind("Ort", 0) == 0);
            REQUIRE_FALSE(sym.rfind("whisper_", 0) == 0);
            REQUIRE_FALSE(sym.rfind("llama_", 0) == 0);
            REQUIRE_FALSE(sym.rfind("sherpa_", 0) == 0);
            REQUIRE_FALSE(sym.rfind("ggml_", 0) == 0);
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
}

TEST_CASE("daemon excludes pulse and gtk", "[binary-slim][e4]") {
    const std::string daemon = build_dir() + "/recmeet-daemon";
    const std::string ldd_out = run_capture("ldd '" + daemon + "'");
    REQUIRE_FALSE(ldd_out.empty());

    // No PulseAudio (removed Phase B — capture lives in the tray now).
    REQUIRE_FALSE(contains(ldd_out, "libpulse"));

    // No GTK widget toolkit. We check the precise SONAME suffixes
    // (libgtk-3 / libgdk-3) rather than the bare "libgdk" substring,
    // because libgdk_pixbuf-2.0 is a separate image-decoding library
    // legitimately pulled in by libglycin / libgio via libnotify on the
    // daemon side and is not part of GTK proper.
    REQUIRE_FALSE(contains(ldd_out, "libgtk-3"));
    REQUIRE_FALSE(contains(ldd_out, "libgtk-4"));
    REQUIRE_FALSE(contains(ldd_out, "libgdk-3"));
    REQUIRE_FALSE(contains(ldd_out, "libgdk-4"));

    // nm -u: no undefined Pulse/GTK-symbol references. Filter symbol
    // names from each line (last whitespace-separated field). Allow
    // gdk_pixbuf_* explicitly since libgdk_pixbuf is permitted above.
    const std::string nm_out = run_capture("nm -u '" + daemon + "'");
    std::size_t pos = 0;
    while (pos < nm_out.size()) {
        const std::size_t eol = nm_out.find('\n', pos);
        const std::string_view line(nm_out.data() + pos,
                                    (eol == std::string::npos ? nm_out.size() : eol) - pos);
        const std::size_t sp = line.find_last_of(" \t");
        const std::string_view sym = (sp == std::string_view::npos)
                                         ? line
                                         : line.substr(sp + 1);
        if (!sym.empty()) {
            INFO("undefined symbol: " << std::string(sym));
            REQUIRE_FALSE(sym.rfind("pa_", 0) == 0);
            REQUIRE_FALSE(sym.rfind("gtk_", 0) == 0);
            if (sym.rfind("gdk_pixbuf_", 0) != 0) {
                REQUIRE_FALSE(sym.rfind("gdk_", 0) == 0);
            }
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
}

TEST_CASE("binaries have expected core deps", "[binary-slim][sanity]") {
    // Positive-direction sanity: catches the failure mode where ldd
    // probes the wrong path, returns empty, or the binary is static —
    // in which case the negative-only assertions above would
    // pathologically pass.
    const std::string tray = build_dir() + "/recmeet-tray";
    const std::string daemon = build_dir() + "/recmeet-daemon";

    const std::string tray_ldd = run_capture("ldd '" + tray + "'");
    REQUIRE_FALSE(tray_ldd.empty());
    // Tray is a GTK 3 application — libgtk-3 must be a direct or
    // transitive dependency.
    REQUIRE(contains(tray_ldd, "libgtk-3"));

    const std::string daemon_ldd = run_capture("ldd '" + daemon + "'");
    REQUIRE_FALSE(daemon_ldd.empty());
    // Daemon performs sherpa-onnx diarization — libonnxruntime is a
    // hard dependency.
    REQUIRE(contains(daemon_ldd, "libonnxruntime"));
    // Sanity: daemon must also pull in whisper for transcription.
    REQUIRE(contains(daemon_ldd, "libwhisper"));
}

// Phase E.6.2 — positive-direction sanity that cpp-httplib actually
// linked into the tray. Pre-E.6.2 the tray spawned an external web
// subprocess for the WebUI; post-E.6.2 the tray embeds an
// httplib::Server itself (src/tray_web.cpp). A degenerate build that
// configured tray_web.cpp out of the source list but left CMakeLists.txt
// expecting the symbols would otherwise pass the negative-direction
// "tray excludes ML deps" check above (httplib has no ML deps) while
// still being broken. `nm <binary>` lists defined + undefined symbols;
// grep for any httplib:: occurrence picks up either the inlined member
// functions or the unity-TU httplib.cpp definitions.
TEST_CASE("tray has httplib symbols linked", "[binary-slim][e6][sanity]") {
    const std::string tray = build_dir() + "/recmeet-tray";
    const std::string nm_out = run_capture("nm '" + tray + "'");
    REQUIRE_FALSE(nm_out.empty());
    REQUIRE(contains(nm_out, "httplib"));
}
