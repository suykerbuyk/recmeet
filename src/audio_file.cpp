// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "audio_file.h"
#include "log.h"

#include <sndfile.h>
#include <cstdio>
#include <cmath>

namespace recmeet {

void write_wav(const fs::path& path, const std::vector<int16_t>& samples) {
    SF_INFO info = {};
    info.samplerate = SAMPLE_RATE;
    info.channels = CHANNELS;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* sf = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!sf)
        throw RecmeetError("Failed to open WAV for writing: " + path.string() +
                           " (" + sf_strerror(nullptr) + ")");

    sf_count_t written = sf_write_short(sf, samples.data(), samples.size());
    sf_close(sf);

    if (written != static_cast<sf_count_t>(samples.size()))
        throw RecmeetError("WAV write incomplete: " + path.string());
}

std::vector<float> read_wav_float(const fs::path& path) {
    SF_INFO info = {};
    SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
    if (!sf)
        throw RecmeetError("Failed to open WAV for reading: " + path.string() +
                           " (" + sf_strerror(nullptr) + ")");

    // Read all frames as float (libsndfile handles format conversion)
    std::vector<float> samples(info.frames * info.channels);
    sf_count_t read = sf_read_float(sf, samples.data(), samples.size());
    sf_close(sf);

    if (read <= 0)
        throw RecmeetError("WAV file contains no data: " + path.string());

    // If stereo, downmix to mono
    if (info.channels > 1) {
        std::vector<float> mono(info.frames);
        for (sf_count_t i = 0; i < info.frames; ++i) {
            float sum = 0;
            for (int ch = 0; ch < info.channels; ++ch)
                sum += samples[i * info.channels + ch];
            mono[i] = sum / info.channels;
        }
        return mono;
    }

    samples.resize(read);
    return samples;
}

double validate_audio(const fs::path& path, double min_duration,
                      const std::string& label) {
    if (!fs::exists(path) || fs::file_size(path) == 0)
        throw AudioValidationError(label + " file is missing or empty.");

    SF_INFO info = {};
    SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
    if (!sf) {
        // Fallback: estimate from file size
        auto data_size = static_cast<double>(fs::file_size(path)) - 44;
        if (data_size <= 0)
            throw AudioValidationError(label + " file contains no data.");
        double duration = data_size / BYTES_PER_SEC;
        if (duration < min_duration)
            throw AudioValidationError(label + " too short (~" +
                                       std::to_string(duration) + "s).");
        log_info("%s validated (estimated): ~%.1fs", label.c_str(), duration);
        return duration;
    }

    double duration = static_cast<double>(info.frames) / info.samplerate;
    sf_close(sf);

    if (duration < min_duration)
        throw AudioValidationError(label + " too short (" +
                                   std::to_string(duration) + "s).");

    log_info("%s validated: %.1fs, %dHz", label.c_str(), duration, info.samplerate);
    return duration;
}

} // namespace recmeet
