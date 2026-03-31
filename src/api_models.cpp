// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "api_models.h"
#include "http_client.h"

#include <algorithm>
#include <map>
#include <string>

namespace recmeet {

bool is_chat_model(const std::string& id) {
    static const char* skip[] = {
        "embed", "tts", "whisper", "dall-e", "image",
        "video", "moderation", "audio", "realtime",
    };
    for (const auto* s : skip)
        if (id.find(s) != std::string::npos)
            return false;
    return true;
}

std::vector<std::string> fetch_models(const std::string& models_url, const std::string& api_key) {
    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + api_key},
    };
    std::string response = http_get(models_url, headers);

    // Extract all "id" values from the response JSON
    std::vector<std::string> models;
    std::string search = "\"id\"";
    size_t pos = 0;
    while ((pos = response.find(search, pos)) != std::string::npos) {
        pos += search.size();
        // Skip to the value string
        pos = response.find('"', pos + 1); // skip ':'
        if (pos == std::string::npos) break;
        pos++; // skip opening quote

        std::string id;
        while (pos < response.size() && response[pos] != '"') {
            id += response[pos];
            pos++;
        }

        if (!id.empty() && is_chat_model(id))
            models.push_back(id);
    }

    std::sort(models.begin(), models.end());
    return models;
}

} // namespace recmeet
