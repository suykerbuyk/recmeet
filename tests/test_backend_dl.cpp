// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "backend_info.h"

#include <ggml-backend.h>

#include <catch2/catch_test_macros.hpp>

namespace {

// Static-init constructor so the ggml plugin registry is populated before
// any Catch2 test body runs. Without this, GGML_BACKEND_DL=ON leaves CPU
// inference targeting an empty registry — whisper_init aborts in tests
// that touch the postprocess pipeline. Pairs with the runtime call to
// recmeet::load_backends() at process startup in src/daemon.cpp and
// src/main.cpp.
struct BackendBootstrap {
    BackendBootstrap() { recmeet::load_backends(); }
};
const BackendBootstrap g_backend_bootstrap;

} // namespace

TEST_CASE("backend-dl: ggml_backend_load_all populates the registry", "[backend-dl]") {
    REQUIRE(ggml_backend_reg_count() >= 1);
}

TEST_CASE("backend-dl: at least one device is enumerable", "[backend-dl]") {
    REQUIRE(ggml_backend_dev_count() >= 1);
}

TEST_CASE("backend-dl: a CPU device is always present", "[backend-dl]") {
    bool has_cpu = false;
    const size_t n = ggml_backend_dev_count();
    for (size_t i = 0; i < n; ++i) {
        if (ggml_backend_dev_type(ggml_backend_dev_get(i)) == GGML_BACKEND_DEVICE_TYPE_CPU) {
            has_cpu = true;
            break;
        }
    }
    REQUIRE(has_cpu);
}
