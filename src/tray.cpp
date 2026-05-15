// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "audio_capture.h"
#include "caption_format.h"
#include "config.h"
#include "config_json.h"
#include "device_enum.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "log.h"
#include "notify.h"
#include "api_models.h"
#include "tray_capture.h"
#include "util.h"
#include "version.h"

#include <libayatana-appindicator/app-indicator.h>
#include <gtk/gtk.h>
#include <glib-unix.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace recmeet;

// Icon names (custom recmeet theme under share/icons/hicolor)
static const char* ICON_IDLE       = "recmeet-idle";
static const char* ICON_RECORDING  = "recmeet-recording";
static const char* ICON_PROCESSING = "recmeet-processing";

static const char* WHISPER_MODELS[] = {"tiny", "base", "small", "medium", "large-v3"};

struct LangEntry {
    const char* code;
    const char* label;
};

static const LangEntry LANGUAGES[] = {
    {"en", "English"},
    {"de", "German"},
    {"es", "Spanish"},
    {"fr", "French"},
    {"ja", "Japanese"},
    {"zh", "Chinese"},
    {"ko", "Korean"},
    {"pt", "Portuguese"},
    {"ru", "Russian"},
    {"it", "Italian"},
};

struct TrayState {
    AppIndicator* indicator = nullptr;

    Config cfg;
    IpcClient ipc;   // daemon connection
    std::string daemon_addr;  // --daemon ADDRESS or RECMEET_DAEMON_ADDR

    bool recording = false;
    bool postprocessing = false;
    bool downloading = false;
    bool is_reprocess = false;  // derived from state string for display
    bool daemon_connected = false;
    guint reconnect_timer = 0;
    int reconnect_delay = 1;  // exponential backoff: 1, 2, 4, 8, ... max 30s

    // GIOChannel for socket event integration with GTK main loop
    GIOChannel* ipc_channel = nullptr;
    guint ipc_watch = 0;

    // Current model download status (for status label)
    std::string download_model;

    // Progress tracking
    int progress_percent = -1;
    std::string current_phase;
    GtkWidget* status_menu_item = nullptr;

    // Cached sources
    std::vector<AudioSource> mics;
    std::vector<AudioSource> monitors;

    // Cached API models for current provider
    std::vector<std::string> cached_models;
    std::mutex models_mutex;
    bool models_fetching = false;
    std::string models_provider; // which provider the cache is for

    // Managed web server process
    pid_t web_server_pid = -1;  // -1 = not managed by us

    // Pre-recording context dialog
    struct {
        GtkWidget* window = nullptr;
        GtkWidget* subject_entry = nullptr;
        GtkWidget* participants_entry = nullptr;
        GtkWidget* notes_view = nullptr;
    } ctx;

    // Phase 5.1 — Live captions overlay. The GtkWindow is created lazily on
    // the first `caption` event of a recording (or up-front on `record.start`
    // when captions were enabled in the params); destroyed on tray exit. The
    // CaptionRenderState holds the rolling caption buffer; tick_id drives
    // periodic auto-hide checks while a recording is active.
    struct {
        GtkWidget* window = nullptr;
        GtkWidget* label  = nullptr;
        recmeet::CaptionRenderState state;
        guint tick_id = 0;        // GLib timer for auto-hide ticks
        bool captions_enabled_for_recording = false;  // honored at record.start
    } cap;

    // Phase B.2 — local capture state. The tray now owns the audio
    // capture lifecycle end-to-end:
    //   Idle (no `pw` instance) →
    //   Recording (`pw` running, WAV stager subscribed) →
    //   Stopped (capture torn down; WAV path waiting on user
    //            Submit/Discard/Save-for-later disposition).
    //
    // `pw` is a unique_ptr because PipeWireCapture's destructor calls
    // pw_thread_loop_stop / destroy which can take a non-trivial moment;
    // the unique_ptr lets us reset it on every stop without leaking
    // the previous instance during a quick start→stop→start sequence.
    //
    // The WAV stager accumulates int16 samples on its own vector,
    // protected by `wav_mtx`. start_capture clears the buffer, the
    // subscriber appends on every chunk, stop_capture drains it to a
    // file and resets to Idle.
    struct {
        std::unique_ptr<PipeWireCapture> pw;
        recmeet::PipeWireCapture::SubscriberHandle wav_handle = 0;
        std::mutex wav_mtx;
        std::vector<int16_t> wav_buffer;
        fs::path wav_path;             // currently-active staging WAV path
        std::string wav_timestamp;     // YYYY-MM-DD_HH-MM for sidecar
        std::string wav_source;        // mic source name used for this recording
        bool waiting_disposition = false;  // capture stopped, awaiting Submit/Discard/Save

        // Phase C.10a — live-caption streaming. When captions are enabled
        // for a recording the tray opens a `process.stream` session on the
        // daemon and stacks a SECOND fan-out subscriber on the B.1 capture
        // (the first is the WAV stager above). That subscriber appends PCM
        // to `stream_buffer` (RT thread, brief lock on `stream_mtx`); a GTK
        // timeout pump (`stream_pump_id`) drains the buffer on the GTK main
        // thread and ships it to the daemon as `0x03` frames via
        // IpcClient::send_stream_audio(). Draining on the GTK thread keeps
        // all socket writes single-threaded — the same thread every other
        // `ipc.call()` runs on — so there is no fd race with request/response
        // traffic. `stream_token` is the handle returned by `process.stream`,
        // used to `process.stream.cancel` on stop.
        recmeet::PipeWireCapture::SubscriberHandle stream_handle = 0;
        std::mutex stream_mtx;
        std::vector<int16_t> stream_buffer;
        std::string stream_token;       // empty → no live streaming session
        guint stream_pump_id = 0;       // GTK timeout draining stream_buffer
    } capture_state;

    // Phase A.6 + B-bonus — session.init bookkeeping. The tray sends
    // `session.init` exactly once per IPC connection, immediately after
    // the connect() call returns. The flag prevents duplicate sends and
    // resets on disconnect (handled in close-side code).
    bool session_inited = false;
};

static TrayState g_tray;

// Forward declarations
static void build_menu();
static void refresh_sources();
static void fetch_provider_models();
static void close_context_window();

// --- Helpers ---

// Strip common PipeWire/PulseAudio source name prefixes for display
static std::string source_hint(const std::string& name) {
    static const char* prefixes[] = {
        "alsa_input.", "alsa_output.", "bluez_input.", "bluez_output.",
    };
    for (const auto* prefix : prefixes) {
        size_t len = strlen(prefix);
        if (name.size() > len && name.compare(0, len, prefix) == 0)
            return name.substr(len);
    }
    return name;
}

static std::string source_label(const AudioSource& s) {
    return s.description + " (" + source_hint(s.name) + ")";
}

// --- Daemon connection ---

static void setup_ipc_watch();
static void teardown_ipc_watch();
static gboolean try_reconnect(gpointer);

static void update_state(bool rec, bool pp, bool dl, bool reproc = false) {
    bool was_recording = g_tray.recording;
    g_tray.recording = rec;
    g_tray.postprocessing = pp;
    g_tray.downloading = dl;
    g_tray.is_reprocess = reproc;

    // Close context window when recording ends (postprocessing begins)
    if (was_recording && !rec)
        close_context_window();

    // Phase 5.1 — hide and reset the captions overlay when recording ends.
    // (We don't destroy the GtkWindow here; recreating costs more than the
    // tiny widget retained between recordings. The destroy path runs at
    // tray exit.)
    if (was_recording && !rec) {
        g_tray.cap.captions_enabled_for_recording = false;
        if (g_tray.cap.window) {
            gtk_widget_hide(g_tray.cap.window);
            g_tray.cap.state.clear();
            // Re-render to clear out any leftover label content.
            if (g_tray.cap.label)
                gtk_label_set_markup(GTK_LABEL(g_tray.cap.label), "");
        }
    }

    if (!rec && !pp && !dl) {
        g_tray.progress_percent = -1;
        g_tray.current_phase.clear();
    }

    const char* icon = ICON_IDLE;
    const char* desc = "Idle";
    if (rec) {
        icon = ICON_RECORDING;
        desc = reproc ? "Reprocessing" : "Recording";
    } else if (pp) {
        icon = ICON_PROCESSING;
        desc = "Processing";
    } else if (dl) {
        icon = ICON_PROCESSING;
        desc = "Downloading";
    }
    app_indicator_set_icon_full(g_tray.indicator, icon, desc);
    build_menu();
}

// --- Phase 5.1 — Live captions overlay ---

static void caption_overlay_apply_markup() {
    if (!g_tray.cap.label) return;
    std::string markup = g_tray.cap.state.to_label_markup();
    if (markup.empty())
        gtk_label_set_markup(GTK_LABEL(g_tray.cap.label), "");
    else
        gtk_label_set_markup(GTK_LABEL(g_tray.cap.label), markup.c_str());
}

static gboolean caption_overlay_tick(gpointer) {
    auto now = recmeet::CaptionRenderState::Clock::now();
    if (!g_tray.cap.window) return G_SOURCE_CONTINUE;

    // Expire degraded marker when its TTL passes. The state machine keeps
    // the reason string until explicitly cleared — `degraded("")` resets it
    // and a re-apply of the markup drops the banner.
    static bool last_degraded_active = false;
    bool now_active = g_tray.cap.state.degraded_active(now);
    if (last_degraded_active && !now_active) {
        g_tray.cap.state.degraded("", now);   // clear marker
        caption_overlay_apply_markup();
    }
    last_degraded_active = now_active;

    // Auto-hide check.
    if (g_tray.cap.state.tick(now))
        gtk_widget_hide(g_tray.cap.window);

    return G_SOURCE_CONTINUE;
}

static void caption_overlay_create() {
    if (g_tray.cap.window) return;

    auto* win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_title(GTK_WINDOW(win), "recmeet captions");
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 600, 80);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_stick(GTK_WINDOW(win));
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);
    // Anchor near the bottom of the screen by default — most desktops
    // accept POPUP windows even with explicit x,y. If the WM ignores
    // these hints the window simply lands at the WM's default position.
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);

    auto* label = gtk_label_new("");
    gtk_label_set_use_markup(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 80);
    gtk_label_set_xalign(GTK_LABEL(label), 0.5f);
    gtk_label_set_yalign(GTK_LABEL(label), 0.5f);
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
    gtk_container_add(GTK_CONTAINER(win), label);

    g_tray.cap.window = win;
    g_tray.cap.label = label;

    // 500ms tick — drives auto-hide and degraded-banner expiration.
    if (!g_tray.cap.tick_id) {
        g_tray.cap.tick_id = g_timeout_add(500, caption_overlay_tick, nullptr);
    }
}

