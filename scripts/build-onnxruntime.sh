#!/usr/bin/env bash
# SPDX-License-Identifier: MIT OR Apache-2.0
#
# Build onnxruntime from source for ABI-safe sherpa-onnx integration.
# Output: $PREFIX/lib/libonnxruntime.so, $PREFIX/include/onnxruntime_c_api.h
#
# Why: Arch Linux rolling upgrades break ABI compatibility between the system
# onnxruntime-cpu package and its bundled protobuf/abseil. Building from source
# with the host GCC bundles protobuf internally, eliminating both known crash
# vectors (protobuf version skew AND GCC std::regex ABI).
#
# Usage:
#   ./scripts/build-onnxruntime.sh              # defaults
#   ORT_VERSION=1.23.2 ./scripts/build-onnxruntime.sh
#   PREFIX=/opt/ort ./scripts/build-onnxruntime.sh

set -euo pipefail

ORT_VERSION="${ORT_VERSION:-1.23.2}"
ORT_TAG="v${ORT_VERSION}"
BUILD_DIR="${BUILD_DIR:-/tmp/onnxruntime-build}"
PREFIX="${PREFIX:-$(cd "$(dirname "$0")/.." && pwd)/vendor/onnxruntime-local}"
JOBS="${JOBS:-$(nproc)}"

echo "=== Building onnxruntime ${ORT_VERSION} ==="
echo "  Build dir:       ${BUILD_DIR}"
echo "  Install prefix:  ${PREFIX}"
echo "  Parallel jobs:   ${JOBS}"
echo "  Compiler:        $(c++ --version | head -1)"
echo ""

# Clone (shallow) if not already present
if [ ! -d "${BUILD_DIR}/onnxruntime" ]; then
    echo "--- Cloning onnxruntime ${ORT_TAG} ---"
    git clone --depth 1 --branch "${ORT_TAG}" --recurse-submodules --shallow-submodules \
        https://github.com/microsoft/onnxruntime.git "${BUILD_DIR}/onnxruntime"
else
    echo "--- Using existing clone at ${BUILD_DIR}/onnxruntime ---"
fi

cd "${BUILD_DIR}/onnxruntime"

# Patch GCC 15 compatibility issues — ORT 1.23.2 predates GCC 15's stricter
# transitive-include behavior (headers no longer implicitly include <cstdint>).
echo "--- Applying GCC 15 compatibility patches ---"
SEMVER_H="onnxruntime/core/common/semver.h"
if [ -f "${SEMVER_H}" ] && ! grep -q '<cstdint>' "${SEMVER_H}"; then
    sed -i '/#include "core\/common\/status.h"/a #include <cstdint>' "${SEMVER_H}"
    echo "  Patched ${SEMVER_H}: added #include <cstdint>"
fi

# Build with the official build.sh — handles protobuf bundling, abseil, etc.
# CPU only, shared lib, Release. No --minimal_build (requires ORT format models;
# sherpa-onnx uses standard ONNX format).
echo "--- Building (this takes 15-30 minutes) ---"
./build.sh \
    --config Release \
    --build_shared_lib \
    --parallel "${JOBS}" \
    --skip_tests \
    --compile_no_warning_as_error \
    --cmake_extra_defines \
        CMAKE_INSTALL_PREFIX="${PREFIX}" \
        onnxruntime_BUILD_UNIT_TESTS=OFF \
        FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER

echo "--- Installing to ${PREFIX} ---"
cmake --install "${BUILD_DIR}/onnxruntime/build/Linux/Release" --prefix "${PREFIX}"

# Verify the install — lib
if [ ! -f "${PREFIX}/lib/libonnxruntime.so" ]; then
    echo "ERROR: libonnxruntime.so not found in ${PREFIX}/lib/" >&2
    ls -la "${PREFIX}/lib/" >&2
    exit 1
fi

# Verify the install — headers (onnxruntime may install to include/ or
# include/onnxruntime/ depending on version)
INCLUDE_DIR="${PREFIX}/include"
if [ -f "${PREFIX}/include/onnxruntime/onnxruntime_c_api.h" ]; then
    INCLUDE_DIR="${PREFIX}/include/onnxruntime"
elif [ -f "${PREFIX}/include/onnxruntime_c_api.h" ]; then
    INCLUDE_DIR="${PREFIX}/include"
else
    echo "WARNING: onnxruntime_c_api.h not found in ${PREFIX}/include/" >&2
    find "${PREFIX}/include" -name 'onnxruntime_c_api.h' 2>/dev/null
fi

echo ""
echo "=== onnxruntime ${ORT_VERSION} installed to ${PREFIX} ==="
echo ""
echo "The Makefile auto-detects this location. Just run:"
echo "  make clean build"
echo ""
echo "Or set manually:"
echo "  export SHERPA_ONNXRUNTIME_LIB_DIR=${PREFIX}/lib"
echo "  export SHERPA_ONNXRUNTIME_INCLUDE_DIR=${INCLUDE_DIR}"
