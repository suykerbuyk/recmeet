#include "pipeline.h"
#include "device_enum.h"
#include "audio_capture.h"
#include "audio_monitor.h"
#include "audio_file.h"
#include "audio_mixer.h"
#include "model_manager.h"
#include "transcribe.h"
#include "summarize.h"
#include "obsidian.h"
#include "notify.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <thread>

namespace recmeet {

namespace {

void display_elapsed(StopToken& stop) {
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

std::string read_context_file(const fs::path& path) {
    if (path.empty() || !fs::exists(path)) return "";
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

} // anonymous namespace

PipelineResult run_pipeline(const Config& cfg, StopToken& stop, PhaseCallback on_phase) {
    auto phase = [&](const std::string& name) {
        if (on_phase) on_phase(name);
    };

    // --- Detect sources ---
    std::string mic_source = cfg.mic_source;
    std::string monitor_source = cfg.monitor_source;

    if (mic_source.empty()) {
        auto detected = detect_sources(cfg.device_pattern);
        if (detected.mic.empty()) {
            fprintf(stderr, "No mic source matching '%s'\n", cfg.device_pattern.c_str());
            fprintf(stderr, "Available sources:\n");
            for (const auto& s : detected.all)
                fprintf(stderr, "  %s  (%s)\n", s.name.c_str(), s.description.c_str());
            throw DeviceError("No mic source found matching pattern: " + cfg.device_pattern);
        }
        mic_source = detected.mic;
        if (!cfg.mic_only && monitor_source.empty())
            monitor_source = detected.monitor;
    }

    bool dual_mode = !cfg.mic_only && !monitor_source.empty();

    if (dual_mode) {
        fprintf(stderr, "Mic source:     %s\n", mic_source.c_str());
        fprintf(stderr, "Monitor source: %s\n", monitor_source.c_str());
    } else {
        fprintf(stderr, "Audio source: %s\n", mic_source.c_str());
        if (!cfg.mic_only)
            fprintf(stderr, "No monitor source found — recording mic only.\n");
    }

    // --- Create output directory ---
    fs::path out_dir = create_output_dir(cfg.output_dir);
    fprintf(stderr, "Output directory: %s\n", out_dir.c_str());

    fs::path audio_path = out_dir / "audio.wav";
    fs::path transcript_path = out_dir / "transcript.txt";
    fs::path summary_path = out_dir / "summary.md";

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
                fprintf(stderr, "PipeWire monitor failed (%s), falling back to pa_simple\n", e.what());
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

        fs::path mic_path = out_dir / "mic.wav";
        fs::path mon_path = out_dir / "monitor.wav";
        write_wav(mic_path, mic_samples);
        write_wav(mon_path, mon_samples);

        // Validate mic (fatal)
        validate_audio(mic_path, 1.0, "Mic audio");

        // Validate monitor (non-fatal)
        try {
            validate_audio(mon_path, 1.0, "Monitor audio");
            // Mix
            auto mixed = mix_audio(mic_samples, mon_samples);
            write_wav(audio_path, mixed);
            fprintf(stderr, "Mixed audio saved: %s\n", audio_path.c_str());
        } catch (const AudioValidationError& e) {
            fprintf(stderr, "Warning: Monitor audio unusable (%s). Using mic only.\n", e.what());
            write_wav(audio_path, mic_samples);
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
        write_wav(audio_path, samples);
        validate_audio(audio_path);
    }

    // --- Transcribe ---
    phase("transcribing");
    notify("Transcribing...", "Model: " + cfg.whisper_model);

    fs::path model_path = ensure_whisper_model(cfg.whisper_model);
    auto result = transcribe(model_path, audio_path);

    std::string transcript_text = result.to_string();
    if (transcript_text.empty())
        throw RecmeetError("Transcription produced no text.");

    {
        std::ofstream out(transcript_path);
        out << transcript_text;
    }
    fprintf(stderr, "Transcript saved: %s\n", transcript_path.c_str());

    // --- Summarize ---
    PipelineResult pipe_result;
    pipe_result.transcript_path = transcript_path;
    pipe_result.output_dir = out_dir;

    std::string summary_text;
    std::string context_text = read_context_file(cfg.context_file);

    if (!cfg.no_summary) {
        phase("summarizing");

#if RECMEET_USE_LLAMA
        if (!cfg.llm_model.empty()) {
            // Local summarization
            notify("Summarizing...", "Local LLM");
            try {
                fs::path llm_path = ensure_llama_model(cfg.llm_model);
                summary_text = summarize_local(transcript_text, llm_path, context_text);
            } catch (const std::exception& e) {
                fprintf(stderr, "Warning: Local summary failed — %s\n", e.what());
            }
        } else
#endif
        if (!cfg.api_key.empty()) {
            notify("Summarizing...", "Sending to " + cfg.api_model);
            try {
                summary_text = summarize_http(transcript_text, cfg.api_url,
                                               cfg.api_key, cfg.api_model, context_text);
            } catch (const std::exception& e) {
                fprintf(stderr, "Warning: Summary failed — %s\n", e.what());
                fprintf(stderr, "Transcript is still available.\n");
            }
        } else {
            fprintf(stderr, "No API key and no local LLM — skipping summary.\n");
        }

        if (!summary_text.empty()) {
            std::ofstream out(summary_path);
            out << summary_text;
            pipe_result.summary_path = summary_path;
            fprintf(stderr, "Summary saved: %s\n", summary_path.c_str());
        }
    } else {
        fprintf(stderr, "Summary skipped (--no-summary).\n");
    }

    // --- Obsidian output ---
    if (cfg.obsidian_enabled && !cfg.obsidian.vault_path.empty()) {
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
            md.output_dir = out_dir;

            pipe_result.obsidian_path = write_obsidian_note(cfg.obsidian, md);
        } catch (const std::exception& e) {
            fprintf(stderr, "Warning: Obsidian note failed — %s\n", e.what());
        }
    }

    // --- Done ---
    phase("complete");
    notify("Meeting complete", out_dir.string());
    fprintf(stderr, "\nDone! Files in: %s\n", out_dir.c_str());
    return pipe_result;
}

} // namespace recmeet
