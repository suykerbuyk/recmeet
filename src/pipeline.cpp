// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "pipeline.h"
#include "config.h"
#include "diarize.h"
#include "device_enum.h"
#include "audio_capture.h"
#include "audio_monitor.h"
#include "audio_file.h"
#include "audio_mixer.h"
#include "log.h"
#include "model_manager.h"
#include "transcribe.h"
#include "summarize.h"
#include "note.h"
#include "notify.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <unistd.h>

namespace recmeet {

std::string read_context_file(const fs::path& path) {
    if (path.empty() || !fs::exists(path)) return "";
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

namespace {

void display_elapsed(StopToken& stop) {
    if (!isatty(STDERR_FILENO)) return;  // no timer under systemd/journald
    auto start = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
        auto now = std::chrono::steady_clock::now();
        int elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        int mins = elapsed / 60;
        int secs = elapsed % 60;
        fprintf(stderr, "\rRecording... %02d:%02d", mins, secs);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    fprintf(stderr, "\r                    \r");
}

} // anonymous namespace

PostprocessInput run_recording(const Config& cfg, StopToken& stop, PhaseCallback on_phase) {
    auto phase = [&](const std::string& name) {
        if (on_phase) on_phase(name);
    };

    PostprocessInput pp;

    if (!cfg.reprocess_dir.empty()) {
        // --- Reprocess existing recording from audio.wav ---
        fs::path source_dir = cfg.reprocess_dir;
        if (!fs::is_directory(source_dir))
            throw RecmeetError("Reprocess directory does not exist: " + source_dir.string());

        pp.audio_path = source_dir / "audio.wav";
        if (!fs::exists(pp.audio_path))
            throw RecmeetError("No audio.wav in reprocess directory: " + source_dir.string());

        // Output goes to --output-dir if explicitly set, otherwise back to source dir
        if (cfg.output_dir_explicit) {
            pp.out_dir = cfg.output_dir;
            fs::create_directories(pp.out_dir);
        } else {
            pp.out_dir = source_dir;
        }
        log_info("Reprocessing: %s", pp.out_dir.c_str());
    } else {
        // --- Normal mode: detect sources, record audio ---

        // --- Detect sources ---
        std::string mic_source = cfg.mic_source;
        std::string monitor_source = cfg.monitor_source;

        if (mic_source.empty()) {
            auto detected = detect_sources(cfg.device_pattern);
            if (detected.mic.empty()) {
                if (cfg.device_pattern.empty()) {
                    fprintf(stderr, "No mic source detected\n");
                } else {
                    fprintf(stderr, "No mic source matching '%s'\n", cfg.device_pattern.c_str());
                }
                fprintf(stderr, "Available sources:\n");
                for (const auto& s : detected.all)
                    fprintf(stderr, "  %s  (%s)\n", s.name.c_str(), s.description.c_str());
                if (cfg.device_pattern.empty())
                    throw DeviceError("No mic source detected");
                else
                    throw DeviceError("No mic source found matching pattern: " + cfg.device_pattern);
            }
            mic_source = detected.mic;
            if (!cfg.mic_only && monitor_source.empty())
                monitor_source = detected.monitor;
        }

        bool dual_mode = !cfg.mic_only && !monitor_source.empty();

        if (dual_mode) {
            log_info("Mic source:     %s", mic_source.c_str());
            log_info("Monitor source: %s", monitor_source.c_str());
        } else {
            log_info("Audio source: %s", mic_source.c_str());
            if (!cfg.mic_only)
                log_warn("No monitor source found — recording mic only.");
        }

        // --- Create output directory ---
        pp.out_dir = create_output_dir(cfg.output_dir);
        log_info("Output directory: %s", pp.out_dir.c_str());

        pp.audio_path = pp.out_dir / "audio.wav";

        // --- Record ---
        phase("recording");

        if (dual_mode) {
            notify("Recording started", "Mic: " + mic_source + "\nMonitor: " + monitor_source);

            // Start mic capture via PipeWire
            PipeWireCapture mic_cap(mic_source);
            mic_cap.start();

            // Start monitor capture — try PipeWire CAPTURE_SINK first, fall back to pa_simple
            std::unique_ptr<PipeWireCapture> mon_pw;
            std::unique_ptr<PulseMonitorCapture> mon_pa;

            // For .monitor sources, go straight to pa_simple (pw_stream doesn't handle them)
            const std::string mon_suffix = ".monitor";
            bool is_pa_monitor = monitor_source.size() >= mon_suffix.size() &&
                monitor_source.compare(monitor_source.size() - mon_suffix.size(),
                                        mon_suffix.size(), mon_suffix) == 0;
            if (is_pa_monitor) {
                mon_pa = std::make_unique<PulseMonitorCapture>(monitor_source);
                mon_pa->start();
            } else {
                try {
                    mon_pw = std::make_unique<PipeWireCapture>(monitor_source, /*capture_sink=*/true);
                    mon_pw->start();
                } catch (const RecmeetError& e) {
                    log_warn("PipeWire monitor failed (%s), falling back to pa_simple", e.what());
                    mon_pa = std::make_unique<PulseMonitorCapture>(monitor_source);
                    mon_pa->start();
                }
            }

            // Display timer and wait for stop
            StopToken timer_stop;
            std::thread timer_thread(display_elapsed, std::ref(timer_stop));

            while (!stop.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

            timer_stop.request();
            timer_thread.join();
            fprintf(stderr, "Recording stopped.\n");

            // Stop captures
            mic_cap.stop();
            if (mon_pw) mon_pw->stop();
            if (mon_pa) mon_pa->stop();

            // Drain and write
            auto mic_samples = mic_cap.drain();
            auto mon_samples = mon_pw ? mon_pw->drain() : mon_pa->drain();

            fs::path mic_path = pp.out_dir / "mic.wav";
            fs::path mon_path = pp.out_dir / "monitor.wav";
            write_wav(mic_path, mic_samples);
            write_wav(mon_path, mon_samples);

            // Validate mic (fatal)
            validate_audio(mic_path, 1.0, "Mic audio");

            // Validate monitor (non-fatal)
            try {
                validate_audio(mon_path, 1.0, "Monitor audio");
                // Mix
                auto mixed = mix_audio(mic_samples, mon_samples);
                write_wav(pp.audio_path, mixed);
                log_info("Mixed audio saved: %s", pp.audio_path.c_str());
            } catch (const AudioValidationError& e) {
                log_warn("Monitor audio unusable (%s). Using mic only.", e.what());
                write_wav(pp.audio_path, mic_samples);
            }

            // Clean up source files unless --keep-sources
            if (!cfg.keep_sources) {
                fs::remove(mic_path);
                fs::remove(mon_path);
            }
        } else {
            notify("Recording started", "Source: " + mic_source);

            PipeWireCapture cap(mic_source);
            cap.start();

            StopToken timer_stop;
            std::thread timer_thread(display_elapsed, std::ref(timer_stop));

            while (!stop.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

            timer_stop.request();
            timer_thread.join();
            fprintf(stderr, "Recording stopped.\n");

            cap.stop();
            auto samples = cap.drain();
            write_wav(pp.audio_path, samples);
            validate_audio(pp.audio_path);
        }

    }

    return pp;
}

PipelineResult run_postprocessing(const Config& cfg, const PostprocessInput& input,
                                  PhaseCallback on_phase) {
    auto phase = [&](const std::string& name) {
        if (on_phase) on_phase(name);
    };

    int threads = cfg.threads > 0 ? cfg.threads : default_thread_count();

    // --- Transcribe + Diarize (if not pre-computed) ---
    std::string transcript_text = input.transcript_text;

    if (transcript_text.empty()) {
        phase("transcribing");
        notify("Transcribing...", "Model: " + cfg.whisper_model);
        log_info("Using %d threads for inference.", threads);

        fs::path model_path = ensure_whisper_model(cfg.whisper_model);
        auto result = transcribe(model_path, input.audio_path, cfg.language, threads);

#if RECMEET_USE_SHERPA
        if (cfg.diarize) {
            phase("diarizing");
            notify("Diarizing...", "Identifying speakers");
            auto diar = diarize(input.audio_path, cfg.num_speakers, threads, cfg.cluster_threshold);
            result.segments = merge_speakers(result.segments, diar);
        }
#endif

        transcript_text = result.to_string();
        if (transcript_text.empty())
            throw RecmeetError("Transcription produced no text.");
    }

    // --- Summarize ---
    PipelineResult pipe_result;
    pipe_result.output_dir = input.out_dir;

    std::string summary_text;
    std::string context_text = read_context_file(cfg.context_file);

    MeetingMetadata metadata;

    if (!cfg.no_summary) {
        phase("summarizing");

#if RECMEET_USE_LLAMA
        if (!cfg.llm_model.empty()) {  // NOLINT(readability-misleading-indentation)
            // Local summarization
            notify("Summarizing...", "Local LLM");
            try {
                fs::path llm_path = ensure_llama_model(cfg.llm_model);
                summary_text = summarize_local(transcript_text, llm_path, context_text, threads);
            } catch (const std::exception& e) {
                log_warn("Local summary failed: %s", e.what());
            }
        } else
#endif
        if (!cfg.api_key.empty()) {
            std::string url = cfg.api_url;
            if (url.empty()) {
                const auto* prov = find_provider(cfg.provider);
                if (prov) url = std::string(prov->base_url) + "/chat/completions";
                else url = "https://api.x.ai/v1/chat/completions";
            }
            notify("Summarizing...", "Sending to " + cfg.api_model);
            try {
                summary_text = summarize_http(transcript_text, url,
                                               cfg.api_key, cfg.api_model, context_text);
            } catch (const std::exception& e) {
                log_warn("Summary failed: %s", e.what());
                log_warn("Transcript is still available.");
            }
        } else {
            log_warn("No API key and no local LLM — skipping summary.");
        }

        if (!summary_text.empty()) {  // NOLINT(readability-misleading-indentation)
            metadata = extract_meeting_metadata(summary_text);
            summary_text = strip_metadata_block(summary_text);
        }
    } else {
        log_info("Summary skipped (--no-summary).");
    }

    // --- Meeting note output ---
    try {
        // Get current date/time
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_r(&time_t, &tm);

        char date_buf[16], time_buf[8];
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);
        strftime(time_buf, sizeof(time_buf), "%H:%M", &tm);

        MeetingData md;
        md.date = date_buf;
        md.time = time_buf;
        md.summary_text = summary_text;
        md.transcript_text = transcript_text;
        md.context_text = context_text;
        md.output_dir = input.out_dir;

        // AI-derived metadata
        md.title = metadata.title;
        md.description = metadata.description;
        md.ai_tags = metadata.tags;
        md.participants = metadata.participants;
        md.duration_seconds = get_audio_duration_seconds(input.audio_path);
        md.whisper_model = cfg.whisper_model;

        pipe_result.note_path = write_meeting_note(cfg.note, md);
    } catch (const std::exception& e) {
        log_warn("Meeting note failed: %s", e.what());
    }

    // --- Done ---
    phase("complete");
    notify("Meeting complete", input.out_dir.string());
    fprintf(stderr, "\nDone! Files in: %s\n", input.out_dir.c_str());
    return pipe_result;
}

PipelineResult run_pipeline(const Config& cfg, StopToken& stop, PhaseCallback on_phase) {
    auto input = run_recording(cfg, stop, on_phase);
    return run_postprocessing(cfg, input, on_phase);
}

} // namespace recmeet
