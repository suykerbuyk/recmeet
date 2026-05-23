// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Filesystem-touching cleanup helper for `run_recording`'s cancel branch.
// Lives in its own translation unit (rather than an anonymous namespace in
// pipeline.cpp) so unit tests can call it directly with controlled tmp dirs
// — DaemonSim cannot drive `run_recording` end-to-end, so this is the only
// place the actual `fs::remove_all` behaviour gets exercised.

#pragma once

#include "util.h"  // fs:: alias

namespace recmeet {

/// Discard a freshly-minted recording output directory. Intended to be
/// called from `run_recording`'s cancel branch AFTER captures have stopped
/// (so no producer-thread is still writing into the dir) and BEFORE
/// returning `PostprocessInput{ .cancelled = true }`.
///
/// Sanity gates `out_dir` to a non-empty, non-root, existing path. The
/// `out_dir != "/"` check is sufficient — by contract, this helper is only
/// invoked on a path freshly returned by `create_output_dir()` (which
/// builds `base_dir / YYYY-MM-DD_HH-MM[_N]`), so even if `cfg.output_dir`
/// is "/" (operator misconfiguration), the resulting path is a
/// YYYY-prefixed subdir that was created milliseconds earlier by this
/// same recording — its contents are exclusively this recording's files.
///
/// Returns `true` if removal succeeded (or the path was already absent),
/// `false` if `fs::remove_all` threw. Cleanup failures are never
/// propagated as recording exceptions — the cancel-as-intent succeeds
/// regardless; callers should log on `false` return.
bool cleanup_cancelled_recording_dir(const fs::path& out_dir);

} // namespace recmeet
