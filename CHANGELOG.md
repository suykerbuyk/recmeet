# Changelog

All notable changes to recmeet are documented in this file. The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.6.2] — 2026-05-20

Patch release on `v1-maintenance`. Resolves the long-tail postprocess crash on short recordings by syncing the vendored sherpa-onnx `TopkIndex` clamp patch to match upstream PR review feedback (k2-fsa/sherpa-onnx#3628). Same crash class first surfaced as the iter-175 "Hyp D parent-activity-modulated" investigation; this release ships the root-cause fix.

### Fixed

- **`TopkIndex` heap-buffer-overflow in vendored sherpa-onnx** (iter 179–182, `cmake/sherpa-onnx-patches/0002-topkindex-clamp-overflow.patch`):
  - `std::partial_sort(begin, begin + topk, end, cmp)` in `sherpa-onnx/csrc/math.h:115` required `topk <= end-begin`, but the original size-clamp at line 120 only sized the returned vector and did not protect the sort itself. `OfflineSpeakerDiarizationPyannoteImpl::FinalizeLabels` (`offline-speaker-diarization-pyannote-impl.h:624`) passes `topk > size` on short/degenerate inputs, producing a past-the-end read whose crash modulator was parent-process heap layout (tray-presence shifted ASLR enough to change whether the past-the-end read crossed a redzone).
  - Diagnosis chain: strip-guard restoring DWARF on RelWithDebInfo installs (commit `58c4715`) → `coredumpctl gdb` pinned `~DiarizeSession` → `SherpaOnnxDestroyOfflineSpeakerDiarization` → `~Graph` → ASAN rebuild produced definitive heap-buffer-overflow at `math.h:115` with `FinalizeLabels` line 624 as the caller.
  - Patch clamps `topk` to `[0, size]` via `std::max<int32_t>(0, std::min<int32_t>(size, topk))` BEFORE the `partial_sort`, and short-circuits the truncating-copy branch when `k_num == size` (both per upstream PR review feedback from Gemini Code Assist + CodeRabbit). Applied idempotently via FetchContent `PATCH_COMMAND` with `git apply --reverse --check` guard.
  - Validation: ASAN-rebuilt recmeet runs 10 standalone iterations clean (vs unpatched: iter 1 overflow); non-ASAN production binary 10 standalone iterations clean (vs unpatched: iter 5–6 crash); operator's original daemon-routed repro with `recmeet-tray` running for 50 iterations all clean (vs unpatched: iter 5–6 crash).

### Changed

- **Strip-on-install scoped to Release / MinSizeRel** (iter 179, commit `58c4715`, `CMakeLists.txt:425-433`). `CMAKE_INSTALL_DO_STRIP` no longer fires on RelWithDebInfo or Debug installs, so diagnostic builds retain DWARF for `coredumpctl gdb` symbolication. Independent quality improvement surfaced by the iter-179 investigation.

### Upstream

- **PR k2-fsa/sherpa-onnx#3628** submitted upstream with the full diagnostic chain, ASAN report excerpt, and recmeet's reproduction numbers. The vendored patch can be dropped when recmeet bumps `FetchContent_Declare(sherpa-onnx ...)` to a containing version.

## [1.6.1] — 2026-05-13

Patch release on `v1-maintenance`. Closes the binary-coverage gap on the Go tooling layer (`recmeet-mcp`, `recmeet-agent` had zero binary-level tests before this release) and fixes the `make install` flow so the Go binaries actually install alongside the C++ ones — operators relying on either tool were copying them by hand each rebuild.

### Added

- **Go tools install on `cmake --install`** (iter 146, `CMakeLists.txt:318-371,498-501`). New `add_custom_target(recmeet_go_tools ALL)` with ninja incremental tracking sourced from `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` over `tools/**/*.go` + `go.mod` + `go.sum`. `install(PROGRAMS)` lands `recmeet-mcp` and `recmeet-agent` in `<prefix>/bin/`. New `RECMEET_BUILD_GO_TOOLS` option (default ON) with `find_program(go)` + `FATAL_ERROR` if missing — symmetric with the v1.6.0 Vulkan opt-in fail-fast pattern.
- **Go tools integration test suite** (iter 148, 5-phase landing, 39 new tests). Production-code prereqs in Phase 0 (`ANTHROPIC_BASE_URL` plumbing, `RECMEET_CONFIG` env var with explicit-path fast-fail, `--max-iterations` flag on both subcommands). Shared `tools/testutil/` package with `BuildBinaryOnce` (TestMain-pattern build cache), `MockAnthropic` (httptest + FIFO response queue), `TeeStdioTransport` implementing `mcp-go/client/transport.Interface` for the stdio-hygiene test. Phase A: 18 `recmeet-mcp` protocol tests across handshake / tool-call correctness / stdio hygiene (in-process transport for correctness, stdio subprocess for hygiene; subprocess coverage 100% on `cmd/recmeet-mcp/main.go`). Phase B: 21 `recmeet-agent` CLI tests covering both Cobra subcommands' help, per-subcommand flag plumbing, dry-run modes, and mock-Anthropic round-trip (subprocess coverage 92.8%).
- **Make-level submodule auto-init** (iter 148, Phase D). New idempotent `.PHONY: ensure-submodules` wired as a prereq of every CMake-invoking target (`build`, `test`, `integration-cxx`, `integration-t2-1`, `benchmark`, `full-stack`). Fixes the fresh-worktree ergonomics gap where `git worktree add` checkouts found `vendor/whisper.cpp` + `vendor/llama.cpp` empty.
- **`make integration-go` / `integration-cxx` / `integration-go-coverage` / `integration` umbrella** targets (iter 148, Phase D). Build-tag opt-in (`//go:build integration`) so default `go test ./...` stays fast.

### Changed

- **Force-relink C++ binaries before install** (iter 146, `Makefile:108-121`). The `install:` target removes the four C++ binaries before re-invoking ninja, forcing a fresh link under the current configure state. Fixes a CMake-install `RPATH_CHANGE` error that any reconfigure could trigger when the link search path changes (discovering or losing `vendor/onnxruntime-local`, toggling `RECMEET_USE_SHERPA`, switching system vs vendored onnxruntime). Scoped to the install path; day-to-day `make build` is unaffected.
- **`cmake/prerequisites.cmake` submodule-missing message** now leads with `make build` as the recommended fix.

## [1.6.0] — 2026-05-13

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
- **Version derivation single-source-of-truth from git tag** (iter 145, `CMakeLists.txt:1-21`, `cmake/version.cmake:19,33-39`). Pre-`project()` `find_package(Git)` + `execute_process(git describe --tags --match "[0-9]*" --abbrev=0)` feeds the parsed `M.m.p` into `project(recmeet VERSION ...)`. Cutting a release is now `git tag X.Y.Z && git push --tags` with zero file edits. Fixes the `--match "v[0-9]*"` regex that never matched this repo's unprefixed tag style; every prior release silently used the `0.9.0` PROJECT_VERSION literal fallback.

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

## [1.4.5] — 2026-05-08

Documentation validation pass that closes drift accumulated across 1.4.x. README, QUICKSTART, ARCHITECTURE, COMPONENT-DIAGRAMS, BUILD, and ROADMAP refreshed against current code; `docs/DEADLOCK-INVESTIGATION.md` retired to `docs/history/` with a resolution narrative now that the iter-94/95/98 + T1A/B/C iter-111–114 fixes have shipped.

### Changed

- **README "Postprocessing memory limits" section** rewrote the stale "current effective limit is ~45 minutes" claim to reflect the post-iter-121 reality: target met, three load-bearing containment layers (cgroup caps + subprocess isolation + chunked diarize) plus the `--diarize-chunk-minutes 12` operator knob.
- **README CLI flag table** filled in 8 missing flags (`--diarize-chunk-minutes`, `--diarize-chunk-overlap-sec`, `--diarize-stitch-threshold`, `--reset-speakers`, `--log-retention`, `--daemon-addr`, `--progress-json`, `--config-json`). Daemon flag table gains `--listen` for the TCP transport.
- **BUILD.md "five targets" diagram** corrected to the iter-104 library split (`recmeet_ipc` thin-client lib + `recmeet_core` ML-bundle; tray links only `recmeet_ipc`).
- **ARCHITECTURE.md daemon state-machine description** rewritten from the obsolete 4-value enum to the actual three-independent-atomics model (`g_recording`, `g_postprocessing`, `g_downloading`) under `g_state_mu` with `composite_state_name()` projection.
- **Test count claim** ("440 cases / 1435+ assertions / 25 modules") replaced with current numbers: 451 unit / 1734 assertions / 28 modules + per-tag breakdowns.
- **README IPC events list** now includes the `progress` event (was missing).
- **`docs/ROADMAP.md` Phase 2** marked superseded by `agentctx/tasks/thin-client-recording-server.md`; historical sketch preserved as context.
- **`docs/BUILD.md` Prerequisites** now lists explicit CMake ≥3.28 and GCC ≥12 toolchain floors with rationale (the `<filesystem>` ABI handling and sherpa-onnx's bundled GCC-11-compiled `libonnxruntime.a`); GCC 15 auto-patch in `scripts/build-onnxruntime.sh:43-50` (sed-injection of `<cstdint>` into `semver.h`) now documented.

### Added

- **New developer-facing diagrams** in `docs/COMPONENT-DIAGRAMS.md` (iter-128 work that had not been documented visually):
  - §10b: `--reprocess-batch` orchestration flowchart — classify_batch_entries → mode-lock → ensure_models_cached_or_fail (once) → dispatch loop with hybrid SIGINT.
  - §11e: `batch_mode` field propagation through subprocess JSON config + `batch_job` field on `job.complete` event + tray notification gating (one notification per batch instead of one per meeting).
- **`docs/history/DEADLOCK-INVESTIGATION.md`** — `git mv` from `docs/DEADLOCK-INVESTIGATION.md` with a Resolution narrative prepended documenting iter 94/95/98 + T1A/B/C iter 111–114 fixes; cross-references in `docs/BUILD.md:261`, `docs/ROADMAP.md:140`, `dist/recmeet-daemon.service.in:28` retargeted in lockstep.

### Verification

- 451 unit / 1734 assertions all green (matches the 1.4.4 baseline; no regressions because zero source changes).
- Cross-ref sweep: zero stale `DEADLOCK-INVESTIGATION` references outside `docs/history/`.

## [1.4.4] — 2026-05-01

The memory-containment release. Ships T1A + T1B + T1C of the postprocess-memory-containment plan to convert the OOM-killer-induced silent failures observed on long meetings into clean, actionable errors with the daemon still running and the audio preserved. Codifies the 4-hour-audio / 16-GB-total-RAM long-audio requirement that headlines the README's "build for limited hardware" positioning.

### Added

- **Tier 1A — systemd cgroup caps + no-malloc heartbeat with RSS** (iter 113, commit `b5c2264`):
  - `dist/recmeet-daemon.service.in` ships `MALLOC_ARENA_MAX=2` (caps glibc per-thread arenas — onnxruntime spawns many short-lived threads, default fragments allocations 2–4×), `RECMEET_RSS_LIMIT_MB=12288` (child self-limit), `MemoryHigh=10G` (soft throttle), `MemoryMax=14G` (hard cgroup cap — the real backstop), `MemorySwapMax=0` to make the no-swap state explicit at the unit level independent of host zram.
  - Three new helpers in `src/util.{h,cpp}` designed for the no-malloc / no-libc-stdio-lock emission path that the iter-110 OOM incident proved was necessary: `read_self_rss_kb()` reads `/proc/self/statm`; `write_heartbeat_ndjson(fd, rss_kb)` formats the heartbeat NDJSON into a 96-byte stack buffer and writes via raw `write(2)` (no `fprintf`, no `fflush`, no allocation); `write_rss_limit_msg(fd)` emits the canonical `child RSS limit exceeded — split audio (ffmpeg) or raise RECMEET_RSS_LIMIT_MB` line that the daemon's existing `last_stderr_line` capture path surfaces to the user.
  - `src/main.cpp` heartbeat thread replaces the prior `write_ndjson("heartbeat", "{}")` body with the no-malloc emission + RSS self-limit; on overflow writes the stderr message and `_Exit(1)`.
  - `src/daemon.cpp` poll loop adds per-job `last_rss_kb` / `peak_rss_kb` / `last_rss_log` state; logs `info` once per minute (rate-limited, includes peak), `warn` once when RSS exceeds 8 GB (past `MemoryHigh`), `error` once when RSS exceeds 9.5 GB (approaching `MemoryMax`).
- **Tier 1B — vendored sherpa-onnx CPU arena disable** (iter 114, commit `828d6ea`, `cmake/sherpa-onnx-patches/0001-disable-cpu-arena.patch`):
  - One-line patch adds `sess_opts.DisableCpuMemArena()` to the `kCPU` branch of `GetSessionOptionsImpl` in sherpa-onnx's session factory. All 109 `Ort::Session` constructors flow through this factory, so the patch reaches diarization and speaker embedding without missing a code path.
  - Applied via FetchContent's `PATCH_COMMAND` with a `git apply --reverse --check && git apply` idempotency wrapper (because `patch -N` exits 1 on already-applied patches and FetchContent treats that as build failure on every reconfigure). Gated by `RECMEET_PATCH_SHERPA_ARENA=ON` (default ON); `make clean-deps` wipes only `build/_deps` + cmake cache for fast A/B toggle.
- **Tier 1C — identify-phase watchdog liveness + cgroup-aware kill grace** (iter 114, commit `828d6ea`):
  - **T1C.1**: `pipeline.cpp` emits `phase("identifying speakers")` and `on_progress("identifying speakers", 0/100)` brackets unconditionally; `daemon.cpp`'s poll loop tracks `last_known_phase` from `phase` events and treats incoming `heartbeat` events as the liveness signal during `last_known_phase == "identifying speakers"` (the phase has no per-segment progress callback in sherpa-onnx). Eliminates the iter-94/95 watchdog false-positive on memory-pressured hosts during the per-cluster embedding extraction.
  - **T1C.2**: `kill_pp_child_with_grace()` replaces the prior one-shot `kill(SIGTERM)` at the watchdog site: SIGTERM → 5 s grace → SIGKILL → 30 s grace → `MemoryHigh=infinity` via `systemctl --user set-property` → 30 s grace → deferred restore. Liveness uses `waitpid(WNOHANG)` so each grace iteration both detects death AND reaps the zombie atomically. Restore reads the operator-current `MemoryHigh` via `systemctl show` before bumping (so a customized drop-in setting round-trips), and branches on `LONG_MAX` to write `"infinity"` back when that was the original. Deferred while `pp_queue` or `pp_child` is non-empty so a healthy concurrent child doesn't suddenly hit reclaim throttling.
- **4-hour / 16 GB long-audio requirement codified** in `agentctx/resume.md` as a Project Constraints section, `docs/ROADMAP.md` Phase 2c "Long-Audio Containment (4 hours on 16 GB)" between Phase 2b (Live Captioning) and Phase 3 (Multi-Client Session Management), and `README.md`'s "Postprocessing memory limits" section.

### Changed

- **Daemon log defaults to `error`** (carry-over from iter 98, but re-pinned in `Environment=RECMEET_LOG_LEVEL=error` on the systemd unit alongside the cgroup caps).

### Verification

- 396 unit / 1539 assertions all pass (T1A baseline 393/1527 + 3 cases for `parse_memory_property_line`); 51 IPC integration / 339 assertions, no regressions. Full clean build: 405/405 targets, zero compiler warnings.
- **Manual gate — iter-110 audio reprocess** (the 59-min real meeting that originally triggered the OOM): T1B reduced VmPeak 21.3 GB → 12.7 GB (41 % drop, exactly the predicted arena-disable fingerprint). T1C.1 prevented the identify-phase watchdog false-positive (stayed quiet for 2 h 19 m until the RSS self-limit took over instead of killing the child at the 5-minute mark). T1A's self-limit + clean-error path fired exactly as designed; daemon survived.
- The architectural finding worth recording: **T1B reduces VmPeak, not VmRSS** — the 60-min audio's identify-speakers resident working set is intrinsically ~10 GB because sherpa-onnx's embedding extractor processes all per-speaker audio in one streaming call. Containment + diagnostics + clean errors is the right end-state behavior pending the T2 chunked-diarize work that landed in 1.4.5's groundwork.

## [1.4.3] — 2026-04-01

Portable-onnxruntime release. Eliminates the iter-99 protobuf-33→34 ABI skew crash by linking against a vendored onnxruntime 1.23.2 built from source, and splits the monolithic `recmeet_core` library into `recmeet_ipc` (no ML deps) + `recmeet_core` (ML pipeline) so `recmeet-tray` no longer drags onnxruntime / whisper / llama / sherpa into its address space — the foundation for the eventual V2 thin-client architecture.

### Added

- **`scripts/build-onnxruntime.sh`** (iter 101): shallow-clones onnxruntime v1.23.2, builds a shared library via the official `build.sh` wrapper (CPU only, Release, tests disabled), and installs to `vendor/onnxruntime-local/`. Build happens in `/tmp/onnxruntime-build` (~5 GB, disposable). Detects the header install layout (headers may land in `include/` or `include/onnxruntime/` depending on ORT version) and reports the correct `SHERPA_ONNXRUNTIME_INCLUDE_DIR` path. Three GCC-15 / Arch-rolling-release patches applied during the build: `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER` to force from-source dependencies, `--compile_no_warning_as_error` for the abseil `InlinedVector` false-positive, and `sed`-injection of `<cstdint>` into `semver.h`.
- **`make build-onnxruntime` target** (iter 101) with auto-detection of `vendor/onnxruntime-local/` exporting `SHERPA_ONNXRUNTIME_LIB_DIR` and `SHERPA_ONNXRUNTIME_INCLUDE_DIR` env vars to the sherpa-onnx subbuild. `make clean` preserves the onnxruntime install (~20-min rebuild cost); `make clean-ort` is the explicit removal target.
- **Three-tier onnxruntime detection in `cmake/prerequisites.cmake`**: (1) locally-built via env var, (2) system package via `find_library`, (3) sherpa-onnx pre-built fallback. On GCC 12+ without option 1 or 2, the build now fails with `FATAL_ERROR` — the pre-built lib is known to crash at runtime under that toolchain.
- **`recmeet_ipc` static library** (iter 104, 12 source files): `config`, `IPC`, utilities, logging, `http_client`, `device_enum`, `notify`. No ML dependencies. Links `Threads::Threads`, `PkgConfig::PULSE`, `PkgConfig::CURL`, conditionally `PkgConfig::NOTIFY`. `recmeet-tray` now links `recmeet_ipc` instead of `recmeet_core` — no onnxruntime / whisper / llama / sherpa in `ldd recmeet-tray` and `nm librecmeet_ipc.a` reports zero ML symbols.
- **`json_util.h/.cpp`** (`json_escape()`, `json_extract_string()` — pure string ops, no deps) and **`api_models.h/.cpp`** (`is_chat_model()`, `fetch_models()` — depends on `http_client.h` only) extracted from `summarize.cpp` so they land in the ML-free `recmeet_ipc` half. `summarize.h` retains backward-compat `#include` directives so existing callers (pipeline.cpp, daemon.cpp, tests) need no changes.
- **TCP transport layer** (iter 107, commit `6851e16`): dual-mode IPC supporting both Unix domain sockets (existing, default) and TCP (opt-in) for remote daemon connections. `IpcAddress` struct, `parse_ipc_address()` heuristic (last colon + all-digits = TCP), IPv6 rejection guard, non-blocking TCP connect + 5-s poll timeout (prevents GTK main-loop freeze on unreachable hosts), TCP_NODELAY, SO_KEEPALIVE with aggressive Linux tuning (30s idle, 10s interval, 3 probes = ~60s dead-connection detection), `--listen ADDRESS` / `--daemon-addr ADDRESS` flags, `RECMEET_DAEMON_ADDR` env var. 15 new tests including a TCP reconnect integration test.

### Fixed

- **`make install` produced binaries that failed at runtime with `version 'VERS_1.23.2' not found`** (iter 109). The vendored `libonnxruntime.so.1.23.2` was not installed alongside the binaries, and CMake stripped the build-time RPATH during install — the installed binary fell back to the system `/usr/lib/libonnxruntime.so.1` (v1.24.4 from Arch's `onnxruntime-cpu` package). Two `CMakeLists.txt` additions: `CMAKE_INSTALL_RPATH` set to `$ORIGIN/../lib` so all installed binaries find shared libraries relative to themselves, and an install rule that copies `vendor/onnxruntime-local/lib/libonnxruntime*.so*` to `${CMAKE_INSTALL_LIBDIR}`.

### Verification

- Clean build, zero compiler warnings.
- 386 unit tests / 1508 assertions and 51 IPC integration tests / 339 assertions all pass.
- Three full-stack pipeline tests (34 assertions) pass with zero SIGABRT — the protobuf-33→34 ABI skew crash from iter 99 is eliminated.
- `ldd recmeet-tray` shows no onnxruntime / whisper / llama / sherpa.

## [1.4.2] — 2026-03-29

Subprocess isolation + full-stack test release. Postprocessing now runs in a child subprocess via `fork`/`exec`; crashes in onnxruntime kill the child instead of the daemon, audio is preserved, and a desktop notification reports the failure. A 3-case full-stack pipeline test exercises the complete record→transcribe→diarize→identify→summarize→note flow against the 15-min debate audio fixture.

### Added

- **Subprocess postprocessing** (iter 90, `src/main.cpp:subprocess_main()`, `src/daemon.cpp:pp_worker_loop()`, +new `src/ndjson_parse.h`). Daemon fork/exec's the `recmeet` CLI binary with `--progress-json --config-json <path> --no-daemon`, reads NDJSON progress from the child's stdout pipe, and re-broadcasts to IPC clients with the existing throttle logic. `waitpid()` reaps the child and interprets exit codes (0=success, 1=error, 2=cancelled, signal=crash). Crashes detected via `waitpid()` exit status and reported via desktop notification + IPC error broadcast. Config transfers via temp JSON file (`config_to_json` / `config_from_json` round-trip) — lossless and avoids the 5-fields-have-no-CLI-flag problem. Binary discovery via `/proc/self/exe` sibling lookup, fallback to PATH. Signal handlers reset to `SIG_DFL` immediately after fork to avoid segfault from daemon's handler referencing `g_server`. 27 new tests (19 NDJSON parse, 8 config-json file-based-transfer round-trip, 5 CLI).
- **`note.tags` survives JSON round-trip** (iter 90, `src/config_json.cpp`). Added to `config_to_map()` (as comma-separated string) and `config_from_map()` — the last `Config` field not surviving subprocess transfer.
- **Pre-recording context dialog** (iter 87, commit `0769cdc`). Non-modal GTK context window (not `GtkDialog`, no `gtk_dialog_run()`) with Subject entry, Participants entry, Notes text view (word-wrap, scrolled, 80px min). Recording starts immediately on click (zero delay); the window opens alongside recording for context entry during the meeting. Tray sends `job.context` IPC with assembled context + vocabulary additions before sending `record.stop`. Daemon stores pending context in globals (`g_pending_context`, `g_pending_vocab`) under `g_context_mu`; recording worker picks it up when enqueuing the postprocessing job. `context.json` persists alongside the audio WAV in the meeting output directory; `--reprocess` auto-loads saved context as fallback. Priority chain: `context_inline` > `context_file` > saved `context.json`. CLI parity via `--context-text TEXT` flag. 13 new tests across config round-trip, CLI parse, pipeline save/load, and IPC integration.
- **Full-stack pipeline integration test** (iter 92, commit `781ebde`, `tests/test_full_stack.cpp`). Three new TEST_CASEs exercising the complete `run_postprocessing()` pipeline on the debate audio fixture and verifying all output products (WER < 0.45 against reference, 2–5 speakers with positive duration, summary headings when API key available, frontmatter metadata, note structure, context injection via `context.json`, and phase timing metrics). `PipelineResult::transcript_text` field added so tests can compute WER without parsing the note file. Shared utilities extracted from `test_benchmark.cpp`'s anonymous namespace into a new `tests/test_helpers.h` (`find_project_root`, `compute_wer`, `tokenize_words`, `strip_reference_transcript`, `BenchmarkResults`, new `strip_transcript_labels()` for `[MM:SS - MM:SS] Speaker_XX:` prefix removal). `make full-stack` target with model auto-download (`--download-models --model base`).

### Changed

- **`make test` excludes `[full-stack]`** by default; observed WER on the debate test was 24.4%.

### Verification

- 371 C++ unit (1435 assertions) + 50 IPC integration + 2 web integration + 10 device/pipeline + 7 benchmark + 3 full-stack + 93 Go = 440 total test cases.

## [1.4.1] — 2026-03-29

Vocabulary hints release. Biases whisper.cpp's decoder toward correct spellings of names and domain-specific terms — eliminates the "John Suykerbuyk → John Seck-Rick" failure mode that whisper produces phonetically on unusual proper nouns.

### Added

- **Vocabulary hints for whisper transcription** (iter 85, commit `969d0d3`):
  - New `initial_prompt` field on `TranscribeOptions` (`src/transcribe.h`), wired through `transcribe_impl()` to `whisper_full_params::initial_prompt` (`src/transcribe.cpp`).
  - New `vocabulary` field on `Config` (`src/config.h`), parsed from `transcription.vocabulary` in YAML and serialized through IPC via `config_to_map`/`config_from_map`. Vocabulary hints flow through daemon recordings.
  - `build_initial_prompt()` helper in `pipeline.h/cpp` combines enrolled speaker names (loaded via `list_speakers()` before transcription begins, when `cfg.speaker_id` is true) with user-specified vocabulary into a single comma-separated prompt string. Enrolled names are included automatically — no extra configuration needed.
  - Five new CLI commands: `--vocab WORDS` (override for single run), `--add-vocab WORD` (append to config + save), `--remove-vocab WORD`, `--list-vocab`, `--reset-vocab`. Duplicate detection prevents adding the same word twice.
- **Documentation**: README.md (feature description, pipeline explanation, CLI reference with 6 new flags, config example), QUICKSTART.md (new "Vocabulary hints" section with management guide), ARCHITECTURE.md (new "Vocabulary Hints" section with data flow, token limits, config table, IPC support, updated pipeline Mermaid diagram).

### Verification

- 331 C++ unit (1367 assertions) + 47 IPC integration + 2 web integration + 10 device/pipeline + 7 benchmark + 93 Go.
- 13 new test cases — config round-trip, IPC serialization, 6 CLI flag tests, 7 `build_initial_prompt` unit tests covering empty / speakers-only / single-speaker / vocab-only / combined / whitespace-trimming / empty-token-skipping.

## [1.4.0] — 2026-03-27

The Go tooling release. Adds a Go MCP server (`recmeet-mcp`) exposing recmeet's meeting data over the Model Context Protocol for Claude Desktop / Claude Code integration, and an AI agent CLI (`recmeet-agent`) implementing pre-meeting prep and post-meeting follow-up workflows via the Anthropic SDK. The commit message frames the release as docs-only, but the substantive work is the IMPLEMENTATION — 93 new Go tests covering all three packages.

### Added

- **`tools/` Go module** (iter 83, commit `f18c80f`, `github.com/syketech/recmeet-tools`) with three packages and two binaries.
- **`meetingdata` package — shared data access library** that reads recmeet's filesystem output. `config.go` implements a custom YAML parser matching the C++ config parser's exact behavior (flat sections, quoted value stripping, bool/int/float parsing). `notes.go` parses meeting notes including YAML frontmatter, Obsidian-style callout extraction (`> [!type]` with `> ` prefix stripping), and content search with date/participant/query filters. `meetings.go` discovers meeting directories by `YYYY-MM-DD_HH-MM` pattern, finds audio files (timestamped preferred, legacy `audio.wav` fallback), and matches notes to meetings by filename timestamp. `actionitems.go` extracts checkbox items from `## Action Items` sections (rejecting callout-nested headings, filtering "None identified"). `speakers.go` loads speaker profiles from the DB directory and per-meeting `speakers.json` (stripping embedding vectors from responses). 30 tests, 88.6% coverage.
- **`recmeet-mcp` MCP server** — five tool handlers via `mcp-go` library with stdio JSON-RPC transport: `search_meetings`, `get_meeting`, `list_action_items`, `get_speaker_profiles`, `write_context_file`. The `write_context_file` tool writes to a staging directory (`~/.local/share/recmeet/context/`) since meeting directories don't exist before recording starts. Entry point redirects `os.Stdout` to `os.Stderr` to prevent corrupting the MCP protocol stream. 23 tests, 85.0% coverage.
- **`recmeet-agent` AI agent CLI** — agentic loop using `anthropic-sdk-go` with manual tool dispatch (call Claude → execute `tool_use` blocks → feed results back → repeat until end_turn or max iterations). Seven tools: four meeting-data wrappers plus `web_search` (Brave API), `web_fetch` (HTTP + HTML-to-text via `x/net/html`), and `write_file`. Two Cobra subcommands: `prep` (pre-meeting briefing — researches participants via Brave Search, finds past meetings, synthesizes context file) and `follow-up` (post-meeting — classifies action items, drafts follow-up communications). Both support `--dry-run` mode. 40 tests, 74.3% coverage.
- **Makefile integration** — `go-build`, `go-test`, `go-coverage`, `go-clean` targets. `make clean` now also cleans Go artifacts. Both binaries build to `build/recmeet-mcp` (7.1 MB) and `build/recmeet-agent` (12.8 MB).
- **Documentation** (iter 84, commit `4679ef3`): README.md gains MCP server + AI agent bullets in "What it does", an updated architecture diagram (4 → 6 binaries), and full MCP/agent sections with 5-tool table + Claude Code/Desktop setup examples + prep/follow-up CLI reference. QUICKSTART.md adds Go binary table to Step 2 and new Steps 12–13 (MCP server setup, AI agent workflow). ARCHITECTURE.md gains a comprehensive "Go Tools Module" section with module directory structure, key dependencies, package internals, agentic-loop flowchart, and prep/follow-up sequence diagrams.

### Verification

- 318 C++ unit (1350 assertions) + 47 IPC integration + 2 web integration + 10 device/pipeline + 7 benchmark + 93 Go = 411 tests total.
- C++ build and unit tests unaffected by the Go addition.

## [1.3.6] — 2026-03-27

Graceful tray shutdown fix release. `recmeet-tray` was killed externally (SIGTERM from systemd, `make uninstall`) and the `on_quit` GTK menu callback never ran, orphaning the managed `recmeet-web` child process — the old process continued running from memory even after reinstall, serving stale code.

### Fixed

- **Graceful tray shutdown terminates managed `recmeet-web`** (iter 81, commit `dcae045`):
  - Extracted the web server teardown into `stop_web_server()` and moved all cleanup (IPC teardown, `stop_web_server()`, timer removal) from `on_quit` to the post-`gtk_main()` block, which runs regardless of how the main loop exits. Simplified `on_quit` to just `gtk_main_quit()`.
  - Added `g_unix_signal_add(SIGTERM/SIGINT)` handlers (via `<glib-unix.h>`) that call `gtk_main_quit()` from the GLib main loop — the GTK-recommended signal-safe pattern using an internal pipe to wake the main thread.
- **WebUI reprocess JSON parse error** (iter 80, commit `1a10d74`). `/api/meetings/:dir/reprocess` extracted `job_id` from the daemon IPC response using `json_val_as_string()`, but the daemon returns `job_id` as `int64_t` — produced malformed JSON `{"ok":true,"job_id":}`. Switched to `json_val_as_int()` + `std::to_string()`.
- **Enroll cleared sibling input fields** (iter 80, commit `1a10d74`). After enrolling a speaker, `loadMeetingSpeakers()` rebuilt the entire container via `container.innerHTML = ''`, wiping unsaved names typed into other input fields. Fixed by collecting non-empty input values (keyed by `cluster_id` via `data-cluster-id`) before clearing, then restoring them after rebuild. Also covers the relabel path.

## [1.3.5] — 2026-03-23

Local-LLM stability + concurrent-postprocessing fix release. Closes the swap-thrashing failure that made the workstation unresponsive for minutes during local summarization, plus three smaller fixes shaken out by real-world meeting recording.

### Fixed

- **Disable mmap for LLM model loading** (iter 79, commit `1cb0672`). llama.cpp defaults to `use_mmap=true` for model loading, which memory-maps the entire GGUF file with `MAP_SHARED`. With `use_mlock=false` (also the default), the kernel treats these file-backed pages as evictable and swaps them out even when physical RAM is free — classic mmap thrashing. Whisper.cpp was confirmed innocent (uses `std::ifstream::read()` into heap). Set `model_params.use_mmap = false` in `summarize_local()`, defaulting to heap-based loading. Added `bool llm_mmap` to `Config` (default `false`), `--mmap`/`--no-mmap` CLI flags, YAML config support (`summary.llm_mmap`), and IPC serialization.
- **CLI hang on empty transcript** (iter 78, commit `d433923`). When reprocessing a silence-only WAV, the daemon's postprocessing worker threw `RecmeetError("Transcription produced no text.")`, broadcast as `state.changed` with an error field. The CLI callback received it, printed the error, and called `close_connection()` — but `read_and_dispatch()` did not check for a closed fd before calling `poll()` with `timeout_ms = -1`, so `poll()` blocked forever ignoring the negative fd entry. CLI hung until Ctrl+C. Fixed by adding `if (fd_ < 0) return false;` in `read_and_dispatch()` after the first buffered-line processing loop (where callbacks may fire and close the connection).
- **Tray stop cancelling concurrent postprocessing** (iter 76, commit `d5e27ae`). When a second recording was started from the tray while the first meeting was still postprocessing, clicking "Stop Recording" on the second recording cancelled the first meeting's postprocessing. Root cause: `on_stop()` sent `record.stop` with `target="all"` whenever both `g_tray.recording` and `g_tray.postprocessing` were true — but with concurrent recording these flags can be true simultaneously from *different* jobs. Fixed by always sending `target="recording"` when `g_tray.recording` is true; the separate `on_cancel_pp()` handler (attached to "Cancel Processing") handles explicit postprocessing cancellation independently.
- **mpg123 stderr noise during test suite** (iter 74, commit `10adb92`). Spurious "Illegal Audio-MPEG-Header 0x00000000 at offset 9" messages printed during `make test` came from the `validate_reprocess_input` MP3 test case. The function called `sf_open()` on the file before checking the extension; libsndfile's mpg123 backend emitted diagnostic warnings to stderr before returning failure. Fixed by moving the file extension check for known unsupported formats (`.mp3`, `.m4a`, `.aac`, `.wma`, `.opus`) before the `sf_open()` call.

### Added

- **Batch re-identify speakers** (iter 75, commit `cd20990`). `POST /api/speakers/batch-reidentify` endpoint and "Batch Re-identify" WebUI button update all past meetings' speaker labels using updated speaker profiles, without re-running diarization or needing audio/model files. New `re_identify_meeting()` (`src/speaker_id.cpp`, +90 lines): infers dimension from the first non-empty meeting embedding, registers all DB embeddings in a fresh `SherpaOnnxSpeakerEmbeddingManager`, queries `GetBestMatches` for each non-manual speaker (confidence != 1.0), resolves conflicts by sorting candidates by score desc and assigning greedily. Manual labels (confidence == 1.0) are always preserved. 14 new tests (10 unit + 4 web endpoint).
- **README: local summarization documentation** (iter 77, commit `c1e1a74`). New "Summarization" section covering: cloud API setup (3 built-in providers with env vars and default models), local LLM setup (download GGUF, CLI flag, config file, tray file chooser), recommended models table (Qwen2.5 7B, Mistral 7B, Llama 3 8B, Phi-3 Mini), decision logic (how recmeet chooses local vs cloud vs skip), cloud-vs-local trade-offs table (privacy, cost, speed, quality, setup, network), and constraints note (CPU-only, 32K context, 4096 generation budget).

## [1.3.4] — 2026-03-10

WebUI speaker correction release. Closes the ~25% sibling-misidentification rate that the WebUI surfaced but provided no tools to fix — the WebUI previously supported enrollment but no way to correct mistakes, remove bad embeddings, or re-diarize with a speaker-count hint.

### Added

- **Four speaker-correction features (F1–F4)** (iter 72, commit `95cb16e`):
  - **F1: Relabel speaker in meeting** — `POST /api/meetings/:dir/speakers/relabel`. When `update_profile` is true (default), removes the embedding from the old speaker's profile and appends it to the new speaker's profile (creating if needed). The key correction flow: wrong identification → relabel → embedding moves to correct profile. Frontend: identified speakers in meeting detail show a "Relabel" button revealing inline edit with name input, "Update profiles" checkbox, Save/Cancel.
  - **F2: Remove an enrollment from a speaker profile** — `POST /api/speakers/:name/remove-embedding`. Removes an embedding by index. Validates bounds, deletes the profile file if last embedding removed, returns remaining count. Frontend: speaker detail view shows individual enrollments with "Remove" buttons.
  - **F3: Reprocess a meeting with a num_speakers hint** — `POST /api/meetings/:dir/reprocess`. Connects to the daemon via `IpcClient` and sends `record.start` with `reprocess_dir` and optional `num_speakers` override. Returns `job_id` on success, 409 if daemon busy, 502 if daemon unreachable. Frontend: meeting cards have a "Reprocess" button opening a modal dialog with `num_speakers` input.
  - **F4: Enrollment quality gate** — rejects enrollments with `duration_sec < 5.0` (400 error with explanation); adds a low-confidence warning (`confidence < 0.5`). Response now includes `duration_sec` and `confidence` fields. Confidence badges (green ≥70%, yellow ≥50%, red <50%) shown next to each meeting speaker in the WebUI.
- **Backend helpers** in `src/speaker_id.{h,cpp}`: `remove_embedding(db_dir, name, embedding, epsilon)` removes a specific embedding from a speaker profile by L2 distance matching (threshold = `epsilon² × dim`). `relabel_meeting_speaker(meeting_dir, cluster_id, new_label, confidence)` updates label, identified flag, and confidence in a meeting's `speakers.json`.
- **25 new tests** across `speaker_id` unit suite (12 — `remove_embedding` exact match / epsilon tolerance / last-embedding-deletes-profile / dimension mismatch; `relabel_meeting_speaker` happy path / unknown cluster_id) and web endpoint suite (13 — remove-embedding bounds, 404, 400 paths; relabel with profile-move; integration of relabel-then-list).

### Verification

- 300 unit (1286 assertions) + 47 IPC integration (307 assertions) + 2 web integration (13 assertions) + 10 device/pipeline integration + 7 benchmark = 359 non-benchmark test cases, 1632 assertions. Zero compiler warnings.

## [1.3.3] — 2026-03-10

Concurrent recording release. The daemon previously treated the entire pipeline (record → transcribe → diarize → summarize) as a single atomic job — `record.start` returned `Busy` during postprocessing, blocking users from recording new meetings while a previous one was being processed. After two architecture-review revisions, this release lets users start a new recording while a prior meeting's postprocessing is still running.

### Added

- **Concurrent recording + postprocessing** (iter 71, commit `9b7b887`):
  - **State model**: replaced the single `DaemonState` enum and `g_state` atomic with three independent atomic booleans (`g_recording`, `g_postprocessing`, `g_downloading`) guarded by `g_state_mu` mutex for multi-flag transitions.
  - **Worker topology**: separated the single `g_worker` thread into `g_rec_worker` (recording only), `g_pp_worker` (long-lived thread that blocks on `g_queue_cv` and drains a `PostprocessJob` queue), and `g_dl_worker` (model downloads). Separated `g_stop` into `g_rec_stop` and `g_pp_stop` for independent cancellation.
  - **Atomic recording → postprocessing handoff**: the recording worker sets `g_postprocessing=true` BEFORE clearing `g_recording` under `g_state_mu`, preventing any transient idle state visible to clients. The pp worker's redundant set+broadcast is suppressed via an `already_flagged` check.
  - **IPC protocol extensions** (backward-compatible): `state.changed` events now include boolean fields (`recording`, `postprocessing`, `downloading`) alongside the `state` string; `record.start` returns a monotonic int64 `job_id`; `record.stop` accepts optional `target` param (`"recording"`, `"postprocessing"`, `"all"`; default `"all"` for backward compat); `status.get` includes `queue_depth` and boolean fields.
  - **Tray UX**: tray.cpp boolean state (`bool recording, postprocessing, downloading, is_reprocess`) replaces the single `State` enum. Menu shows concurrent state labels ("Recording... (processing previous)"); during postprocessing-only shows both "Record" and "Cancel Processing" buttons. `on_record()` guards on `recording || downloading` (allows starting during postprocessing).
  - **CLI**: `client_record()` captures `job_id` from `record.start` and filters `job.complete` events by it, so concurrent completions from other clients don't cause premature exit. `client_status()` displays `queue_depth` when > 0.
- **16 new `[concurrent]` integration tests** covering `record.start` allowed during postprocessing, `job_id` round-trip, `state.changed` boolean fields, no-transient-idle invariant, `target=recording`/`postprocessing`-specific cancellation, `models.ensure` allowed during postprocessing, error isolation, queued-jobs-in-order, and shutdown with queued jobs.

### Architecture review (iter 70, pre-implementation)

- **5 fixes folded into the plan revision before implementation began**:
  - Atomic recording→postprocessing handoff (critical) — eliminated the transient "idle" visible to clients.
  - Handler signature mismatch (critical) — all pseudocode used the wrong return type and `make_error()` calls; the actual `MethodHandler` signature is `bool(const IpcRequest&, IpcResponse&, IpcError&)`.
  - CLI Ctrl+C behavior (high) — default `record.stop` (no target) = `"all"` to match current behavior; tray uses explicit `target` for granular control.
  - Dropped `g_reprocessing` from IPC protocol (medium) — derived from the `state` string instead; booleans in IPC are authoritative for guards only.
  - Monotonic `job_id` for multi-client disambiguation (medium) — `job.complete` events are ambiguous when multiple clients initiate recordings; CLI filters by expected ID.

### Verification

- 275 unit (1206 assertions) + 47 IPC integration (307 assertions) = 332 test cases, 1539 assertions. Zero compiler warnings. 15× stress run with no flakiness.

## [1.3.2] — 2026-03-10

Multi-provider API key storage + tray prompt release. The first documented release in this changelog. Adds per-provider API key storage with a GTK dialog prompt and a `resolve_api_key` priority chain so users can switch between xAI, OpenAI, and Anthropic summarization without editing config files.

### Added

- **Multi-provider API key storage** (iter 69, commit `ec4c8d9`):
  - New `api_keys:` config section storing one key per provider (xAI, OpenAI, Anthropic). Keys stored at `~/.config/recmeet/api_keys.yaml` with `0600` file permissions.
  - GTK dialog prompt on first use of an unconfigured provider (Tray "Configure API Key…" menu item; also surfaces automatically when a recording would otherwise skip summarization for lack of a key).
  - `resolve_api_key(provider)` priority chain: env var (`XAI_API_KEY`, `OPENAI_API_KEY`, `ANTHROPIC_API_KEY`) → `api_keys.yaml` per-provider → legacy `api_key:` field (deprecated but accepted). Daemon, CLI, and tray all use the same resolution.
- **Tray-managed `recmeet-web` lifecycle** (iter 66, commit `e8d793a`). Tray spawns/reaps `recmeet-web` on demand; new `/api/health` endpoint for liveness checks.
- **Speaker WebUI completion** (iter 65, commit `afdb140`). Frontend detail view + tray "Speaker Management" menu item.
- **Transcription progress reporting + cancel during postprocessing** (iter 62, commit `8ecef03`). Progress callbacks plumb through whisper → pipeline → daemon → CLI/tray; cancel works during postprocessing, not just recording.
- **Reprocess robustness — file input, format validation, graceful errors** (iter 61, commit `b84cb4d`). `validate_reprocess_input()` accepts file or directory input, validates format, and emits ffmpeg conversion hints for unsupported formats.
- **Timestamped audio filenames** (iter 60, commit `19f3766`). `audio.wav` → `audio_YYYY-MM-DD_HH-MM.wav`. New `OutputDir` + `find_audio_file()` helper preserves backward compatibility with legacy `audio.wav`.
- **Speaker WebUI Phase 2 — `recmeet-web` REST API server** (iter 58, commit `6048a44`). Speaker CRUD + meeting browsing via REST endpoints; static assets installed via new `share/web/` install rules (iter 57 + commit `2c47a98`).
- **Cross-session speaker identification via voiceprint enrollment** (iter 56, commit `cb57bcf`). `--enroll NAME --from DIR/`, `--speakers`, `--identify DIR/`, `--reset-speakers` CLI commands; per-speaker embedding profiles in `~/.local/share/recmeet/speakers/`.

### Fixed

- **Daemon `StopToken` not reset between recording and postprocessing** (iter 67, commit `6137c74`). Daemon postprocessing was cancelled immediately after every recording because `StopToken` carried state from the recording stop into the postprocessing worker.
- **IPC client event-loop hangs on job completion and pipeline errors** (iter 63, commit `2baf3e0`). `read_events` until_event matching; CLI now exits cleanly on error and job completion.
- **Reprocess output directory** (iter 64, commit `31efdce`). Relative path resolution; reuse audio parent dir instead of creating a new timestamped dir.
- **Note path / timestamp / dir** (iter 55, commit `27240c7`). Canonical paths, `resolve_meeting_time()` timestamp preservation, YYYY/MM/ note subdirectories.
- **IPC state sync bugs, CLI test isolation, 18 integration tests** (iter 49, commit `6fa42b0`). 3 IPC bug fixes + the first 18 IPC integration tests landing.

### Documentation

- **README + QUICKSTART sync** (iter 54, commit `c7c306b`). Test counts, `onnxruntime-cpu` deps, config examples.
- **ROADMAP.md + live captioning feasibility study** (iter 48, commit `8595a57`). First sketch of the future captioning architecture; foundational document for the eventual 1.5.0 capstone.
