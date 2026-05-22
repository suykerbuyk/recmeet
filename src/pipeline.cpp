// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "pipeline.h"
#include "caption_engine.h"
#include "caption_start_channel.h"
#include "caption_vtt.h"
#include "config.h"
#include "diarize.h"
#include "ipc_protocol.h"
#include "speaker_id.h"
#include "vad.h"
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
#include <cstdlib>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
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

void save_meeting_context(const fs::path& out_dir, const std::string& context_inline,
                          const fs::path& context_file, const std::string& timestamp) {
    if (context_inline.empty() && context_file.empty()) return;
    fs::path path = timestamp.empty()
        ? out_dir / LEGACY_CONTEXT_NAME
        : out_dir / (std::string(CONTEXT_PREFIX) + timestamp + ".json");
    std::ofstream out(path);
    if (!out) {
        log_warn("Failed to write %s: %s", path.filename().c_str(), path.c_str());
        return;
    }
    out << "{\"context\":\"" << json_escape(context_inline)
        << "\",\"context_file\":\"" << json_escape(context_file.string()) << "\"}";
    log_info("Saved %s", path.filename().c_str());
}

std::string load_meeting_context(const fs::path& out_dir) {
    fs::path path = find_context_file(out_dir);
    if (path.empty()) return "";
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string json = buf.str();

    // Minimal parse: extract "context" value using IPC parser
    IpcMessage msg;
    std::string wrapped = "{\"id\":0,\"result\":" + json + "}";
    if (parse_ipc_message(wrapped, msg) && msg.type == IpcMessageType::Response) {
        auto it = msg.response.result.find("context");
        if (it != msg.response.result.end())
            return json_val_as_string(it->second);
    }
    return "";
}

std::string resolve_context_text(const Config& cfg, const fs::path& out_dir) {
    // Inline > file > saved context.json (reprocess fallback). Preserves the
    // pre-Phase-B summarizer-prep merge semantics exactly (former site
    // `run_postprocessing()` post-diarize block).
    std::string context_text = cfg.context_inline;
    if (!cfg.context_file.empty()) {
        std::string file_ctx = read_context_file(cfg.context_file);
        if (!file_ctx.empty()) {
            if (!context_text.empty()) context_text += "\n\n";
            context_text += file_ctx;
        }
    }
    if (context_text.empty() && !cfg.reprocess_dir.empty()) {
        context_text = load_meeting_context(out_dir);
    }
    return context_text;
}