static void caption_overlay_show_with_markup() {
    if (!g_tray.cap.window) caption_overlay_create();
    caption_overlay_apply_markup();
    gtk_widget_show_all(g_tray.cap.window);
}

static void caption_overlay_hide() {
    if (g_tray.cap.window)
        gtk_widget_hide(g_tray.cap.window);
}

static void caption_overlay_destroy() {
    if (g_tray.cap.tick_id) {
        g_source_remove(g_tray.cap.tick_id);
        g_tray.cap.tick_id = 0;
    }
    if (g_tray.cap.window) {
        gtk_widget_destroy(g_tray.cap.window);
        g_tray.cap.window = nullptr;
        g_tray.cap.label = nullptr;
    }
    g_tray.cap.state.clear();
}

static void handle_ipc_event(const IpcEvent& ev) {
    log_debug("tray: event '%s'", ev.event.c_str());
    if (ev.event == "phase") {
        std::string name = json_val_as_string(ev.data.at("name"));
        log_info("[tray] Phase: %s", name.c_str());
        // Reset progress on phase change and update status label
        g_tray.current_phase = name;
        g_tray.progress_percent = -1;
        if (g_tray.status_menu_item) {
            std::string label = "Status: " + name + "...";
            // Capitalize first letter
            if (!label.empty() && label.size() > 8)
                label[8] = static_cast<char>(toupper(static_cast<unsigned char>(label[8])));
            gtk_menu_item_set_label(GTK_MENU_ITEM(g_tray.status_menu_item), label.c_str());
        }
    } else if (ev.event == "progress") {
        std::string phase = json_val_as_string(ev.data.at("phase"));
        int percent = static_cast<int>(json_val_as_int(ev.data.at("percent")));
        g_tray.current_phase = phase;
        g_tray.progress_percent = percent;
        // Update status label in-place (no full menu rebuild)
        if (g_tray.status_menu_item) {
            std::string label = "Status: " + phase + "... " + std::to_string(percent) + "%";
            // Capitalize first letter of phase
            if (!label.empty() && label.size() > 8)
                label[8] = static_cast<char>(toupper(static_cast<unsigned char>(label[8])));
            gtk_menu_item_set_label(GTK_MENU_ITEM(g_tray.status_menu_item), label.c_str());
        }
    } else if (ev.event == "state.changed") {
        auto err_it = ev.data.find("error");
        if (err_it != ev.data.end()) {
            std::string error = json_val_as_string(err_it->second);
            if (!error.empty())
                notify("Pipeline error", error);
        }
        // Phase B.2: tray owns its own `recording` state. The daemon
        // continues to broadcast `recording=true` whenever it has an
        // active reprocess job in the live-recording slot, but from the
        // tray operator's perspective that's a postprocess (the
        // recording is already done; the WAV is being chewed on). Fold
        // the daemon's `recording` / `reprocessing` flags into the
        // local `postprocessing` indicator instead.
        bool was_recording = g_tray.recording;
        auto rec_it = ev.data.find("recording");
        if (rec_it != ev.data.end()) {
            bool daemon_rec = json_val_as_bool(rec_it->second);
            bool pp  = json_val_as_bool(ev.data.at("postprocessing"));
            bool dl  = json_val_as_bool(ev.data.at("downloading"));
            // Preserve the local recording indicator — only the daemon
            // post-recording phases drive the tray's pp/dl bits now.
            update_state(g_tray.recording, pp || daemon_rec, dl, false);
        } else {
            // Legacy fallback: parse state string. Daemon-side recording
            // / reprocessing are remapped to local postprocessing as
            // above.
            std::string state = json_val_as_string(ev.data.at("state"));
            if (state == "idle") update_state(g_tray.recording, false, false);
            else if (state == "recording" || state == "reprocessing")
                update_state(g_tray.recording, true, false, false);
            else if (state == "postprocessing")
                update_state(g_tray.recording, true, false);
            else if (state == "downloading")
                update_state(g_tray.recording, false, true);
        }
        (void)was_recording;
    } else if (ev.event == "job.complete") {
        std::string note = json_val_as_string(ev.data.at("note_path"));
        std::string dir = json_val_as_string(ev.data.at("output_dir"));
        // Suppress per-meeting notifications during a reprocess-batch run; the
        // batch driver process emits a single end-of-batch summary instead.
        // Failure notifications above (state.changed with error) keep firing
        // regardless — operator attention is wanted on failures.
        bool batch_job = false;
        auto bj_it = ev.data.find("batch_job");
        if (bj_it != ev.data.end())
            batch_job = json_val_as_bool(bj_it->second);
        if (!note.empty() && !batch_job)
            notify("Meeting note ready", note);
        // Don't reset state here — the subsequent state.changed event handles it
    } else if (ev.event == "caption") {
        // Phase 5.1 — only render captions if this recording was started
        // with captions_enabled=true. The flag is captured at record.start.
        // Defensive: if the daemon ever forwards a caption without us
        // having opted in, drop it silently.
        if (!g_tray.cap.captions_enabled_for_recording) return;
        std::string text = json_val_as_string(ev.data.at("text"));
        bool is_partial = json_val_as_bool(ev.data.at("is_partial"));
        g_tray.cap.state.update(text, is_partial,
                                g_tray.cfg.caption_normalize_display);
        caption_overlay_show_with_markup();
    } else if (ev.event == "caption.degraded") {
        if (!g_tray.cap.captions_enabled_for_recording) return;
        std::string reason = json_val_as_string(ev.data.at("reason"));
        g_tray.cap.state.degraded(reason);
        caption_overlay_show_with_markup();
    } else if (ev.event == "model.downloading") {
        std::string status = json_val_as_string(ev.data.at("status"));
        if (status == "downloading") {
            g_tray.download_model = json_val_as_string(ev.data.at("model"));
            build_menu();  // update status label
        } else if (status == "complete") {
            std::string model = json_val_as_string(ev.data.at("model"));
            log_info("[tray] Model ready: %s", model.c_str());
        } else if (status == "error") {
            auto err_it = ev.data.find("error");
            std::string error = (err_it != ev.data.end())
                ? json_val_as_string(err_it->second) : "Unknown error";
            notify("Model download failed", error);
        }
    }
}

static gboolean on_ipc_data(GIOChannel*, GIOCondition cond, gpointer) {
    log_debug("tray: on_ipc_data (cond=%d)", (int)cond);
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        log_debug("tray: daemon disconnected");
        log_warn("[tray] Daemon disconnected");
        g_tray.daemon_connected = false;
        g_tray.session_inited = false;
        teardown_ipc_watch();
        g_tray.ipc.close_connection();
        // Phase B.2: keep tray-local recording state across disconnect —
        // a lost daemon connection does not stop the operator's
        // microphone, only the downstream postprocessing path.
        update_state(g_tray.recording, false, false);
        // Schedule reconnect with backoff
        g_tray.reconnect_delay = 1;
        g_tray.reconnect_timer = g_timeout_add_seconds(1, try_reconnect, nullptr);
        return G_SOURCE_REMOVE;
    }

    // Read and dispatch one round of data
    if (!g_tray.ipc.read_and_dispatch(0)) {
        log_debug("tray: daemon disconnected");
        log_warn("[tray] Daemon connection lost");
        g_tray.daemon_connected = false;
        g_tray.session_inited = false;
        teardown_ipc_watch();
        g_tray.ipc.close_connection();
        update_state(g_tray.recording, false, false);
        g_tray.reconnect_delay = 1;
        g_tray.reconnect_timer = g_timeout_add_seconds(1, try_reconnect, nullptr);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void setup_ipc_watch() {
    teardown_ipc_watch();
    if (g_tray.ipc.fd() < 0) return;
    g_tray.ipc_channel = g_io_channel_unix_new(g_tray.ipc.fd());
    g_io_channel_set_encoding(g_tray.ipc_channel, nullptr, nullptr);
    g_tray.ipc_watch = g_io_add_watch(g_tray.ipc_channel,
        static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
        on_ipc_data, nullptr);
}

static void teardown_ipc_watch() {
    if (g_tray.ipc_watch) {
        g_source_remove(g_tray.ipc_watch);
        g_tray.ipc_watch = 0;
    }
    if (g_tray.ipc_channel) {
        g_io_channel_unref(g_tray.ipc_channel);
        g_tray.ipc_channel = nullptr;
    }
}

static bool connect_to_daemon() {
    if (g_tray.daemon_connected) return true;
    if (!g_tray.ipc.connect()) return false;

    g_tray.ipc.set_event_callback(handle_ipc_event);
    g_tray.daemon_connected = true;
    setup_ipc_watch();

    // Sync state. Phase B.2 — the tray's local `recording` field is the
    // sole driver of the recording indicator; the daemon's
    // recording/reprocessing flags fold into the postprocessing
    // indicator (same shape as in the state.changed event handler).
    IpcResponse resp;
    IpcError err;
    if (g_tray.ipc.call("status.get", resp, err, 2000)) {
        auto rec_it = resp.result.find("recording");
        if (rec_it != resp.result.end()) {
            bool daemon_rec = json_val_as_bool(rec_it->second);
            bool pp  = json_val_as_bool(resp.result["postprocessing"]);
            bool dl  = json_val_as_bool(resp.result["downloading"]);
            update_state(g_tray.recording, pp || daemon_rec, dl, false);
        } else {
            // Legacy fallback
            std::string state = json_val_as_string(resp.result["state"]);
            if (state == "recording" || state == "reprocessing")
                update_state(g_tray.recording, true, false, false);
            else if (state == "postprocessing")
                update_state(g_tray.recording, true, false);
            else if (state == "downloading")
                update_state(g_tray.recording, false, true);
            else update_state(g_tray.recording, false, false);
        }
    }

    // Phase A.6 + B-bonus — establish the per-client session credential /
    // preference slot on the daemon. This replaces the per-`record.start`
    // config_to_map() blob; after the session.init, `record.start`
    // requests carry only `reprocess_dir`. The handshake is one-shot per
    // IPC connection; the flag clears on disconnect.
    if (!g_tray.session_inited) {
        JsonMap creds;
        creds["provider"] = g_tray.cfg.provider;
        if (!g_tray.cfg.api_key.empty())
            creds["api_key"] = g_tray.cfg.api_key;
        JsonMap nested_keys;
        for (const auto& [name, key] : g_tray.cfg.api_keys) {
            if (!key.empty()) nested_keys["api_keys." + name] = key;
        }
        // Per-provider keys are flattened into the credentials map as
        // dot-prefixed top-level fields; the daemon's session.init
        // handler pulls them out via the existing api_keys.<provider>
        // shape used by config_to_map().
        for (const auto& [k, v] : nested_keys) creds[k] = v;

        JsonMap prefs;
        if (!g_tray.cfg.output_dir.empty())
            prefs["output_dir"] = g_tray.cfg.output_dir.string();
        if (!g_tray.cfg.note_dir.empty())
            prefs["note_dir"] = g_tray.cfg.note_dir.string();
        if (!g_tray.cfg.language.empty())
            prefs["language"] = g_tray.cfg.language;
        if (!g_tray.cfg.vocabulary.empty())
            prefs["vocabulary"] = g_tray.cfg.vocabulary;
        if (!g_tray.cfg.mic_source.empty())
            prefs["mic_source"] = g_tray.cfg.mic_source;
        if (!g_tray.cfg.monitor_source.empty())
            prefs["monitor_source"] = g_tray.cfg.monitor_source;
        if (!g_tray.cfg.whisper_model.empty())
            prefs["whisper_model"] = g_tray.cfg.whisper_model;
        if (!g_tray.cfg.llm_model.empty()) {
            prefs["llm_model"] = g_tray.cfg.llm_model;
            prefs["summarization_backend"] = std::string("local");
        } else if (!g_tray.cfg.no_summary) {
            prefs["summarization_backend"] = std::string("http");
        }
        prefs["captions_enabled"] = g_tray.cfg.captions_enabled;

        IpcResponse sresp;
        IpcError serr;
        if (g_tray.ipc.session_init(creds, prefs, sresp, serr, 5000)) {
            g_tray.session_inited = true;
            log_debug("[tray] session.init ok");
        } else {
            log_warn("[tray] session.init failed: %s",
                     serr.message.empty() ? "unknown" : serr.message.c_str());
        }
    }

    log_info("[tray] Connected to daemon");
    return true;
}

static gboolean try_reconnect(gpointer) {
    g_tray.reconnect_timer = 0;
    log_debug("tray: reconnect attempt");
    if (connect_to_daemon()) {
        log_debug("tray: reconnected to daemon");
        g_tray.reconnect_delay = 1;  // reset backoff
        build_menu();
        return G_SOURCE_REMOVE;
    }
    log_debug("tray: reconnect failed");
    // Exponential backoff: 1, 2, 4, 8, 16, 30, 30, ...
    g_tray.reconnect_timer = g_timeout_add_seconds(
        g_tray.reconnect_delay, try_reconnect, nullptr);
    g_tray.reconnect_delay = std::min(g_tray.reconnect_delay * 2, 30);
    return G_SOURCE_REMOVE;
}

// --- API key prompt ---

static std::string prompt_api_key(const ProviderInfo& provider) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "API Key Required", nullptr,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK", GTK_RESPONSE_OK,
        nullptr);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    std::string label_text = std::string("Enter API key for ") + provider.display + ":";
    GtkWidget* label = gtk_label_new(label_text.c_str());
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 4);

    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 50);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 4);

    std::string hint = std::string("Get your key at ") + provider.base_url;
    GtkWidget* hint_label = gtk_label_new(hint.c_str());
    gtk_widget_set_opacity(hint_label, 0.6);
    gtk_box_pack_start(GTK_BOX(content), hint_label, FALSE, FALSE, 4);

    gtk_widget_show_all(dialog);

    std::string result;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const char* text = gtk_entry_get_text(GTK_ENTRY(entry));
        if (text && *text)
            result = text;
    }

    gtk_widget_destroy(dialog);
    return result;
}

