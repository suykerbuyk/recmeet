# cmake/prerequisites.cmake — Distro detection and helpful error messages
#
# Detects the Linux distribution from /etc/os-release and provides wrapper
# functions that print distro-specific install commands when packages are
# missing.  Included early in CMakeLists.txt, before any find_package calls.

# ---------------------------------------------------------------------------
# 1. Detect the distribution family
# ---------------------------------------------------------------------------
set(RECMEET_DISTRO "unknown")

if(EXISTS "/etc/os-release")
    file(STRINGS "/etc/os-release" _os_release)
    foreach(_line IN LISTS _os_release)
        if(_line MATCHES "^ID=(.+)")
            string(STRIP "${CMAKE_MATCH_1}" _distro_id)
            string(REPLACE "\"" "" _distro_id "${_distro_id}")
            break()
        endif()
    endforeach()
    foreach(_line IN LISTS _os_release)
        if(_line MATCHES "^ID_LIKE=(.+)")
            string(STRIP "${CMAKE_MATCH_1}" _distro_like)
            string(REPLACE "\"" "" _distro_like "${_distro_like}")
            break()
        endif()
    endforeach()

    if(_distro_id STREQUAL "arch" OR _distro_like MATCHES "arch")
        set(RECMEET_DISTRO "arch")
    elseif(_distro_id MATCHES "debian|ubuntu|linuxmint|pop" OR _distro_like MATCHES "debian|ubuntu")
        set(RECMEET_DISTRO "debian")
    elseif(_distro_id MATCHES "fedora|rhel|centos|rocky|alma" OR _distro_like MATCHES "fedora|rhel")
        set(RECMEET_DISTRO "fedora")
    endif()
endif()

message(STATUS "Detected distro family: ${RECMEET_DISTRO}")

# ---------------------------------------------------------------------------
# 2. Per-distro install command templates
# ---------------------------------------------------------------------------
set(_RECMEET_INSTALL_CMD_arch   "sudo pacman -S")
set(_RECMEET_INSTALL_CMD_debian "sudo apt install")
set(_RECMEET_INSTALL_CMD_fedora "sudo dnf install")

# Maps: logical name → distro package name
# Toolchain
set(_RECMEET_PKG_arch_pkg-config   "pkg-config")
set(_RECMEET_PKG_debian_pkg-config "pkg-config")
set(_RECMEET_PKG_fedora_pkg-config "pkgconf-pkg-config")

# Libraries (keyed by the pkg-config module name used in pkg_check_modules)
set(_RECMEET_PKG_arch_libpipewire-0.3          "pipewire")
set(_RECMEET_PKG_debian_libpipewire-0.3        "libpipewire-0.3-dev")
set(_RECMEET_PKG_fedora_libpipewire-0.3        "pipewire-devel")

set(_RECMEET_PKG_arch_libpulse                 "libpulse")
set(_RECMEET_PKG_debian_libpulse               "libpulse-dev")
set(_RECMEET_PKG_fedora_libpulse               "pulseaudio-libs-devel")

set(_RECMEET_PKG_arch_libpulse-simple          "libpulse")
set(_RECMEET_PKG_debian_libpulse-simple        "libpulse-dev")
set(_RECMEET_PKG_fedora_libpulse-simple        "pulseaudio-libs-devel")

set(_RECMEET_PKG_arch_sndfile                  "libsndfile")
set(_RECMEET_PKG_debian_sndfile                "libsndfile1-dev")
set(_RECMEET_PKG_fedora_sndfile                "libsndfile-devel")

set(_RECMEET_PKG_arch_libcurl                  "curl")
set(_RECMEET_PKG_debian_libcurl                "libcurl4-openssl-dev")
set(_RECMEET_PKG_fedora_libcurl                "libcurl-devel")

set(_RECMEET_PKG_arch_libnotify                "libnotify")
set(_RECMEET_PKG_debian_libnotify              "libnotify-dev")
set(_RECMEET_PKG_fedora_libnotify              "libnotify-devel")

set(_RECMEET_PKG_arch_gtk+-3.0                 "gtk3")
set(_RECMEET_PKG_debian_gtk+-3.0               "libgtk-3-dev")
set(_RECMEET_PKG_fedora_gtk+-3.0               "gtk3-devel")

set(_RECMEET_PKG_arch_ayatana-appindicator3-0.1    "libayatana-appindicator")
set(_RECMEET_PKG_debian_ayatana-appindicator3-0.1  "libayatana-appindicator3-dev")
set(_RECMEET_PKG_fedora_ayatana-appindicator3-0.1  "libayatana-appindicator-gtk3-devel")

# onnxruntime (not pkg-config; found via find_library by sherpa-onnx)
set(_RECMEET_PKG_arch_onnxruntime     "onnxruntime-cpu")
set(_RECMEET_PKG_debian_onnxruntime   "libonnxruntime-dev")
set(_RECMEET_PKG_fedora_onnxruntime   "onnxruntime-devel")