// Phase C: parse `Participants:` lines from the resolved context-text.
//
// Scan line-by-line and match `^\s*Participants?\s*:\s*(.+)$` (case-insensitive;
// singular form accepted defensively). Split the captured group on `,`, then
// each segment on ` and ` / ` & ` (case-insensitive). Trim whitespace; drop
// empty entries. Multiple matching lines sum (defensive — handles split
// contexts). Returns 0 if no Participants line is present.
//
// No name validation: "Bob (maybe)" and "Carol if she joins" each count as 1.
// Phase B.1's collapse pass cleans up if listed names don't actually speak.
//
// Locale-independence: `std::regex::icase` for case-folding; no `collate`.
namespace {

// Trim ASCII whitespace from both ends. Locale-independent (matches the
// behavior of the rest of the parser).
std::string trim_ws(const std::string& s) {
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
               c == '\f' || c == '\v';
    };
    size_t b = 0;
    while (b < s.size() && is_ws(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && is_ws(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Split `s` on every occurrence of `delim` (case-insensitive ASCII match).
// Empty input → single empty segment; that's filtered by the caller.
std::vector<std::string> split_icase(const std::string& s, const std::string& delim) {
    std::vector<std::string> out;
    if (delim.empty()) { out.push_back(s); return out; }
    auto eq_icase = [](char a, char b) {
        unsigned char ua = static_cast<unsigned char>(a);
        unsigned char ub = static_cast<unsigned char>(b);
        if (ua >= 'A' && ua <= 'Z') ua = ua + ('a' - 'A');
        if (ub >= 'A' && ub <= 'Z') ub = ub + ('a' - 'A');
        return ua == ub;
    };
    size_t start = 0;
    for (size_t i = 0; i + delim.size() <= s.size(); ) {
        bool match = true;
        for (size_t j = 0; j < delim.size(); ++j) {
            if (!eq_icase(s[i + j], delim[j])) { match = false; break; }
        }
        if (match) {
            out.push_back(s.substr(start, i - start));
            i += delim.size();
            start = i;
        } else {
            ++i;
        }
    }
    out.push_back(s.substr(start));
    return out;
}

}  // namespace

int parse_context_participants(const std::string& context) {
    if (context.empty()) return 0;

    // Anchored line regex: leading/trailing whitespace tolerated; singular
    // "Participant:" matched defensively; capture group is the list payload.
    // Note: do NOT use std::regex::multiline (C++17-portable but inconsistent
    // across libstdc++ versions) — we tokenize lines ourselves.
    static const std::regex line_re(
        R"(^\s*Participants?\s*:\s*(.+?)\s*$)",
        std::regex::icase | std::regex::ECMAScript);

    int total = 0;

    // Tokenize on '\n'. The final segment (possibly without a trailing
    // newline) is still emitted by this loop because we walk past end().
    size_t pos = 0;
    const size_t n = context.size();
    while (pos <= n) {
        size_t nl = context.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? context.substr(pos)
            : context.substr(pos, nl - pos);
        // Strip trailing '\r' to tolerate CRLF input defensively.
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::smatch m;
        if (std::regex_match(line, m, line_re) && m.size() >= 2) {
            const std::string payload = m[1].str();
            // First-level split on comma.
            std::vector<std::string> segs;
            {
                std::string cur;
                for (char c : payload) {
                    if (c == ',') { segs.push_back(cur); cur.clear(); }
                    else { cur.push_back(c); }
                }
                segs.push_back(cur);
            }
            // Second-level: split each segment on " and " then " & ".
            for (const std::string& seg : segs) {
                std::vector<std::string> a = split_icase(seg, " and ");
                for (const std::string& sub_a : a) {
                    std::vector<std::string> b = split_icase(sub_a, " & ");
                    for (const std::string& tok : b) {
                        if (!trim_ws(tok).empty()) ++total;
                    }
                }
            }
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return total;
}

int resolve_target_speakers(int cli_num_speakers,
                            int context_speaker_count,
                            int max_auto_speakers,
                            const char** source_out) {
    int target;
    const char* source;
    if (cli_num_speakers > 0) {
        target = cli_num_speakers;
        source = "--num-speakers";
    } else if (context_speaker_count > 0) {
        target = context_speaker_count;
        source = "context";
    } else {
        target = max_auto_speakers;
        source = "max_auto";
    }
    if (source_out) *source_out = source;
    return target;
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

fs::path resolve_caption_model_dir(const std::string& name) {
    // Default model directory name pinned by Phase 0.2. Phase 4 will
    // expand this into proper config plumbing; for now this string is the
    // single source of truth.
    constexpr const char* kDefaultCaptionModel = "en-2023-06-26";
    std::string subdir = name.empty() ? std::string(kDefaultCaptionModel) : name;

    const char* home = std::getenv("HOME");
    fs::path base = home && home[0]
        ? fs::path(home) / ".local" / "share" / "recmeet"
        : fs::path("/tmp") / "recmeet";
    return base / "models" / "sherpa" / "online" / subdir;
}

namespace {

// Owner of an active streaming caption engine + its capture-side
// subscription. Construction wires the engine to the capture's
// set_audio_callback; the destructor enforces the Phase 3 teardown
// ordering: the producer-side callback is unsubscribed first, THEN the
// engine is stopped (which joins its worker after draining the ring).
//
// In dual-mode recordings the engine is wired to the **monitor** capture
// (remote-speaker comprehension); in mic-only recordings it falls back
// to the mic capture. The template parameter resolves the bifurcation.
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
        const Config& cfg, const CaptionHooks* hooks,
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
    // Phase 2: notify the daemon that the engine + adapter are constructed so
    // it can broadcast `caption.started` to all subscribed clients. Mirrors
    // the on_engine_error null-check pattern above so test harnesses that
    // omit the hook still build.
    if (hooks->on_engine_started) {
        hooks->on_engine_started(hooks->engine_started_ud);
    }
    out_adapter = std::move(adapter);
    return engine;
}

} // anonymous namespace

PostprocessInput run_recording(const Config& cfg, StopToken& stop, PhaseCallback on_phase,
                               const CaptionHooks* caption_hooks) {
    // Phase 2: belt-and-braces clear of the caption-start channel so any
    // stale state from a prior recording's race window (between
    // `g_rec_stop.request()` and `g_recording.store(false)`) cannot leak in.
    // The matching cleanup at the bottom of this function covers the normal
    // exit path; this one covers re-entry on the next recording.
    reset_caption_start_channel();

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
        //
        // Phase 2 (captions-mid-recording-ipc-verb rev 4): `want_captions`
        // is the INITIAL state only — whether to construct the engine at
        // recording start. It is no longer the only gate on the caption
        // engine's lifetime within the recording. Post-Phase-2, the tray
        // can request a mid-recording engine start via the
        // `captions.start_engine` verb, which queues a request that the
        // 200ms worker loop drains regardless of `want_captions`. The
        // authoritative "is an engine running for this recording?" signal
        // is `engine_running` inside caption_start_channel.cpp; the
        // `caption_hooks` pointer is now always non-null when the daemon
        // is in the loop (see daemon.cpp:1102), so callers MUST NOT use
        // `caption_hooks != nullptr` as a runtime captions-enabled gate.
        const bool want_captions = cfg.captions_enabled && caption_hooks != nullptr;

        if (dual_mode) {
            notify("Recording started", "Mic: " + mic_source + "\nMonitor: " + monitor_source);

            // Start mic capture via PipeWire
            PipeWireCapture mic_cap(mic_source);
            log_debug("pipeline: PipeWireCapture created");
            mic_cap.start();
            log_debug("pipeline: capture start (mic)");

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

            // Caption engine — wired to monitor in dual mode (remote-speaker
            // comprehension). Mic audio is recorded and transcribed in
            // post-processing but not live-captioned. Lifetime: from here through
            // the explicit `caption_*.reset()` after the captures stop.
            std::unique_ptr<ActiveCaptionEngine<PipeWireCapture>>     caption_pw;
            std::unique_ptr<ActiveCaptionEngine<PulseMonitorCapture>> caption_pa;
            if (want_captions) {
                std::unique_ptr<CaptionFanoutAdapter> adapter;
                if (auto eng = try_start_caption_engine(cfg, caption_hooks,
                                                        pp.out_dir, adapter)) {
                    if (mon_pw) {
                        caption_pw = std::make_unique<ActiveCaptionEngine<PipeWireCapture>>(
                            std::move(eng), mon_pw.get(), std::move(adapter));
                    } else {
                        caption_pa = std::make_unique<ActiveCaptionEngine<PulseMonitorCapture>>(
                            std::move(eng), mon_pa.get(), std::move(adapter));
                    }
                    // Phase 2: the channel only reports `engine_running=true`
                    // once both the engine AND the audio callback are wired
                    // (ActiveCaptionEngine ctor publishes the callback).
                    // Placement inside the success branch matters: a failed
                    // engine init must NOT mark the channel running, or a
                    // verb caller would see `already_running` while no engine
                    // exists.
                    mark_caption_engine_running();
                }
            }

            // Phase 2: mid-recording engine-start closure. Captures the
            // monitor-side captures (mon_pw / mon_pa) — the engine wires to
            // the monitor in dual mode, NOT the mic — plus both
            // ActiveCaptionEngine slots, the output dir, the hooks pointer,
            // and the per-recording cfg snapshot. The closure runs on this
            // worker thread when the 200ms loop drains a pending verb
            // request. It does NOT call mark_caption_engine_running() —
            // poll_and_handle_caption_start_request handles the atomic
            // (F,T,T) → (T,F,T) transition after start_fn returns true.
            auto start_fn = [&caption_hooks, &cfg, &pp,
                             &mon_pw, &mon_pa,
                             &caption_pw, &caption_pa]
                            (const std::string& model_override) -> bool {
                Config local_cfg = cfg;
                if (!model_override.empty()) {
                    local_cfg.caption_model = model_override;
                }
                std::unique_ptr<CaptionFanoutAdapter> adapter;
                auto eng = try_start_caption_engine(local_cfg, caption_hooks,
                                                    pp.out_dir, adapter);
                if (!eng) return false;
                if (mon_pw) {
                    caption_pw = std::make_unique<ActiveCaptionEngine<PipeWireCapture>>(
                        std::move(eng), mon_pw.get(), std::move(adapter));
                } else {
                    caption_pa = std::make_unique<ActiveCaptionEngine<PulseMonitorCapture>>(
                        std::move(eng), mon_pa.get(), std::move(adapter));
                }
                return true;
            };

            // Phase 2: open the verb-side gate IMMEDIATELY before entering
            // the polling loop. After this, request_caption_engine_start is
            // willing to return Queued. Paired with clear_worker_active()
            // on loop exit so the gate closes BEFORE any teardown — closing
            // the race window between g_rec_stop.request() and g_recording=false.
            mark_worker_active();

            // Display timer and wait for stop
            StopToken timer_stop;
            std::thread timer_thread(display_elapsed, std::ref(timer_stop));

            while (!stop.stop_requested()) {
                // Phase 2: drain any pending captions.start_engine request
                // before sleeping. start_fn blocks 1-2 s on engine init;
                // that's documented as Risk R10 (stop-token check delayed
                // for the duration of start_fn — same property as the
                // record.start-time engine init).
                poll_and_handle_caption_start_request(start_fn);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            log_debug("pipeline: stop requested, draining audio");
            // Phase 2: close the verb-side gate IMMEDIATELY on loop exit,
            // BEFORE any teardown. Verb callers arriving during teardown now
            // see WorkerNotReady and the daemon maps that to NotRecording
            // with the "Recording is ending..." message.
            clear_worker_active();
            timer_stop.request();
            timer_thread.join();
            fprintf(stderr, "Recording stopped.\n");

            // Stop captures. After capture.stop() returns, the capture-thread
            // callback no longer fires, so it is safe to tear down the
            // caption engine before draining the buffers.
            mic_cap.stop();
            if (mon_pw) mon_pw->stop();
            if (mon_pa) mon_pa->stop();

            // Phase 3 teardown ordering: capture.stop() -> caption engine
            // teardown -> capture.drain(). The engine is wired to the monitor
            // capture in dual mode; its worker drains its own ring buffer and
            // joins inside the destructor. Explicitly resetting both slots
            // here makes the ordering visible (only one is non-null).
            caption_pw.reset();
            caption_pa.reset();

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
                    // Phase 2: see dual-mode branch above for the placement
                    // rationale — only mark running when the audio callback
                    // is also wired.
                    mark_caption_engine_running();
                }
            }

            // Phase 2: mid-recording engine-start closure for mic-only mode.
            // Wires to the single mic capture (&cap). Same closure contract
            // as the dual-mode start_fn above.
            auto start_fn = [&caption_hooks, &cfg, &pp,
                             &cap, &caption]
                            (const std::string& model_override) -> bool {
                Config local_cfg = cfg;
                if (!model_override.empty()) {
                    local_cfg.caption_model = model_override;
                }
                std::unique_ptr<CaptionFanoutAdapter> adapter;
                auto eng = try_start_caption_engine(local_cfg, caption_hooks,
                                                    pp.out_dir, adapter);
                if (!eng) return false;
                caption = std::make_unique<ActiveCaptionEngine<PipeWireCapture>>(
                    std::move(eng), &cap, std::move(adapter));
                return true;
            };

            // Phase 2: see dual-mode branch above for the worker-active
            // gating rationale.
            mark_worker_active();

            StopToken timer_stop;
            std::thread timer_thread(display_elapsed, std::ref(timer_stop));

            while (!stop.stop_requested()) {
                poll_and_handle_caption_start_request(start_fn);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            log_debug("pipeline: stop requested, draining audio");
            clear_worker_active();
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

    // Phase 2: final cleanup of the caption-start channel — matches the
    // reset at entry. This covers the reprocess path (which never calls
    // mark_worker_active) and the post-loop cleanup window so the next
    // recording starts with a known-zeroed channel.
    reset_caption_start_channel();

    log_debug("pipeline: run_recording EXIT (out_dir=%s)", pp.out_dir.c_str());
    return pp;
}

/// Compute weighted progress across VAD segments.
/// Returns overall percent (0-100) given current segment index and within-segment percent.
int vad_weighted_progress(size_t seg_index, int seg_percent, const std::vector<size_t>& seg_samples) {
    if (seg_samples.empty()) return 0;
    size_t total = 0;
    for (auto n : seg_samples) total += n;
    if (total == 0) return 0;

    size_t done = 0;
    for (size_t i = 0; i < seg_index && i < seg_samples.size(); ++i)
        done += seg_samples[i];
    if (seg_index < seg_samples.size())
        done += static_cast<size_t>(seg_samples[seg_index] * seg_percent / 100.0);

    return static_cast<int>(done * 100 / total);
}

std::string build_initial_prompt(const std::vector<std::string>& speaker_names,
                                 const std::string& vocabulary) {
    std::string result;

    for (const auto& name : speaker_names) {
        if (!result.empty()) result += ", ";
        result += name;
    }

    if (!vocabulary.empty()) {
        std::istringstream ss(vocabulary);
        std::string token;
        while (std::getline(ss, token, ',')) {
            auto start = token.find_first_not_of(" \t");
            auto end = token.find_last_not_of(" \t");
            if (start == std::string::npos) continue;
            std::string trimmed = token.substr(start, end - start + 1);
            if (!result.empty()) result += ", ";
            result += trimmed;
        }
    }

    return result;
}

PipelineResult run_postprocessing(const Config& cfg, const PostprocessInput& input,
                                  PhaseCallback on_phase, ProgressCallback on_progress,
                                  StopToken* stop) {
    log_debug("pipeline: run_postprocessing ENTER (dir=%s)", input.out_dir.c_str());

    auto phase = [&](const std::string& name) {
        if (on_phase) on_phase(name);
    };

    auto check_cancel = [&]() {
        if (stop && stop->stop_requested())
            throw RecmeetError("Cancelled");
    };

    int threads = cfg.threads > 0 ? cfg.threads : default_thread_count();

    // Build initial_prompt from enrolled speaker names + vocabulary hints
    std::string initial_prompt;
    {
        std::vector<std::string> names;
        if (cfg.speaker_id) {
            fs::path db_dir = cfg.speaker_db.empty()
                ? default_speaker_db_dir() : cfg.speaker_db;
            names = list_speakers(db_dir);
        }
        initial_prompt = build_initial_prompt(names, cfg.vocabulary);
        if (!initial_prompt.empty())
            log_info("Vocabulary hints: %s", initial_prompt.c_str());
    }

    // Phase B.0: resolve the full context-text up front so Phase C's
    // `parse_context_participants` can feed the diarize-side target_speakers
    // precedence chain on reprocess scenarios (where the only source is
    // `load_meeting_context(out_dir)`, otherwise read only post-diarize by
    // the summarizer-prep block). Behavior preserved: the summarizer below
    // reads this same `context_text` value instead of rebuilding it.
    std::string context_text = resolve_context_text(cfg, input.out_dir);

    // --- Transcribe + Diarize (if not pre-computed) ---
    std::string transcript_text = input.transcript_text;

    if (transcript_text.empty()) {
        log_info("Using %d threads for inference.", threads);

        TranscriptResult result;
        {   // --- audio buffer scope --- freed after diarization, before summarization
            auto samples = read_wav_float(input.audio_path);
            log_info("Audio: %.1fs (%zu samples)",
                    samples.size() / (float)SAMPLE_RATE, samples.size());
            log_debug("pipeline: loaded audio (%.1fs, %zu samples)",
                      samples.size() / (float)SAMPLE_RATE, samples.size());

            {   // --- whisper model scope --- freed before diarization
                log_debug("pipeline: loading whisper model '%s'...", cfg.whisper_model.c_str());
                fs::path model_path = ensure_whisper_model(cfg.whisper_model);
                WhisperModel model(model_path);
                log_debug("pipeline: whisper model loaded");

#if RECMEET_USE_SHERPA
                if (cfg.vad) {
                    phase("detecting speech");
                    notify("Detecting speech...", "VAD segmentation");

                    VadConfig vad_cfg;
                    vad_cfg.threshold = cfg.vad_threshold;
                    vad_cfg.min_silence_duration = cfg.vad_min_silence;
                    vad_cfg.min_speech_duration = cfg.vad_min_speech;
                    vad_cfg.max_speech_duration = cfg.vad_max_speech;

                    log_debug("pipeline: running VAD");
                    auto vad_result = detect_speech(samples, vad_cfg, threads);
                    log_debug("pipeline: VAD complete (%zu speech segments)",
                              vad_result.segments.size());

                    if (!vad_result.segments.empty()) {
                        phase("transcribing");
                        notify("Transcribing...", "Model: " + cfg.whisper_model +
                               " (" + std::to_string(vad_result.segments.size()) + " segments)");

                        // Build sample counts for weighted progress
                        std::vector<size_t> seg_samples;
                        for (const auto& seg : vad_result.segments)
                            seg_samples.push_back(static_cast<size_t>(seg.end_sample - seg.start_sample));

                        log_debug("pipeline: transcribing...");
                        for (size_t si = 0; si < vad_result.segments.size(); ++si) {
                            check_cancel();
                            const auto& seg = vad_result.segments[si];
                            size_t n = seg_samples[si];

                            TranscribeOptions opts;
                            opts.language = cfg.language;
                            opts.initial_prompt = initial_prompt;
                            opts.threads = threads;
                            opts.stop = stop;
                            if (on_progress) {
                                opts.on_progress = [&, si](int pct) {
                                    int overall = vad_weighted_progress(si, pct, seg_samples);
                                    on_progress("transcribing", overall);
                                };
                            }

                            auto seg_result = transcribe(model, samples.data() + seg.start_sample,
                                                         n, seg.start, opts);
                            for (auto& s : seg_result.segments)
                                result.segments.push_back(std::move(s));
                            if (result.language.empty())
                                result.language = seg_result.language;
                        }
                        log_info("Transcribed %zu segments across %zu VAD regions",
                                result.segments.size(), vad_result.segments.size());
                        log_debug("pipeline: transcription complete (%zu segments)",
                                  result.segments.size());
                    } else {
                        log_info("VAD found no speech — skipping transcription.");
                    }
                } else
#endif
                {
                    phase("transcribing");
                    notify("Transcribing...", "Model: " + cfg.whisper_model);

                    TranscribeOptions opts;
                    opts.language = cfg.language;
                    opts.initial_prompt = initial_prompt;
                    opts.threads = threads;
                    opts.stop = stop;
                    if (on_progress) {
                        opts.on_progress = [&](int pct) {
                            on_progress("transcribing", pct);
                        };
                    }

                    log_debug("pipeline: transcribing...");
                    result = transcribe(model, samples.data(), samples.size(), 0.0, opts);
                    log_info("Transcribed %d segments (language: %s)",
                            (int)result.segments.size(), result.language.c_str());
                    log_debug("pipeline: transcription complete (%zu segments)",
                              result.segments.size());
                }
            }   // whisper model freed

#if RECMEET_USE_SHERPA
            check_cancel();
            if (cfg.diarize && !result.segments.empty()) {
                phase("diarizing");
                notify("Diarizing...", "Identifying speakers");
                DiarizeProgressCallback diar_progress;
                if (on_progress) {
                    diar_progress = [&on_progress](int done, int total) {
                        on_progress("diarizing", total > 0 ? done * 100 / total : 0);
                    };
                }

                // T2.2 dispatch: chunked path triggers when audio is large
                // enough to actually produce ≥ 2 real chunks plus 2 minutes
                // of headroom. Below the threshold the single-call path runs
                // unchanged. Threshold formula pinned in plan rev 7 line 429.
                DiarizeChunkConfig chunk_cfg;
                chunk_cfg.chunk_minutes = cfg.chunk_minutes;
                chunk_cfg.overlap_seconds = cfg.chunk_overlap_sec;
                chunk_cfg.stitch_threshold = cfg.stitch_threshold;
                chunk_cfg.collapse_threshold = cfg.collapse_threshold;
                // Phase A instrumentation: pass dump path + meeting timestamp
                // into stitch_chunks. Empty path = no-op (hot-path negligible).
                chunk_cfg.debug_dump_centroids_path =
                    cfg.debug_dump_centroids_path.string();
                chunk_cfg.meeting_timestamp = input.timestamp;
                const size_t chunk_threshold_samples = static_cast<size_t>(
                    (chunk_cfg.chunk_minutes * 60.0f
                     + chunk_cfg.overlap_seconds + 120.0f)
                    * static_cast<float>(SAMPLE_RATE));
                const bool use_chunked =
                    samples.size() > chunk_threshold_samples;

                // Phase B.2: resolve `target_speakers` from the precedence
                // chain BEFORE invoking diarize (helper exposed in
                // pipeline.h so unit tests can exercise the formula
                // without running the full pipeline). Order:
                //   1. cfg.num_speakers (--num-speakers, explicit operator
                //      override wins everything)
                //   2. context_speaker_count (Phase C parser; stub returns 0
                //      until Phase C lands; reads the resolved context_text
                //      lifted up by Phase B.0)
                //   3. cfg.max_auto_speakers (default cap, 8 unless overridden)
                int context_speaker_count =
                    parse_context_participants(context_text);
                const char* target_source = nullptr;
                int target_speakers = resolve_target_speakers(
                    cfg.num_speakers, context_speaker_count,
                    cfg.max_auto_speakers, &target_source);
                log_info("Speaker target: %d (source: %s; "
                         "cli=%d, context=%d, max_auto=%d)",
                         target_speakers, target_source,
                         cfg.num_speakers, context_speaker_count,
                         cfg.max_auto_speakers);

                DiarizeResult diar;
                std::map<int, std::vector<float>> chunked_centroids;
                if (use_chunked) {
                    log_debug("pipeline: diarizing chunked "
                              "(target %d speakers, %.1f min chunks, %.1f s "
                              "overlap, stitch %.2f, collapse %.2f, threshold "
                              "%.2f)",
                              target_speakers, chunk_cfg.chunk_minutes,
                              chunk_cfg.overlap_seconds,
                              chunk_cfg.stitch_threshold,
                              chunk_cfg.collapse_threshold,
                              cfg.cluster_threshold);
                    // Phase 1a (diarize-apply-collapse-over-merges): the floor
                    // branch of `apply_collapse` only fires for CLI-explicit
                    // `--num-speakers N`. `target_source` was populated by
                    // `resolve_target_speakers` above; treat its label
                    // verbatim.
                    bool enforce_floor = (target_source != nullptr
                        && std::string(target_source) == "--num-speakers");
                    auto chunked = diarize_chunked(
                        samples.data(), samples.size(),
                        target_speakers, threads, cfg.cluster_threshold,
                        chunk_cfg, enforce_floor, diar_progress);
                    diar = std::move(chunked.diar);
                    chunked_centroids = std::move(chunked.centroids);
                    log_debug("pipeline: chunked diarization complete "
                              "(%zu segments, %zu centroids)",
                              diar.segments.size(), chunked_centroids.size());
                } else {
                    log_debug("pipeline: diarizing single-shot "
                              "(target %d speakers)", target_speakers);
                    // Phase B.4 design note: the free `diarize()` call's
                    // `num_speakers` param is forwarded to sherpa's
                    // FastClustering (the CLUSTER count knob), distinct from
                    // our post-stitch unified merge target. We pass
                    // `cfg.num_speakers` unchanged here — the operator's
                    // explicit --num-speakers wins (auto-detect = 0). Our
                    // own ceiling is applied below by `apply_collapse`.
                    diar = diarize(samples.data(), samples.size(),
                                   cfg.num_speakers, threads, cfg.cluster_threshold,
                                   diar_progress);
                    log_debug("pipeline: single-shot diarization complete "
                              "(%zu segments, %d speakers pre-collapse)",
                              diar.segments.size(), diar.num_speakers);

                    // Short-audio ghost-cluster defense (Phase A.3 follow-up
                    // to iter 194). Drop sub-threshold clusters BEFORE
                    // centroid extraction so `build_short_audio_globals`
                    // never embeds them and `apply_collapse` never sees
                    // them. Guarded by `!use_chunked` (this branch already
                    // is) and a >0 threshold so
                    // `--min-cluster-duration 0` disables the filter.
                    apply_short_audio_min_duration_filter(
                        diar, cfg.min_cluster_duration_sec);

                    // Phase B.3: short-audio post-collapse wiring. Build a
                    // synthetic globals vector by extracting one centroid per
                    // unique cluster ID over the cluster's segment audio,
                    // then run the unified greedy-merge loop with the
                    // precedence-resolved target_speakers ceiling and the
                    // collapse_threshold floor. Same machinery as the
                    // long-audio path (apply_collapse owns both).
                    //
                    // Phase A.1 instrumentation: dump TWICE when
                    // --debug-dump-centroids is set — pre-collapse and
                    // post-collapse, distinguished by the JSON `source`
                    // field. An investigator wants both: the pre-collapse
                    // state is what an over-count looks like before the
                    // fix; the post-collapse state is what an operator
                    // actually sees.
                    if (!diar.segments.empty()) {
                        try {
                            auto globals = build_short_audio_globals(
                                samples.data(), samples.size(), diar, threads);

                            if (!cfg.debug_dump_centroids_path.empty()
                                && !globals.empty()) {
                                std::vector<std::vector<float>> dump_c;
                                std::vector<long> dump_w;
                                copy_globals_for_dump(globals, dump_c, dump_w);
                                dump_centroids_json(
                                    cfg.debug_dump_centroids_path.string(),
                                    input.timestamp + "_pre",
                                    dump_c, dump_w,
                                    /*local_to_global=*/{},
                                    "diarize_short_audio_pre_collapse");
                            }

                            // Run the unified merge loop on the synthetic
                            // globals view. `apply_collapse` rewrites
                            // diar.segments[i].speaker by the merge map +
                            // compaction so downstream consumers
                            // (`identify_speakers_with_centroids`,
                            // meeting_speakers loop) see 0..M-1 contiguous
                            // IDs.
                            // Phase 1a (diarize-apply-collapse-over-merges):
                            // floor branch fires only for CLI-explicit
                            // `--num-speakers`. `target_source` came from
                            // `resolve_target_speakers` above.
                            bool enforce_floor = (target_source != nullptr
                                && std::string(target_source) == "--num-speakers");
                            auto collapsed = apply_collapse(
                                diar, globals, target_speakers,
                                cfg.collapse_threshold, enforce_floor);
                            chunked_centroids = collapsed.centroids;
                            log_debug("pipeline: short-audio apply_collapse "
                                      "complete (%d speakers post-collapse, "
                                      "%zu centroids)",
                                      diar.num_speakers,
                                      chunked_centroids.size());

                            if (!cfg.debug_dump_centroids_path.empty()
                                && !globals.empty()) {
                                std::vector<std::vector<float>> dump_c;
                                std::vector<long> dump_w;
                                copy_globals_for_dump(globals, dump_c, dump_w);
                                dump_centroids_json(
                                    cfg.debug_dump_centroids_path.string(),
                                    input.timestamp + "_post",
                                    dump_c, dump_w,
                                    /*local_to_global=*/{},
                                    "diarize_short_audio_post_collapse");
                            }
                        } catch (const std::exception& e) {
                            log_warn("pipeline: short-audio collapse failed: %s",
                                     e.what());
                        }
                    }
                }

                // Speaker identification + embedding extraction
                std::map<int, std::string> speaker_names;
                {
                    fs::path db_dir = cfg.speaker_db.empty()
                        ? default_speaker_db_dir() : cfg.speaker_db;
                    auto db = cfg.speaker_id ? load_speaker_db(db_dir)
                                             : std::vector<SpeakerProfile>{};

                    // Phase event fires regardless of db state — the embedding
                    // extraction call below is the long-running step and the
                    // daemon's progress-staleness watchdog needs the phase
                    // marker to recognize identify-speakers and apply the
                    // heartbeat-as-liveness rule (T1C.1).
                    phase("identifying speakers");
                    if (on_progress) on_progress("identifying speakers", 0);
                    if (!db.empty()) {
                        notify("Identifying speakers...",
                               std::to_string(db.size()) + " enrolled");
                    }
                    // Phase B.3: both code paths now populate
                    // `chunked_centroids` post-collapse — the long-audio path
                    // via stitch_chunks, the short-audio path via the
                    // apply_collapse wiring above. When centroids are
                    // available, use the bypass entry point (no second pass
                    // over audio); otherwise fall back to the legacy audio
                    // re-extract path (e.g. if the short-audio B.3 wiring
                    // bailed via the catch block above).
                    const bool centroid_bypass = !chunked_centroids.empty();
                    log_debug("pipeline: identifying speakers (%s)",
                              centroid_bypass ? "centroid bypass"
                                              : "audio re-extract");
                    IdentifyResult id_result;
                    if (centroid_bypass) {
                        // Reuse centroids already extracted during diarize —
                        // chunked stitching or short-audio apply_collapse.
                        // No second pass over the audio. Bypass T2.2 H1
                        // skips the ~10 GB working-set spike of the
                        // per-cluster re-extraction.
                        id_result = identify_speakers_with_centroids(
                            chunked_centroids, db, cfg.speaker_threshold);
                    } else {
                        auto model_paths = ensure_sherpa_models();
                        id_result = identify_speakers(
                            samples.data(), samples.size(), diar, db,
                            model_paths.embedding, cfg.speaker_threshold, threads);
                    }
                    if (on_progress) on_progress("identifying speakers", 100);
                    log_debug("pipeline: speaker ID complete");
                    speaker_names = id_result.names;

                    // Build and save speakers.json
                    std::vector<MeetingSpeaker> meeting_speakers;
                    for (const auto& [sid, emb] : id_result.embeddings) {
                        MeetingSpeaker ms{};
                        ms.cluster_id = sid;
                        ms.embedding = emb;

                        // Compute duration from diarization segments
                        for (const auto& seg : diar.segments)
                            if (seg.speaker == sid)
                                ms.duration_sec += static_cast<float>(seg.end - seg.start);

                        auto name_it = id_result.names.find(sid);
                        if (name_it != id_result.names.end()) {
                            ms.label = name_it->second;
                            ms.identified = true;
                            ms.confidence = id_result.scores[sid];
                        } else {
                            char buf[16];
                            std::snprintf(buf, sizeof(buf), "Speaker_%02d", sid + 1);
                            ms.label = buf;
                            ms.identified = false;
                            ms.confidence = 0.0f;
                        }
                        meeting_speakers.push_back(std::move(ms));
                    }

                    if (!meeting_speakers.empty()) {
                        try {
                            save_meeting_speakers(input.out_dir, meeting_speakers, input.timestamp);
                            log_info("Saved speakers.json with %zu speaker(s)",
                                     meeting_speakers.size());
                        } catch (const std::exception& e) {
                            log_warn("Failed to save speakers.json: %s", e.what());
                        }
                    }
                }

                result.segments = merge_speakers(result.segments, diar, speaker_names);
            }
#endif
        }   // audio buffer freed

        transcript_text = result.to_string();
        if (transcript_text.empty())
            throw RecmeetError("Transcription produced no text.");
    }

    // --- Summarize ---
    check_cancel();
    PipelineResult pipe_result;
    pipe_result.output_dir = input.out_dir;

    std::string summary_text;
    // Context text was resolved early via `resolve_context_text(cfg, input.out_dir)`
    // (Phase B.0) so Phase C's `parse_context_participants` could feed the
    // diarize-side target_speakers precedence chain. Read it here unchanged.

    MeetingMetadata metadata;

    if (!cfg.no_summary) {
        phase("summarizing");

#if RECMEET_USE_LLAMA
        if (!cfg.llm_model.empty()) {  // NOLINT(readability-misleading-indentation)
            // Local summarization
            if (!cfg.batch_mode) notify("Summarizing...", "Local LLM");
            log_debug("pipeline: summarizing (provider=local)");
            try {
                fs::path llm_path = ensure_llama_model(cfg.llm_model);
                summary_text = summarize_local(transcript_text, llm_path, context_text, threads, cfg.llm_mmap);
                log_debug("pipeline: summary complete");
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
            if (!cfg.batch_mode) notify("Summarizing...", "Sending to " + cfg.api_model);
            log_debug("pipeline: summarizing (provider=%s)", cfg.provider.c_str());
            try {
                summary_text = summarize_http(transcript_text, url,
                                               cfg.api_key, cfg.api_model, context_text);
                log_debug("pipeline: summary complete");
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
        auto [date_str, time_str] = resolve_meeting_time(input.out_dir, input.audio_path);

        MeetingData md;
        md.date = date_str;
        md.time = time_str;
        md.summary_text = summary_text;
        md.transcript_text = transcript_text;
        pipe_result.transcript_text = transcript_text;
        md.context_text = context_text;
        md.output_dir = input.out_dir;
        if (!cfg.note_dir.empty()) {
            md.note_dir = cfg.note_dir;
            fs::create_directories(md.note_dir);
        } else {
            md.note_dir = input.out_dir;
        }

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
    if (!cfg.batch_mode) notify("Meeting complete", input.out_dir.string());
    fprintf(stderr, "\nDone! Files in: %s\n", input.out_dir.c_str());
    log_debug("pipeline: run_postprocessing EXIT");
    return pipe_result;
}

PipelineResult run_pipeline(const Config& cfg, StopToken& stop, PhaseCallback on_phase) {
    auto input = run_recording(cfg, stop, on_phase);
    if (cfg.reprocess_dir.empty()) {
        save_meeting_context(input.out_dir, cfg.context_inline, cfg.context_file, input.timestamp);
    }
    return run_postprocessing(cfg, input, on_phase);
}

} // namespace recmeet