// --- Pre-recording context dialog ---

static gboolean on_context_window_delete(GtkWidget*, GdkEvent*, gpointer) {
    // Hide instead of destroy so values persist
    gtk_widget_hide(g_tray.ctx.window);
    return TRUE;  // prevent destruction
}

static void show_context_window() {
    if (g_tray.ctx.window) {
        gtk_window_present(GTK_WINDOW(g_tray.ctx.window));
        return;
    }

    auto* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Meeting Context");
    gtk_window_set_default_size(GTK_WINDOW(win), 400, 280);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_container_set_border_width(GTK_CONTAINER(win), 12);
    g_signal_connect(win, "delete-event", G_CALLBACK(on_context_window_delete), nullptr);

    auto* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    // Hint label
    auto* hint = gtk_label_new("Fill in while recording. All fields optional.");
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    PangoAttrList* attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_style_new(PANGO_STYLE_ITALIC));
    gtk_label_set_attributes(GTK_LABEL(hint), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(vbox), hint, FALSE, FALSE, 0);

    // Subject
    auto* subject_label = gtk_label_new("Subject");
    gtk_widget_set_halign(subject_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), subject_label, FALSE, FALSE, 0);
    auto* subject = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(subject), "Meeting subject (optional)");
    gtk_box_pack_start(GTK_BOX(vbox), subject, FALSE, FALSE, 0);

    // Participants
    auto* part_label = gtk_label_new("Participants");
    gtk_widget_set_halign(part_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), part_label, FALSE, FALSE, 0);
    auto* participants = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(participants), "Participant names, comma-separated (optional)");
    gtk_box_pack_start(GTK_BOX(vbox), participants, FALSE, FALSE, 0);

    // Notes
    auto* notes_label = gtk_label_new("Notes");
    gtk_widget_set_halign(notes_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), notes_label, FALSE, FALSE, 0);
    auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 80);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    auto* notes = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(notes), GTK_WRAP_WORD);
    gtk_container_add(GTK_CONTAINER(scroll), notes);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    // Close button
    auto* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    auto* close_btn = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(gtk_widget_hide), win);
    gtk_box_pack_end(GTK_BOX(btn_box), close_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), btn_box, FALSE, FALSE, 0);

    g_tray.ctx.window = win;
    g_tray.ctx.subject_entry = subject;
    g_tray.ctx.participants_entry = participants;
    g_tray.ctx.notes_view = notes;

    gtk_widget_show_all(win);
}

/// Capture context values, destroy the window, return assembled context string.
/// Also returns comma-separated participant names for vocabulary hints.
struct CapturedContext {
    std::string context_inline;
    std::string vocab_additions;
};

static CapturedContext capture_and_clear_context() {
    CapturedContext result;
    if (!g_tray.ctx.window) return result;

    std::string subject = gtk_entry_get_text(GTK_ENTRY(g_tray.ctx.subject_entry));
    std::string participants = gtk_entry_get_text(GTK_ENTRY(g_tray.ctx.participants_entry));

    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_tray.ctx.notes_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gchar* notes_raw = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    std::string notes = notes_raw ? notes_raw : "";
    g_free(notes_raw);

    // Assemble context_inline
    if (!subject.empty())
        result.context_inline += "Subject: " + subject + "\n";
    if (!participants.empty()) {
        result.context_inline += "Participants: " + participants + "\n";
        result.vocab_additions = participants;
    }
    if (!notes.empty()) {
        if (!result.context_inline.empty()) result.context_inline += "\n";
        result.context_inline += notes;
    }

    // Destroy window
    gtk_widget_destroy(g_tray.ctx.window);
    g_tray.ctx.window = nullptr;
    g_tray.ctx.subject_entry = nullptr;
    g_tray.ctx.participants_entry = nullptr;
    g_tray.ctx.notes_view = nullptr;

    return result;
}

static void close_context_window() {
    if (g_tray.ctx.window) {
        gtk_widget_destroy(g_tray.ctx.window);
        g_tray.ctx.window = nullptr;
        g_tray.ctx.subject_entry = nullptr;
        g_tray.ctx.participants_entry = nullptr;
        g_tray.ctx.notes_view = nullptr;
    }
}

// --- Phase B.2 — local audio capture (tray-owned) ---

