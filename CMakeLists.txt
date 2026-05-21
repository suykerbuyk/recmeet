cmake_minimum_required(VERSION 3.28)

# Single source of truth for version is `git describe --tags`. The literal
# below is a sentinel used only when .git/ is absent (source tarball builds).
# Cutting a release is `git tag X.Y.Z && git push --tags` — no file edits.
find_package(Git QUIET)
set(_RECMEET_VERSION_FALLBACK "0.0.0")
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --match "[0-9]*" --abbrev=0
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _RECMEET_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _GIT_RESULT
    )
    if(_GIT_RESULT EQUAL 0 AND _RECMEET_TAG MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)$")
        set(_RECMEET_VERSION_FALLBACK "${_RECMEET_TAG}")
    endif()
endif()

project(recmeet VERSION ${_RECMEET_VERSION_FALLBACK} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Reduce binary size: emit each function/data item in its own section
# so the linker can discard unreferenced symbols.
add_compile_options(-ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections)

# Set install RPATH so installed binaries find vendored shared libs (e.g.
# libonnxruntime.so) relative to themselves: <prefix>/bin/../lib.
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

# Options
option(RECMEET_BUILD_TRAY  "Build system tray applet" ON)
option(RECMEET_USE_LLAMA   "Enable local summarization via llama.cpp" ON)
option(RECMEET_USE_SHERPA  "Enable speaker diarization via sherpa-onnx" ON)
option(RECMEET_USE_NOTIFY "Enable desktop notifications via libnotify" ON)
option(RECMEET_BUILD_TESTS "Build unit tests" ON)
# Phase 1 — C++ source coverage. When ON, injects `--coverage -O0 -g` into
# compile + link flags for first-party targets ONLY (recmeet_ipc,
# recmeet_capture, recmeet_core, recmeet_live_capture, recmeet, recmeet-daemon,
# recmeet-tray, recmeet_tests). Vendor subbuilds (whisper.cpp, llama.cpp,
# sherpa-onnx, onnxruntime, cpp-httplib, catch2) are NOT instrumented — the
# flags are applied via target_compile_options / target_link_options on each
# first-party target, never globally. See `make cxx-coverage` for the
# Makefile entry point and `tests/coverage-baseline.txt` for the gate.
option(RECMEET_COVERAGE    "Instrument first-party C++ targets with --coverage (gcov)" OFF)

# Version header — regenerated at build time from git describe
# (find_package(Git QUIET) was already called pre-project; GIT_EXECUTABLE is set.)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
add_custom_target(recmeet_version ALL
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -DVERSION_IN=${CMAKE_SOURCE_DIR}/src/version.h.in
        -DVERSION_OUT=${CMAKE_BINARY_DIR}/generated/version.h
        -DFALLBACK_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/version.h
    COMMENT "Updating version.h"
)

# Prerequisite checks — distro detection, submodules, helpful error messages
include(cmake/prerequisites.cmake)
recmeet_check_submodules()
recmeet_check_pkg_config()
find_package(Threads REQUIRED)
recmeet_pkg_check(PIPEWIRE      libpipewire-0.3)
recmeet_pkg_check(PULSE         libpulse)
recmeet_pkg_check(PULSE_SIMPLE  libpulse-simple)
recmeet_pkg_check(SNDFILE       sndfile)
recmeet_pkg_check(CURL          libcurl)
# Phase D.5 — libuuid (util-linux) for the canonical UUID v4 mint used by
# `recmeet::new_uuid_v4()` (the per-recording `meeting_id` minted at
# `start_capture`). Available on every supported distro as
# `util-linux`/`libuuid1`/`uuid` packages.
recmeet_pkg_check(UUID          uuid)
if(RECMEET_USE_NOTIFY)
    recmeet_pkg_check(NOTIFY    libnotify)
endif()

if(RECMEET_BUILD_TRAY)
    recmeet_pkg_check(GTK3          gtk+-3.0)
    recmeet_pkg_check(APPINDICATOR  ayatana-appindicator3-0.1)
endif()

# Vendor libraries use cmake_minimum_required as low as 3.1. Suppress the CMake 4.x
# deprecation warnings ("Compatibility with CMake < 3.10 will be removed") since we
# can't fix third-party CMakeLists.txt files (kaldi-native-fbank, kaldifst, openfst,
# kissfft all override CMAKE_POLICY_VERSION_MINIMUM in their own scopes).
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.10 CACHE STRING "" FORCE)

# CMP0177 (CMake 3.31+) normalizes install() DESTINATION paths. The ggml
# subbuild passes our GGML_BACKEND_DIR ($ORIGIN/../lib) as a DESTINATION;
# with the policy unset, CMake literally creates a $ORIGIN/ subdirectory
# under the install prefix and then traverses back out via .., leaving
# install detritus. NEW behavior normalizes the value to plain "lib",
# placing files where they belong with no side effects. The string baked
# into libggml-base.so via target_compile_definitions stays untouched —
# only install DESTINATIONs are normalized — so the runtime dynamic-loader
# expansion of $ORIGIN continues to work as designed.
# CMAKE_POLICY_DEFAULT_CMPxxxx (rather than cmake_policy(SET ...)) is
# required so the policy survives vendor cmake_minimum_required(VERSION 3.10)
# resets at the add_subdirectory() boundary.
set(CMAKE_POLICY_DEFAULT_CMP0177 NEW)

