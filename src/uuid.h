// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase D.5 — canonical UUID v4 mint, used to assign every recording a
// stable `meeting_id` at `start_capture` time. The id flows through to
// `process.submit` / `process.stream` request params AND into the v2
// sidecar / pending_jobs journal so the C.11.4 dedup contract can route a
// retry-after-crash back to the same meeting directory on the server side.
//
// Output shape is the canonical 8-4-4-4-12 lowercase hex layout with
// version nibble 4 at offset 14 and variant bits 10xx at offset 19 — the
// exact shape `is_valid_meeting_id` (`util.cpp`) accepts. The mint is
// backed by libuuid (`uuid_generate_random`) which itself draws from
// `getrandom(2)` / `/dev/urandom`.

#pragma once

#include <string>

namespace recmeet {

/// Mint a fresh UUID v4 in canonical lowercase form. Always returns 36
/// characters matching the regex
/// `^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$`.
/// Never throws; libuuid's random source is the kernel CSPRNG and the
/// formatter is a bounded, allocation-free `snprintf` against a fixed
/// 36-char output buffer.
std::string new_uuid_v4();

} // namespace recmeet
