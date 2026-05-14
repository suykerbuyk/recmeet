// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "tray_capture.h"

#include <sndfile.h>

#include <cstdio>
#include <fstream>
#include <system_error>

namespace recmeet {
namespace tray_capture {

fs::path default_staging_dir() {
    return data_dir() / "staging";
}

std::string format_timestamp(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M", &tm);
    return std::string(buf);
}

fs::path next_staging_wav_path(const fs::path& staging_dir,
                               const std::string& timestamp) {
    fs::path candidate = staging_dir / ("audio_" + timestamp + ".wav");
    if (!fs::exists(candidate)) return candidate;
    for (int i = 1; i <= 10000; ++i) {
        candidate = staging_dir /
            ("audio_" + timestamp + "_" + std::to_string(i) + ".wav");
        if (!fs::exists(candidate)) return candidate;
    }
    throw RecmeetError("tray_capture: could not allocate a unique WAV filename "
                       "under " + staging_dir.string());
}

bool write_wav(const fs::path& path, const std::vector<int16_t>& samples,
               std::string& err_msg) {
    SF_INFO info = {};
    info.samplerate = SAMPLE_RATE;
    info.channels   = CHANNELS;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* sf = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!sf) {
        err_msg = std::string("sf_open failed: ") + sf_strerror(nullptr);
        return false;
    }
    sf_count_t written = sf_write_short(sf, samples.data(),
                                        static_cast<sf_count_t>(samples.size()));
    sf_close(sf);
    if (written != static_cast<sf_count_t>(samples.size())) {
        err_msg = "sf_write_short truncated";
        std::error_code ec;
        fs::remove(path, ec);
        return false;
    }
    return true;
}

fs::path pending_sidecar_path(const fs::path& wav_path) {
    fs::path s = wav_path;
    s.replace_extension(".pending");
    return s;
}

bool write_pending_sidecar(const fs::path& wav_path,
                           const std::string& timestamp,
                           const std::string& mic_source,
                           bool captions_enabled) {
    fs::path sidecar = pending_sidecar_path(wav_path);
    std::ofstream out(sidecar);
    if (!out) return false;
    auto esc = [](const std::string& s) {
        std::string r;
        r.reserve(s.size() + 2);
        for (char c : s) {
            if (c == '\\' || c == '"') r += '\\';
            r += c;
        }
        return r;
    };
    out << "{\n"
        << "  \"wav_path\": \""        << esc(wav_path.string()) << "\",\n"
        << "  \"timestamp\": \""       << esc(timestamp)         << "\",\n"
        << "  \"mic_source\": \""      << esc(mic_source)        << "\",\n"
        << "  \"captions_enabled\": "  << (captions_enabled ? "true" : "false") << "\n"
        << "}\n";
    out.flush();
    return static_cast<bool>(out);
}

} // namespace tray_capture
} // namespace recmeet
