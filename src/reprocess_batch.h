// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include "cli.h"
#include "config.h"
#include "util.h"

#include <atomic>
#include <string>
#include <vector>

namespace recmeet {

class IpcClient;

/// Classification of a candidate meeting subdirectory under the parent
/// reprocess-batch dir. Drives both `--dry-run` reporting and the
/// per-iteration dispatch decision.
enum class BatchEntryKind {
    WillReprocess,      ///< matches naming pattern, has audio, no existing note
    SkipNoteExists,     ///< matches naming pattern, has audio, note already on disk
    SkipNoAudio,        ///< matches naming pattern but no audio file present
};

struct BatchEntry {
    fs::path dir;                  ///< absolute path to the meeting directory
    std::string timestamp;         ///< canonical "YYYY-MM-DD_HH-MM" (collision suffix stripped)
    BatchEntryKind kind = BatchEntryKind::SkipNoAudio;
};

/// Per-iteration outcome reported by `dispatch_one_reprocess`. Used by the
/// loop to assemble the final summary tally and to detect daemon-disappearance
/// mid-batch.
struct Outcome {
    enum class Kind {
        Ok,
        Skipped,
        Failed,
        DaemonUnreachable,
        Cancelled,         ///< Phase 3 SIGINT path
    };
    Kind kind = Kind::Failed;
    std::string error_message;
    fs::path note_path;
    double duration_seconds = 0.0;
};

/// Locked-once dispatcher mode for the batch run (resolved at batch entry from
/// `cli.daemon_mode` + `daemon_running(addr)` and held constant through every
/// iteration).
enum class BatchDispatchMode { Daemon, Standalone };

/// Enumerate immediate subdirectories of `parent_dir` matching
/// `^YYYY-MM-DD_HH-MM(_N)?$`, classify each as WillReprocess / SkipNoteExists
/// / SkipNoAudio, and return them sorted chronologically by timestamp. The
/// classification consults `cfg.note_dir` and `find_audio_file()` exactly as
/// the production pipeline does. Exposed for unit testing.
std::vector<BatchEntry> classify_batch_entries(
    const fs::path& parent_dir, const JobConfig& cfg);

/// Atomic pointer to the live IpcClient driving the in-flight daemon-mode
/// iteration, or nullptr otherwise. Set by `client_record_no_sigaction` while
/// a daemon-mode reprocess upload is in flight; cleared on return. Kept after
/// the Phase E.1-fix migration off `record.start` because the
/// `process.submit` upload loop still needs an externally-visible client
/// handle for the SIGINT path's `close_connection()` call (which unblocks
/// any in-progress read and lets the call site shut down promptly). NEVER
/// used to call daemon verbs from a signal handler (v2: daemon has no
/// `record.stop`); see `g_active_iter_stop` for the cancellation flag.
/// Acquire/release semantics; no mutexes — signal-handler safety.
extern std::atomic<IpcClient*> g_active_ipc_client;

/// Phase E.1-fix — atomic pointer to the per-iteration StopToken published by
/// `client_record_no_sigaction` for the duration of an in-flight daemon-mode
/// upload. SIGINT/SIGTERM handlers (single-meeting CLI + batch driver) flip
/// the token via `StopToken::request()`; the upload loop polls it between
/// chunks to break out cleanly. Set to nullptr outside the upload window.
extern std::atomic<StopToken*> g_active_client_stop;

/// Test-only hooks for Phase 3 SIGINT plumbing. Not part of the public
/// API surface; declared here so the Catch2 unit test can invoke the
/// handler in-process and inspect the atomics without poking file-static
/// state. Production callers go through signal delivery, not these.
/// (Renamed to avoid colliding with the `extern "C"` production handler
/// of the same base name in the same translation unit.)
namespace test_hooks {
/// `extern "C"` so it can be passed directly as `sa.sa_handler`. Forwards
/// to the production `batch_sigint_handler` in this TU.
extern "C" void test_batch_sigint_handler(int);
void set_active_iter_stop(StopToken* tok);
bool batch_stop_requested();
void reset_batch_stop_requested();
} // namespace test_hooks

/// Sentinel exit code returned by `client_record_no_sigaction` when the IPC
/// connect() call fails. Mapped to `Outcome::DaemonUnreachable` by the batch
/// dispatcher (which aborts the batch). The single-meeting wrapper masks
/// this to a generic 1.
constexpr int kClientConnectFailedExitCode = 2;

/// IPC body shared between the single-meeting `client_record` wrapper and
/// the reprocess-batch dispatcher's daemon-mode path. Does NOT install
/// signal handlers (callers do that around it). Returns 0 on success, 1 on
/// iteration error, or `kClientConnectFailedExitCode` (2) on connect failure.
///
/// `show_captions_on_stderr` (Phase 5.2) — when true, `caption` and
/// `caption.degraded` events are rendered to stderr alongside phase / progress
/// lines. Reads `cfg.caption_normalize_display` to decide whether to apply
/// `normalize_caption()` before printing. Defaults to false; the
/// reprocess-batch driver leaves it off (no captions during reprocess).
int client_record_no_sigaction(const JobConfig& cfg, const std::string& addr,
                               bool show_captions_on_stderr = false);

/// Once-per-batch model precheck. Validates that all models any iteration
/// might need (whisper, sherpa, VAD, llama, summary readiness) are cached
/// or otherwise satisfied. Returns the empty string on success, or a
/// human-readable error message on the first failure. Stays in main.cpp
/// (no new file); declared here for the batch dispatcher's call site.
std::string ensure_models_cached_or_fail(const JobConfig& cfg);

/// Run the reprocess batch for the given CLI invocation. Returns the process
/// exit code:
///   0   — all meetings ok or all skipped
///   1   — at least one iteration failed, or fail-fast (model precheck,
///         non-existent parent dir, daemon disappearance)
///   130 — SIGINT (clean shutdown convention; Phase 3 wires this)
int run_reprocess_batch(const CliResult& cli);

} // namespace recmeet
