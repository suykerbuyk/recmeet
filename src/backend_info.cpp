// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "backend_info.h"
#include "log.h"

#include "ggml-backend.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace recmeet {

namespace fs = std::filesystem;

namespace {

// Probe the candidate ladder for the directory holding libggml-*.so plugins.
// Returns empty when none of the candidates resolve to an existing directory.
fs::path resolve_plugin_dir() {
    if (const char* env = std::getenv("RECMEET_GGML_BACKEND_PATH")) {
        if (*env) {
            std::error_code ec;
            fs::path override_path(env);
            if (fs::is_directory(override_path, ec)) return override_path;
        }
    }

    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec || self.empty()) return {};
    const fs::path exe_dir = self.parent_path();

    const fs::path candidates[] = {
        (exe_dir / ".." / "lib").lexically_normal(),
        (exe_dir / "bin").lexically_normal(),
        exe_dir,
    };
    for (const auto& candidate : candidates) {
        if (fs::is_directory(candidate, ec)) return candidate;
    }
    return {};
}

std::string join_registry_names() {
    std::string out;
    const size_t n = ggml_backend_reg_count();
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) out += ", ";
        const char* name = ggml_backend_reg_name(ggml_backend_reg_get(i));
        out += (name ? name : "(unnamed)");
    }
    return out;
}

// Find the best enumerable device: GPU > IGPU > ACCEL > CPU.
ggml_backend_dev_t pick_active_device() {
    ggml_backend_dev_t gpu = nullptr, igpu = nullptr, accel = nullptr, cpu = nullptr;
    const size_t n = ggml_backend_dev_count();
    for (size_t i = 0; i < n; ++i) {
        auto dev = ggml_backend_dev_get(i);
        switch (ggml_backend_dev_type(dev)) {
            case GGML_BACKEND_DEVICE_TYPE_GPU:   if (!gpu)   gpu = dev;   break;
            case GGML_BACKEND_DEVICE_TYPE_IGPU:  if (!igpu)  igpu = dev;  break;
            case GGML_BACKEND_DEVICE_TYPE_ACCEL: if (!accel) accel = dev; break;
            case GGML_BACKEND_DEVICE_TYPE_CPU:   if (!cpu)   cpu = dev;   break;
        }
    }
    if (gpu)   return gpu;
    if (igpu)  return igpu;
    if (accel) return accel;
    return cpu;
}

// A non-CPU backend was registered but contributes zero devices — e.g.
// libggml-vulkan.so loaded on a host without a working ICD driver. Returns
// the registry name to surface, or nullptr when everything is healthy.
const char* find_unenumerable_non_cpu_backend() {
    const size_t n = ggml_backend_reg_count();
    for (size_t i = 0; i < n; ++i) {
        auto reg = ggml_backend_reg_get(i);
        const char* name = ggml_backend_reg_name(reg);
        if (!name || std::strcmp(name, "CPU") == 0) continue;
        if (ggml_backend_reg_dev_count(reg) == 0) return name;
    }
    return nullptr;
}

// Banner output: log_info captures the line in the persistent log when
// RECMEET_LOG_LEVEL >= info, and an unconditional fprintf to stderr makes
// it visible under journalctl / interactive even at the default `error`
// level. Mirrors the daemon's existing "listening on" precedent at the
// recmeet-server entry point.
__attribute__((format(printf, 1, 2)))
void banner_emit(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_info("%s", buf);
    fprintf(stderr, "%s\n", buf);
}

__attribute__((format(printf, 1, 2)))
void banner_warn(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_warn("%s", buf);
    fprintf(stderr, "%s\n", buf);
}

} // namespace

void load_backends() {
    const fs::path dir = resolve_plugin_dir();
    if (!dir.empty()) {
        ggml_backend_load_all_from_path(dir.c_str());
    } else {
        // Last-resort: let ggml try its baked-in search chain (GGML_BACKEND_DIR
        // → executable dir → CWD). The compile-time GGML_BACKEND_DIR pin to
        // $ORIGIN/../lib is dead string from dlopen's perspective but the
        // executable-dir fallback may still strike paydirt on unusual layouts.
        ggml_backend_load_all();
    }
}

void log_backend_summary() {
    const std::string regs = join_registry_names();
    banner_emit("ggml: backend registry: %s", regs.empty() ? "(none)" : regs.c_str());

    if (const char* gap = find_unenumerable_non_cpu_backend()) {
        banner_warn("ggml: %s compiled in but 0 devices enumerable — falling back to CPU", gap);
    }

    auto dev = pick_active_device();
    if (!dev) {
        banner_warn("ggml: no devices enumerable — whisper will fail to initialize");
        return;
    }

    ggml_backend_dev_props props{};
    ggml_backend_dev_get_props(dev, &props);
    const char* desc = props.description ? props.description
                     : (props.name ? props.name : "(unknown device)");

    if (props.type == GGML_BACKEND_DEVICE_TYPE_CPU) {
        unsigned threads = std::thread::hardware_concurrency();
        if (threads == 0) threads = 1;
        banner_emit("ggml: active backend: CPU (%u threads, %s)", threads, desc);
        return;
    }

    const char* reg_name = "GPU";
    if (auto reg = ggml_backend_dev_backend_reg(dev)) {
        if (const char* n = ggml_backend_reg_name(reg)) reg_name = n;
    }
    const size_t mem_mb = props.memory_total / (1024 * 1024);
    if (mem_mb > 0) {
        banner_emit("ggml: active backend: %s (%s, %zu MB)", reg_name, desc, mem_mb);
    } else {
        banner_emit("ggml: active backend: %s (%s)", reg_name, desc);
    }
}

} // namespace recmeet
