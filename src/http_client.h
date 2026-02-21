#pragma once

#include <map>
#include <string>

namespace recmeet {

/// HTTP GET — returns response body. Throws RecmeetError on failure.
std::string http_get(const std::string& url);

/// HTTP POST JSON — returns response body. Throws RecmeetError on failure.
std::string http_post_json(const std::string& url,
                            const std::string& json_body,
                            const std::map<std::string, std::string>& headers = {});

} // namespace recmeet