namespace {

// Subscriber callback — appends every chunk to the in-memory WAV
// buffer. Runs on the PipeWire RT thread; the contract is "no
// allocation beyond the vector's amortized doubling, no logging, no
// blocking beyond the existing buf_mtx the capture itself takes".
// We use the same mutex the WAV buffer is protected by (capture_state.wav_mtx)
// — held briefly here, not nested with any other tray lock.
void tray_wav_subscriber(const int16_t* samples, std::size_t n, void* /*ud*/) {
    std::lock_guard<std::mutex> lk(g_tray.capture_state.wav_mtx);
    g_tray.capture_state.wav_buffer.insert(
        g_tray.capture_state.wav_buffer.end(), samples, samples + n);
}

// Phase C.10a — second fan-out subscriber: stages captured PCM for the
// live-caption streaming uploader. Runs on the PipeWire RT thread; the
// contract is identical to tray_wav_subscriber — brief lock, append, no
// logging, no blocking. The GTK-thread pump (tray_stream_pump) drains this
// buffer and ships it to the daemon as `0x03` frames. Stacking this on the
// B.1 capture alongside the WAV stager is exactly the "C.10's streaming
// uploader can stack additional subscribers on the same handle without
// coordination" path the start_capture comment anticipated.
void tray_stream_subscriber(const int16_t* samples, std::size_t n, void* /*ud*/) {
    std::lock_guard<std::mutex> lk(g_tray.capture_state.stream_mtx);
    g_tray.capture_state.stream_buffer.insert(
        g_tray.capture_state.stream_buffer.end(), samples, samples + n);
}

// Phase C.10a — GTK timeout pump. Drains the streaming PCM staging buffer
// and ships it to the daemon as a single `0x03` frame. Runs on the GTK main
// thread, so the socket write is serialized with every other `ipc.call()`
// the tray makes — no fd race. Returns G_SOURCE_CONTINUE while the session
// is live; G_SOURCE_REMOVE once the token is cleared (stop path) so the
// timer self-retires.
gboolean tray_stream_pump(gpointer) {
    if (g_tray.capture_state.stream_token.empty())
        return G_SOURCE_REMOVE;   // session ended — retire the pump.

    std::vector<int16_t> batch;
    {
        std::lock_guard<std::mutex> lk(g_tray.capture_state.stream_mtx);
        batch.swap(g_tray.capture_state.stream_buffer);
    }
    if (batch.empty()) return G_SOURCE_CONTINUE;

    if (!g_tray.daemon_connected || !g_tray.ipc.connected()) {
        // Daemon gone — drop this batch; the disconnect path will clean up
        // the streaming state. Do not spin trying to reconnect here.
        return G_SOURCE_CONTINUE;
    }
    if (!g_tray.ipc.send_stream_audio(batch.data(), batch.size())) {
        log_warn("[tray] streaming: send_stream_audio failed — "
                 "stopping live-caption stream");
        g_tray.capture_state.stream_token.clear();
        g_tray.capture_state.stream_pump_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

// Resolve the mic source to record from. Priority: explicit
// g_tray.cfg.mic_source > device_pattern auto-detect > system default.
// Returns empty string if no source could be resolved; caller emits a
// user-facing error in that case.
std::string tray_resolve_mic_source() {
    if (!g_tray.cfg.mic_source.empty()) return g_tray.cfg.mic_source;
    try {
        auto detected = detect_sources(g_tray.cfg.device_pattern);
        if (!detected.mic.empty()) return detected.mic;
    } catch (const std::exception& e) {
        log_warn("[tray] detect_sources failed: %s", e.what());
    }
    return "";
}

}  // anonymous namespace

namespace tray {

// Phase B.2 — start local capture. Idempotent: returns false if a
// capture is already running. On success:
//   * Creates the staging dir if it does not yet exist.
//   * Builds a unique wav_path (audio_<timestamp>.wav) with collision
//     suffix `_N` when the timestamp-minute filename already exists.
//   * Wires the WAV stager as the first subscriber via
//     add_audio_subscriber; B.5's submit path and C.10's streaming
//     uploader can stack additional subscribers on the same handle
//     without coordination.
//
// `err_msg` is populated with an operator-facing message on failure.
bool start_capture(const std::string& mic_source, std::string& err_msg) {
    if (g_tray.capture_state.pw) {
        err_msg = "capture already running";
        return false;
    }
    if (mic_source.empty()) {
        err_msg = "no microphone source selected (set Mic Source from the menu)";
        return false;
    }

    fs::path staging = tray_capture::default_staging_dir();
    std::error_code ec;
    fs::create_directories(staging, ec);
    if (ec) {
        err_msg = "cannot create staging dir: " + staging.string() + " — " + ec.message();
        return false;
    }

    std::string ts = tray_capture::format_timestamp(std::time(nullptr));
    fs::path wav_path;
    try {
        wav_path = tray_capture::next_staging_wav_path(staging, ts);
    } catch (const std::exception& e) {
        err_msg = e.what();
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_tray.capture_state.wav_mtx);
        g_tray.capture_state.wav_buffer.clear();
    }
    g_tray.capture_state.wav_path      = wav_path;
    g_tray.capture_state.wav_timestamp = ts;
    g_tray.capture_state.wav_source    = mic_source;
    g_tray.capture_state.waiting_disposition = false;

    try {
        g_tray.capture_state.pw = std::make_unique<PipeWireCapture>(mic_source);
        g_tray.capture_state.wav_handle =
            g_tray.capture_state.pw->add_audio_subscriber(&tray_wav_subscriber, nullptr);
        g_tray.capture_state.pw->start();
    } catch (const std::exception& e) {
        err_msg = std::string("capture start failed: ") + e.what();
        g_tray.capture_state.pw.reset();
        g_tray.capture_state.wav_handle = 0;
        return false;
    }

    log_info("[tray] capture started: source=%s wav=%s",
             mic_source.c_str(), wav_path.c_str());
    return true;
}

// Phase B.2 — stop local capture. Drains the WAV buffer to the
// staging path. After this returns successfully, the capture is
// torn down and `capture_state.waiting_disposition = true`; the
// operator picks Submit/Discard/Save-for-later via the stop dialog
// (B.3). On any write failure the function still tears down the
// capture and clears the path so the tray can return to Idle without
// leaking the unique_ptr.
bool stop_capture(std::string& err_msg) {
    if (!g_tray.capture_state.pw) {
        err_msg = "no active capture";
        return false;
    }

    // Phase C.10a — tear down the live-caption stream first, while the
    // capture is still up: retire the GTK pump, clear the token (so a
    // pump tick already in flight self-retires), and tell the daemon to
    // discard the streaming job + its temp WAV via process.stream.cancel.
    // The stream subscriber is removed implicitly when the capture is
    // destroyed below; clearing the token here makes the pump a no-op in
    // the meantime.
    if (!g_tray.capture_state.stream_token.empty()) {
        std::string token = g_tray.capture_state.stream_token;
        g_tray.capture_state.stream_token.clear();
        if (g_tray.capture_state.stream_pump_id) {
            g_source_remove(g_tray.capture_state.stream_pump_id);
            g_tray.capture_state.stream_pump_id = 0;
        }
        if (g_tray.daemon_connected && g_tray.ipc.connected()) {
            JsonMap cp;
            cp["stream_token"] = token;
            IpcResponse cr; IpcError ce;
            if (!g_tray.ipc.call("process.stream.cancel", cp, cr, ce, 5000)) {
                log_warn("[tray] process.stream.cancel failed (%s)",
                         ce.message.c_str());
            } else {
                log_info("[tray] live-caption stream cancelled (token=%s)",
                         token.c_str());
            }
        }
    }
    g_tray.capture_state.stream_handle = 0;
    {
        std::lock_guard<std::mutex> lk(g_tray.capture_state.stream_mtx);
        g_tray.capture_state.stream_buffer.clear();
    }

    // Tear down the capture first so the RT thread quiesces and no
    // late chunks land in the buffer after we drain it.
    g_tray.capture_state.pw->stop();
    g_tray.capture_state.pw.reset();
    g_tray.capture_state.wav_handle = 0;

    std::vector<int16_t> drained;
    {
        std::lock_guard<std::mutex> lk(g_tray.capture_state.wav_mtx);
        drained.swap(g_tray.capture_state.wav_buffer);
    }

    fs::path wav_path = g_tray.capture_state.wav_path;
    std::string write_err;
    if (!tray_capture::write_wav(wav_path, drained, write_err)) {
        err_msg = "WAV write failed: " + write_err;
        // Don't leave a half-written file around.
        std::error_code ec;
        fs::remove(wav_path, ec);
        g_tray.capture_state.wav_path.clear();
        g_tray.capture_state.wav_timestamp.clear();
        g_tray.capture_state.wav_source.clear();
        g_tray.capture_state.waiting_disposition = false;
        return false;
    }

    g_tray.capture_state.waiting_disposition = true;
    log_info("[tray] capture stopped: %zu samples written to %s",
             drained.size(), wav_path.c_str());
    return true;
}

}  // namespace tray

// --- Phase B.3 — submit / discard / save-for-later dialog ---

enum class StopDisposition {
    Submit,
    Discard,
    SaveForLater,
    Cancelled,   // dialog closed without choice — treat as Save (file kept)
};

// Run the modal three-button disposition dialog. Returns the
// operator's choice. The dialog matches the existing GTK style used
// elsewhere in tray (gtk_dialog_new_with_buttons + content area).
static StopDisposition tray_run_disposition_dialog(const fs::path& wav_path,
                                                   std::size_t sample_count) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Recording stopped", nullptr,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "_Save for later", 1,
        "_Discard",        2,
        "_Submit",         GTK_RESPONSE_OK,
        nullptr);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    // Headline
    double seconds = sample_count > 0
        ? static_cast<double>(sample_count) / static_cast<double>(SAMPLE_RATE)
        : 0.0;
    int mins = static_cast<int>(seconds) / 60;
    int secs = static_cast<int>(seconds) % 60;
    char headline[160];
    std::snprintf(headline, sizeof(headline),
                  "Recording captured: %d:%02d (%.1f MB)\nWhat would you like to do?",
                  mins, secs,
                  static_cast<double>(sample_count) * sizeof(int16_t) / (1024.0 * 1024.0));
    GtkWidget* head_lbl = gtk_label_new(headline);
    gtk_label_set_xalign(GTK_LABEL(head_lbl), 0.0f);
    gtk_box_pack_start(GTK_BOX(content), head_lbl, FALSE, FALSE, 4);

    // Path + per-button explanation
    std::string path_hint = "Staging path: " + wav_path.string();
    GtkWidget* path_lbl = gtk_label_new(path_hint.c_str());
    gtk_label_set_xalign(GTK_LABEL(path_lbl), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(path_lbl), TRUE);
    gtk_widget_set_opacity(path_lbl, 0.7);
    gtk_box_pack_start(GTK_BOX(content), path_lbl, FALSE, FALSE, 4);

    GtkWidget* help_lbl = gtk_label_new(
        "  • Submit         — send to daemon for transcription + summary now\n"
        "  • Discard        — delete the WAV immediately (no upload, no compute)\n"
        "  • Save for later — keep the WAV in staging; resume later");
    gtk_label_set_xalign(GTK_LABEL(help_lbl), 0.0f);
    gtk_box_pack_start(GTK_BOX(content), help_lbl, FALSE, FALSE, 4);

    gtk_widget_show_all(dialog);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    switch (response) {
        case GTK_RESPONSE_OK: return StopDisposition::Submit;
        case 1:              return StopDisposition::SaveForLater;
        case 2:              return StopDisposition::Discard;
        default:             return StopDisposition::Cancelled;
    }
}

// --- Recording (via IPC) ---

