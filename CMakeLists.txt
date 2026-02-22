cmake_minimum_required(VERSION 3.20)
project(recmeet VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(RECMEET_BUILD_TRAY  "Build system tray applet" ON)
option(RECMEET_USE_LLAMA   "Enable local summarization via llama.cpp" ON)
option(RECMEET_USE_SHERPA  "Enable speaker diarization via sherpa-onnx" OFF)
option(RECMEET_BUILD_TESTS "Build unit tests" ON)

# Version header
set(RECMEET_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(RECMEET_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(RECMEET_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(RECMEET_VERSION "${PROJECT_VERSION}")
configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h)

# System dependencies
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
pkg_check_modules(PULSE_SIMPLE REQUIRED IMPORTED_TARGET libpulse-simple)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(NOTIFY REQUIRED IMPORTED_TARGET libnotify)

if(RECMEET_BUILD_TRAY)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
    pkg_check_modules(APPINDICATOR REQUIRED IMPORTED_TARGET ayatana-appindicator3-0.1)
endif()

# Vendor: whisper.cpp
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/whisper.cpp)

# Vendor: llama.cpp
if(RECMEET_USE_LLAMA)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    add_subdirectory(vendor/llama.cpp)
endif()

# Vendor: sherpa-onnx (speaker diarization)
if(RECMEET_USE_SHERPA)
    include(FetchContent)
    FetchContent_Declare(sherpa-onnx
        GIT_REPOSITORY https://github.com/k2-fsa/sherpa-onnx.git
        GIT_TAG v1.10.39
    )
    set(SHERPA_ONNX_ENABLE_TTS OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_WEBSOCKET OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_BINARY OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # sherpa-onnx's transitive deps require older CMake minimum versions
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    # sherpa-onnx uses CMAKE_SOURCE_DIR for its cmake modules, which is wrong
    # under FetchContent. Pre-populate the module path before MakeAvailable.
    FetchContent_GetProperties(sherpa-onnx)
    if(NOT sherpa-onnx_POPULATED)
        FetchContent_Populate(sherpa-onnx)
        list(APPEND CMAKE_MODULE_PATH
            ${sherpa-onnx_SOURCE_DIR}/cmake/Modules
            ${sherpa-onnx_SOURCE_DIR}/cmake)
        # sherpa-onnx uses CMAKE_SOURCE_DIR for include paths, which is wrong
        # under FetchContent. Add the correct include path globally.
        include_directories(${sherpa-onnx_SOURCE_DIR})
        add_subdirectory(${sherpa-onnx_SOURCE_DIR} ${sherpa-onnx_BINARY_DIR})
    endif()
endif()

# Core library (everything except main.cpp and tray.cpp)
set(CORE_SOURCES
    src/util.cpp
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
    PkgConfig::NOTIFY
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
