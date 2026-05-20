// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Phase C.9 — extracted from src/pipeline.cpp.
//
// Before C.9 the live-recording path (PipeWire/PulseAudio capture +
// optional streaming CaptionEngine + WAV writer) lived alongside the rest
// of the pipeline in src/pipeline.cpp. The daemon transitively pulled
// PipeWire/PulseAudio through `recmeet_core` even after switching to v2
// verbs. C.9 retires the daemon-side live-recording path entirely, so the
// daemon no longer needs the capture layer; this file factors the live
// recording surface (`run_recording` + `run_pipeline` + the caption-engine
// owners) out into its own translation unit so we can scope its
// PipeWire/Pulse dependency to a `recmeet_live_capture` library that only
// the CLI binary links.
//
// `run_postprocessing()` (and its dependencies) remain in pipeline.cpp —
// the daemon needs them via `recmeet_core` for the subprocess postprocess
// path and for the no-daemon CLI / reprocess flows.

#include "pipeline.h"
#include "caption_engine.h"
#include "caption_vtt.h"
#include "config.h"
#include "ipc_protocol.h"
#include "device_enum.h"
#include "audio_capture.h"
#include "audio_monitor.h"
#include "audio_file.h"
#include "audio_mixer.h"
#include "log.h"
#include "notify.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <unistd.h>

namespace recmeet {

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

// Owner of an active streaming caption engine + its capture-side
// subscription. Construction wires the engine to the capture's
// set_audio_callback; the destructor enforces the Phase 3 teardown
// ordering: the producer-side callback is unsubscribed first, THEN the
// engine is stopped (which joins its worker after draining the ring).
//
// Two convenience overloads cover the dual-mode (mic + monitor) and
// single-mode (mic only) capture topologies. The engine is wired to the
// mic capture only — monitor audio is recorded but not captioned in V1.
//
// Phase 6 — the engine's single result callback is fanned out via a
// `CaptionFanoutAdapter` heap-owned by this RAII wrapper: it forwards every
// result to the daemon-supplied hook (IPC broadcast) and additionally
// appends finalized cues to a `VttWriter` (sidecar persistence). The
// adapter's lifetime tracks ActiveCaptionEngine, which is reset BEFORE the
// capture is drained — so the adapter outlives every callback the engine
// might post.
struct CaptionFanoutAdapter {
    // Daemon-supplied hooks (forwarded verbatim). May be null when running
    // a writer-only test setup, but in production both are set.
    CaptionResultCallback   downstream_on_result   = nullptr;
    void*                   downstream_result_ud   = nullptr;

    // Sidecar writer + per-cue (start_ms, end_ms) tracker. The writer is
    // owned by the adapter so the destruction order is deterministic:
    // engine.stop() (joins the worker) -> adapter dtor (closes the file).
    std::unique_ptr<VttWriter> vtt;
    VttCueTimer cue_timer;
};

// Engine result callback installed by try_start_caption_engine when the
// caller has a writable meeting directory. Forwards to the daemon's hook,
// then appends finalized cues to the VTT sidecar. Partials skip the writer
// (the writer also has its own defensive partial filter).
inline void caption_fanout_on_result(const CaptionResult& r, void* ud) {
    auto* a = static_cast<CaptionFanoutAdapter*>(ud);
    if (a->downstream_on_result) {
        a->downstream_on_result(r, a->downstream_result_ud);
    }
    if (!r.is_partial && a->vtt) {
        auto [start_ms, end_ms] = a->cue_timer.next(r.timestamp_ms);
        // append() returns false on I/O error and sets last_error(); we
        // log and continue — captions are non-critical and must never abort
        // recording.
        if (!a->vtt->append(start_ms, end_ms, r.text, /*is_partial=*/false)) {
            log_warn("captions: VTT append failed (%s) — continuing without sidecar",
                     a->vtt->last_error().c_str());
        }
    }
}

template <typename Capture>
class ActiveCaptionEngine {
public:
    ActiveCaptionEngine(std::unique_ptr<CaptionEngine> engine, Capture* capture,
                        std::unique_ptr<CaptionFanoutAdapter> adapter = nullptr)
        : engine_(std::move(engine)), capture_(capture),
          adapter_(std::move(adapter)) {
        if (capture_ && engine_) {
            capture_->set_audio_callback(&CaptionEngine::on_audio_chunk, engine_.get());
        }
    }
    ~ActiveCaptionEngine() {
        // Belt-and-braces — capture has already been .stop()'d at the
        // call site so no more callbacks fire. Unsubscribe first so the
        // capture's atomic callback ptr can never observe a destroyed
        // engine even on a misordered teardown path.
        if (capture_) {
            capture_->set_audio_callback(nullptr, nullptr);
        }
        if (engine_) {
            engine_->stop();
        }
        // Adapter (and its VttWriter, if any) destroyed last — closes the
        // sidecar fd after the engine worker has joined.
    }
    ActiveCaptionEngine(const ActiveCaptionEngine&) = delete;
    ActiveCaptionEngine& operator=(const ActiveCaptionEngine&) = delete;

