// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "uuid.h"

#include <uuid/uuid.h>

#include <cstdio>

namespace recmeet {

std::string new_uuid_v4() {
    uuid_t raw;
    uuid_generate_random(raw);  // 16 random bytes with v4 + variant-10 bits set

    // uuid_unparse_lower writes 37 chars (36 + NUL) into the buffer in
    // canonical 8-4-4-4-12 lowercase hex form. We pre-size to 37 so we can
    // copy into a std::string without a length-querying round-trip.
    char buf[37];
    uuid_unparse_lower(raw, buf);
    return std::string(buf, 36);
}

} // namespace recmeet
