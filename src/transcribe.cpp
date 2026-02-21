#include "transcribe.h"
#include "audio_file.h"

#include <whisper.h>

#include <cstdio>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace recmeet {

static std::string format_timestamp(double seconds) {
    int total = static_cast<int>(seconds);
    int mins = total / 60;
    int secs = total % 60;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
    return buf;
}

std::string TranscriptResult::to_string() const {
    std::ostringstream oss;
    for (const auto& seg : segments) {
        oss << "[" << format_timestamp(seg.start) << " - "
            << format_timestamp(seg.end) << "] " << seg.text << "\n";
    }
    return oss.str();
}

TranscriptResult transcribe(const fs::path& model_path, const fs::path& audio_path,
                            const std::string& language) {
    fprintf(stderr, "Loading whisper model: %s\n", model_path.filename().c_str());

    whisper_context_params cparams = whisper_context_default_params();
    whisper_context* ctx = whisper_init_from_file_with_params(
        model_path.c_str(), cparams);
    if (!ctx)
        throw RecmeetError("Failed to load whisper model: " + model_path.string());

    // Read audio as float32 [-1, 1]
    auto samples = read_wav_float(audio_path);
    fprintf(stderr, "Audio: %.1fs (%zu samples)\n",
            samples.size() / (float)SAMPLE_RATE, samples.size());

    // Set up whisper params
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.n_threads = 4;
    wparams.print_progress = true;
    wparams.print_timestamps = false;

    // Relax no-speech suppression — default logprob_thold (-1.0) can be too
    // aggressive for smaller models on Bluetooth mic audio.
    wparams.logprob_thold = -2.0f;

    // Language forcing
    if (!language.empty()) {
        int lang_check = whisper_lang_id(language.c_str());
        if (lang_check < 0)
            throw RecmeetError("Unknown language code: " + language);
        wparams.language = language.c_str();
        wparams.detect_language = false;
        fprintf(stderr, "Language forced: %s\n", language.c_str());
    } else {
        wparams.language = nullptr;
        wparams.detect_language = false;
    }

    fprintf(stderr, "Transcribing...\n");
    int ret = whisper_full(ctx, wparams, samples.data(), samples.size());
    if (ret != 0) {
        whisper_free(ctx);
        throw RecmeetError("Whisper transcription failed (code " + std::to_string(ret) + ")");
    }

    // Extract segments
    TranscriptResult result;
    int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        TranscriptSegment seg;
        seg.start = whisper_full_get_segment_t0(ctx, i) / 100.0;
        seg.end   = whisper_full_get_segment_t1(ctx, i) / 100.0;
        seg.text  = whisper_full_get_segment_text(ctx, i);

        // Trim leading/trailing whitespace
        auto ltrim = seg.text.find_first_not_of(" \t\n\r");
        if (ltrim != std::string::npos)
            seg.text = seg.text.substr(ltrim);
        auto rtrim = seg.text.find_last_not_of(" \t\n\r");
        if (rtrim != std::string::npos)
            seg.text = seg.text.substr(0, rtrim + 1);

        if (!seg.text.empty())
            result.segments.push_back(std::move(seg));
    }

    // Language detection
    int lang_id = whisper_full_lang_id(ctx);
    result.language = whisper_lang_str(lang_id);
    result.language_prob = 0.0f; // whisper.cpp doesn't expose per-segment lang probs easily

    fprintf(stderr, "Transcribed %d segments (language: %s)\n",
            (int)result.segments.size(), result.language.c_str());

    whisper_free(ctx);
    return result;
}

} // namespace recmeet