# GGML compute backends shared by whisper.cpp and llama.cpp. The
# recmeet-vulkan module handles AUTO/ON/OFF tri-state + per-distro install
# hints + decision-matrix messaging; see cmake/recmeet-vulkan.cmake. Must
# run AFTER prerequisites.cmake so RECMEET_DISTRO is populated.
include(cmake/recmeet-vulkan.cmake)

# Vendor subbuilds (whisper.cpp + llama.cpp) ship as shared libs so the
# ggml plugin model (GGML_BACKEND_DL) can drop libggml-*.so files alongside
# the binary at runtime. GGML_BACKEND_DIR is pinned to $ORIGIN/../lib —
# symmetric with the CMAKE_INSTALL_RPATH established for libonnxruntime.so
# — so ggml's plugin search never falls through to the CWD fallback (which
# would be an attacker-plantable location). Pinning is MANDATORY for
# security; see agentctx/tasks/runtime-loadable-gpu-backends.md.
set(GGML_BACKEND_DL ON CACHE BOOL "" FORCE)
set(GGML_BACKEND_DIR "$ORIGIN/../lib" CACHE STRING "" FORCE)
# GGML_BACKEND_DL=ON is incompatible with GGML_NATIVE (the default), which
# would emit -march=native into the single libggml-cpu.so plugin and break
# the "build once per platform, run everywhere" goal. Use CPU_ALL_VARIANTS
# so ggml emits per-ISA plugins (sse4.2 / avx / avx2 / avx512 / amx on x86)
# and the runtime ggml_backend_score() picks the highest-scoring variant
# the host CPU supports.
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_CPU_ALL_VARIANTS ON CACHE BOOL "" FORCE)

# Vendor: whisper.cpp
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/whisper.cpp EXCLUDE_FROM_ALL)

# Vendor: llama.cpp
if(RECMEET_USE_LLAMA)
    # llama.cpp bundles its own ggml; same BUILD_SHARED_LIBS=ON requirement
    # under GGML_BACKEND_DL=ON applies (vendor/whisper.cpp/ggml/src/CMakeLists.txt:188).
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    add_subdirectory(vendor/llama.cpp EXCLUDE_FROM_ALL)
endif()

# Vendor: sherpa-onnx (speaker diarization)
if(RECMEET_USE_SHERPA)
    recmeet_check_onnxruntime()
    include(FetchContent)

    # T1B: disable the onnxruntime CPU memory arena. The arena pools
    # allocations across inference calls and does not release across session
    # boundaries; on a full transcribe+diarize+identify pipeline this manifests
    # as a 17x RSS jump at the diarize->identify handover (T1A field
    # measurements, 2026-04-30). DisableCpuMemArena() is session-scope and the
    # only public knob; sherpa-onnx's session factory does not expose it, so
    # we patch its default. Toggle off for A/B benchmarking; requires
    # `make clean-deps` between toggles because FetchContent caches populate.
    option(RECMEET_PATCH_SHERPA_ARENA
           "Disable onnxruntime CPU mem arena via sherpa-onnx patch" ON)
    set(_sherpa_patch_args "")
    if(RECMEET_PATCH_SHERPA_ARENA)
        # Idempotent patch application. The FetchContent populate sub-build
        # re-runs PATCH_COMMAND every time `cmake -B build` is invoked (its
        # stamp files are wiped on each configure). Plain `patch -N` exits 1
        # on already-applied patches, which FetchContent treats as failure.
        # Use `git apply --reverse --check` to detect already-applied state
        # (the populate dir is a git checkout); apply only when the reverse
        # check fails. Real apply failures (genuine conflicts) propagate.
        set(_arena_patch ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sherpa-onnx-patches/0001-disable-cpu-arena.patch)
        set(_sherpa_patch_args
            PATCH_COMMAND sh -c "git apply --reverse --check ${_arena_patch} 2>/dev/null || git apply ${_arena_patch}"
        )
    endif()

    FetchContent_Declare(sherpa-onnx
        GIT_REPOSITORY https://github.com/k2-fsa/sherpa-onnx.git
        GIT_TAG v1.12.27
        EXCLUDE_FROM_ALL
        ${_sherpa_patch_args}
    )
    set(SHERPA_ONNX_ENABLE_TTS OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_WEBSOCKET OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_BINARY OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # Use system onnxruntime if available (e.g. Arch: pacman -S onnxruntime-cpu).
    # The pre-built static libonnxruntime.a was compiled with GCC 11.2.1 (Red Hat),
    # which has an ABI-incompatible std::regex with GCC 12+. Using the system
    # package (built with the host GCC) avoids the SIGABRT in ParseSemVerVersion().
    set(SHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE ON CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_LINK_LIBSTDCPP_STATICALLY OFF CACHE BOOL "" FORCE)
    # Suppress dev warnings from sherpa-onnx's cmake files:
    # CMP0169: FetchContent_Populate() deprecated in CMake 3.30 (used by onnxruntime, kaldi-* downloads)
    # CMP0177: install() DESTINATION "./" normalization in CMake 3.31 (sherpa-onnx.pc install)
    set(CMAKE_POLICY_DEFAULT_CMP0169 OLD)
    set(CMAKE_POLICY_DEFAULT_CMP0177 OLD)
    FetchContent_MakeAvailable(sherpa-onnx)
endif()

# Re-enable deprecation warnings for our own code
set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)