# ---------------------------------------------------------------------------
# 3. recmeet_check_submodules() — fail early if git submodules are missing
# ---------------------------------------------------------------------------
function(recmeet_check_submodules)
    set(_missing)
    foreach(_sub IN ITEMS vendor/whisper.cpp vendor/llama.cpp)
        if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${_sub}/CMakeLists.txt")
            list(APPEND _missing "${_sub}")
        endif()
    endforeach()
    if(_missing)
        string(REPLACE ";" ", " _list "${_missing}")
        message(FATAL_ERROR
            "Git submodules not initialized: ${_list}\n"
            "  Run this from the source directory:\n"
            "    git submodule update --init --recursive\n"
            "\n"
            "  Or clone with submodules next time:\n"
            "    git clone --recurse-submodules <url>\n")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 4. recmeet_check_pkg_config() — find pkg-config with a helpful error
# ---------------------------------------------------------------------------
# Must be a macro (not function) so find_package sets PKG_CONFIG_FOUND,
# PKG_CONFIG_VERSION, etc. in the caller's scope where pkg_check_modules
# needs them.
macro(recmeet_check_pkg_config)
    find_package(PkgConfig QUIET)
    if(NOT PKG_CONFIG_FOUND)
        set(_hint "")
        if(DEFINED _RECMEET_INSTALL_CMD_${RECMEET_DISTRO})
            set(_pkg "${_RECMEET_PKG_${RECMEET_DISTRO}_pkg-config}")
            set(_hint "\n  Install it:\n    ${_RECMEET_INSTALL_CMD_${RECMEET_DISTRO}} ${_pkg}\n")
        endif()
        message(FATAL_ERROR
            "pkg-config is required but was not found.${_hint}")
    endif()
endmacro()

# ---------------------------------------------------------------------------
# 5. recmeet_pkg_check() — wrapper around pkg_check_modules with hints
# ---------------------------------------------------------------------------
#   recmeet_pkg_check(<prefix> <pkg-config-module>)
#
# Equivalent to:
#   pkg_check_modules(<prefix> REQUIRED IMPORTED_TARGET <pkg-config-module>)
# but prints a distro-specific install suggestion on failure.
#
# Must be a macro (not function) because pkg_check_modules is itself a macro
# that sets <prefix>_FOUND, IMPORTED_TARGET results, etc. in the caller's
# scope.  A function would swallow those variables.
macro(recmeet_pkg_check _rm_prefix _rm_module)
    pkg_check_modules(${_rm_prefix} QUIET IMPORTED_TARGET ${_rm_module})
    if(NOT ${_rm_prefix}_FOUND)
        set(_hint "")
        if(DEFINED _RECMEET_PKG_${RECMEET_DISTRO}_${_rm_module})
            set(_pkg "${_RECMEET_PKG_${RECMEET_DISTRO}_${_rm_module}}")
            set(_hint "\n  Install it:\n    ${_RECMEET_INSTALL_CMD_${RECMEET_DISTRO}} ${_pkg}\n")
        endif()
        message(FATAL_ERROR
            "Required library '${_rm_module}' not found (looked up via pkg-config).${_hint}")
    endif()
endmacro()

# ---------------------------------------------------------------------------
# 6. recmeet_check_onnxruntime() — warn if system onnxruntime is missing
# ---------------------------------------------------------------------------
# sherpa-onnx's pre-built static libonnxruntime.a was compiled with GCC 11.
# On GCC 12+ the std::regex ABI changed, causing SIGABRT in onnxruntime's
# ParseSemVerVersion(). A system-installed onnxruntime (built with the host
# GCC) avoids this. The build still succeeds without it — sherpa-onnx falls
# back to the pre-built download — but diarization will crash at runtime.
function(recmeet_check_onnxruntime)
    find_library(_onnxrt_lib onnxruntime)
    if(_onnxrt_lib)
        message(STATUS "System onnxruntime found: ${_onnxrt_lib}")
    else()
        set(_hint "")
        if(DEFINED _RECMEET_PKG_${RECMEET_DISTRO}_onnxruntime)
            set(_pkg "${_RECMEET_PKG_${RECMEET_DISTRO}_onnxruntime}")
            set(_hint "\n  Install it:\n    ${_RECMEET_INSTALL_CMD_${RECMEET_DISTRO}} ${_pkg}\n")
        endif()
        message(WARNING
            "System onnxruntime not found. sherpa-onnx will use its pre-built "
            "static library, which was compiled with GCC 11 and may crash "
            "(SIGABRT in std::regex) on GCC 12+ due to ABI incompatibility.${_hint}")
    endif()
endfunction()
