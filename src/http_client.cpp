#include "http_client.h"
#include "util.h"
#include "version.h"

#include <curl/curl.h>
#include <cstring>
#include <string>

namespace recmeet {

namespace {

size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* buf = static_cast<std::string*>(userp);
    buf->append(static_cast<char*>(contents), total);
    return total;
}

struct CurlInit {
    CurlInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlInit() { curl_global_cleanup(); }
};

CurlInit& ensure_curl() {
    static CurlInit init;
    return init;
}

} // anonymous namespace

std::string http_get(const std::string& url) {
    ensure_curl();

    CURL* curl = curl_easy_init();
    if (!curl) throw RecmeetError("curl_easy_init failed");

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    std::string ua = std::string("recmeet/") + RECMEET_VERSION;
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::string err = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        throw RecmeetError("HTTP GET failed: " + err + " (" + url + ")");
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code >= 400)
        throw RecmeetError("HTTP GET " + std::to_string(http_code) + ": " + url);

    return response;
}

std::string http_get(const std::string& url,
                      const std::map<std::string, std::string>& headers) {
    ensure_curl();

    CURL* curl = curl_easy_init();
    if (!curl) throw RecmeetError("curl_easy_init failed");

    struct curl_slist* hdr_list = nullptr;
    for (const auto& [key, val] : headers)
        hdr_list = curl_slist_append(hdr_list, (key + ": " + val).c_str());

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (hdr_list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    std::string ua = std::string("recmeet/") + RECMEET_VERSION;
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());

    CURLcode res = curl_easy_perform(curl);
    if (hdr_list) curl_slist_free_all(hdr_list);

    if (res != CURLE_OK) {
        std::string err = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        throw RecmeetError("HTTP GET failed: " + err + " (" + url + ")");
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code >= 400)
        throw RecmeetError("HTTP GET " + std::to_string(http_code) + ": " + url);

    return response;
}

std::string http_post_json(const std::string& url,
                            const std::string& json_body,
                            const std::map<std::string, std::string>& headers) {
    ensure_curl();

    CURL* curl = curl_easy_init();
    if (!curl) throw RecmeetError("curl_easy_init failed");

    struct curl_slist* hdr_list = nullptr;
    hdr_list = curl_slist_append(hdr_list, "Content-Type: application/json");
    for (const auto& [key, val] : headers)
        hdr_list = curl_slist_append(hdr_list, (key + ": " + val).c_str());

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr_list);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, json_body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    std::string ua2 = std::string("recmeet/") + RECMEET_VERSION;
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua2.c_str());

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdr_list);

    if (res != CURLE_OK) {
        std::string err = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        throw RecmeetError("HTTP POST failed: " + err + " (" + url + ")");
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code >= 400)
        throw RecmeetError("API error (" + std::to_string(http_code) + "): " + response);

    return response;
}

} // namespace recmeet