# IPC library (config, IPC, utilities — no ML, no PulseAudio dependencies).
# Phase B.1: device_enum moved out of this target into recmeet_capture so
# the daemon's IPC surface stays unencumbered by audio-stack deps.
set(IPC_SOURCES
    src/json_util.cpp
    src/api_models.cpp
    src/util.cpp
    src/log.cpp
    src/config.cpp
    src/notify.cpp
    src/http_client.cpp
    src/ipc_protocol.cpp
    src/config_json.cpp
    src/ipc_client.cpp
    src/ipc_server.cpp
    src/session_merge.cpp
    src/caption_format.cpp
    # Phase C.4 — artifact enumeration for process.fetch. Pure
    # std::filesystem; lives in recmeet_ipc so the IPC layer can resolve
    # the artifact list at request time without dragging in recmeet_core.
    src/fetch_artifacts.cpp
    # Phase D.5 — persistence-across-tray-restart surfaces. UUID v4 mint
    # (libuuid), the per-server resume_token store, and the in-flight
    # pending_jobs journal. All three are pure-stdlib (+libuuid) and
    # consumed by tray.cpp via recmeet_ipc, so they live here so the
    # daemon (which also links recmeet_ipc) does not need to drag the
    # tray-side state into its address space — none of the D.5 classes
    # are referenced by daemon TUs; -Wl,--gc-sections drops them.
    src/uuid.cpp
    src/resume_token_store.cpp
    src/pending_jobs_journal.cpp
    # Phase D.3 — pure helpers for the tray reconnect path: jittered
    # exponential backoff + post-reconnect job.list re-sync classifier.
    # Carved out of tray.cpp so the formulas can be unit-tested without
    # booting GTK. Lives in recmeet_ipc because the classifier consumes
    # the IPC `job.list` wire shape and the backoff helpers do not pull
    # any new deps.
    src/reconnect_backoff.cpp
    # Phase D.4 — pure renderer helpers for the tray menu (per-slot row,
    # per-server row, reconnect countdown line, caption-inline truncation).
    # Carved out of tray.cpp so the [d4] tests can drive the formatters
    # without booting GTK; lives in recmeet_ipc because it touches no new
    # deps beyond <string>.
    src/tray_status.cpp
    # Phase D.6 — pure disk-budget eviction sweep over ~/.local/share/recmeet/
    # staging/. Scan + plan + apply, with sidecar/journal protection.
    # Carved out of tray.cpp so the eviction policy is unit-tested
    # without booting GTK. Lives in recmeet_ipc (one layer below
    # recmeet_capture which owns tray_capture::is_sidecar_protected);
    # the sidecar check is inlined as a single fs::exists call in
    # staging_sweep.cpp to keep the dep chain unidirectional.
    src/staging_sweep.cpp
)

