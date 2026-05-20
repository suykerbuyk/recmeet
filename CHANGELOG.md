# Changelog

All notable changes to recmeet are documented in this file. The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.0] — Unreleased (`feat/v2-thin-client` branch)

### Added

- **Tray-bundled WebUI** (Phase E.6.2, iter 183):
  - The speaker-management web UI now ships as an embedded HTTP listener inside `recmeet-tray` (`src/tray_web.cpp`) instead of as a supervised subprocess. The tray binds on `127.0.0.1:<kernel-picked-port>` via `httplib::Server::bind_to_any_port` on first "Open Speaker Management" menu click; `xdg-open` carries the resolved URL.
  - Four static assets (`index.html`, `app.js`, `style.css`, `favicon.svg`) are baked into the binary by a pure-CMake `file(READ HEX)` generator (`cmake/embed_assets.cmake`). No xxd / Perl / Python dependency. Each asset is served with `Cache-Control: no-cache`.
  - 13 HTTP-to-IPC endpoint translations cover every operation the WebUI exposes: speaker introspection (`/api/speakers`, `/api/speakers/:name`), mutation (`enroll`, `remove`, `remove-embedding`, `reset`, `batch-reidentify`), and per-meeting actions (`/api/meetings`, `/api/meetings/:meeting_id/speakers`, `/relabel`, `/reprocess`, `/note`).
  - 32 new tests under `[e6][tray-web]` (`tests/test_tray_web.cpp`) exercise the real translator (not a parallel reimplementation) against an in-process DaemonSim.

### Changed (user-visible)

- **WebUI no longer renders notes from operator-side `note_dir` mirror.** Pre-E.6 the WebUI's note view fell back to `note_dir/YYYY/MM/Meeting_*.md` when the canonical copy in the meeting dir was missing. Under the thin-client architecture the daemon does not see operator-side directories; `meetings.read_note` reads from the meeting dir only. For tray-and-daemon-same-host operators with an Obsidian-mirror workflow this is a regression on cosmetic browsing of the mirror copy — the canonical copy in the meeting dir is unaffected.
- **Legacy V1 meetings (pre-`meeting_id`) are read-only in the WebUI.** `meetings.list` returns them with `meeting_id: null`. The frontend hides the relabel / reprocess / enroll buttons for these and shows a "Legacy meeting — stamp meeting_id to enable editing" banner. Operators who want write access regenerate the meeting through the tray's reprocess flow (C.11.4 dedup stamps a fresh `meeting_id`).
- **`recmeet-web` binary is gone.** Source, CMake target, systemd unit, install rule, `--port` / `--bind` CLI flags — all removed. Operators who relied on a headless `recmeet-web` process (no tray running) lose that surface. A future `recmeet-webd` headless variant can factor `src/tray_web.cpp` into a small library; not on the v2.0.0 critical path.

### Removed

- `src/web.cpp`, `tests/test_web.cpp`, `dist/recmeet-web.service.in`, the `RECMEET_BUILD_WEB` CMake option, the `recmeet-web` install rule, the `share/web/` install rule.
- `tray.cpp` web-server lifecycle plumbing: `spawn_web_server`, `stop_web_server`, `reap_web_server`, `is_port_listening`, and the `web_server_pid` field on `TrayState`. The SIGCHLD reaper is preserved as a safety net for stray `system()`-spawned children (xdg-open, $EDITOR).

## [1.6.0] — Unreleased

First release on the `v1-maintenance` branch. Delivers the "build once per platform, run everywhere" GPU acceleration story as a bundled pair: configure-time auto-detect of Vulkan + a runtime-loadable plugin model so the same binary uses GPU when available and silently falls back to CPU when not.

### Added

- **Vulkan GPU acceleration, auto-detected at configure time** (Phase 1, iter 142, `cmake/recmeet-vulkan.cmake`):
  - New tri-state cache variable `RECMEET_GGML_VULKAN` with values `AUTO` (default), `ON` (require), `OFF` (force-disable). Boolean values `1`/`0`/`YES`/`NO`/`TRUE`/`FALSE` accepted for back-compat with v1.5.x usage.
  - `find_package(Vulkan COMPONENTS glslc)` probes for the loader, headers, and `glslc` shader compiler. All three required to enable.
  - Per-distro install hints rendered into the configure-time warning: Arch, Debian/Ubuntu, Fedora/RHEL, NixOS, Alpine, Gentoo, openSUSE, plus a generic fallback.
  - `AUTO` + missing toolchain → `WARN` with install hint, CPU-only build. `ON` + missing → `FATAL_ERROR` with the same hint.