static void on_record(GtkMenuItem*, gpointer) {
    // Phase B.2: tray-local recording. Daemon connection is no longer
    // a hard prerequisite — recording works offline and the operator
    // can Save-for-later if the daemon is unreachable. The downloading
    // guard remains so model downloads in flight don't get clobbered.
    if (g_tray.recording || g_tray.downloading) return;
    if (g_tray.capture_state.waiting_disposition) {
        notify("Pending recording",
               "An earlier recording is awaiting Submit / Discard / Save. "
               "Pick a disposition before starting a new one.");
        return;
    }

    // Resolve API key from env / per-provider store / legacy. We still
    // do this up front so the user gets prompted before recording
    // (a recording is no use without a path to summarization).
    if (g_tray.cfg.llm_model.empty()) {
        const auto* prov = find_provider(g_tray.cfg.provider);
        if (prov) {
            std::string key = resolve_api_key(*prov, g_tray.cfg.api_keys, g_tray.cfg.api_key);
            if (!key.empty()) {
                g_tray.cfg.api_key = key;
            } else if (!g_tray.cfg.no_summary) {
                std::string entered = prompt_api_key(*prov);
                if (!entered.empty()) {
                    g_tray.cfg.api_keys[prov->name] = entered;
                    g_tray.cfg.api_key = entered;
                    save_config(g_tray.cfg);
                    // Push the fresh key over to the daemon for the
                    // current session so the Submit path picks it up.
                    if (g_tray.daemon_connected && g_tray.session_inited) {
                        JsonMap creds;
                        creds["provider"] = g_tray.cfg.provider;
                        creds["api_key"]  = entered;
                        IpcResponse cr; IpcError ce;
                        g_tray.ipc.session_update_credentials(creds, cr, ce, 5000);
                    }
                }
            }
        }
    }

    std::string mic_source = tray_resolve_mic_source();
    if (mic_source.empty()) {
        notify("No microphone source",
               "Pick a mic source from the tray menu (Mic Source >).");
        return;
    }

    std::string start_err;
    if (!tray::start_capture(mic_source, start_err)) {
        notify("Recording failed", start_err);
        log_error("[tray] start_capture failed: %s", start_err.c_str());
        return;
    }

    // Captions: snapshot the config value at recording start. Mid-
    // recording toggles do not take effect; same semantics as Phase 5.1.
    g_tray.cap.captions_enabled_for_recording = g_tray.cfg.captions_enabled;
    if (g_tray.cfg.captions_enabled) {
        // Pre-create the overlay window so the first caption event has
        // somewhere to render.
        caption_overlay_create();

        // Phase C.10a — open a live-caption streaming session on the
        // daemon. On success the daemon hands back a `stream_token`; we
        // stack a second fan-out subscriber on the B.1 capture and start
        // the GTK pump that ships PCM as `0x03` frames. The `caption` /
        // `caption.degraded` events flow back over IPC and render through
        // the existing overlay handler (process_event, below). A failure
        // here is non-fatal: the local WAV recording continues, just
        // without live captions.
        if (g_tray.daemon_connected && g_tray.ipc.connected()) {
            JsonMap sp;
            sp["format"]            = std::string("s16le");
            sp["sample_rate"]       = static_cast<int64_t>(recmeet::SAMPLE_RATE);
            sp["channels"]          = static_cast<int64_t>(recmeet::CHANNELS);
            sp["language"]          = g_tray.cfg.language.empty()
                                          ? std::string("en") : g_tray.cfg.language;
            sp["captions_enabled"]  = true;
            // latency_budget_ms — the tray has no per-config knob for this
            // (it lives in the daemon's per-session SessionPreferences); use
            // the protocol default 500 ms. The daemon clamps/rejects out of
            // [200, 2000] anyway.
            sp["latency_budget_ms"] = static_cast<int64_t>(500);
            IpcResponse sr; IpcError se;
            if (g_tray.ipc.call("process.stream", sp, sr, se, 5000)) {
                auto tit = sr.result.find("stream_token");
                std::string token = tit != sr.result.end()
                    ? recmeet::json_val_as_string(tit->second) : "";
                if (!token.empty()) {
                    g_tray.capture_state.stream_token = token;
                    {
                        std::lock_guard<std::mutex> lk(
                            g_tray.capture_state.stream_mtx);
                        g_tray.capture_state.stream_buffer.clear();
                    }
                    if (g_tray.capture_state.pw) {
                        g_tray.capture_state.stream_handle =
                            g_tray.capture_state.pw->add_audio_subscriber(
                                &tray_stream_subscriber, nullptr);
                    }
                    // Pump every 100 ms — matches the ~100 ms / 16 kHz
                    // capture chunking the wire protocol is sized for.
                    g_tray.capture_state.stream_pump_id =
                        g_timeout_add(100, tray_stream_pump, nullptr);
                    log_info("[tray] live-caption stream opened (token=%s)",
                             token.c_str());
                } else {
                    log_warn("[tray] process.stream returned no stream_token "
                             "— continuing without live captions");
                }
            } else {
                log_warn("[tray] process.stream failed (%s) — continuing "
                         "without live captions", se.message.c_str());
            }
        }
    }

    update_state(true, g_tray.postprocessing, false, false);

    // Open non-modal context dialog (always — there is no reprocess
    // path through on_record any more; reprocess flows through Submit).
    show_context_window();
}

// Phase B.3 — disposition handlers, split out of on_stop so each is
// testable and the failure paths stay symmetric.

namespace tray {

// Discard the staged WAV immediately. Plan acceptance line: "Discard-
// on-stop: WAV gone in <100 ms; no upload, no compute." We unlink the
// file and clear the staging path; nothing else fires.
void apply_discard() {
    fs::path wav_path = g_tray.capture_state.wav_path;
    std::error_code ec;
    fs::remove(wav_path, ec);
    if (ec) {
        log_warn("[tray] discard: unlink failed for %s: %s",
                 wav_path.c_str(), ec.message().c_str());
    } else {
        log_info("[tray] discarded WAV: %s", wav_path.c_str());
    }
    g_tray.capture_state.wav_path.clear();
    g_tray.capture_state.wav_timestamp.clear();
    g_tray.capture_state.wav_source.clear();
    g_tray.capture_state.waiting_disposition = false;
}

// Phase B.5 — submit the staged WAV to the daemon via record.start
// with `reprocess_dir`. This is the transitional submission path
// (lives until Phase C's `process.submit` IPC). Local-host only.
//
// Returns true on successful IPC dispatch; false on connect / call
// failure. On failure the WAV stays in staging so the operator can
// pick Save-for-later or retry without re-recording.
bool apply_submit(std::string& err_msg) {
    if (!g_tray.daemon_connected) {
        if (!connect_to_daemon()) {
            err_msg = "daemon not running — start recmeet-daemon and try again";
            return false;
        }
    }

    JsonMap params;
    params["reprocess_dir"] = g_tray.capture_state.wav_path.string();

    IpcResponse resp;
    IpcError err;
    if (!g_tray.ipc.call("record.start", params, resp, err, 10000)) {
        err_msg = err.message.empty() ? "record.start failed" : err.message;
        return false;
    }

    log_info("[tray] submitted WAV for reprocess: %s",
             g_tray.capture_state.wav_path.c_str());

    // The WAV is now owned by the daemon's reprocess flow (iter 64
    // "reuse audio parent dir" applies). Clear the staging path so the
    // tray returns to Idle; the daemon will broadcast progress events
    // that update the postprocessing indicator.
    g_tray.capture_state.wav_path.clear();
    g_tray.capture_state.wav_timestamp.clear();
    g_tray.capture_state.wav_source.clear();
    g_tray.capture_state.waiting_disposition = false;
    return true;
}

// Phase B.3 — leave the WAV in staging and drop a `.pending` sidecar
// so D.5 can resume the disposition flow after a tray restart. The
// sidecar shape is intentionally minimal (D.5 will tighten it).
void apply_save_for_later() {
    bool ok = tray_capture::write_pending_sidecar(
        g_tray.capture_state.wav_path,
        g_tray.capture_state.wav_timestamp,
        g_tray.capture_state.wav_source,
        g_tray.cap.captions_enabled_for_recording);
    if (!ok) {
        log_warn("[tray] save-for-later: failed to write .pending sidecar for %s",
                 g_tray.capture_state.wav_path.c_str());
    } else {
        log_info("[tray] saved WAV for later: %s",
                 g_tray.capture_state.wav_path.c_str());
    }
    // Clear the active staging fields — the WAV is now "saved",
    // outside the active capture lifecycle.
    g_tray.capture_state.wav_path.clear();
    g_tray.capture_state.wav_timestamp.clear();
    g_tray.capture_state.wav_source.clear();
    g_tray.capture_state.waiting_disposition = false;
}

}  // namespace tray

static void on_stop(GtkMenuItem*, gpointer) {
    // Phase B.2: stop covers three independent state machines now:
    //   1. Local recording → invoke tray::stop_capture, run B.3 dialog,
    //      apply chosen disposition.
    //   2. No local recording but daemon postprocessing → ask the
    //      daemon to cancel postprocessing.
    //   3. Neither → no-op.
    if (g_tray.recording) {
        // Capture context from dialog before stopping — kept for the
        // job.context IPC after Submit so the daemon sees vocab + notes.
        CapturedContext captured_ctx;
        if (g_tray.ctx.window)
            captured_ctx = capture_and_clear_context();

        std::string stop_err;
        if (!tray::stop_capture(stop_err)) {
            notify("Recording stop failed", stop_err);
            log_error("[tray] stop_capture failed: %s", stop_err.c_str());
            update_state(false, g_tray.postprocessing, g_tray.downloading);
            return;
        }
        update_state(false, g_tray.postprocessing, g_tray.downloading);

        std::size_t sample_count = 0;
        // The buffer is already drained inside stop_capture; recover
        // count from on-disk WAV via libsndfile would be a round-trip.
        // Use the file size instead (PCM-16 mono → bytes/2 == samples).
        std::error_code ec;
        auto sz = fs::file_size(g_tray.capture_state.wav_path, ec);
        if (!ec && sz >= 44) sample_count = (sz - 44) / sizeof(int16_t);

        StopDisposition choice =
            tray_run_disposition_dialog(g_tray.capture_state.wav_path, sample_count);

        switch (choice) {
            case StopDisposition::Submit: {
                std::string sub_err;
                if (!tray::apply_submit(sub_err)) {
                    notify("Submit failed", sub_err);
                    // WAV remains in staging; tray stays in
                    // waiting_disposition so the operator can retry or
                    // pick Save-for-later. Re-present dialog state via
                    // build_menu so the indicator reflects reality.
                    build_menu();
                    return;
                }
                // Send context-window data along with the submit so the
                // daemon's job.context handler folds it into the
                // running reprocess job. We send it AFTER record.start
                // so the daemon has the job ready.
                if (!captured_ctx.context_inline.empty()) {
                    JsonMap ctx_params;
                    ctx_params["context_inline"] = captured_ctx.context_inline;
                    if (!captured_ctx.vocab_additions.empty())
                        ctx_params["vocabulary_append"] = captured_ctx.vocab_additions;
                    IpcResponse ctx_resp;
                    IpcError ctx_err;
                    if (!g_tray.ipc.call("job.context", ctx_params, ctx_resp, ctx_err, 5000))
                        log_warn("[tray] job.context failed: %s", ctx_err.message.c_str());
                }
                break;
            }
            case StopDisposition::Discard:
                tray::apply_discard();
                break;
            case StopDisposition::SaveForLater:
            case StopDisposition::Cancelled:
                tray::apply_save_for_later();
                break;
        }
        build_menu();
        return;
    }

    if (g_tray.postprocessing) {
        // Daemon-side postprocess cancel — same as the legacy on_cancel_pp.
        JsonMap params;
        params["target"] = std::string("postprocessing");
        IpcResponse resp;
        IpcError err;
        if (!g_tray.ipc.call("record.stop", params, resp, err, 5000)) {
            log_error("[tray] record.stop (postprocessing) failed: %s",
                      err.message.c_str());
        }
    }
}

static void on_cancel_pp(GtkMenuItem*, gpointer) {
    if (!g_tray.postprocessing) return;

    JsonMap params;
    params["target"] = std::string("postprocessing");

    IpcResponse resp;
    IpcError err;
    if (!g_tray.ipc.call("record.stop", params, resp, err, 5000)) {
        log_error("[tray] cancel postprocessing failed: %s", err.message.c_str());
    }
}

// --- Radio / checkbox callbacks ---

static void on_mic_selected(GtkCheckMenuItem* item, gpointer data) {
    if (!gtk_check_menu_item_get_active(item)) return;
    auto* name = static_cast<const char*>(data);
    g_tray.cfg.mic_source = name ? name : "";
    save_config(g_tray.cfg);
}