add_library(recmeet_ipc STATIC ${IPC_SOURCES})
add_dependencies(recmeet_ipc recmeet_version)
target_include_directories(recmeet_ipc PUBLIC
    src
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(recmeet_ipc PUBLIC
    Threads::Threads
    PkgConfig::CURL
    PkgConfig::UUID
)

if(RECMEET_USE_NOTIFY)
    target_link_libraries(recmeet_ipc PUBLIC PkgConfig::NOTIFY)
    target_compile_definitions(recmeet_ipc PUBLIC RECMEET_USE_NOTIFY=1)
endif()

# Phase B.1 — audio capture + device enumeration library. Holds the
# PipeWire / PulseAudio surface so the daemon (thin-client server) stays
# PulseAudio-free while the tray (and CLI) own the local capture lifecycle.
# Phase C.9: `audio_monitor.cpp` (PulseMonitorCapture — the pa_simple
# fallback for `.monitor` sources) joined this lib because the only
# remaining consumer is the live-recording path (`run_recording` in
# `src/live_recording.cpp`), which now lives in `recmeet_live_capture`.
set(CAPTURE_SOURCES
    src/audio_capture.cpp
    src/audio_monitor.cpp
    src/device_enum.cpp
    src/tray_capture.cpp
)
add_library(recmeet_capture STATIC ${CAPTURE_SOURCES})
add_dependencies(recmeet_capture recmeet_version)
target_link_libraries(recmeet_capture PUBLIC
    recmeet_ipc
    PkgConfig::PIPEWIRE
    PkgConfig::PULSE
    PkgConfig::PULSE_SIMPLE
    PkgConfig::SNDFILE
)

# Core library (ML pipeline + postprocess). Phase C.9: the live-recording
# half of pipeline.cpp moved to `src/live_recording.cpp` (in
# `recmeet_live_capture`), so `recmeet_core` no longer pulls in PipeWire /
# PulseAudio. The daemon links only `recmeet_core` and stays capture-free.
set(CORE_SOURCES
    src/audio_file.cpp
    src/audio_mixer.cpp
    src/job_queue.cpp
    src/model_manager.cpp
    src/transcribe.cpp
    src/summarize.cpp
    src/note.cpp
    src/pipeline.cpp
    src/cli.cpp
    src/diarize.cpp
    src/speaker_id.cpp
    src/vad.cpp
    src/caption_vtt.cpp
    src/backend_info.cpp
)

add_library(recmeet_core STATIC ${CORE_SOURCES})
add_dependencies(recmeet_core recmeet_version)
target_link_libraries(recmeet_core PUBLIC
    recmeet_ipc
    PkgConfig::SNDFILE
    whisper
)

if(RECMEET_USE_LLAMA)
    target_link_libraries(recmeet_core PUBLIC llama)
    target_compile_definitions(recmeet_core PUBLIC RECMEET_USE_LLAMA=1)
endif()

if(RECMEET_USE_SHERPA)
    target_link_libraries(recmeet_core PUBLIC sherpa-onnx-c-api)
    target_compile_definitions(recmeet_core PUBLIC RECMEET_USE_SHERPA=1)
endif()

# Phase 2 — streaming caption engine. Compiled in both build flavors:
#  - With RECMEET_USE_SHERPA=ON the .cpp pulls in <sherpa-onnx/c-api/c-api.h>
#    and builds the real recognizer-backed engine.
#  - With RECMEET_USE_SHERPA=OFF the same .cpp compiles to stub bodies; the
#    only side effect of start() is to set last_error() to
#    "captions require RECMEET_USE_SHERPA=ON build" and return false. The
#    header is single-source-of-truth so callers compile uniformly.
target_sources(recmeet_core PRIVATE src/caption_engine.cpp)

# Phase C.10a — server-side streaming-caption session. Owns the
# stream_token -> session map, the per-session disk-backed temp WAV, and
# the migrated CaptionEngine producer (network 0x03 frames feed the SPSC
# ring in place of the old PipeWire-Capture callback). Lives in recmeet_core
# because it composes caption_engine + job_queue + the cli.cpp English-only
# guard; the daemon links it transitively.
target_sources(recmeet_core PRIVATE src/streaming_session.cpp)

# Phase C.2 — server-side upload session for process.submit. Owns the
# upload_token -> session map, the per-job staging directory + audio file
# (libsndfile for raw PCM, std::ofstream for container formats), and the
# JobQueue postprocess reservation. Same composition story as C.10a — lives
# in recmeet_core; the daemon links it transitively.
target_sources(recmeet_core PRIVATE src/upload_session.cpp)

# Phase C.11 — in-memory meeting_id -> meeting_dir_path index. Owned by the
# daemon, populated at startup via rebuild_from_disk(meetings_root), consulted
# by the upload finalize path (C.11.4) to dedupe known meeting_ids onto their
# existing directory. Lives in recmeet_core because rebuild_from_disk reaches
# into pipeline.cpp's load_meeting_id() (context.json reader).
target_sources(recmeet_core PRIVATE src/meeting_index.cpp)

# Phase E.6.1 — server-side meeting discovery helper (discover_meetings).
# Pure std::filesystem; no ML deps. Colocated with the meetings.list IPC
# verb in daemon.cpp rather than folded into meeting_index.cpp (which is a
# strict meeting_id → path map — different concern). Lives in recmeet_core
# alongside meeting_index.cpp; the daemon links it transitively.
target_sources(recmeet_core PRIVATE src/meetings_browse.cpp)

# Phase C.8 — server-resident per-job diarization cache. Pure stdlib; no
# sherpa dependency (consumes already-extracted centroids). Lives in
# recmeet_core so the daemon and pipeline.cpp's writer share the file
# format constants. Header-only API into the daemon's enroll.finalize.
target_sources(recmeet_core PRIVATE src/diarization_cache.cpp)

# Phase C.13 — server-side resume_token store + GC sweep + --evict primitive.
# Pure stdlib + getrandom(2); no ML or transport dependency. Lives in
# recmeet_core alongside meeting_index.cpp and diarization_cache.cpp (the
# other daemon-side stateful registries).
target_sources(recmeet_core PRIVATE src/session_manager.cpp)

# Phase C.9 — live-recording library. Owns `run_recording()` /
# `run_pipeline()` and the CaptionEngine-with-capture wrapper, all of which
# the CLI standalone mode (`recmeet` without `--daemon`) still needs. The
# daemon-side path was retired in C.9 (process.submit + process.stream
# replace record.start), so the daemon does NOT link this lib — keeping
# `recmeet-daemon` PipeWire/Pulse/recmeet_capture-free.
add_library(recmeet_live_capture STATIC
    src/live_recording.cpp
    src/reprocess_batch.cpp  # CLI-only batch driver — calls run_pipeline()
)
add_dependencies(recmeet_live_capture recmeet_version)
target_link_libraries(recmeet_live_capture PUBLIC
    recmeet_core
    recmeet_capture
)

# CLI binary — links the live-capture lib so the standalone (no-daemon)
# path can still record audio locally.
add_executable(recmeet src/main.cpp)
target_link_libraries(recmeet PRIVATE recmeet_core recmeet_live_capture)

# Daemon binary — links recmeet_core only. After C.9 it must NOT pull in
# PipeWire / PulseAudio / recmeet_capture. Verify with
# `ldd build/recmeet-daemon | grep -E 'pipewire|pulse'` — expect no hits.
add_executable(recmeet-daemon src/daemon.cpp)
target_link_libraries(recmeet-daemon PRIVATE recmeet_core)

# Tray binary
# Phase B.1+B.2: tray owns the local audio capture lifecycle, so it links
# recmeet_capture for PipeWireCapture + list_sources(). recmeet_ipc still
# carries the daemon IPC client surface; SNDFILE comes via recmeet_ipc
# transitively is not the case (SNDFILE belongs to recmeet_core), so the
# tray's WAV stager needs SNDFILE linked directly here.
#
# Phase E.6.2: tray gains an embedded HTTP listener (tray_web.cpp +
# vendored cpp-httplib unity TU) so the WebUI ships as a tray subsystem
# instead of a supervised subprocess. Four static assets (share/web/) are
# baked into the binary via cmake/embed_assets.cmake -> generated/web_assets.h.
# The asset generator is wired as an OUTPUT dependency on a custom command
# so editing any share/web/ file rebuilds the header automatically.
if(RECMEET_BUILD_TRAY)
    set(_RECMEET_WEB_ASSETS_HDR
        ${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets.h)
    set(_RECMEET_WEB_ASSETS_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/share/web/index.html
        ${CMAKE_CURRENT_SOURCE_DIR}/share/web/app.js
        ${CMAKE_CURRENT_SOURCE_DIR}/share/web/style.css
        ${CMAKE_CURRENT_SOURCE_DIR}/share/web/favicon.svg)

    add_custom_command(
        OUTPUT  ${_RECMEET_WEB_ASSETS_HDR}
        DEPENDS ${_RECMEET_WEB_ASSETS_SRC}
                ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake
        COMMAND ${CMAKE_COMMAND}
            "-DASSETS=${_RECMEET_WEB_ASSETS_SRC}"
            "-DNAMES=index_html;app_js;style_css;favicon_svg"
            "-DCONTENT_TYPES=text/html\\;charset=utf-8;application/javascript;text/css;image/svg+xml"
            "-DOUTPUT=${_RECMEET_WEB_ASSETS_HDR}"
            "-DNAMESPACE=recmeet::web_assets"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake
        COMMENT "Generating embedded WebUI asset bundle (web_assets.h)"
        VERBATIM)

    add_executable(recmeet-tray
        src/tray.cpp
        src/tray_web.cpp
        vendor/cpp-httplib/httplib.cpp
        ${_RECMEET_WEB_ASSETS_HDR})
    target_include_directories(recmeet-tray PRIVATE
        vendor/cpp-httplib
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(recmeet-tray PRIVATE
        recmeet_ipc
        recmeet_capture
        PkgConfig::SNDFILE
        PkgConfig::GTK3
        PkgConfig::APPINDICATOR
    )
    target_compile_definitions(recmeet-tray PRIVATE
        RECMEET_ICON_DIR="${CMAKE_INSTALL_FULL_DATADIR}/icons"
    )
endif()

# Go tools — recmeet-mcp (Model Context Protocol server) and recmeet-agent
# (AI meeting prep + follow-up CLI). Built via `go build` driven by an
# add_custom_command so the binaries land in ${CMAKE_BINARY_DIR} alongside
# the C++ outputs and participate in `cmake --install` / CPack on equal
# footing. Dep tracking uses CONFIGURE_DEPENDS over tools/**/*.go +
# go.mod/go.sum — adding a new .go file requires a `cmake -B build` reconfigure,
# same convention as adding a new C++ source.
option(RECMEET_BUILD_GO_TOOLS
    "Build and install recmeet-mcp + recmeet-agent (requires Go toolchain)" ON)

if(RECMEET_BUILD_GO_TOOLS)
    find_program(GO_EXECUTABLE go)
    if(NOT GO_EXECUTABLE)
        message(FATAL_ERROR
            "recmeet: Go toolchain required for recmeet-mcp + recmeet-agent.\n"
            "  Install: pacman -S go | apt install golang-go | dnf install golang\n"
            "  Or skip with: -DRECMEET_BUILD_GO_TOOLS=OFF")
    endif()

    file(GLOB_RECURSE _RECMEET_GO_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/tools/*.go)
    list(APPEND _RECMEET_GO_SOURCES
        ${CMAKE_SOURCE_DIR}/tools/go.mod
        ${CMAKE_SOURCE_DIR}/tools/go.sum)

    set(_RECMEET_MCP_BIN   ${CMAKE_BINARY_DIR}/recmeet-mcp)
    set(_RECMEET_AGENT_BIN ${CMAKE_BINARY_DIR}/recmeet-agent)

    add_custom_command(
        OUTPUT ${_RECMEET_MCP_BIN}
        COMMAND ${GO_EXECUTABLE} build -o ${_RECMEET_MCP_BIN} ./cmd/recmeet-mcp
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools
        DEPENDS ${_RECMEET_GO_SOURCES}
        COMMENT "Building recmeet-mcp (Go)"
        VERBATIM)

    add_custom_command(
        OUTPUT ${_RECMEET_AGENT_BIN}
        COMMAND ${GO_EXECUTABLE} build -o ${_RECMEET_AGENT_BIN} ./cmd/recmeet-agent
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools
        DEPENDS ${_RECMEET_GO_SOURCES}
        COMMENT "Building recmeet-agent (Go)"
        VERBATIM)

    add_custom_target(recmeet_go_tools ALL
        DEPENDS ${_RECMEET_MCP_BIN} ${_RECMEET_AGENT_BIN})
endif()

# Tests
if(RECMEET_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.0
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(CTest)
    include(Catch)

    add_executable(recmeet_tests
        tests/test_audio_mixer.cpp
        tests/test_audio_file.cpp
        tests/test_note.cpp
        tests/test_log.cpp
        tests/test_config.cpp
        tests/test_util.cpp
        tests/test_summarize_json.cpp
        tests/test_device_enum.cpp
        tests/test_transcribe.cpp
        tests/test_http_client.cpp
        tests/test_model_manager.cpp
        tests/test_summarize_prompt.cpp
        tests/test_pipeline_helpers.cpp
        tests/test_cli.cpp
        tests/test_reprocess_batch.cpp
        tests/test_benchmark.cpp
        tests/test_diarize.cpp
        tests/test_speaker_id.cpp
        tests/test_vad.cpp
        tests/test_ipc_protocol.cpp
        tests/test_config_json.cpp
        tests/test_ipc_server.cpp
        tests/test_ipc_client.cpp
        tests/test_ipc_integration.cpp
        tests/test_session_handshake.cpp
        tests/test_ndjson_parse.cpp
        tests/test_full_stack.cpp
        tests/test_full_stack_captions.cpp
        tests/test_integration_pipeline.cpp
        tests/streaming_capture_tests.cpp
        tests/streaming_engine_tests.cpp
        tests/test_streaming_session.cpp
        tests/test_stream_commit.cpp
        tests/test_upload_session.cpp
        tests/test_caption_ipc_integration.cpp
        tests/test_caption_model_manager.cpp
        tests/test_caption_format.cpp
        tests/test_caption_render.cpp
        tests/test_caption_vtt.cpp
        tests/test_backend_dl.cpp
        tests/test_recmeet_capture.cpp
        # tests/test_ipc_reprocess_b5.cpp — DELETED in Phase C.9. The file
        # exercised the B.5 transitional glue (tray staging → record.start
        # --reprocess); record.start is gone in C.9, so the entire test
        # surface has no v2 analogue. The v2 equivalent (process.submit
        # upload → JobQueue → pp_worker) is covered by test_upload_session
        # + test_job_queue + test_fetch.
        tests/test_tray_capture.cpp
        tests/test_job_queue.cpp
        tests/test_routed_events.cpp
        tests/test_fetch.cpp
        tests/test_cancel.cpp
        tests/test_job_query.cpp
        tests/test_job_progress.cpp
        tests/test_enroll.cpp
        tests/test_meeting_index.cpp
        tests/test_reprocess.cpp
        tests/test_v2_thin_client_e2e.cpp
        tests/test_live_recording.cpp
        # Phase C.13 substantive behavior tests (iter-162).
        tests/test_session_manager.cpp
        tests/test_session_gc.cpp
        tests/test_session_ipc.cpp
        # Phase D.5 persistence-across-tray-restart tests (iter-164).
        tests/test_sidecar_v2.cpp
        tests/test_pending_jobs_journal.cpp
        tests/test_resume_token_store.cpp
        tests/test_tray_resume_recovery.cpp
        # Phase D.1 + D.2 per-slot queue + drain worker (iter-167).
        tests/test_slot_queue.cpp
        # Phase D.3 — reconnect-with-backoff + resume_token resync (iter-168).
        tests/test_reconnect_backoff.cpp
        # Phase D.4 — tray status renderer (per-slot rows, per-server row,
        # jitter-aware reconnect countdown, caption-inline) (iter-169).
        tests/test_tray_status.cpp
        # Phase D.6 — disk-budget staging eviction sweep.
        tests/test_staging_sweep.cpp
        # Phase E.3 + E.4 — binary slimming assertions
        # (tray excludes ML deps; daemon excludes pulse/gtk).
        tests/test_binary_slim.cpp
        # Phase E.4.1 — `recmeet-daemon --check-backends` diagnostic CLI flag.
        tests/test_check_backends.cpp
        # Phase E.1-fix — regression tests for the 5 C.9-legacy refs migrated
        # to v2 (process.submit / local StopToken on SIGINT) + the unused
        # `ClientState::pending_close` field removal.
        tests/test_e1_legacy_fix.cpp
        # Phase E.2 Wave 2.1 — additive Config fields (summary_style,
        # caption_latency_ms, servers list) landed BEFORE the file split.
        tests/test_config_e2_fields.cpp
        # Phase E.2 Wave 2.2a — split config (ServerConfig + ClientConfig
        # + legacy migration). Monolithic Config preserved alongside.
        tests/test_config_split.cpp
        # Phase E.2 Wave 2.2b — Phase 4 verification suite (T1–T6) closing
        # out the three-type JobConfig model: precedence, submit/stream
        # round-trip, servers list, grep guards, moved-field migration.
        tests/test_w22b_phase4.cpp
        # Phase E.6.1 — IPC round-trip tests for the 8 new speakers.* /
        # meetings.* verbs that back the tray-bundled WebUI.
        tests/test_speakers_meetings_ipc.cpp
        # Phase E.6.2 — tray_web HTTP→IPC translator tests. Exercises the
        # production translator against an in-process DaemonSim that
        # registers the 11 new + 3 existing speakers/meetings verbs +
        # process.reprocess. Embedded cpp-httplib unity TU is linked here.
        tests/test_tray_web.cpp
        # Phase E.6.3 — `recmeet-tray --listen-now / --headless` flag
        # parser tests. Spawns the real recmeet-tray binary to exercise
        # the pre-gtk_init flag pre-parse in main().
        tests/test_tray_cli.cpp
        src/tray_web.cpp
        vendor/cpp-httplib/httplib.cpp
        ${_RECMEET_WEB_ASSETS_HDR}
    )
    target_include_directories(recmeet_tests PRIVATE
        vendor/cpp-httplib
        ${CMAKE_CURRENT_BINARY_DIR}/generated)
    # The test binary exercises both daemon-side surfaces (recmeet_core) and
    # client-side / standalone surfaces (run_recording + recmeet_capture), so
    # it must link the full stack — same as the CLI binary.
    target_link_libraries(recmeet_tests PRIVATE
        recmeet_core
        recmeet_live_capture
        recmeet_capture
        Catch2::Catch2WithMain
    )

    # Phase C.13 — RECMEET_TESTING gates test-only seams (e.g.
    # `SessionManager::insert_for_test`) so production translation units
    # cannot reach them. Applied to BOTH `recmeet_core` (so the seam method
    # body is emitted in the library and the test link resolves) AND
    # `recmeet_tests` (so the test TUs see the gated declaration in
    # `session_manager.h`). The macro is only set when RECMEET_BUILD_TESTS
    # is ON; a tests-off production build never sees the symbol — and even
    # when tests ARE built, `recmeet-daemon` (which also links
    # `recmeet_core`) drops the unused seam under `-Wl,--gc-sections`.
    target_compile_definitions(recmeet_core  PRIVATE RECMEET_TESTING)
    target_compile_definitions(recmeet_tests PRIVATE RECMEET_TESTING)
    # Phase E.3 + E.4 — `test_binary_slim.cpp` needs an absolute build-
    # tree path to invoke `ldd build/recmeet-tray` regardless of where
    # the test binary is launched from (catch_discover_tests sets
    # WORKING_DIRECTORY to the binary dir, but `./build` is a relative
    # path the user might also invoke from). Env override
    # RECMEET_BUILD_DIR wins if set.
    target_compile_definitions(recmeet_tests PRIVATE
        RECMEET_BUILD_DIR_DEFAULT="${CMAKE_BINARY_DIR}")
    catch_discover_tests(recmeet_tests)
endif()

# --- Phase 1: C++ source coverage instrumentation (first-party only) ---
# When RECMEET_COVERAGE=ON, inject `--coverage -O0 -g` into compile + link
# flags on each first-party target. Applied per-target (NEVER globally) so
# vendor subbuilds (whisper.cpp, llama.cpp, sherpa-onnx, onnxruntime,
# cpp-httplib, catch2) are not instrumented — vendor source has no
# coverage policy and instrumenting it would pollute the report with
# tens of thousands of lines of third-party code we have no authority to
# change. `gcovr` then scopes the report to `src/` + `tests/` via
# --exclude vendor/ --exclude build/. See `make cxx-coverage`.
if(RECMEET_COVERAGE)
    message(STATUS "RECMEET_COVERAGE=ON — instrumenting first-party C++ targets")
    set(_RECMEET_COVERAGE_FLAGS --coverage -O0 -g)
    set(_RECMEET_COVERAGE_TARGETS
        recmeet_ipc
        recmeet_capture
        recmeet_core
        recmeet_live_capture
        recmeet
        recmeet-daemon
    )
    if(RECMEET_BUILD_TRAY)
        list(APPEND _RECMEET_COVERAGE_TARGETS recmeet-tray)
    endif()
    if(RECMEET_BUILD_TESTS)
        list(APPEND _RECMEET_COVERAGE_TARGETS recmeet_tests)
    endif()
    foreach(_tgt ${_RECMEET_COVERAGE_TARGETS})
        target_compile_options(${_tgt} PRIVATE ${_RECMEET_COVERAGE_FLAGS})
        target_link_options(${_tgt}    PRIVATE ${_RECMEET_COVERAGE_FLAGS})
    endforeach()
endif()

# --- Install rules ---
# Strip binaries on install (cmake --install); CPack already strips via CPACK_STRIP_FILES.
install(CODE "set(CMAKE_INSTALL_DO_STRIP ON)")
include(GNUInstallDirs)

install(TARGETS recmeet RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS recmeet-daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Install vendored whisper.cpp + llama.cpp shared libraries. The ggml
# backend MODULE plugins (libggml-base.so, libggml-cpu.so, libggml-vulkan.so,
# ...) self-install via ggml_add_backend_library() into GGML_BACKEND_DIR
# ($ORIGIN/../lib → ${CMAKE_INSTALL_LIBDIR}), so they are NOT listed here —
# adding them would duplicate or conflict with the subbuild's own install
# rules. Only the top-level linked targets (whisper, ggml, llama) need
# explicit rules.
# Vendor whisper.cpp/ggml/llama.cpp set PUBLIC_HEADER on these targets for
# their own SDK-style install rules. recmeet ships as a binary distro, not
# a developer SDK — we don't want whisper.h / ggml*.h / llama.h leaking
# into the system include path. Clear the property before our LIBRARY
# install so CMake doesn't warn about orphaned PUBLIC_HEADER file sets.
set_target_properties(whisper ggml PROPERTIES PUBLIC_HEADER "")
if(RECMEET_USE_LLAMA)
    set_target_properties(llama PROPERTIES PUBLIC_HEADER "")
endif()

# Top-level shared libs that recmeet links against directly.
install(TARGETS whisper ggml ggml-base LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(RECMEET_USE_LLAMA)
    install(TARGETS llama LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# GGML backend MODULE plugins. With add_subdirectory(... EXCLUDE_FROM_ALL),
# vendor's own install(TARGETS ${backend} LIBRARY DESTINATION ${GGML_BACKEND_DIR})
# rules inside ggml_add_backend_library() are suppressed — install rules in
# EXCLUDE_FROM_ALL subdirectories don't fire. We re-issue explicit install
# rules at the top level so CMake performs proper RPATH fix-up on each
# plugin's $ORIGIN/../lib runpath. ggml exports the full backend-target
# list in the INTERNAL cache variable GGML_AVAILABLE_BACKENDS (populated
# inside ggml_add_backend_library() at src/CMakeLists.txt:282-291); we
# iterate it to stay portable across host architectures (x86 variants
# differ from arm64 variants, ROCm/CUDA add their own, etc.).
if(GGML_AVAILABLE_BACKENDS)
    install(TARGETS ${GGML_AVAILABLE_BACKENDS} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# Install vendored onnxruntime shared libraries (built by scripts/build-onnxruntime.sh).
# The binaries link against these at build time; RPATH ($ORIGIN/../lib) finds them at runtime.
set(_ORT_LOCAL_LIB "${CMAKE_SOURCE_DIR}/vendor/onnxruntime-local/lib")
if(IS_DIRECTORY "${_ORT_LOCAL_LIB}")
    file(GLOB _ORT_LIBS "${_ORT_LOCAL_LIB}/libonnxruntime*.so*")
    install(FILES ${_ORT_LIBS} DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# systemd user service for daemon
set(RECMEET_DAEMON_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/recmeet-daemon")
configure_file(dist/recmeet-daemon.service.in
    ${CMAKE_BINARY_DIR}/recmeet-daemon.service @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/recmeet-daemon.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/systemd/user)
install(FILES dist/recmeet-daemon.socket
    DESTINATION ${CMAKE_INSTALL_DATADIR}/systemd/user)

if(RECMEET_BUILD_TRAY)
    install(TARGETS recmeet-tray RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES dist/recmeet-tray.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(DIRECTORY share/icons/hicolor
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)

    # systemd user service (prefix-aware ExecStart path)
    set(RECMEET_TRAY_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/recmeet-tray")
    configure_file(dist/recmeet-tray.service.in
        ${CMAKE_BINARY_DIR}/recmeet-tray.service @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/recmeet-tray.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/systemd/user)
endif()

if(RECMEET_BUILD_GO_TOOLS)
    install(PROGRAMS ${_RECMEET_MCP_BIN} ${_RECMEET_AGENT_BIN}
        DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(FILES LICENSE AUTHORS
    DESTINATION ${CMAKE_INSTALL_DOCDIR})

# --- CPack DEB packaging ---
set(CPACK_PACKAGE_NAME "recmeet")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "recmeet contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Record, transcribe, and summarize meetings locally")
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Shared library deps are auto-resolved by SHLIBDEPS from ELF binaries.
# Only hardcode non-library runtime deps that dpkg-shlibdeps cannot detect.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "pipewire")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_STRIP_FILES ON)

# --- CPack RPM packaging ---
set(CPACK_RPM_PACKAGE_LICENSE "MIT and Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
set(CPACK_RPM_PACKAGE_URL "https://github.com/suykerbuyk/recmeet")
set(CPACK_RPM_PACKAGE_DESCRIPTION
    "Record, transcribe, and summarize meetings locally using whisper.cpp")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_PACKAGE_AUTOPROV ON)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_REQUIRES "pipewire")

include(CPack)
