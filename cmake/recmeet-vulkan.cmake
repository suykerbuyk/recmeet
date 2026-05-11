# cmake/recmeet-vulkan.cmake — Auto-detect Vulkan toolchain for ggml backend
#
# Exposes the tri-state cache variable RECMEET_GGML_VULKAN (AUTO | ON | OFF):
#   AUTO (default) — probe for libvulkan, headers, and glslc; enable if all
#                    three present, otherwise WARN with a per-distro install
#                    hint and fall back to CPU.
#   ON             — require the toolchain; fail configure with FATAL_ERROR
#                    if anything is missing.
#   OFF            — silently disable (legacy/packaged-build behaviour).
#
# Back-compat: any of 1/0/YES/NO/TRUE/FALSE are accepted and remapped to
# ON/OFF before the tri-state is evaluated, so existing scripts that pass
# `-DRECMEET_GGML_VULKAN=ON` (or `=1`) continue to work unchanged.
#
# When enabled, this module forces `GGML_VULKAN=ON` in the cache so the
# whisper.cpp/ggml subbuild emits libggml-vulkan.so. When disabled, it
# leaves GGML_VULKAN untouched (ggml's own default is OFF).
#
# Requires RECMEET_DISTRO from cmake/recmeet-distro.cmake.

include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/recmeet-distro.cmake)

# ---------------------------------------------------------------------------
# Cache variable — tri-state with back-compat boolean coercion
# ---------------------------------------------------------------------------
set(RECMEET_GGML_VULKAN AUTO CACHE STRING
    "Enable Vulkan compute backend in ggml: AUTO (detect), ON (require), OFF (disable)")
set_property(CACHE RECMEET_GGML_VULKAN PROPERTY STRINGS AUTO ON OFF)

# Map common boolean spellings to canonical ON/OFF before evaluating the
# tri-state. CMake `set(... CACHE STRING)` does not auto-coerce 1/YES/TRUE.
if(RECMEET_GGML_VULKAN MATCHES "^(1|YES|TRUE|yes|true|Yes|True)$")
    set(RECMEET_GGML_VULKAN ON  CACHE STRING "" FORCE)
elseif(RECMEET_GGML_VULKAN MATCHES "^(0|NO|FALSE|no|false|No|False)$")
    set(RECMEET_GGML_VULKAN OFF CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Per-distro install hints
# ---------------------------------------------------------------------------
# `glslc` (shaderc) is the ggml-required GLSL compiler. `glslang-tools` on
# Debian/Ubuntu provides only `glslangValidator`, which ggml does NOT use.
set(_recmeet_vulkan_hint_arch
    "sudo pacman -S vulkan-headers shaderc vulkan-icd-loader \\\n        vulkan-radeon   # or vulkan-intel / vulkan-nouveau")
set(_recmeet_vulkan_hint_debian
    "sudo apt install libvulkan-dev glslc mesa-vulkan-drivers")
set(_recmeet_vulkan_hint_fedora
    "sudo dnf install vulkan-headers glslc vulkan-loader mesa-vulkan-drivers")
set(_recmeet_vulkan_hint_nixos
    "nix-shell -p vulkan-headers vulkan-loader shaderc mesa\n    # or add the same attrs to configuration.nix / a flake.nix devShell")
set(_recmeet_vulkan_hint_alpine
    "apk add vulkan-headers vulkan-loader-dev shaderc mesa-vulkan-ati\n    # substitute mesa-vulkan-intel / mesa-vulkan-nouveau / mesa-vulkan-radeon per GPU")
set(_recmeet_vulkan_hint_gentoo
    "sudo emerge media-libs/vulkan-loader dev-util/vulkan-headers media-libs/shaderc\n    # ensure media-libs/mesa has the 'vulkan' USE flag and appropriate VIDEO_CARDS")
set(_recmeet_vulkan_hint_opensuse
    "sudo zypper install vulkan-devel shaderc Mesa-libvulkan-devel")
set(_recmeet_vulkan_hint_unknown
    "install Vulkan headers, glslc (shaderc), and a Vulkan ICD driver for your GPU")

if(DEFINED _recmeet_vulkan_hint_${RECMEET_DISTRO})
    set(_recmeet_vulkan_hint "${_recmeet_vulkan_hint_${RECMEET_DISTRO}}")
else()
    set(_recmeet_vulkan_hint "${_recmeet_vulkan_hint_unknown}")
endif()

# ---------------------------------------------------------------------------
# Detection
# ---------------------------------------------------------------------------
set(_recmeet_vulkan_enabled FALSE)

if(NOT RECMEET_GGML_VULKAN STREQUAL "OFF")
    find_package(Vulkan QUIET COMPONENTS glslc)

    set(_recmeet_vulkan_missing "")
    if(NOT Vulkan_FOUND OR NOT Vulkan_INCLUDE_DIRS)
        list(APPEND _recmeet_vulkan_missing "Vulkan headers / loader (libvulkan.so.1, vulkan/vulkan.h)")
    endif()
    if(NOT Vulkan_GLSLC_EXECUTABLE)
        list(APPEND _recmeet_vulkan_missing "glslc shader compiler")
    endif()

    if(_recmeet_vulkan_missing STREQUAL "")
        # Toolchain present — enable regardless of AUTO vs ON.
        set(_recmeet_vulkan_enabled TRUE)
        message(STATUS "recmeet: Vulkan auto-detected — GPU acceleration enabled")
        message(STATUS "  loader:  ${Vulkan_LIBRARIES}")
        message(STATUS "  glslc:   ${Vulkan_GLSLC_EXECUTABLE}")
    else()
        string(REPLACE ";" "\n    - " _missing_block "${_recmeet_vulkan_missing}")
        if(RECMEET_GGML_VULKAN STREQUAL "ON")
            # Explicit request, toolchain missing — fail loudly.
            message(FATAL_ERROR
                "recmeet: RECMEET_GGML_VULKAN=ON but Vulkan toolchain is incomplete.\n"
                "\n"
                "  Missing:\n"
                "    - ${_missing_block}\n"
                "\n"
                "  On this host (distro: ${RECMEET_DISTRO}), install with:\n"
                "\n"
                "    ${_recmeet_vulkan_hint}\n"
                "\n"
                "  Then re-run cmake. To proceed without Vulkan, pass\n"
                "    -DRECMEET_GGML_VULKAN=OFF\n")
        else()
            # AUTO + missing — WARN with hint, fall back to CPU silently.
            message(WARNING
                "recmeet: Vulkan GPU acceleration disabled — falling back to CPU.\n"
                "\n"
                "  Missing:\n"
                "    - ${_missing_block}\n"
                "\n"
                "  Building CPU-only. On this host (distro: ${RECMEET_DISTRO}), install with:\n"
                "\n"
                "    ${_recmeet_vulkan_hint}\n"
                "\n"
                "  Then re-run cmake. To force CPU-only and silence this warning, pass\n"
                "    -DRECMEET_GGML_VULKAN=OFF\n")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Propagate to ggml
# ---------------------------------------------------------------------------
if(_recmeet_vulkan_enabled)
    set(GGML_VULKAN ON CACHE BOOL "" FORCE)
endif()