static void on_monitor_selected(GtkCheckMenuItem* item, gpointer data) {
    if (!gtk_check_menu_item_get_active(item)) return;
    auto* name = static_cast<const char*>(data);
    g_tray.cfg.monitor_source = name ? name : "";
    save_config(g_tray.cfg);
}

static void on_model_selected(GtkCheckMenuItem* item, gpointer data) {
    if (!gtk_check_menu_item_get_active(item)) return;
    auto* name = static_cast<const char*>(data);
    g_tray.cfg.whisper_model = name;
    save_config(g_tray.cfg);

    // Trigger model download if daemon is connected and idle
    if (g_tray.daemon_connected && !g_tray.recording && !g_tray.downloading) {
        JsonMap params;
        params["whisper_model"] = std::string(name);
        IpcResponse resp;
        IpcError err;
        g_tray.ipc.call("models.ensure", params, resp, err, 5000);
    }
}

static void on_language_selected(GtkCheckMenuItem* item, gpointer data) {
    if (!gtk_check_menu_item_get_active(item)) return;
    auto* code = static_cast<const char*>(data);
    g_tray.cfg.language = code ? code : "";
    save_config(g_tray.cfg);
}

static void on_mic_only_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.mic_only = gtk_check_menu_item_get_active(item);
    save_config(g_tray.cfg);
}

static void on_no_summary_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.no_summary = gtk_check_menu_item_get_active(item);
    save_config(g_tray.cfg);
}

static void on_diarize_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.diarize = gtk_check_menu_item_get_active(item);
    save_config(g_tray.cfg);
}

static void on_vad_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.vad = gtk_check_menu_item_get_active(item);
    save_config(g_tray.cfg);
}

// Phase 5.4 — Live captions toggle. Persists to config and applies to the
// NEXT recording (captions are bound to record.start params; mid-recording
// toggles do not take effect — the menu's tooltip makes this explicit).
static void on_captions_enabled_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.captions_enabled = gtk_check_menu_item_get_active(item);
    save_config(g_tray.cfg);
}

// --- Provider / model callbacks ---

static void choose_gguf_model();

static void on_provider_selected(GtkCheckMenuItem* item, gpointer data) {
    if (!gtk_check_menu_item_get_active(item)) return;
    auto* name = static_cast<const char*>(data);

    if (!name) {
        // Local LLM selected
        if (g_tray.cfg.llm_model.empty())
            choose_gguf_model();
        if (g_tray.cfg.llm_model.empty()) {
            build_menu(); // user cancelled — revert radio state
            return;
        }
        save_config(g_tray.cfg);
        build_menu();
        return;
    }

    // Cloud provider selected
    g_tray.cfg.provider = name;
    g_tray.cfg.llm_model.clear();

    const auto* prov = find_provider(name);
    if (prov) {
        g_tray.cfg.api_model = prov->default_model;
        // Load stored key for this provider
        auto kit = g_tray.cfg.api_keys.find(name);
        g_tray.cfg.api_key = (kit != g_tray.cfg.api_keys.end()) ? kit->second : "";
    }

    save_config(g_tray.cfg);
    fetch_provider_models();
    build_menu();
}

static void on_api_model_selected(GtkCheckMenuItem* item, gpointer data) {
    if (!gtk_check_menu_item_get_active(item)) return;
    auto* model = static_cast<const char*>(data);
    if (model) {
        g_tray.cfg.api_model = model;
        save_config(g_tray.cfg);
    }
}

// --- Model fetching ---

static gboolean on_models_fetched(gpointer) {
    g_tray.models_fetching = false;
    build_menu();
    return G_SOURCE_REMOVE;
}

static void fetch_provider_models() {
    if (g_tray.models_fetching) return;
    if (!g_tray.cfg.llm_model.empty()) return; // using local LLM

    const auto* prov = find_provider(g_tray.cfg.provider);
    if (!prov) return;

    g_tray.models_fetching = true;
    g_tray.models_provider = g_tray.cfg.provider;

    std::string provider_name = g_tray.cfg.provider;
    std::string base_url = prov->base_url;
    std::string fallback_key = g_tray.cfg.api_key;
    auto api_keys = g_tray.cfg.api_keys;

    std::thread([provider_name, base_url, fallback_key, api_keys]() {
        const auto* prov = find_provider(provider_name);
        if (!prov) return;

        std::string key = resolve_api_key(*prov, api_keys, fallback_key);
        if (key.empty()) {
            g_idle_add(on_models_fetched, nullptr);
            return;
        }

        try {
            auto models = fetch_models(base_url + "/models", key);
            std::lock_guard<std::mutex> lock(g_tray.models_mutex);
            g_tray.cached_models = std::move(models);
        } catch (const std::exception& e) {
            log_error("[tray] Model fetch failed: %s", e.what());
        }
        g_idle_add(on_models_fetched, nullptr);
    }).detach();
}

// --- File/folder chooser helpers ---

static std::string run_folder_chooser(const char* title) {
    auto* dialog = gtk_file_chooser_dialog_new(
        title, nullptr, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT,
        nullptr);
    std::string result;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (path) {
            result = path;
            g_free(path);
        }
    }
    gtk_widget_destroy(dialog);
    return result;
}

static std::string run_gguf_chooser(const char* title) {
    auto* dialog = gtk_file_chooser_dialog_new(
        title, nullptr, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT,
        nullptr);
    auto* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "GGUF Models (*.gguf)");
    gtk_file_filter_add_pattern(filter, "*.gguf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    auto* all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All Files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all_filter);

    std::string result;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (path) {
            result = path;
            g_free(path);
        }
    }
    gtk_widget_destroy(dialog);
    return result;
}

static void choose_gguf_model() {
    std::string path = run_gguf_chooser("Select LLM Model (.gguf)");
    if (!path.empty()) {
        g_tray.cfg.llm_model = path;
        save_config(g_tray.cfg);
        build_menu();
    }
}

// --- Output path callbacks ---

static void on_set_output_dir(GtkMenuItem*, gpointer) {
    std::string path = run_folder_chooser("Select Output Directory");
    if (!path.empty()) {
        g_tray.cfg.output_dir = path;
        save_config(g_tray.cfg);
        build_menu();
    }
}

static void on_set_note_dir(GtkMenuItem*, gpointer) {
    std::string path = run_folder_chooser("Select Note Directory");
    if (!path.empty()) {
        g_tray.cfg.note_dir = path;
        save_config(g_tray.cfg);
        build_menu();
    }
}

static void on_set_llm_model(GtkMenuItem*, gpointer) {
    choose_gguf_model();
}

static void on_open_latest_session(GtkMenuItem*, gpointer) {
    fs::path out_dir = g_tray.cfg.output_dir;
    if (!fs::exists(out_dir) || !fs::is_directory(out_dir)) {
        notify("No sessions", "Output directory does not exist: " + out_dir.string());
        return;
    }

    std::vector<fs::path> sessions;
    for (const auto& entry : fs::directory_iterator(out_dir)) {
        if (entry.is_directory())
            sessions.push_back(entry.path());
    }

    if (sessions.empty()) {
        notify("No sessions", "No recording sessions found in: " + out_dir.string());
        return;
    }

    std::sort(sessions.begin(), sessions.end());
    std::string cmd = "xdg-open '" + sessions.back().string() + "' &";
    if (std::system(cmd.c_str()) != 0)
        log_warn("[tray] xdg-open failed for: %s", sessions.back().c_str());
}

// --- Utility actions ---

static void on_edit_config(GtkMenuItem*, gpointer) {
    fs::path cfg_path = config_dir() / "config.yaml";
    if (!fs::exists(cfg_path))
        save_config(g_tray.cfg);

    const char* editor = std::getenv("EDITOR");
    if (!editor) editor = "nvim";
    const char* terminal = std::getenv("TERMINAL");
    if (!terminal) terminal = "foot";

    std::string cmd = std::string(terminal) + " -e " + editor + " " + cfg_path.string() + " &";
    if (std::system(cmd.c_str()) != 0) {
        // Fallback to xdg-open
        std::string fallback = "xdg-open " + cfg_path.string() + " &";
        if (std::system(fallback.c_str()) != 0)
            notify("Cannot open config", cfg_path.string());
    }
}

static bool is_port_listening(const std::string& addr, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET,
              (addr == "0.0.0.0" || addr.empty()) ? "127.0.0.1" : addr.c_str(),
              &sa.sin_addr);

    bool ok = (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0);
    close(fd);
    return ok;
}

static void reap_web_server() {
    if (g_tray.web_server_pid > 0) {
        int status;
        pid_t r = waitpid(g_tray.web_server_pid, &status, WNOHANG);
        if (r == g_tray.web_server_pid || r == -1)
            g_tray.web_server_pid = -1;
    }
}

static bool spawn_web_server() {
    reap_web_server();
    if (g_tray.web_server_pid > 0) return true;  // still running

    // Resolve binary: sibling of our own executable first, then PATH
    std::string web_bin;
    {
        std::error_code ec;
        auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            auto sibling = self.parent_path() / "recmeet-web";
            if (std::filesystem::exists(sibling, ec))
                web_bin = sibling.string();
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_warn("[tray] fork() failed: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        // Child — redirect stdout/stderr to /dev/null to avoid blocking
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }

        std::string port_arg = std::to_string(g_tray.cfg.web_port);
        const std::string& bind_arg = g_tray.cfg.web_bind;

        if (!web_bin.empty()) {
            execl(web_bin.c_str(), "recmeet-web",
                  "--port", port_arg.c_str(),
                  "--bind", bind_arg.c_str(),
                  nullptr);
        }
        // Fallback to PATH
        execlp("recmeet-web", "recmeet-web",
               "--port", port_arg.c_str(),
               "--bind", bind_arg.c_str(),
               nullptr);
        _exit(127);
    }

    g_tray.web_server_pid = pid;
    log_info("[tray] spawned recmeet-web (pid %d) on port %d", pid, g_tray.cfg.web_port);
    return true;
}

static void on_open_speaker_ui(GtkMenuItem*, gpointer) {
    std::string bind = g_tray.cfg.web_bind;
    int port = g_tray.cfg.web_port;

    if (!is_port_listening(bind, port)) {
        if (!spawn_web_server()) {
            notify("Speaker Management", "Failed to start recmeet-web");
            return;
        }
        // Poll for up to 2s
        for (int i = 0; i < 40; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (is_port_listening(bind, port)) break;
        }
        if (!is_port_listening(bind, port)) {
            notify("Speaker Management", "recmeet-web did not start in time");
            return;
        }
    }

    std::string display_host = (bind == "0.0.0.0" || bind.empty()) ? "127.0.0.1" : bind;
    std::string url = "http://" + display_host + ":" + std::to_string(port);
    std::string cmd = "xdg-open '" + url + "' &";
    if (std::system(cmd.c_str()) != 0)
        log_warn("[tray] xdg-open failed for: %s", url.c_str());
}

