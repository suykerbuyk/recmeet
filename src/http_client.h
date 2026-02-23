// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#pragma once

#include <map>
#include <string>

namespace recmeet {

/// HTTP GET — returns response body. Throws RecmeetError on failure.
std::string http_get(const std::string& url);

/// HTTP GET with custom headers (e.g. Authorization). 15s timeout.
std::string http_get(const std::string& url,
                      const std::map<std::string, std::string>& headers);

/// HTTP POST JSON — returns response body. Throws RecmeetError on failure.
std::string http_post_json(const std::string& url,
                            const std::string& json_body,
                            const std::map<std::string, std::string>& headers = {});

} // namespace recmeet