- **Runtime-loadable GPU backends via `GGML_BACKEND_DL`** (Phase 1, iter 142):
  - Vendored `whisper.cpp` and `llama.cpp` subbuilds now configure with `BUILD_SHARED_LIBS=ON` (required for `GGML_BACKEND_DL=ON`).
  - `GGML_BACKEND_DIR` pinned to `$ORIGIN/../lib` (mandatory; closes the ggml CWD-fallback attacker-plantable-plugin path).
  - `GGML_NATIVE=OFF` + `GGML_CPU_ALL_VARIANTS=ON` so ggml emits per-ISA CPU plugins (`libggml-cpu-sse42.so`, `libggml-cpu-avx.so`, …) scored at startup by host capabilities. Same binary runs on a 2015 server and a 2024 laptop.
  - Compute backends (`libggml-vulkan.so`, `libggml-cpu-*.so`) ship as separate `.so` files installed into `<prefix>/lib/`. The `recmeet` binary has no `libvulkan.so.1` (or any other GPU lib) in `DT_NEEDED`.
- **Daemon-startup active-backend banner** (Phase 2, iter 142, `src/backend_info.{h,cpp}`):
  - `recmeet::load_backends()` resolves the plugin directory deterministically from `/proc/self/exe` (env-override → `<exe>/../lib` install layout → `<exe>/bin` in-tree → `<exe>`) so `dlopen` sees a real path. Bypasses ggml's `$ORIGIN`-macro search, which `dlopen` does not expand the way `ld.so` does for RPATH.
  - `recmeet::log_backend_summary()` emits two banner lines on stderr (and in the log at `info`): the full backend registry and the highest-priority enumerable device (GPU > IGPU > ACCEL > CPU). Stderr-mirrored unconditionally so it shows up under `journalctl` even at the default `RECMEET_LOG_LEVEL=error`.
  - WARN line surfaces non-CPU backends that registered but exposed zero devices (e.g. `libggml-vulkan.so` loaded on a host without a working Vulkan ICD) before showing the CPU-fallback active-backend line.
  - Wired into both call sites: `src/daemon.cpp:851` (daemon startup, after `log_init()`) and `src/main.cpp:863` (standalone CLI top of `standalone_main()`, before subprocess-mode dispatch).
- **Plugin-bootstrap static initializer for tests** (`tests/test_backend_dl.cpp`):
  - C++ static-constructor `BackendBootstrap` calls `load_backends()` before Catch2's `main` runs, so plugins are registered before any test body needs them.
  - Fixes a previously-latent failure: `tests/test_pipeline_helpers.cpp:184` (run_postprocessing transcribe test) was silently broken by the `GGML_BACKEND_DL=ON` flip since `whisper_init_from_file_with_params` aborts against an empty registry.
  - 3 new `[backend-dl]` tests cover plugin discovery, registry enumeration, and the GPU > IGPU > ACCEL > CPU active-device picker.
- **Documentation**: BUILD.md gains a "Plugin architecture (ggml backends)" section and a "GPU acceleration (Vulkan)" section. README.md gains a top-level "GPU acceleration" section and a `RECMEET_GGML_VULKAN` entry in the Build options table.

### Changed

- `whisper.cpp` and (when `RECMEET_USE_LLAMA=ON`) `llama.cpp` are now configured with `BUILD_SHARED_LIBS=ON`. The vendor `.so` files (`libwhisper.so`, `libggml.so`, `libggml-base.so`, `libllama.so`) install into `<prefix>/lib/` alongside the compute-backend plugins. `recmeet_ipc` and `recmeet_core` remain static libraries — the change is scoped to the vendor ML subtree.
- `CMAKE_POLICY_DEFAULT_CMP0177=NEW` set at the top of `CMakeLists.txt` so install `DESTINATION` paths normalize correctly under CMake 3.31+. The compile-time `GGML_BACKEND_DIR=$ORIGIN/../lib` string baked into `libggml-base.so` survives the policy unchanged — only install destinations are normalized.

### Migration notes (distro packagers)

- The installed file set now includes `libggml-*.so` plugins in `<prefix>/lib/`. Update package contents lists / `.install` files accordingly. `make install` and the CMake install step handle this automatically; only manual packaging recipes need attention.
- To ship a CPU-only artifact (matching v1.5.x behavior), pin `-DRECMEET_GGML_VULKAN=OFF` in your PKGBUILD / Debian rules / RPM spec. The new default (`AUTO`) probes the build host's toolchain, which may not match the target audience's hardware.
- The split-packaging model (a base `recmeet` package + companion `recmeet-vulkan` / `recmeet-cuda` packages each shipping their own `libggml-*.so`) is a roadmap item — see `agentctx/tasks/runtime-loadable-gpu-backends.md` Step 7.

