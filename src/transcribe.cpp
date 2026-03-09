// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "transcribe.h"
#include "audio_file.h"
#include "log.h"

#include <whisper.h>

#include <cstdio>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unistd.h>

namespace recmeet {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// WhisperModel
// ---------------------------------------------------------------------------

WhisperModel::WhisperModel(const fs::path& model_path) : path_(model_path) {
    log_info("Loading whisper model: %s", path_.filename().c_str());
    whisper_context_params cparams = whisper_context_default_params();
    ctx_ = whisper_init_from_file_with_params(path_.c_str(), cparams);
    if (!ctx_)
        throw RecmeetError("Failed to load whisper model: " + path_.string());
}

WhisperModel::~WhisperModel() {
    if (ctx_)
        whisper_free(ctx_);
}

WhisperModel::WhisperModel(WhisperModel&& other) noexcept
    : ctx_(other.ctx_), path_(std::move(other.path_)) {
    other.ctx_ = nullptr;
}

WhisperModel& WhisperModel::operator=(WhisperModel&& other) noexcept {
    if (this != &other) {
        if (ctx_)
            whisper_free(ctx_);
        ctx_ = other.ctx_;
        path_ = std::move(other.path_);
        other.ctx_ = nullptr;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Transcription
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Callback state for whisper C function pointers
// ---------------------------------------------------------------------------

struct TranscribeCallbackState {
    std::function<void(int)> on_progress;
    StopToken* stop = nullptr;
};

static void whisper_progress_cb(whisper_context*, whisper_state*, int progress, void* user_data) {
    auto* state = static_cast<TranscribeCallbackState*>(user_data);
    if (state && state->on_progress)
        state->on_progress(progress);
}

static bool whisper_abort_cb(void* user_data) {
    auto* state = static_cast<TranscribeCallbackState*>(user_data);
    return state && state->stop && state->stop->stop_requested();
}

// ---------------------------------------------------------------------------
// Core transcription (shared implementation)
// ---------------------------------------------------------------------------

static TranscriptResult transcribe_impl(whisper_context* ctx, const float* samples,
                                         size_t num_samples, double offset_seconds,
                                         const std::string& language, int threads,
                                         TranscribeCallbackState* cb_state) {
    // Set up whisper params
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.n_threads = threads > 0 ? threads : default_thread_count();
    // Suppress built-in progress when we have our own callback
    wparams.print_progress = !cb_state && (offset_seconds == 0.0) && isatty(STDERR_FILENO);
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
    } else {
        wparams.language = nullptr;
        wparams.detect_language = false;
    }

    // Wire up progress + abort callbacks if provided
    if (cb_state) {
        if (cb_state->on_progress) {
            wparams.progress_callback = whisper_progress_cb;
            wparams.progress_callback_user_data = cb_state;
        }
        if (cb_state->stop) {
            wparams.abort_callback = whisper_abort_cb;
            wparams.abort_callback_user_data = cb_state;
        }
    }

    int ret = whisper_full(ctx, wparams, samples, static_cast<int>(num_samples));
    if (ret != 0) {
        // Distinguish cancellation from real failure
        if (cb_state && cb_state->stop && cb_state->stop->stop_requested())
            throw RecmeetError("Cancelled");
        throw RecmeetError("Whisper transcription failed (code " + std::to_string(ret) + ")");
    }

    // Extract segments
    TranscriptResult result;
    int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        TranscriptSegment seg;
        seg.start = whisper_full_get_segment_t0(ctx, i) / 100.0 + offset_seconds;
        seg.end   = whisper_full_get_segment_t1(ctx, i) / 100.0 + offset_seconds;
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

    return result;
}

TranscriptResult transcribe(WhisperModel& model, const float* samples,
                            size_t num_samples, double offset_seconds,
                            const std::string& language, int threads) {
    return transcribe_impl(model.get(), samples, num_samples, offset_seconds,
                           language, threads, nullptr);
}

TranscriptResult transcribe(WhisperModel& model, const float* samples,
                            size_t num_samples, double offset_seconds,
                            const TranscribeOptions& opts) {
    TranscribeCallbackState cb_state;
    cb_state.on_progress = opts.on_progress;
    cb_state.stop = opts.stop;
    return transcribe_impl(model.get(), samples, num_samples, offset_seconds,
                           opts.language, opts.threads, &cb_state);
}

TranscriptResult transcribe(WhisperModel& model, const fs::path& audio_path,
                            const std::string& language, int threads) {
    // Read audio as float32 [-1, 1]
    auto samples = read_wav_float(audio_path);
    log_info("Audio: %.1fs (%zu samples)",
            samples.size() / (float)SAMPLE_RATE, samples.size());

    if (!language.empty())
        log_info("Language forced: %s", language.c_str());

    log_info("Transcribing...");
    auto result = transcribe(model, samples.data(), samples.size(), 0.0, language, threads);

    log_info("Transcribed %d segments (language: %s)",
            (int)result.segments.size(), result.language.c_str());

    return result;
}

TranscriptResult transcribe(const fs::path& model_path, const fs::path& audio_path,
                            const std::string& language, int threads) {
    WhisperModel model(model_path);
    return transcribe(model, audio_path, language, threads);
}

} // namespace recmeet
