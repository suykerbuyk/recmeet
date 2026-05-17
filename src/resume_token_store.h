// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.5 — client-side resume_token store.
//
// Persists a `{server_address: resume_token}` map at
// `$XDG_STATE_HOME/recmeet/session.tokens.json` (fallback
// `~/.local/state/recmeet/session.tokens.json`). The tray:
//
//   1. Before each `IpcClient::connect()`, calls `get(server_address)`
//      and threads the result into `connect(psk, resume_token)`.
//   2. After connect returns OK, calls `IpcClient::resume_token()` and,
//      if non-empty, `put(server_address, token)` so the next restart
//      resumes the same daemon-side session (and recovers the same
//      `client_id` per C.13).
//
// Keyed by `server_address` so multi-server (multi-server hook #1) lands
// without a schema migration when E.2 (f) wires the plural `servers: [...]`
// shape. v1 today only has one entry but the file format is plural-from-
// day-one — `{"<addr>": "<token>", ...}`, NOT a bare token string.
//
// Atomic-write contract: every mutation goes through
// `util::atomic_write_file(path, bytes, 0600)`, inheriting the
// write-tmp + `fsync` + `rename` + `fsync(parent)` durability tail from
// C.11.4. Mode 0600 enforces secrecy on shared hosts — the resume_token
// is auth material on the server.
//
// In-memory state is a snapshot loaded lazily on the first `get`/`put`
// (so a `put` on a missing file does NOT clobber other entries written by
// a concurrent process — we re-read before mutating). The class is NOT
// thread-safe; the tray is single-threaded for IPC traffic, and the
// snapshot/persist round-trip happens on the GTK main thread.

#pragma once

#include "util.h"

#include <map>
#include <optional>
#include <string>

namespace recmeet {

class ResumeTokenStore {
public:
    /// Use the default `~/.local/state/recmeet/session.tokens.json` path.
    ResumeTokenStore();

    /// Custom path — exposed for unit tests so each `TEST_CASE` writes
    /// to its own scratch path under `/tmp` and the production state
    /// dir is never touched.
    explicit ResumeTokenStore(fs::path path);

    /// Return the cached token for `server_address`, or `std::nullopt`
    /// if no token has been persisted for that address. Reads from disk
    /// on first call; subsequent calls in the same process are served
    /// from the in-memory map unless `reload()` is called explicitly.
    std::optional<std::string> get(const std::string& server_address);

    /// Persist a token for `server_address`. The full map is rewritten
    /// atomically — concurrent processes write-then-overwrite is
    /// last-writer-wins, but each individual write is crash-safe and
    /// either observed in full or not observed at all by readers.
    /// Calling `put` with a token equal to the existing cached value is
    /// idempotent and skips the disk write.
    void put(const std::string& server_address, const std::string& token);

    /// Remove an entry from the map and re-persist. Idempotent — a
    /// `erase` of a non-existent address is a no-op (no disk write).
    void erase(const std::string& server_address);

    /// Drop the in-memory snapshot and force the next `get`/`put` to
    /// re-read from disk. Exposed for tests that pre-populate the file
    /// out-of-band.
    void reload();

    /// Path the store reads/writes. Useful for tests that need to assert
    /// the file's existence / mode without computing the XDG path
    /// themselves.
    const fs::path& path() const { return path_; }

private:
    void load_from_disk();
    std::string serialize() const;

    fs::path path_;
    std::map<std::string, std::string> entries_;
    bool loaded_ = false;
};

} // namespace recmeet