### Verification

- Full-stack reprocess of a 63-min real meeting on Radeon Pro W5500 (RDNA1 / 8 GB VRAM / Mesa RADV): ~16 min transcription via Vulkan (~26× CPU baseline), 5.0 GB VRAM peak, 600 MB host RSS, chunked-diarize peak 5.7 GB host RSS, no VRAM leak across chunks, cloud summary + Obsidian note written.
- 540/540 unit tests, 78/78 integration tests, zero compiler warnings (`sherpa-ON` build).
- `ldd build/recmeet | grep -i vulkan` returns empty on the Vulkan-enabled build.

## [1.5.0] — 2026-05-09

The V1 capstone release. Live captioning lands as the final V1 feature, completing the V1 line at version 1.5.0 (following 1.3.x and 1.4.x). The `v1-maintenance` branch is cut from this tag and V2 work begins on `main` thereafter; future V1 patch releases ship as `v1.5.x` from `v1-maintenance`.

### Added

- **Live captioning** (Phases 1–6 of the V1 capstone, iter 135):
  - `CaptionEngine` wraps sherpa-onnx streaming Zipformer (`sherpa-onnx-streaming-zipformer-en-2023-06-26`, ~74 MB int8) behind a SPSC ring buffer + dedicated worker thread.
  - Per-recording opt-in via `record.start {captions_enabled: true, caption_model: ...}` params; CLI flags `--show-captions`, `--caption-model`, `--list-caption-models`, `--no-captions`; tray "Show Live Captions" checkbox.
  - IPC `caption` + `caption.degraded` events with `job_id` payload (V2 forward-compat for per-client routing).
  - `.vtt` sidecar persistence at `~/meetings/<dir>/captions.vtt` — append-only WebVTT, finalized cues only, `O_APPEND` atomic writes, lazy header on first cue, no `fsync`.
  - Tray overlay (GtkLabel popup window) + CLI stderr rendering with render-time normalization (`normalize_caption()` lowercases ALL-CAPS engine output and capitalizes sentence boundaries).
  - English-only in V1 (`--language != en` guard disables captions with warning).
  - ~50 new tests across unit, IPC integration, and full-stack suites.
- **`Bash(vv *)` and similar permission allowlist patterns** added to `.claude/settings.json` to reduce permission prompts during AI-driven development.

### Changed

- `pipeline.cpp` `run_recording()` now takes an optional `CaptionHooks*` parameter for caption fan-out (broadcast + .vtt). Default nullptr; existing callers unaffected.
- `recmeet_ipc` library gains `make_caption_event()` / `make_caption_degraded_event()` helpers for the new events.

### Fixed

- **`RECMEET_USE_SHERPA=OFF` build was broken** (regression from commit `95cb16e`, 2026-03-10). `src/speaker_id.h` had `MeetingSpeaker` and the meeting-speaker JSON I/O declarations gated behind `#if RECMEET_USE_SHERPA` while their definitions in `speaker_id.cpp` were unguarded; `tests/test_full_stack.cpp` called `is_sherpa_model_cached()` without the guard. Both fixed; `cmake -B build-nosherpa -DRECMEET_USE_SHERPA=OFF` once again produces a clean build with all unit tests passing.

### V1 Capstone — Verification (this tag)

- 537 C++ unit tests / 2110 assertions (sherpa-ON), 497 / 1924 (sherpa-OFF) — all pass.
- 66 IPC integration cases / 458 assertions — all pass.
- Zero compiler warnings (sherpa-ON, sherpa-OFF).
- ffmpeg round-trip validated for `.vtt` output.
- Full-stack tests with captions enabled validate end-to-end on 30-min and 60-min fixtures (skip cleanly if model/fixture absent).

### V1 → V2 transition

- This tag (`v1.5.0`) is the cut point for the long-lived `v1-maintenance` branch.
- V2 work resumes on `main` immediately, starting with `thin-client-recording-server` (already planned at `agentctx/tasks/thin-client-recording-server.md`).
- `webui-live-captions` follow-up task (Phase 5.3 descope) tracks the WebUI live-captions integration and is gated on V2 maintenance-branch policy.
