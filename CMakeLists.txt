cmake_minimum_required(VERSION 3.20)
project(recmeet VERSION 0.1.0 LANGUAGES C CXX)

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

# Options
option(RECMEET_BUILD_TRAY  "Build system tray applet" ON)
option(RECMEET_USE_LLAMA   "Enable local summarization via llama.cpp" ON)
option(RECMEET_USE_SHERPA  "Enable speaker diarization via sherpa-onnx" ON)
option(RECMEET_USE_NOTIFY "Enable desktop notifications via libnotify" ON)
option(RECMEET_BUILD_TESTS "Build unit tests" ON)

# Version header — regenerated at build time from git describe
find_package(Git QUIET)
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

# Vendor: whisper.cpp
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/whisper.cpp EXCLUDE_FROM_ALL)

# Vendor: llama.cpp
if(RECMEET_USE_LLAMA)
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
    FetchContent_Declare(sherpa-onnx
        GIT_REPOSITORY https://github.com/suykerbuyk/sherpa-onnx.git
        GIT_TAG fix/gcc15-deprecation-warnings
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

# Core library (everything except main.cpp and tray.cpp)
set(CORE_SOURCES
    src/util.cpp
    src/log.cpp
    src/config.cpp
    src/notify.cpp
    src/device_enum.cpp
    src/audio_capture.cpp
    src/audio_monitor.cpp
    src/audio_file.cpp
    src/audio_mixer.cpp
    src/model_manager.cpp
    src/transcribe.cpp
    src/http_client.cpp
    src/summarize.cpp
    src/obsidian.cpp
    src/pipeline.cpp
    src/cli.cpp
    src/diarize.cpp
)

add_library(recmeet_core STATIC ${CORE_SOURCES})
add_dependencies(recmeet_core recmeet_version)
target_include_directories(recmeet_core PUBLIC
    src
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(recmeet_core PUBLIC
    Threads::Threads
    PkgConfig::PIPEWIRE
    PkgConfig::PULSE
    PkgConfig::PULSE_SIMPLE
    PkgConfig::SNDFILE
    PkgConfig::CURL
    whisper
)

if(RECMEET_USE_NOTIFY)
    target_link_libraries(recmeet_core PUBLIC PkgConfig::NOTIFY)
    target_compile_definitions(recmeet_core PUBLIC RECMEET_USE_NOTIFY=1)
endif()

if(RECMEET_USE_LLAMA)
    target_link_libraries(recmeet_core PUBLIC llama)
    target_compile_definitions(recmeet_core PUBLIC RECMEET_USE_LLAMA=1)
endif()

if(RECMEET_USE_SHERPA)
    target_link_libraries(recmeet_core PUBLIC sherpa-onnx-c-api)
    target_compile_definitions(recmeet_core PUBLIC RECMEET_USE_SHERPA=1)
endif()

# CLI binary
add_executable(recmeet src/main.cpp)
target_link_libraries(recmeet PRIVATE recmeet_core)

# Tray binary
if(RECMEET_BUILD_TRAY)
    add_executable(recmeet-tray src/tray.cpp)
    target_link_libraries(recmeet-tray PRIVATE
        recmeet_core
        PkgConfig::GTK3
        PkgConfig::APPINDICATOR
    )
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
        tests/test_obsidian.cpp
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
        tests/test_benchmark.cpp
        tests/test_diarize.cpp
    )
    target_link_libraries(recmeet_tests PRIVATE recmeet_core Catch2::Catch2WithMain)
    catch_discover_tests(recmeet_tests)
endif()

# --- Install rules ---
# Strip binaries on install (cmake --install); CPack already strips via CPACK_STRIP_FILES.
install(CODE "set(CMAKE_INSTALL_DO_STRIP ON)")
include(GNUInstallDirs)

install(TARGETS recmeet RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(RECMEET_BUILD_TRAY)
    install(TARGETS recmeet-tray RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES dist/recmeet-tray.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

    # systemd user service (prefix-aware ExecStart path)
    set(RECMEET_TRAY_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/recmeet-tray")
    configure_file(dist/recmeet-tray.service.in
        ${CMAKE_BINARY_DIR}/recmeet-tray.service @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/recmeet-tray.service
        DESTINATION lib/systemd/user)
endif()

install(FILES LICENSE-MIT LICENSE-APACHE AUTHORS
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