static void on_refresh_devices(GtkMenuItem*, gpointer) {
    refresh_sources();
    build_menu();
    std::string msg = std::to_string(g_tray.mics.size()) + " mic(s), " +
                      std::to_string(g_tray.monitors.size()) + " monitor(s)";
    notify("Devices refreshed", msg);
}

static void on_update_models(GtkMenuItem*, gpointer) {
    if (!g_tray.daemon_connected) {
        notify("Daemon not running", "Start recmeet-daemon first.");
        return;
    }
    if (g_tray.recording || g_tray.downloading) return;

    IpcResponse resp;
    IpcError err;
    if (!g_tray.ipc.call("models.update", resp, err, 5000)) {
        notify("Update failed", err.message);
    }
}

static void on_about(GtkMenuItem*, gpointer) {
    auto* dialog = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "recmeet");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), RECMEET_VERSION);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
        "Record, transcribe, and summarize meetings locally.");
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
        "https://github.com/suykerbuyk/recmeet");
    gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog),
        "GitHub Repository");
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog),
        "Copyright \xC2\xA9 2026 John Suykerbuyk and SykeTech LTD");
    gtk_about_dialog_set_license(GTK_ABOUT_DIALOG(dialog),
        "Dual-licensed under the MIT License and Apache License 2.0.\n\n"
        "See LICENSE for full text.");
    gtk_about_dialog_set_wrap_license(GTK_ABOUT_DIALOG(dialog), TRUE);
    const char* authors[] = {"John Suykerbuyk (SykeTech LTD)", nullptr};
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);
    gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog),
        "audio-input-microphone");

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void stop_web_server() {
    if (g_tray.web_server_pid <= 0) return;
    log_info("[tray] stopping recmeet-web (pid %d)", g_tray.web_server_pid);
    kill(g_tray.web_server_pid, SIGTERM);
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        int status;
        if (waitpid(g_tray.web_server_pid, &status, WNOHANG) != 0) {
            g_tray.web_server_pid = -1;
            return;
        }
    }
    log_warn("[tray] recmeet-web did not exit, sending SIGKILL");
    kill(g_tray.web_server_pid, SIGKILL);
    waitpid(g_tray.web_server_pid, nullptr, 0);
    g_tray.web_server_pid = -1;
}

static void on_quit(GtkMenuItem*, gpointer) {
    gtk_main_quit();
}

// --- Source cache ---

static void refresh_sources() {
    try {
        auto all = list_sources();
        g_tray.mics.clear();
        g_tray.monitors.clear();
        for (auto& s : all) {
            if (s.is_monitor)
                g_tray.monitors.push_back(std::move(s));
            else
                g_tray.mics.push_back(std::move(s));
        }
    } catch (const std::exception& e) {
        log_error("[tray] Source enumeration failed: %s", e.what());
        g_tray.mics.clear();
        g_tray.monitors.clear();
    }
}

// --- Menu construction ---

// Build a radio submenu for source selection.
// current_name: the currently selected source name (empty = auto-detect).
// sources: the list of available sources.
// callback: the signal handler for radio toggling.
// Returns the submenu widget.
static GtkWidget* build_source_submenu(const std::string& current_name,
                                        const std::vector<AudioSource>& sources,
                                        GCallback callback) {
    auto* submenu = gtk_menu_new();
    GSList* group = nullptr;

    // Auto-detect option (data = nullptr means empty string)
    auto* auto_item = gtk_radio_menu_item_new_with_label(group, "Auto-detect");
    group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(auto_item));
    if (current_name.empty())
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(auto_item), TRUE);
    g_signal_connect(auto_item, "toggled", callback, nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), auto_item);

    for (const auto& s : sources) {
        std::string label = source_label(s);
        auto* item = gtk_radio_menu_item_new_with_label(group, label.c_str());
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        if (s.name == current_name)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
        // Use the source name string directly — it lives in g_tray.mics/monitors
        // which persist across menu rebuilds.
        g_signal_connect(item, "toggled", callback, const_cast<char*>(s.name.c_str()));
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
    }

    return submenu;
}

