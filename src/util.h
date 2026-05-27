// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

namespace recmeet {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------

class RecmeetError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class DeviceError : public RecmeetError {
    using RecmeetError::RecmeetError;
};

class AudioValidationError : public RecmeetError {
    using RecmeetError::RecmeetError;
};

// ---------------------------------------------------------------------------
// Stop token — shared between signal handler, tray UI, and capture threads
// ---------------------------------------------------------------------------

struct StopToken {
    std::atomic<bool> requested{false};

    void request() { requested.store(true, std::memory_order_release); }
    bool stop_requested() const { return requested.load(std::memory_order_acquire); }
    void reset() { requested.store(false, std::memory_order_release); }
};

// ---------------------------------------------------------------------------
// Audio constants
// ---------------------------------------------------------------------------

constexpr int SAMPLE_RATE = 16000;
constexpr int CHANNELS = 1;
constexpr int SAMPLE_BITS = 16;
constexpr int BYTES_PER_SAMPLE = SAMPLE_BITS / 8;
constexpr int BYTES_PER_SEC = SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE;

// ---------------------------------------------------------------------------
// Path helpers (XDG-compliant)
// ---------------------------------------------------------------------------

/// ~/.config/recmeet/
fs::path config_dir();

/// ~/.local/share/recmeet/
fs::path data_dir();

/// ~/.local/state/recmeet/  (Phase D.5 — XDG state dir; mirrors
/// `config_dir()` / `data_dir()` with `XDG_STATE_HOME` env var and
/// `.local/state` fallback. Used for the per-server resume_token store
/// (`session.tokens.json`).
fs::path state_dir();

/// ~/.local/share/recmeet/models/
fs::path models_dir();

// ---------------------------------------------------------------------------
// V2 split path helpers (v2-coexistence-with-v1, Phase 1)
//
// V1 and V2 must coexist on the same machine at the default user-prefix
// install. V2 binaries (recmeet-server / recmeet-client / recmeet-cli) use
// per-side XDG roots so a V1 install at `~/.config/recmeet/` is never
// touched. The helpers below honor `XDG_CONFIG_HOME` / `XDG_DATA_HOME` /
// `XDG_STATE_HOME` / `XDG_RUNTIME_DIR` with the standard `~/.config` /
// `~/.local/share` / `~/.local/state` fallbacks. See
// agentctx/tasks/v2-coexistence-with-v1.md for the design.
// ---------------------------------------------------------------------------

/// ~/.config/recmeet-server/
fs::path server_config_dir();

/// ~/.config/recmeet-client/
fs::path client_config_dir();

/// ~/.local/share/recmeet-server/
fs::path server_data_dir();

/// ~/.local/share/recmeet-client/
fs::path client_data_dir();

/// ~/.local/state/recmeet-server/
fs::path server_state_dir();

/// $XDG_RUNTIME_DIR/recmeet-server/  (no on-disk fallback — runtime dir is
/// required for socket placement; callers handle the empty-path case).
fs::path server_runtime_dir();

/// `server_runtime_dir() / "server.sock"` as a string for IPC bind/connect.
std::string server_socket_path();

// ---------------------------------------------------------------------------
// Audio file naming
// ---------------------------------------------------------------------------

constexpr const char* AUDIO_PREFIX = "audio_";
constexpr const char* LEGACY_AUDIO_NAME = "audio.wav";
constexpr const char* CONTEXT_PREFIX = "context_";
constexpr const char* LEGACY_CONTEXT_NAME = "context.json";
constexpr const char* SPEAKERS_PREFIX = "speakers_";
constexpr const char* LEGACY_SPEAKERS_NAME = "speakers.json";

/// Find the audio file in a meeting directory.
/// Prefers audio_YYYY-MM-DD_HH-MM.wav, falls back to audio.wav.
/// Returns empty path if dir doesn't exist or no audio file found.
fs::path find_audio_file(const fs::path& dir);

/// Find the context.json file in a meeting directory.
/// Prefers context_YYYY-MM-DD_HH-MM.json, falls back to context.json.
/// Returns empty path if dir doesn't exist or no matching file found.
fs::path find_context_file(const fs::path& dir);

/// Find the speakers.json file in a meeting directory.
/// Prefers speakers_YYYY-MM-DD_HH-MM.json, falls back to speakers.json.
/// Returns empty path if dir doesn't exist or no matching file found.
fs::path find_speakers_file(const fs::path& dir);

/// Derive the YYYY-MM-DD_HH-MM timestamp for a meeting directory.
/// Strategy:
///   1. If dir.filename() matches the YYYY-MM-DD_HH-MM pattern, return it
///      (collision suffix `_N` stripped).
///   2. Otherwise, parse the discovered audio filename via find_audio_file()
///      (strip "audio_" prefix and ".wav" suffix).
///   3. Otherwise, return "".
std::string derive_meeting_timestamp(const fs::path& dir);

// ---------------------------------------------------------------------------
// Output directory creation
// ---------------------------------------------------------------------------

/// Result of create_output_dir: the directory path and the clean timestamp
/// (without any collision suffix).
struct OutputDir {
    fs::path path;
    std::string timestamp;  // Always "YYYY-MM-DD_HH-MM" (no collision suffix)
};

/// Create timestamped output directory under base_dir, e.g. meetings/2026-02-20_14-30/
OutputDir create_output_dir(const fs::path& base_dir);

// ---------------------------------------------------------------------------
// Default device pattern
// ---------------------------------------------------------------------------

constexpr const char* DEFAULT_DEVICE_PATTERN = "";

// ---------------------------------------------------------------------------
// File writing helper
// ---------------------------------------------------------------------------

/// Write text content to a file, throwing RecmeetError on failure.
void write_text_file(const fs::path& path, const std::string& content);

// ---------------------------------------------------------------------------
// Atomic file write (Phase D.5)
// ---------------------------------------------------------------------------

/// Atomically write `bytes` to `path` with the durability contract used by
/// the C.11.4 staging→meeting WAV relocation primitive (`atomic_relocate`):
///
///   1. write to `<path>.tmp`
///   2. `fsync(file_fd)` so the bytes are durable
///   3. `rename(<path>.tmp, <path>)` — atomic within a single filesystem
///   4. `fsync(parent_dir_fd)` so the rename entry is durable
///
/// EXDEV cross-filesystem case (e.g. tmpfs `path.tmp` next to ext4 `path` is
/// not the typical layout for D.5's small JSON files, but EXDEV is handled
/// transparently because step 1 writes the tmp alongside the final path —
/// the parent dir is identical so EXDEV cannot occur). If `mode != 0` the
/// final file is `chmod()`'d to that mode after rename (before the dir
/// fsync) — used by the resume_token store to enforce 0600 secrecy.
///
/// Throws `RecmeetError` on any filesystem failure; the partial `.tmp`
/// file is removed best-effort on the failure path so the next attempt
/// starts from a clean directory. Concurrent readers of `path` always
/// observe either the prior bytes or the new bytes — never a partial
/// write (this is the invariant the journal + resume_token store inherit
/// from the C.11.4 atomic-write surface).
void atomic_write_file(const fs::path& path,
                       const std::string& bytes,
                       int mode = 0);

// ---------------------------------------------------------------------------
// Thread count helper
// ---------------------------------------------------------------------------

/// Default thread count for inference engines: hardware_concurrency() - 1, minimum 1.
int default_thread_count();

// ---------------------------------------------------------------------------
// Process resident-set-size (Linux)
// ---------------------------------------------------------------------------

/// Read this process's resident set size in kilobytes from /proc/self/statm.
/// Returns 0 on read failure (non-Linux, transient errors, sandboxed /proc).
/// Does not allocate beyond a fixed-size stack buffer in the underlying read.
long read_self_rss_kb();

/// Format a heartbeat NDJSON line for `rss_kb` into a stack buffer and write
/// it to `fd` via raw write(2). Does not allocate, does not take the libc
/// stdio mutex - safe to call from a heartbeat thread under malloc-arena
/// lock contention. Returns the number of bytes written, or 0 on format
/// failure / partial write to a closed fd.
size_t write_heartbeat_ndjson(int fd, long rss_kb);

/// Write the canonical RSS-limit-exceeded stderr line to `fd` via raw
/// write(2). The string is fixed and distinctive ("child RSS limit
/// exceeded") so the daemon's last_stderr_line capture can surface it
/// in the user-facing error.
void write_rss_limit_msg(int fd);

/// Extract date/time from a directory name matching "YYYY-MM-DD_HH-MM" pattern.
/// Falls back to file mtime of audio_path, then to current time.
/// Returns {date_str, time_str} like {"2026-02-15", "14:30"}.
std::pair<std::string, std::string> resolve_meeting_time(
    const fs::path& out_dir, const fs::path& audio_path);

// ---------------------------------------------------------------------------
// meeting_id validation (Phase C.11)
// ---------------------------------------------------------------------------

/// Validate a `meeting_id` string. Returns true for an empty string (sentinel
/// for "no id" — used by v1-written context.json and pre-C.11 IPC requests)
/// OR a canonical lowercase UUID v4 (36 chars, layout 8-4-4-4-12 hex with
/// hyphens at offsets 8/13/18/23, version nibble = 4 at offset 14, variant
/// high bits = 10 at offset 19). Any other input — wrong length, wrong hyphen
/// positions, uppercase hex, non-hex character, wrong version, wrong variant
/// — returns false. Server-side check on every C.11 verb; rejecting malformed
/// IDs at the wire boundary prevents them from poisoning the in-memory
/// `meeting_id → meeting_dir_path` index.
bool is_valid_meeting_id(const std::string& s);

// ---------------------------------------------------------------------------
// systemd property line parser (T1C.2)
// ---------------------------------------------------------------------------

/// Parse a single line of `systemctl show -p MemoryX` output, of the form
/// "MemoryHigh=<value>\n". Returns LONG_MAX for "infinity", >=0 for byte
/// counts, or -1 on malformed input. Pure parsing — no I/O. Exposed for
/// unit testing of the daemon's MemoryHigh restore path.
long parse_memory_property_line(const char* line);

} // namespace recmeet
