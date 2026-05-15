// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.4 — artifact enumeration for `process.fetch`.
//
// `process.fetch` is the sibling of `process.submit` (C.2): once a postprocess
// job has reached `JobState::Done`, the originating client retrieves the
// artifacts produced under `Job::input.out_dir`. The wire shape is an NDJSON
// metadata response followed by N `0x02` BinaryArtifact frames, one per
// listed artifact, in the same order as the metadata array.
//
// This header owns the pure "what to ship" decision so tests can exercise the
// filter without standing up a full JobQueue + IPC server.
//
// ---------------------------------------------------------------------------
// Filter policy (C.4 decision — must match the test fixture set in
// tests/test_fetch.cpp):
//
//   INCLUDED  — regular files in `out_dir` whose extension is one of:
//                 * `.md`   — the meeting note (`Meeting_YYYY-MM-DD_HH-MM[_Title].md`)
//                 * `.vtt`  — captions sidecar (`captions.vtt` from streaming /
//                             live caption fan-out)
//
//   EXCLUDED  — everything else, specifically:
//                 * Hidden files (basename starts with `.`).
//                 * Subdirectories (we never recurse — artifacts live flat in
//                   `out_dir`; meeting subdirs are for organization).
//                 * Audio: any file whose extension is `.wav`, `.flac`,
//                   `.mp3`, `.m4a`, `.ogg` (the upload-driven `audio.<ext>`
//                   staging file from C.2, and the legacy `audio_<ts>.wav` /
//                   `mic.wav` / `monitor.wav` from the record.start path).
//                   The client uploaded the audio in the first place; it does
//                   not need it back.
//                 * Speaker DB files: `.json` named `speakers.json` /
//                   `speakers_*.json`. The speaker DB is server-resident per
//                   the v2 architecture (decided question #1 in the plan).
//                 * Context files: `.json` named `context.json` /
//                   `context_*.json`. These are inputs, not artifacts.
//                 * Any other extension we do not explicitly allowlist —
//                   safer than a blocklist because future pipeline tweaks
//                   (e.g. an LLM-debug `.log`) do not silently leak.
//
// The allowlist is intentionally narrow. If a future artifact type lands in
// `out_dir` (e.g. a `.json` summary), it must be added here explicitly.
// ---------------------------------------------------------------------------
//
// Size cap: each artifact rides one `0x02` frame; C.1's default frame cap is
// `kDefaultMaxBinaryFrameBytes` (16 MiB). An artifact larger than the server's
// `max_binary_frame_bytes` cap is rejected at the `process.fetch` handler
// rather than chunked — chunking is deferred to a future phase. Notes are
// typically <100 KB so this is a degenerate guard, not a hot path.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace recmeet {

namespace fs = std::filesystem;

// Metadata for a single artifact in `out_dir`. The wire shape carries:
//   { "name": <basename>, "size": <bytes>, "content_type": <mime> }
// `path` is the absolute path the daemon reads bytes from; clients never
// see it (they get the basename via `name`).
struct ArtifactInfo {
    fs::path    path;          ///< absolute disk path; daemon-internal
    std::string name;          ///< basename (`note.md`, `captions.vtt`)
    int64_t     size = 0;      ///< file size in bytes at enumeration time
    std::string content_type;  ///< RFC 2046 media type
};

// Predicate that decides whether a single basename + extension should be
// included in the artifact list. Exposed for tests; daemon callers should
// prefer `enumerate_artifacts()` which walks the directory and applies this.
//
// `filename` is the entry's basename (`Meeting_2026-05-15_10-00.md`,
// `audio.wav`, `.hidden`, ...). The extension check is case-insensitive on
// the trailing `.xxx`. `is_regular_file` is false for directories / sockets
// / symlinks-to-dir etc. — they are always rejected.
bool should_include_artifact(const std::string& filename,
                             bool is_regular_file);

// Walk `out_dir` non-recursively, apply `should_include_artifact` to each
// entry, and return the included files. Results are sorted lexicographically
// by basename so the wire ordering is deterministic (the client demultiplexes
// `0x02` frames by counting them against the response's `artifacts[]` array,
// so order is part of the contract — see docs/IPC-WIRE-PROTOCOL.md).
//
// On a missing `out_dir` (operator deleted the meeting), returns an empty
// vector and sets `*out_error` (if non-null) to a human-readable message.
// The caller distinguishes "no artifacts" from "out_dir gone" by checking
// `*out_error`. A readable empty `out_dir` returns an empty vector with no
// error set.
std::vector<ArtifactInfo> enumerate_artifacts(const fs::path& out_dir,
                                              std::string* out_error = nullptr);

// Compute the RFC 2046 media type for an artifact basename. Public so the
// metadata builder and tests share one source of truth.
std::string content_type_for(const std::string& filename);

} // namespace recmeet