static void build_menu() {
    auto* menu = gtk_menu_new();
    bool is_idle = !g_tray.recording && !g_tray.postprocessing && !g_tray.downloading;
    bool can_record = !g_tray.recording && !g_tray.downloading;

    // --- Status label ---
    std::string status;
    if (!g_tray.daemon_connected)
        status = "Status: Disconnected";
    else if (g_tray.recording && g_tray.postprocessing) {
        std::string rec_label = g_tray.is_reprocess ? "Reprocessing" : "Recording";
        status = "Status: " + rec_label + "... (processing previous)";
    } else if (g_tray.recording) {
        status = g_tray.is_reprocess ? "Status: Reprocessing..." : "Status: Recording...";
    } else if (g_tray.postprocessing) {
        status = "Status: Processing...";
        if (g_tray.progress_percent >= 0 && !g_tray.current_phase.empty()) {
            status = "Status: " + g_tray.current_phase + "... "
                     + std::to_string(g_tray.progress_percent) + "%";
            // Capitalize first letter of phase
            if (status.size() > 8)
                status[8] = static_cast<char>(toupper(static_cast<unsigned char>(status[8])));
        }
    } else if (g_tray.downloading) {
        status = "Status: Downloading";
        if (!g_tray.download_model.empty())
            status += " " + g_tray.download_model;
        status += "...";
    } else
        status = "Status: Idle";
    auto* status_item = gtk_menu_item_new_with_label(status.c_str());
    gtk_widget_set_sensitive(status_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), status_item);
    g_tray.status_menu_item = status_item;

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // --- Record / Stop / Cancel ---
    if (g_tray.recording) {
        const char* label = g_tray.is_reprocess ? "Cancel" : "Stop Recording";
        auto* item = gtk_menu_item_new_with_label(label);
        g_signal_connect(item, "activate", G_CALLBACK(on_stop), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

        if (!g_tray.is_reprocess) {
            auto* ctx_item = gtk_menu_item_new_with_label("Edit Context...");
            g_signal_connect(ctx_item, "activate",
                             G_CALLBACK(+[](GtkMenuItem*, gpointer) { show_context_window(); }),
                             nullptr);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), ctx_item);
        }
    }
    if (g_tray.postprocessing && !g_tray.recording) {
        // Postprocessing only — show both Record and Cancel Processing
        auto* rec_item = gtk_menu_item_new_with_label("Record");
        g_signal_connect(rec_item, "activate", G_CALLBACK(on_record), nullptr);
        gtk_widget_set_sensitive(rec_item, g_tray.daemon_connected);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), rec_item);

        auto* cancel_item = gtk_menu_item_new_with_label("Cancel Processing");
        g_signal_connect(cancel_item, "activate", G_CALLBACK(on_cancel_pp), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), cancel_item);
    }
    if (!g_tray.recording && !g_tray.postprocessing) {
        auto* item = gtk_menu_item_new_with_label("Record");
        g_signal_connect(item, "activate", G_CALLBACK(on_record), nullptr);
        gtk_widget_set_sensitive(item, can_record && g_tray.daemon_connected);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // --- Mic Source submenu ---
    {
        auto* item = gtk_menu_item_new_with_label("Mic Source");
        auto* sub = build_source_submenu(g_tray.cfg.mic_source, g_tray.mics,
                                          G_CALLBACK(on_mic_selected));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
        if (!is_idle) gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    // --- Monitor Source submenu ---
    {
        auto* item = gtk_menu_item_new_with_label("Monitor Source");
        auto* sub = build_source_submenu(g_tray.cfg.monitor_source, g_tray.monitors,
                                          G_CALLBACK(on_monitor_selected));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
        if (!is_idle) gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    // --- Whisper Model submenu ---
    {
        auto* item = gtk_menu_item_new_with_label("Whisper Model");
        auto* submenu = gtk_menu_new();
        GSList* group = nullptr;

        for (const auto* model : WHISPER_MODELS) {
            auto* radio = gtk_radio_menu_item_new_with_label(group, model);
            group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(radio));
            if (g_tray.cfg.whisper_model == model)
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(radio), TRUE);
            g_signal_connect(radio, "toggled", G_CALLBACK(on_model_selected),
                             const_cast<char*>(model));
            gtk_menu_shell_append(GTK_MENU_SHELL(submenu), radio);
        }

        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
        if (!is_idle) gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    // --- Language submenu ---
    {
        auto* item = gtk_menu_item_new_with_label("Language");
        auto* submenu = gtk_menu_new();
        GSList* group = nullptr;

        // Auto-detect option
        auto* auto_item = gtk_radio_menu_item_new_with_label(group, "Auto-detect");
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(auto_item));
        if (g_tray.cfg.language.empty())
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(auto_item), TRUE);
        g_signal_connect(auto_item, "toggled", G_CALLBACK(on_language_selected), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), auto_item);

        for (const auto& lang : LANGUAGES) {
            std::string label = std::string(lang.label) + " (" + lang.code + ")";
            auto* radio = gtk_radio_menu_item_new_with_label(group, label.c_str());
            group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(radio));
            if (g_tray.cfg.language == lang.code)
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(radio), TRUE);
            g_signal_connect(radio, "toggled", G_CALLBACK(on_language_selected),
                             const_cast<char*>(lang.code));
            gtk_menu_shell_append(GTK_MENU_SHELL(submenu), radio);
        }

        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
        if (!is_idle) gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // --- Checkboxes ---
    {
        auto* item = gtk_check_menu_item_new_with_label("Mic Only");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), g_tray.cfg.mic_only);
        g_signal_connect(item, "toggled", G_CALLBACK(on_mic_only_toggled), nullptr);
        if (!is_idle) gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    {
        auto* item = gtk_check_menu_item_new_with_label("No Summary");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), g_tray.cfg.no_summary);
        g_signal_connect(item, "toggled", G_CALLBACK(on_no_summary_toggled), nullptr);
        if (!is_idle) gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    {
        auto* item = gtk_check_menu_item_new_with_label("Speaker Diarization");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), g_tray.cfg.diarize);
        g_signal_connect(item, "toggled", G_CALLBACK(on_diarize_toggled), nullptr);
        if (!is_idle) gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    {
        auto* item = gtk_check_menu_item_new_with_label("VAD Segmentation");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), g_tray.cfg.vad);
        g_signal_connect(item, "toggled", G_CALLBACK(on_vad_toggled), nullptr);
        if (!is_idle) gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    // Phase 5.4 — single check-menu-item (NOT a radio submenu — caption-on
    // is binary; the existing build_source_submenu helper at line ~1113 is
    // for multi-choice radio cases like Mic Source / Whisper Model).
    {
        auto* item = gtk_check_menu_item_new_with_label("Show Live Captions");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
                                       g_tray.cfg.captions_enabled);
        gtk_widget_set_tooltip_text(item,
            "Live captions appear in a small overlay during recording. "
            "Toggling this applies to the NEXT recording (captions are "
            "bound to record.start parameters).");
        g_signal_connect(item, "toggled",
                         G_CALLBACK(on_captions_enabled_toggled), nullptr);
        // Always toggleable — even mid-recording, since the change only
        // applies to the next recording. The tooltip explains.
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // --- Summary submenu (Provider + Model) ---
    {
        auto* item = gtk_menu_item_new_with_label("Summary");
        auto* submenu = gtk_menu_new();
        bool use_local = !g_tray.cfg.llm_model.empty();

        // --- Provider sub-submenu ---
        {
            auto* prov_item = gtk_menu_item_new_with_label("Provider");
            auto* prov_sub = gtk_menu_new();
            GSList* group = nullptr;

            for (size_t i = 0; i < NUM_PROVIDERS; ++i) {
                auto* radio = gtk_radio_menu_item_new_with_label(group, PROVIDERS[i].display);
                group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(radio));
                if (!use_local && g_tray.cfg.provider == PROVIDERS[i].name)
                    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(radio), TRUE);
                g_signal_connect(radio, "toggled", G_CALLBACK(on_provider_selected),
                                 const_cast<char*>(PROVIDERS[i].name));
                gtk_menu_shell_append(GTK_MENU_SHELL(prov_sub), radio);
            }

            // Local LLM option
            std::string local_label = "Local LLM";
            if (use_local) {
                fs::path p(g_tray.cfg.llm_model);
                local_label += " (" + p.filename().string() + ")";
            }
            auto* local_radio = gtk_radio_menu_item_new_with_label(group, local_label.c_str());
            if (use_local)
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(local_radio), TRUE);
            g_signal_connect(local_radio, "toggled", G_CALLBACK(on_provider_selected), nullptr);
            gtk_menu_shell_append(GTK_MENU_SHELL(prov_sub), local_radio);

            gtk_menu_item_set_submenu(GTK_MENU_ITEM(prov_item), prov_sub);
            gtk_menu_shell_append(GTK_MENU_SHELL(submenu), prov_item);
        }

        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), gtk_separator_menu_item_new());

        // --- Model sub-submenu ---
        {
            auto* model_item = gtk_menu_item_new_with_label("Model");
            auto* model_sub = gtk_menu_new();

            if (use_local) {
                // Disabled when using local LLM
                gtk_widget_set_sensitive(model_item, FALSE);
                auto* info = gtk_menu_item_new_with_label("(using local LLM)");
                gtk_widget_set_sensitive(info, FALSE);
                gtk_menu_shell_append(GTK_MENU_SHELL(model_sub), info);
            } else if (g_tray.models_fetching) {
                auto* info = gtk_menu_item_new_with_label("Loading...");
                gtk_widget_set_sensitive(info, FALSE);
                gtk_menu_shell_append(GTK_MENU_SHELL(model_sub), info);
            } else {
                std::lock_guard<std::mutex> lock(g_tray.models_mutex);
                GSList* group = nullptr;
                bool found_current = false;

                // If cached models match current provider, show them
                if (g_tray.models_provider == g_tray.cfg.provider && !g_tray.cached_models.empty()) {
                    for (const auto& m : g_tray.cached_models) {
                        auto* radio = gtk_radio_menu_item_new_with_label(group, m.c_str());
                        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(radio));
                        if (m == g_tray.cfg.api_model) {
                            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(radio), TRUE);
                            found_current = true;
                        }
                        g_signal_connect(radio, "toggled", G_CALLBACK(on_api_model_selected),
                                         const_cast<char*>(m.c_str()));
                        gtk_menu_shell_append(GTK_MENU_SHELL(model_sub), radio);
                    }
                }

                // Always show current model if not in list
                if (!found_current) {
                    auto* radio = gtk_radio_menu_item_new_with_label(group, g_tray.cfg.api_model.c_str());
                    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(radio), TRUE);
                    g_signal_connect(radio, "toggled", G_CALLBACK(on_api_model_selected),
                                     const_cast<char*>(g_tray.cfg.api_model.c_str()));
                    gtk_menu_shell_append(GTK_MENU_SHELL(model_sub), radio);
                }
            }

            gtk_menu_item_set_submenu(GTK_MENU_ITEM(model_item), model_sub);
            gtk_menu_shell_append(GTK_MENU_SHELL(submenu), model_item);
        }

        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
        if (!is_idle) gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    // --- Output submenu ---
    {
        auto* item = gtk_menu_item_new_with_label("Output");
        auto* submenu = gtk_menu_new();

        // Current output dir label
        std::string out_label = "Output Dir: " + g_tray.cfg.output_dir.string();
        auto* out_info = gtk_menu_item_new_with_label(out_label.c_str());
        gtk_widget_set_sensitive(out_info, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), out_info);

        // Current note dir label
        std::string note_label = "Note Dir: " +
            (g_tray.cfg.note_dir.empty() ? "(same as output)" : g_tray.cfg.note_dir.string());
        auto* note_info = gtk_menu_item_new_with_label(note_label.c_str());
        gtk_widget_set_sensitive(note_info, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), note_info);

        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), gtk_separator_menu_item_new());

        auto* open_latest = gtk_menu_item_new_with_label("Open Latest Session");
        g_signal_connect(open_latest, "activate", G_CALLBACK(on_open_latest_session), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), open_latest);

        // Chooser actions
        auto* set_out = gtk_menu_item_new_with_label("Set Output Dir...");
        g_signal_connect(set_out, "activate", G_CALLBACK(on_set_output_dir), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), set_out);

        auto* set_note = gtk_menu_item_new_with_label("Set Note Dir...");
        g_signal_connect(set_note, "activate", G_CALLBACK(on_set_note_dir), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), set_note);

        auto* set_llm = gtk_menu_item_new_with_label("Set LLM Model...");
        g_signal_connect(set_llm, "activate", G_CALLBACK(on_set_llm_model), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), set_llm);

        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // --- Utility actions ---
    {
        auto* item = gtk_menu_item_new_with_label("Edit Config");
        g_signal_connect(item, "activate", G_CALLBACK(on_edit_config), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    {
        auto* item = gtk_menu_item_new_with_label("Speaker Management");
        g_signal_connect(item, "activate", G_CALLBACK(on_open_speaker_ui), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    {
        auto* item = gtk_menu_item_new_with_label("Refresh Devices");
        g_signal_connect(item, "activate", G_CALLBACK(on_refresh_devices), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    {
        auto* item = gtk_menu_item_new_with_label("Update Models");
        g_signal_connect(item, "activate", G_CALLBACK(on_update_models), nullptr);
        gtk_widget_set_sensitive(item, is_idle && g_tray.daemon_connected);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    {
        auto* item = gtk_menu_item_new_with_label("About");
        g_signal_connect(item, "activate", G_CALLBACK(on_about), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // --- Quit ---
    {
        auto* item = gtk_menu_item_new_with_label("Quit");
        g_signal_connect(item, "activate", G_CALLBACK(on_quit), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    gtk_widget_show_all(menu);
    app_indicator_set_menu(g_tray.indicator, GTK_MENU(menu));
}

// --- Main ---

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);
    notify_init();

    // Parse --daemon ADDRESS (GTK leaves unknown args in argv)
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--daemon" && i + 1 < argc) {
            g_tray.daemon_addr = argv[++i];
            break;
        }
    }
    // RECMEET_DAEMON_ADDR env var as fallback
    if (g_tray.daemon_addr.empty()) {
        if (const char* env = std::getenv("RECMEET_DAEMON_ADDR"))
            g_tray.daemon_addr = env;
    }
    // Set IPC address before first connect
    if (!g_tray.daemon_addr.empty())
        g_tray.ipc.set_address(g_tray.daemon_addr);

    g_tray.cfg = load_config();

    // Initialize logging (tray always logs to stderr — journald or interactive)
    auto log_level = parse_log_level(g_tray.cfg.log_level_str);
    log_init(log_level, g_tray.cfg.log_dir, g_tray.cfg.log_retention_hours, true);

    // Create indicator
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    g_tray.indicator = app_indicator_new(
        "recmeet-tray", ICON_IDLE,
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    G_GNUC_END_IGNORE_DEPRECATIONS

    // Set custom icon theme path — check relative to binary first (dev build),
    // then fall back to installed location
    {
        std::error_code ec;
        auto bin = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            auto base = bin.parent_path().parent_path() / "share" / "icons";
            if (!std::filesystem::is_directory(base, ec))
                base = std::filesystem::path(RECMEET_ICON_DIR);
            app_indicator_set_icon_theme_path(g_tray.indicator, base.c_str());
        }
    }

    app_indicator_set_status(g_tray.indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(g_tray.indicator, "recmeet");

    refresh_sources();

    // Connect to daemon (non-blocking — will retry via timer if not running)
    if (!connect_to_daemon())
        g_tray.reconnect_timer = g_timeout_add_seconds(1, try_reconnect, nullptr);

    build_menu();
    fetch_provider_models();

    // Install SIGCHLD handler to reap zombie processes (e.g. recmeet-web)
    {
        struct sigaction sa{};
        sa.sa_handler = [](int) {
            while (waitpid(-1, nullptr, WNOHANG) > 0) {}
        };
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, nullptr);
    }

    // Handle SIGTERM/SIGINT gracefully via GLib main loop
    g_unix_signal_add(SIGTERM, [](gpointer) -> gboolean {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }, nullptr);
    g_unix_signal_add(SIGINT, [](gpointer) -> gboolean {
        gtk_main_quit();
        return G_SOURCE_REMOVE;
    }, nullptr);

    log_info("recmeet-tray %s running (%zu mic(s), %zu monitor(s))",
            RECMEET_VERSION, g_tray.mics.size(), g_tray.monitors.size());
    gtk_main();

    // Cleanup — runs whether exited via on_quit, SIGTERM, or SIGINT
    if ((g_tray.recording || g_tray.postprocessing) && g_tray.daemon_connected) {
        IpcResponse resp;
        IpcError err;
        g_tray.ipc.call("record.stop", resp, err, 2000);
    }
    teardown_ipc_watch();
    g_tray.ipc.close_connection();
    stop_web_server();
    caption_overlay_destroy();   // Phase 5.1 — release overlay window/timer

    if (g_tray.reconnect_timer)
        g_source_remove(g_tray.reconnect_timer);

    log_shutdown();
    notify_cleanup();
    return 0;
}
