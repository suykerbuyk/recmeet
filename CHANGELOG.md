# Changelog

All notable changes to recmeet are documented in this file. The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.5.0] — 2026-05-09

The V1 capstone release. Live captioning lands as the final V1 feature, completing the V1 line at version 1.5.0 (following 1.3.x and 1.4.x). The `v1-maintenance` branch is cut from this tag and V2 work begins on `main` thereafter; future V1 patch releases ship as `v1.5.x` from `v1-maintenance`.

### Added

- **Live captioning** (Phases 1–6 of the V1 capstone, iter-???):
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
