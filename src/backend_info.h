// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

namespace recmeet {

// Discover and register the ggml backend plugins shipped alongside the
// recmeet binary (libggml-cpu-*.so + libggml-vulkan.so + ...). Resolves the
// plugin directory deterministically from /proc/self/exe so dlopen sees a
// real path — ggml's compile-time GGML_BACKEND_DIR is `$ORIGIN/../lib`,
// which dlopen does NOT expand the way ld.so does for RPATH, leaving a
// noisy "search path $ORIGIN/../lib does not exist" debug trail. Resolution
// order:
//   1. $RECMEET_GGML_BACKEND_PATH (test/dev override)
//   2. <exe-dir>/../lib  (production install layout)
//   3. <exe-dir>/bin     (in-tree build: binary at build/, plugins at build/bin/)
//   4. <exe-dir>         (alternate co-located layout)
// Falls through to ggml_backend_load_all() (no explicit path) when none
// match, preserving ggml's default executable-dir + CWD search chain.
void load_backends();

// Enumerate ggml's registered backends after load_backends() and emit the
// 2-line banner: (a) full registry list, (b) highest-priority enumerable
// device (GPU > IGPU > ACCEL > CPU). When a non-CPU backend registered but
// exposes zero devices (e.g. libggml-vulkan.so loaded on a host without a
// working ICD), an extra WARN line surfaces the gap before the active-
// backend line shows the CPU fallback.
//
// Intended call order at process startup:
//   recmeet::load_backends();
//   recmeet::log_backend_summary();
//
// Once per process. Safe to call before any whisper context is created.
void log_backend_summary();

} // namespace recmeet