    bool engine_running() const { return engine_ && engine_->is_running(); }

private:
    std::unique_ptr<CaptionEngine> engine_;
    Capture* capture_ = nullptr;
    std::unique_ptr<CaptionFanoutAdapter> adapter_;
};

// Try to start a CaptionEngine. On any failure we DO NOT throw — captions
// are non-critical and recording must continue. Caller-provided
// `on_engine_error` (if any) gets a one-shot notification.
//
// Phase 6: when `meeting_dir` is non-empty, the engine's result callback is
// wrapped with a `CaptionFanoutAdapter` that ALSO appends finalized cues to
// `<meeting_dir>/captions.vtt`. The adapter is returned via `out_adapter`
// so the caller can hand its lifetime to ActiveCaptionEngine. When
// `meeting_dir` is empty (e.g. a test that doesn't want a sidecar) the
// engine is wired directly to the daemon's hooks — no adapter, no writer.
std::unique_ptr<CaptionEngine> try_start_caption_engine(
        const JobConfig& cfg, const CaptionHooks* hooks,
        const fs::path& meeting_dir,
        std::unique_ptr<CaptionFanoutAdapter>& out_adapter) {
    out_adapter.reset();
    if (!hooks) return nullptr;
    auto engine = std::make_unique<CaptionEngine>();
    CaptionEngine::Options opts;
    opts.model_dir = resolve_caption_model_dir(cfg.caption_model).string();
    opts.num_threads = 1;  // Phase 4 will surface a config knob.

    // Choose the result-callback wiring: direct (no sidecar) or fan-out.
    CaptionResultCallback result_cb = hooks->on_result;
    void* result_ud = hooks->result_ud;
    std::unique_ptr<CaptionFanoutAdapter> adapter;
    if (!meeting_dir.empty()) {
        adapter = std::make_unique<CaptionFanoutAdapter>();
        adapter->downstream_on_result = hooks->on_result;
        adapter->downstream_result_ud = hooks->result_ud;
        adapter->vtt = std::make_unique<VttWriter>(
            meeting_dir / "captions.vtt", cfg.caption_normalize_display);
        result_cb = &caption_fanout_on_result;
        result_ud = adapter.get();
    }

    bool ok = engine->start(opts,
                            result_cb,          result_ud,
                            hooks->on_degraded, hooks->degraded_ud);
    if (!ok) {
        std::string err = engine->last_error();
        log_warn("captions: engine failed to start (%s) — continuing without captions",
                 err.c_str());
        if (hooks->on_engine_error) {
            hooks->on_engine_error(err, hooks->engine_error_ud);
        }
        return nullptr;
    }
    log_info("captions: streaming engine started (model=%s)",
             opts.model_dir.c_str());
    out_adapter = std::move(adapter);
    return engine;
}

} // anonymous namespace

