// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <cstdint>
#include <vector>

namespace recmeet {

/// Mix two S16LE mono streams by averaging samples.
/// If streams differ in length, the shorter one is zero-padded.
std::vector<int16_t> mix_audio(const std::vector<int16_t>& a,
                                const std::vector<int16_t>& b);

} // namespace recmeet