PostprocessInput run_recording(const JobConfig& cfg, StopToken& stop, PhaseCallback on_phase,
                               const CaptionHooks* caption_hooks) {
    log_debug("pipeline: run_recording ENTER (mic=%s, monitor=%s)",
              cfg.mic_source.c_str(), cfg.monitor_source.c_str());

    auto phase = [&](const std::string& name) {
        if (on_phase) on_phase(name);
    };

    PostprocessInput pp;

    if (!cfg.reprocess_dir.empty()) {
        // --- Reprocess existing recording (file or directory) ---
        // Resolve relative paths: try as-is first, then relative to output_dir
        fs::path reprocess_path = cfg.reprocess_dir;
        if (!fs::exists(reprocess_path) && reprocess_path.is_relative()) {
            fs::path candidate = cfg.output_dir / reprocess_path;
            if (fs::exists(candidate))
                reprocess_path = candidate;
        }

        pp.audio_path = validate_reprocess_input(reprocess_path);

        // Determine output directory
        if (cfg.output_dir_explicit) {
            pp.out_dir = fs::weakly_canonical(cfg.output_dir);
        } else {
            // Use the directory containing the audio file as output dir
            pp.out_dir = fs::canonical(pp.audio_path.parent_path());
        }
        fs::create_directories(pp.out_dir);
        pp.timestamp = derive_meeting_timestamp(pp.out_dir);
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
        auto out = create_output_dir(cfg.output_dir);
        pp.out_dir = fs::weakly_canonical(out.path);
        pp.timestamp = out.timestamp;
        log_info("Output directory: %s", pp.out_dir.c_str());

        pp.audio_path = pp.out_dir / (std::string(AUDIO_PREFIX) + out.timestamp + ".wav");

        // --- Record ---
        phase("recording");

        // Phase 3: caption engine is opt-in per recording. Wired to the mic
        // capture only — monitor audio is recorded but not captioned in V1.
        // We instantiate AFTER the capture is constructed and started but
        // BEFORE the recording loop, so the producer-side callback is in
        // place for the full recording duration. Teardown is the inverse:
        //   cap.stop()  -> ActiveCaptionEngine dtor (unsub + engine.stop())
        //                -> cap.drain()
        // The dtor order is enforced by stack-frame nesting below.
        const bool want_captions = cfg.captions_enabled && caption_hooks != nullptr;

        if (dual_mode) {
            notify("Recording started", "Mic: " + mic_source + "\nMonitor: " + monitor_source);

            // Start mic capture via PipeWire
            PipeWireCapture mic_cap(mic_source);
            log_debug("pipeline: PipeWireCapture created");
            mic_cap.start();
            log_debug("pipeline: capture start (mic)");

            // Caption engine — wired to mic only (monitor audio is recorded
            // but not captioned in V1). Lifetime: from here through the
            // explicit `caption.reset()` after `mic_cap.stop()`.
            std::unique_ptr<ActiveCaptionEngine<PipeWireCapture>> caption;
            if (want_captions) {
                std::unique_ptr<CaptionFanoutAdapter> adapter;
                if (auto eng = try_start_caption_engine(cfg, caption_hooks,
                                                        pp.out_dir, adapter)) {
                    caption = std::make_unique<ActiveCaptionEngine<PipeWireCapture>>(
                        std::move(eng), &mic_cap, std::move(adapter));
                }
            }

            // Start monitor capture — try PipeWire CAPTURE_SINK first, fall back to pa_simple
            std::unique_ptr<PipeWireCapture> mon_pw;
            std::unique_ptr<PulseMonitorCapture> mon_pa;

            // For .monitor sources, go straight to pa_simple (pw_stream doesn't handle them)
            const std::string mon_suffix = ".monitor";
            bool is_pa_monitor = monitor_source.size() >= mon_suffix.size() &&
                monitor_source.compare(monitor_source.size() - mon_suffix.size(),
                                        mon_suffix.size(), mon_suffix) == 0;
            if (is_pa_monitor) {
                log_debug("pipeline: falling back to PulseMonitorCapture");
                mon_pa = std::make_unique<PulseMonitorCapture>(monitor_source);
                mon_pa->start();
                log_debug("pipeline: capture start (monitor)");
            } else {
                try {
                    mon_pw = std::make_unique<PipeWireCapture>(monitor_source, /*capture_sink=*/true);
                    mon_pw->start();
                    log_debug("pipeline: capture start (monitor)");
                } catch (const RecmeetError& e) {
                    log_warn("PipeWire monitor failed (%s), falling back to pa_simple", e.what());
                    log_debug("pipeline: falling back to PulseMonitorCapture");
                    mon_pa = std::make_unique<PulseMonitorCapture>(monitor_source);
                    mon_pa->start();
                    log_debug("pipeline: capture start (monitor)");
                }
            }

            // Display timer and wait for stop
            StopToken timer_stop;
            std::thread timer_thread(display_elapsed, std::ref(timer_stop));

            while (!stop.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

            log_debug("pipeline: stop requested, draining audio");
            timer_stop.request();
            timer_thread.join();
            fprintf(stderr, "Recording stopped.\n");

            // Stop captures. After cap.stop() returns, the capture-thread
            // callback no longer fires, so it is safe to tear down the
            // caption engine before draining the buffer.
            mic_cap.stop();
            if (mon_pw) mon_pw->stop();
            if (mon_pa) mon_pa->stop();

            // Phase 3 teardown ordering: cap.stop() -> engine teardown ->
            // cap.drain(). The engine's worker drains its own ring buffer
            // and joins inside the destructor; explicitly resetting here
            // makes the ordering visible (and asserts vs. unsubscribe by
            // the time drain() runs).
            caption.reset();

            // Drain and write
            auto mic_samples = mic_cap.drain();
            auto mon_samples = mon_pw ? mon_pw->drain() : mon_pa->drain();
            log_debug("pipeline: drained audio (%.1fs)",
                      mic_samples.size() / (float)SAMPLE_RATE);

            fs::path mic_path = pp.out_dir / "mic.wav";
            fs::path mon_path = pp.out_dir / "monitor.wav";
            write_wav(mic_path, mic_samples);
            log_debug("pipeline: wrote %s", mic_path.c_str());
            write_wav(mon_path, mon_samples);
            log_debug("pipeline: wrote %s", mon_path.c_str());

            // Validate mic (fatal)
            validate_audio(mic_path, 1.0, "Mic audio");

            // Validate monitor (non-fatal)
            try {
                validate_audio(mon_path, 1.0, "Monitor audio");
                // Mix
                auto mixed = mix_audio(mic_samples, mon_samples);
                write_wav(pp.audio_path, mixed);
                log_info("Mixed audio saved: %s", pp.audio_path.c_str());
                log_debug("pipeline: wrote %s", pp.audio_path.c_str());
            } catch (const AudioValidationError& e) {
                log_warn("Monitor audio unusable (%s). Using mic only.", e.what());
                write_wav(pp.audio_path, mic_samples);
                log_debug("pipeline: wrote %s", pp.audio_path.c_str());
            }

            // Clean up source files unless --keep-sources
            if (!cfg.keep_sources) {
                fs::remove(mic_path);
                fs::remove(mon_path);
            }
        } else {
            notify("Recording started", "Source: " + mic_source);

            PipeWireCapture cap(mic_source);
            log_debug("pipeline: PipeWireCapture created");
            cap.start();
            log_debug("pipeline: capture start (mic)");

            // Caption engine — wired to the single mic capture. Same
            // teardown ordering applies: cap.stop() -> caption.reset() ->
            // cap.drain().
            std::unique_ptr<ActiveCaptionEngine<PipeWireCapture>> caption;
            if (want_captions) {
                std::unique_ptr<CaptionFanoutAdapter> adapter;
                if (auto eng = try_start_caption_engine(cfg, caption_hooks,
                                                        pp.out_dir, adapter)) {
                    caption = std::make_unique<ActiveCaptionEngine<PipeWireCapture>>(
                        std::move(eng), &cap, std::move(adapter));
                }
            }

            StopToken timer_stop;
            std::thread timer_thread(display_elapsed, std::ref(timer_stop));

            while (!stop.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

            log_debug("pipeline: stop requested, draining audio");
            timer_stop.request();
            timer_thread.join();
            fprintf(stderr, "Recording stopped.\n");

            cap.stop();
            // Phase 3 teardown ordering: cap.stop() -> engine teardown ->
            // cap.drain(). See dual_mode branch above for the rationale.
            caption.reset();
            auto samples = cap.drain();
            log_debug("pipeline: drained audio (%.1fs)",
                      samples.size() / (float)SAMPLE_RATE);
            write_wav(pp.audio_path, samples);
            log_debug("pipeline: wrote %s", pp.audio_path.c_str());
            validate_audio(pp.audio_path);
        }

    }

    log_debug("pipeline: run_recording EXIT (out_dir=%s)", pp.out_dir.c_str());
    return pp;
}

PipelineResult run_pipeline(const JobConfig& cfg, StopToken& stop, PhaseCallback on_phase) {
    auto input = run_recording(cfg, stop, on_phase);
    if (cfg.reprocess_dir.empty()) {
        save_meeting_context(input.out_dir, cfg.context_inline, cfg.context_file, input.timestamp);
    }
    return run_postprocessing(cfg, input, on_phase);
}

} // namespace recmeet
