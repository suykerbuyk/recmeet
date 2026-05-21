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
#include "pending_jobs_journal.h"
#include "reconnect_backoff.h"
#include "resume_token_store.h"
#include "slot_queue.h"
#include "staging_sweep.h"
#include "tray_capture.h"
#include "tray_status.h"
#include "tray_web.h"
#include "util.h"
#include "uuid.h"
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <random>
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

    // Phase E.6.3 — `--headless` mode: no AppIndicator, no GTK menu, and
    // (critically) no GIOChannel watch on the IPC socket. The embedded
    // WebUI's HTTP handler threads do their own synchronous IPC reads
    // inside `IpcClient::call`; a concurrent GLib-main-loop reader would
    // race them and steal response frames. The flag is set once in
    // `main()` and never cleared — toggle-at-runtime is not supported.
    bool headless = false;

    JobConfig cfg;
    IpcClient ipc;   // daemon connection
    std::string daemon_addr;  // --daemon ADDRESS or RECMEET_DAEMON_ADDR

    bool recording = false;
    bool postprocessing = false;
    bool downloading = false;
    bool is_reprocess = false;  // derived from state string for display
    bool daemon_connected = false;
    guint reconnect_timer = 0;
    // Phase D.3 — nominal (un-jittered) backoff in seconds. Doubled
    // on each failed attempt and capped at `D3_BACKOFF_CAP_SECS` (30 s
    // per plan line 356). Reset to 1 s on a successful connect. The
    // value scheduled with the GTK timer is `jittered_delay_secs(this,
    // D3_JITTER_FRACTION, rng, cap)` — the wire schedule sees
    // ±25 % uniform jitter to defeat lockstep reconnect storms when
    // many clients reconnect after a shared network blip.
    int reconnect_delay = 1;
    // Phase D.3 — seeded once per tray process so the jitter draws are
    // reproducible (seed mixes time + pid for inter-process spread).
    // The single shared `std::mt19937` matches the GTK-thread-only
    // contract on `g_tray.*` access; no internal locking required.
    std::mt19937 reconnect_rng{
        static_cast<unsigned>(std::time(nullptr)) ^ static_cast<unsigned>(::getpid())};
    // Phase D.3 — client_id observed on the most recent SUCCESSFUL
    // connect. Snapshot taken inside `connect_to_daemon` AFTER the
    // server's auth.ok is processed; consulted on the NEXT reconnect
    // to detect the "same vs fresh session" boundary per plan line
    // 357-358. A fresh id (resume_token expired, PSK rotated, or
    // server forgot the session) drops the journal entries that
    // referenced the prior session and fires a notify per entry. The
    // resume_token path (when the daemon's resolver hook honors our
    // persisted token) re-issues the SAME client_id; the equality
    // check is the load-bearing test.
    //
    // Empty on first connect, after close_connection() is called from
    // tray exit, and after the journal-drop path runs to completion
    // (which then captures the fresh id as the new baseline).
    std::string last_client_id;

    // GIOChannel for socket event integration with GTK main loop
    GIOChannel* ipc_channel = nullptr;
    guint ipc_watch = 0;

    // Current model download status (for status label)
    std::string download_model;

    // Phase D.4 follow-up — per-slot phase + progress, keyed by SlotKind.
    // Pre-fix D.4 carried a SINGLE pair of globals (`current_phase` +
    // `progress_percent`) and routed every `phase` / `progress` event
    // through it. With three concurrent typed slots (D.1) — e.g. live
    // streaming + previous recording postprocessing (the primary C.7
    // use case) — the single-pair design caused all three per-slot
    // rows to mirror whichever event arrived last instead of their own
    // state. Per-slot maps fix the routing: the `phase` / `progress`
    // event handler looks up slot_kind via `slot_queues.find_slot_by_in_flight_job_id`
    // (D.1) and writes into the per-slot entry; `build_menu` reads per
    // slot when constructing each `InFlightView`. Maps are cleared on
    // the fresh-client_id branch in `post_reconnect_resync` (same
    // semantics as the journal-drop + slot_queues-clear that fires
    // when the prior session expired server-side).
    std::map<recmeet::SlotKind, std::string> current_phase_by_slot;
    std::map<recmeet::SlotKind, int>         progress_percent_by_slot;

    // Phase D.4 — jitter-aware reconnect countdown surfaces. The status
    // menu row shows "Status: Disconnected — reconnect in <N>s" while
    // disconnected, using the SAME jittered delay D.3 already chose
    // (we never re-roll jitter here — we read what schedule_reconnect_attempt
    // already armed). `reconnect_jittered_secs` is the chosen wait,
    // `reconnect_scheduled_at` is the wall-clock at which the timer was
    // armed, and `reconnect_countdown_timer` is the 1 Hz GTK tick that
    // rebuilds the menu so the count decrements live. Cleared once the
    // reconnect attempt fires (try_reconnect drops the countdown).
    int     reconnect_jittered_secs    = 0;
    time_t  reconnect_scheduled_at     = 0;
    guint   reconnect_countdown_timer  = 0;

    // Cached sources
    std::vector<AudioSource> mics;
    std::vector<AudioSource> monitors;

    // Cached API models for current provider
    std::vector<std::string> cached_models;
    std::mutex models_mutex;
    bool models_fetching = false;
    std::string models_provider; // which provider the cache is for

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
        // Phase D.5 — UUID v4 minted at start_capture, owned for the
        // entire recording lifecycle. Threaded through to
        // `process.submit` / `process.stream` params AND into the v2
        // sidecar on save-for-later. Mint happens EXACTLY ONCE per
        // recording per D.5 plan checklist item #3; the retry-after-
        // crash path (H-D3) re-uses the same id so C.11.4's server-side
        // dedup contract routes the bytes back to the same meeting dir.
        std::string meeting_id;
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

    // Phase D.1 — per-slot-kind submission queues. Three independent
    // slots (`postprocess`, `streaming`, `model_download`), each
    // capacity-1-in-flight + arbitrary FIFO backlog. Replaces the
    // pre-D.1 scalar `last_pp_job_id` (which only tracked the most-
    // recent postprocess job and could not represent the streaming /
    // model_download slots at all). Cancel paths read
    // `slot_queues.postprocess.in_flight()->job_id`; terminal status
    // (`job.complete` / `job.failed`) feeds the drain worker which
    // pops `complete_in_flight()` and dispatches the next entry, if
    // any, from the same slot's backlog.
    recmeet::SlotQueues slot_queues;

    // Phase D.5 — persistence layer.
    //   * `resume_tokens` is the per-server token cache; loaded lazily
    //     on first `get`/`put`, atomically persisted on every mutation.
    //   * `pending_jobs` is the submitted-but-incomplete journal. D.5
    //     only provides the class; D.2 wires the journal write call site
    //     at submit-return time. On startup we `load()` and dispatch
    //     `job.status` for each entry (recovery loop below).
    //   * `pending_resumes` is the in-memory snapshot of `.pending`
    //     sidecars scanned from staging at startup. Drives the
    //     `Resume Pending (N)` tray submenu.
    ResumeTokenStore resume_tokens;
    PendingJobsJournal pending_jobs;
    std::vector<tray_capture::PendingSidecarV2> pending_resumes;
    // Captured on every successful `connect()` so subsequent disconnect
    // / reconnect / persist cycles all reference the same address key.
    // Set to `daemon_addr` when non-empty; falls back to
    // `default_ipc_address()` formatted string otherwise.
    std::string resume_server_key;
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

// Phase D.3 — exponential backoff with jitter constants. The schedule
// is 1, 2, 4, 8, 16, 30, 30, ... s nominal with ±25 % uniform jitter
// applied to each scheduled wait. Plan line 356 locks both numbers.
// Surfaced as compile-time constants (not config knobs) per the same
// "no operator-tunable timing knob" discipline C.13's TTL plan rejected:
// jitter is mathematics, not policy.
static constexpr int    D3_BACKOFF_CAP_SECS  = 30;
static constexpr double D3_JITTER_FRACTION   = 0.25;

static void setup_ipc_watch();
static void teardown_ipc_watch();
static gboolean try_reconnect(gpointer);
static void post_reconnect_resync();
static void schedule_reconnect_attempt();

static void update_state(bool rec, bool pp, bool dl, bool reproc = false) {
    bool was_recording = g_tray.recording;
    g_tray.recording = rec;
    g_tray.postprocessing = pp;
    g_tray.downloading = dl;
    g_tray.is_reprocess = reproc;

    // Phase E.6.3 — in headless mode there is no app indicator, no
    // captions overlay window, no menu — every GUI update below is a
    // no-op. Short-circuit here so callers (connection events, slot
    // queue transitions, progress ticks, etc.) don't need their own
    // guards. The non-GUI state fields above are still updated so any
    // downstream logic reading them sees the right values.
    if (!g_tray.indicator) return;

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
        // Phase D.4 follow-up — fully-idle clears the per-slot maps so
        // the next phase event starts from a clean slate. Pre-fix this
        // zeroed the single `current_phase` / `progress_percent` pair;
        // the per-slot maps replace them.
        g_tray.current_phase_by_slot.clear();
        g_tray.progress_percent_by_slot.clear();
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

// Phase D.2 — drain worker entry point. Called on terminal status of
// any tracked in-flight job (`job.complete` with its job_id, or
// `progress.job` with phase ∈ {failed, cancelled}). Removes the
// journal entry by job_id and, if the slot has a backlog, dispatches
// the next entry via `apply_submit_with_context` (postprocess) or a
// future re-open path (streaming).
//
// Today the production tray's UI does not surface a "queue another
// postprocess submission" affordance, so the backlog path is exercised
// solely by the `[d2][drain]` test. The path is wired here so the
// invariant "in_flight is always either empty or a server-side
// reservation" stays true for D.3+: when D.4 lands the queue-depth
// indicator the dispatch will start firing for real.
static void handle_terminal_status(int64_t job_id) {
    if (job_id <= 0) return;
    auto drain = recmeet::drain_on_terminal(g_tray.slot_queues, job_id);
    if (!drain.matched) {
        log_debug("[tray] D.2 drain: job=%lld not in any slot in-flight set",
                  (long long)job_id);
        return;
    }
    // Atomic-write journal entry removal — the on-disk file is now
    // consistent with the in-memory drain transition.
    g_tray.pending_jobs.remove_by_job_id(std::to_string(job_id));
    log_info("[tray] D.2 drain: slot=%s job=%lld terminal "
             "(meeting_id=%s, backlog_depth_after=%zu)",
             recmeet::to_string(drain.slot),
             (long long)job_id,
             drain.meeting_id.c_str(),
             g_tray.slot_queues.select(drain.slot).backlog_size());

    if (drain.next_to_dispatch.has_value()) {
        // A backlog entry has been promoted to in-flight; the wire-
        // side dispatch is the appropriate `process.submit` /
        // `process.stream` round-trip. The production tray has no UI
        // path that produces backlog entries today (single-submit
        // disposition dialog) — the dispatcher seam is reserved for
        // D.4. The promoted entry sits in `in_flight` already, so a
        // future re-issue path will see it via the same
        // `slot_queues.<slot>.in_flight()` accessor used by cancel
        // and recovery paths.
        log_warn("[tray] D.2 drain: slot=%s promoted backlog entry to "
                 "in-flight (meeting_id=%s) — dispatcher not yet wired "
                 "(D.4 will surface it)",
                 recmeet::to_string(drain.slot),
                 drain.next_to_dispatch->meeting_id.c_str());
    }
}

static void handle_ipc_event(const IpcEvent& ev) {
    log_debug("tray: event '%s'", ev.event.c_str());
    if (ev.event == "phase") {
        std::string name = json_val_as_string(ev.data.at("name"));
        log_info("[tray] Phase: %s", name.c_str());
        // Phase D.4 follow-up — route by SlotKind via the D.1 in-flight
        // set. Pre-fix wrote into a SINGLE shared pair of globals; this
        // caused all three per-slot rows to mirror whichever phase
        // event arrived last. The `phase` / `progress` events carry
        // `job_id` (daemon.cpp:990); we look up the slot via
        // `slot_queues.find_slot_by_in_flight_job_id` (D.1) which is
        // O(1) — three equality checks — so the cost is constant per
        // event regardless of journal size. A miss (terminal-after-
        // drain race, or foreign client_id) is silently ignored; the
        // matching slot row simply does not update.
        auto jit = ev.data.find("job_id");
        int64_t jid = (jit != ev.data.end())
            ? json_val_as_int(jit->second) : 0;
        bool routed = recmeet::route_phase_to_slot(
            g_tray.slot_queues, jid, name,
            g_tray.current_phase_by_slot,
            g_tray.progress_percent_by_slot);
        if (!routed) {
            log_debug("[tray] D.4 phase event: job=%lld not in any slot "
                      "in-flight set (drained or foreign) — skipping row update",
                      (long long)jid);
        }
        // Phase D.4 — full menu rebuild so the in-flight slot row
        // reflects the new phase. Replaces the pre-D.4 in-place mutation
        // of `status_menu_item` (which only updated the single legacy
        // status row that the per-slot rows now obsolete).
        build_menu();
    } else if (ev.event == "progress") {
        std::string phase = json_val_as_string(ev.data.at("phase"));
        int percent = static_cast<int>(json_val_as_int(ev.data.at("percent")));
        // Phase D.4 follow-up — route by SlotKind, same lookup as the
        // `phase` event above. See the comment block on `phase` for the
        // rationale; same O(1) cost, same miss semantics.
        auto jit = ev.data.find("job_id");
        int64_t jid = (jit != ev.data.end())
            ? json_val_as_int(jit->second) : 0;
        bool routed = recmeet::route_progress_to_slot(
            g_tray.slot_queues, jid, phase, percent,
            g_tray.current_phase_by_slot,
            g_tray.progress_percent_by_slot);
        if (!routed) {
            log_debug("[tray] D.4 progress event: job=%lld not in any slot "
                      "in-flight set (drained or foreign) — skipping row update",
                      (long long)jid);
        }
        // Phase D.4 — full menu rebuild (replaces pre-D.4 in-place
        // single-status-row mutation).
        build_menu();
    } else if (ev.event == "state.changed") {
        auto err_it = ev.data.find("error");
        if (err_it != ev.data.end()) {
            std::string error = json_val_as_string(err_it->second);
            if (!error.empty())
                notify("Pipeline error", error);
        }
        // Phase C.9: the daemon no longer broadcasts a `recording`
        // boolean (the legacy live-recording path is gone). The tray's
        // local `recording` flag is the sole driver of the recording
        // indicator. We fold the daemon's `postprocessing` /
        // `downloading` / `streaming` bits into our local state.
        bool pp = false, dl = false;
        auto pp_it = ev.data.find("postprocessing");
        if (pp_it != ev.data.end()) pp = json_val_as_bool(pp_it->second);
        auto dl_it = ev.data.find("downloading");
        if (dl_it != ev.data.end()) dl = json_val_as_bool(dl_it->second);
        // (streaming is reflected separately in the captions overlay
        // flow; it doesn't change the postprocessing indicator.)
        update_state(g_tray.recording, pp, dl, false);
    } else if (ev.event == "job.complete") {
        // Phase D.2 — drain the slot queue on terminal success. The
        // event always carries `job_id` (daemon.cpp:1184); legacy
        // emit sites that pre-date C.3 may omit `note_path` so we
        // defend with .find rather than .at on every key.
        auto jit = ev.data.find("job_id");
        int64_t jid = (jit != ev.data.end())
            ? json_val_as_int(jit->second) : 0;
        handle_terminal_status(jid);

        auto note_it = ev.data.find("note_path");
        auto dir_it  = ev.data.find("output_dir");
        std::string note = (note_it != ev.data.end())
            ? json_val_as_string(note_it->second) : "";
        std::string dir  = (dir_it != ev.data.end())
            ? json_val_as_string(dir_it->second) : "";
        (void)dir;
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
    } else if (ev.event == "progress.job") {
        // Phase D.2 — terminal failures arrive via progress.job with
        // phase ∈ {failed, cancelled}. Done is redundant with
        // job.complete (same drain path); keeping it here is harmless
        // because handle_terminal_status is idempotent on already-
        // drained jobs (find_slot_by_in_flight_job_id returns
        // matched=false).
        auto jit = ev.data.find("job_id");
        auto pit = ev.data.find("phase");
        int64_t jid = (jit != ev.data.end())
            ? json_val_as_int(jit->second) : 0;
        std::string phase = (pit != ev.data.end())
            ? json_val_as_string(pit->second) : "";
        if (phase == "failed" || phase == "cancelled" || phase == "done") {
            handle_terminal_status(jid);
        }
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
        // Phase D.4 — refresh the inline caption row in the tray menu.
        // We do NOT subscribe to the caption event a second time; we
        // re-read the same `g_tray.cap.state` buffer the overlay just
        // updated (via CaptionRenderState::latest_text()).
        build_menu();
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
        // Phase D.3 — reset nominal backoff to 1 s and schedule the
        // first reconnect attempt with ±25 % jitter. `schedule_reconnect_attempt`
        // honors the Unix-out-of-scope guard (plan line 365) — Unix
        // transports do not arm the timer at all.
        g_tray.reconnect_delay = 1;
        schedule_reconnect_attempt();
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
        schedule_reconnect_attempt();
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

    // Phase D.5 — pick the canonical address key for the resume_token
    // store. v1 has one server; multi-server (E.2 + multi-server hook
    // #1) will iterate `servers: [...]` and store per-entry tokens.
    if (g_tray.resume_server_key.empty()) {
        g_tray.resume_server_key =
            g_tray.daemon_addr.empty() ? std::string("default") : g_tray.daemon_addr;
    }

    // Load the prior resume_token for this server (if any) and present
    // it on the next handshake. Empty token → falls back to fresh-mint
    // path on the server side, indistinguishable from a first-time
    // connect to the legacy daemon.
    std::string resume_tok;
    if (auto cached = g_tray.resume_tokens.get(g_tray.resume_server_key)) {
        resume_tok = *cached;
    }

    const char* psk_env = std::getenv("RECMEET_AUTH_TOKEN");
    std::string psk = psk_env ? psk_env : "";
    if (!g_tray.ipc.connect(psk, resume_tok)) return false;

    // Persist the server-issued resume_token. Empty value (legacy /
    // test-path daemon that did not emit the field) is a silent no-op
    // — `put` skips disk writes when the value matches the cache, and
    // an absent key remains absent. Architecture-review checklist
    // item #6: empty resume_token returned by the getter must NOT
    // throw — see `IpcClient::resume_token()` doc.
    const std::string& fresh_tok = g_tray.ipc.resume_token();
    if (!fresh_tok.empty()) {
        try {
            g_tray.resume_tokens.put(g_tray.resume_server_key, fresh_tok);
        } catch (const std::exception& e) {
            log_warn("[tray] resume_token persist failed: %s", e.what());
        }
    }

    g_tray.ipc.set_event_callback(handle_ipc_event);
    g_tray.daemon_connected = true;
    // Phase E.6.3 — headless mode does NOT install a GIOChannel watch
    // on the IPC fd. The WebUI's HTTP handler threads do their own
    // synchronous IPC reads via `IpcClient::call`; a concurrent
    // GLib-main-loop reader (`on_ipc_data` → `read_and_dispatch`) would
    // race those handler threads and consume response frames out from
    // under them, surfacing as spurious "Not connected" errors mid-
    // smoke. Async daemon events (progress.job, job.complete, ...)
    // queue up in the kernel socket buffer; we don't surface them in
    // headless mode anyway since there is no GUI to update.
    if (!g_tray.headless) setup_ipc_watch();

    // Sync state. Phase C.9 — the daemon's `state.changed` /
    // `status.get` no longer carries a `recording` boolean (the
    // live-recording path is gone). The tray's local `recording` field
    // is the sole driver of the recording indicator; the daemon's
    // postprocessing / downloading bits fold in unchanged.
    IpcResponse resp;
    IpcError err;
    if (g_tray.ipc.call("status.get", resp, err, 2000)) {
        bool pp = false, dl = false;
        auto pp_it = resp.result.find("postprocessing");
        if (pp_it != resp.result.end()) pp = json_val_as_bool(pp_it->second);
        auto dl_it = resp.result.find("downloading");
        if (dl_it != resp.result.end()) dl = json_val_as_bool(dl_it->second);
        update_state(g_tray.recording, pp, dl, false);
    }

    // Phase A.6 + B-bonus — establish the per-client session credential /
    // preference slot on the daemon. Pre-C.9 this replaced the
    // per-`record.start` config_to_map() blob; post-C.9 the same slot
    // is consumed by `process.submit` / `process.stream` (the v2 verbs
    // freeze the live Config + session prefs into the per-job
    // snapshot). The handshake is one-shot per IPC connection; the
    // flag clears on disconnect.
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

    // Phase D.3 — snapshot the client_id observed on this successful
    // connect IFF the snapshot is currently empty (first-ever connect
    // or post-tray-restart bootstrap). Subsequent reconnects let
    // `post_reconnect_resync()` do the comparison BEFORE updating the
    // snapshot — otherwise prior_id and fresh_id would be equal by
    // construction and the "fresh client_id" branch could never fire.
    if (g_tray.last_client_id.empty()) {
        g_tray.last_client_id = g_tray.ipc.client_id();
    }

    log_info("[tray] Connected to daemon");
    return true;
}

// Phase D.3 — schedule the next reconnect attempt with jittered
// exponential backoff. `g_tray.reconnect_delay` carries the un-jittered
// nominal that doubles on each failure (1, 2, 4, ..., 30); the value
// actually handed to `g_timeout_add_seconds` is the jittered version
// drawn from a uniform ±25 % around the nominal, then clamped to the
// 30 s cap. The post-clamp guarantee means the wire never schedules
// more than 30 s even when the nominal is at the cap and the jitter
// draw is positive.
//
// Unix-out-of-scope guard (plan line 365): when the transport is Unix,
// the daemon is tray-managed and restarts together with the tray —
// reconnect after a Unix-side disconnect is the D.5 tray-restart path,
// not a live reconnect. We log and DO NOT arm the timer; the tray
// remains in the disconnected state and the operator is expected to
// restart the tray (or the host) to bring it back.
// Phase D.4 — 1 Hz countdown tick that rebuilds the menu so the
// "reconnect in <N>s" line decrements live. The tick is armed inside
// `schedule_reconnect_attempt` (after the jittered delay is captured
// into `g_tray.reconnect_jittered_secs` / `_scheduled_at`) and cleared
// inside `try_reconnect` when the attempt actually fires. The render
// computes the remaining seconds from `_jittered_secs - (now - _at)`
// so a single global clock tick covers all rebuild paths without
// re-reading the GTK timer.
static gboolean on_reconnect_countdown_tick(gpointer) {
    // If we somehow reconnected between scheduling and the tick (e.g.
    // an out-of-order build_menu call set daemon_connected=true), drop
    // the timer — the countdown row is no longer rendered.
    if (g_tray.daemon_connected || g_tray.reconnect_scheduled_at == 0) {
        g_tray.reconnect_countdown_timer = 0;
        return G_SOURCE_REMOVE;
    }
    time_t now = std::time(nullptr);
    int elapsed = static_cast<int>(now - g_tray.reconnect_scheduled_at);
    if (elapsed >= g_tray.reconnect_jittered_secs) {
        // Past the scheduled fire; try_reconnect will run shortly via
        // its own timer. Drop the tick — the countdown row collapses
        // to the "reconnecting..." form via render_reconnect_status_line.
        g_tray.reconnect_countdown_timer = 0;
        build_menu();
        return G_SOURCE_REMOVE;
    }
    build_menu();
    return G_SOURCE_CONTINUE;
}

static void schedule_reconnect_attempt() {
    // Defer to the same guard the IpcClient uses (`is_remote()` reads
    // `addr_.transport == IpcTransport::Tcp`). The check is "remote-ness"
    // not "non-Unix" so a future non-TCP non-Unix transport gets the
    // safe-by-default treatment.
    if (!g_tray.ipc.is_remote()) {
        log_info("[tray] D.3 reconnect skipped — Unix transport "
                 "(tray-managed daemon; restart together via tray restart)");
        return;
    }
    int jittered = recmeet::jittered_delay_secs(
        g_tray.reconnect_delay,
        D3_JITTER_FRACTION,
        g_tray.reconnect_rng,
        D3_BACKOFF_CAP_SECS);
    log_debug("tray: D.3 reconnect scheduled "
              "(nominal=%ds, jittered=%ds, cap=%ds)",
              g_tray.reconnect_delay, jittered, D3_BACKOFF_CAP_SECS);
    g_tray.reconnect_timer = g_timeout_add_seconds(
        jittered, try_reconnect, nullptr);

    // Phase D.4 — capture the jittered delay D.3 just chose so the menu
    // can render a live countdown. We DO NOT re-roll jitter here; we
    // record what D.3 already armed. Then arm a 1 Hz GTK tick that
    // rebuilds the menu so the count decrements. The tick is cleared
    // inside `try_reconnect` when the attempt fires (or here on a
    // re-schedule — we cancel any prior tick first).
    if (g_tray.reconnect_countdown_timer) {
        g_source_remove(g_tray.reconnect_countdown_timer);
        g_tray.reconnect_countdown_timer = 0;
    }
    g_tray.reconnect_jittered_secs = jittered;
    g_tray.reconnect_scheduled_at = std::time(nullptr);
    g_tray.reconnect_countdown_timer = g_timeout_add_seconds(
        1, on_reconnect_countdown_tick, nullptr);
    build_menu();  // surface the initial "reconnect in <N>s" immediately

    // Advance the nominal AFTER scheduling so the NEXT attempt uses the
    // doubled value. The cap is honored inside `next_nominal_backoff`.
    g_tray.reconnect_delay = recmeet::next_nominal_backoff(
        g_tray.reconnect_delay, D3_BACKOFF_CAP_SECS);
}

// Phase D.3 — convergence-principle pattern-2 batch-upload fallback.
// Triggered when a streaming session that was in-flight pre-disconnect
// is reported failed/cancelled/unknown on the post-reconnect job.list
// re-sync. The server's C.10a TCP-drop policy already finalized the
// streaming job; the tray re-submits the SAME meeting_id via
// process.submit, and the server's C.11.4 dedup contract atomically
// overwrites whatever partial WAV the streaming session left behind.
//
// Distinct from `apply_submit_with_context` because:
//   * The streaming-fallback path operates on JOURNAL data (the
//     pre-disconnect entry carrying staging_wav_path + meeting_id),
//     not on `g_tray.capture_state.*` (which has been torn down).
//   * No context payload — the streaming session never collected an
//     interactive context dialog (it was a live recording, not a
//     save-for-later batch).
//   * The slot-queue admission goes into the postprocess slot, not
//     the streaming slot (the streaming session is gone server-side).
//
// Returns true on a successful re-submit + first upload-chunk pump;
// false on any wire / I/O failure. On failure the original journal
// entry stays IN PLACE so a later retry (manual via D.4, or next
// reconnect) can retry — losing the entry would lose operator data.
static bool dispatch_streaming_fallback_submit(
        const recmeet::PendingJobsJournal::Entry& je) {
    if (!g_tray.daemon_connected || !g_tray.ipc.connected()) {
        log_warn("[tray] D.3 streaming-fallback: daemon disconnected, deferring");
        return false;
    }
    fs::path wav_path(je.staging_wav_path);
    std::error_code ec;
    auto wav_size = fs::file_size(wav_path, ec);
    if (ec || wav_size == 0) {
        log_warn("[tray] D.3 streaming-fallback: cannot stat WAV %s: %s",
                 wav_path.c_str(), ec ? ec.message().c_str() : "empty");
        return false;
    }
    std::ifstream wav_in(wav_path, std::ios::binary);
    if (!wav_in) {
        log_warn("[tray] D.3 streaming-fallback: cannot open WAV %s",
                 wav_path.c_str());
        return false;
    }

    JsonMap params;
    params["audio_size"]  = static_cast<int64_t>(wav_size);
    params["format"]      = std::string("wav");
    params["sample_rate"] = static_cast<int64_t>(SAMPLE_RATE);
    params["channels"]    = static_cast<int64_t>(1);
    params["mode"]        = std::string("transcribe");
    // Load-bearing — the SAME meeting_id the streaming session carried.
    // C.11.4 server-side dedup uses this to route the overwrite back to
    // the same meeting dir.
    params["meeting_id"]  = je.meeting_id;

    IpcResponse resp; IpcError err;
    if (!g_tray.ipc.call("process.submit", params, resp, err, 10000)) {
        log_warn("[tray] D.3 streaming-fallback: process.submit failed: %s",
                 err.message.c_str());
        return false;
    }
    auto tok_it = resp.result.find("upload_token");
    auto job_it = resp.result.find("job_id");
    std::string upload_token = (tok_it != resp.result.end())
        ? json_val_as_string(tok_it->second) : "";
    int64_t new_job_id = (job_it != resp.result.end())
        ? json_val_as_int(job_it->second) : 0;
    if (upload_token.empty() || new_job_id <= 0) {
        log_warn("[tray] D.3 streaming-fallback: process.submit response "
                 "missing upload_token / job_id");
        return false;
    }
    log_info("[tray] D.3 streaming-fallback: process.submit OK "
             "(new job=%lld meeting_id=%s size=%llu)",
             (long long)new_job_id, je.meeting_id.c_str(),
             (unsigned long long)wav_size);

    // Re-journal the entry under the postprocess slot with the new
    // job_id; remove the old streaming entry. The follow-up upload-
    // chunk pump is intentionally NOT done here: we register the
    // intent in the journal so even if the upload itself is
    // interrupted (e.g. another disconnect mid-fallback), the next
    // reconnect's H-D3 path retries by meeting_id — the same recovery
    // contract D.5 + C.11.4 already establish for crash recovery.
    recmeet::PendingJobsJournal::Entry new_je;
    new_je.endpoint          = je.endpoint;
    new_je.meeting_id        = je.meeting_id;
    new_je.job_id            = std::to_string(new_job_id);
    new_je.staging_wav_path  = je.staging_wav_path;
    new_je.kind              = "submit";
    new_je.slot_kind         = recmeet::to_string(
                                   recmeet::SlotKind::Postprocess);
    new_je.submitted_at_unix = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    g_tray.pending_jobs.remove_by_job_id(je.job_id);
    g_tray.pending_jobs.append(new_je);
    return true;
}

// Phase D.3 — post-reconnect handler. Called immediately after
// `connect_to_daemon()` returns true, so `g_tray.ipc.client_id()` and
// `g_tray.ipc.resume_token()` carry the fresh per-connection values.
//
// Steps (plan line 357-361):
//   2. client_id reconciliation — compare to the prior session's id;
//      on a fresh id, drop journal entries that referenced the old
//      session and notify per entry. Streaming entries trigger the
//      convergence-pattern-2 fallback to batch upload.
//   3. session.init is already re-sent inside connect_to_daemon when
//      session_inited was cleared on disconnect — no extra work here.
//   4. job.list re-sync — pull the server-authoritative list and
//      classify each entry into Fetch / Monitor / NotifyFailed via
//      reconnect_backoff::classify_resynced_job.
//   5. Per-job dispatch: complete → process.fetch, running → monitor
//      (no work), unknown → notify().
//
// All wire calls observe a short 3 s timeout; a hung daemon during
// resync trips the IPC watch's HUP path and re-enters the backoff
// loop (avoiding a deadlocked reconnect).
static void post_reconnect_resync() {
    if (!g_tray.daemon_connected || !g_tray.ipc.connected()) {
        log_warn("[tray] D.3 post-reconnect resync invoked while disconnected — skipping");
        return;
    }

    // Step 2 — client_id reconciliation. Capture the fresh id BEFORE
    // mutating anything so the journal-drop path can log both ids.
    const std::string fresh_id = g_tray.ipc.client_id();
    const std::string prior_id = g_tray.last_client_id;

    // "Fresh client_id" means: the prior session existed (we had a
    // non-empty last_client_id) AND the server handed us back a
    // different id. A first-ever connect has prior_id == "" and is not
    // a "session expired" event — there was no session to expire.
    const bool prior_session_existed   = !prior_id.empty();
    const bool fresh_client_id         = prior_session_existed && fresh_id != prior_id;

    if (fresh_client_id) {
        log_warn("[tray] D.3 fresh client_id (was=%s, now=%s) — prior session "
                 "expired (TTL or PSK rotation); dropping journal entries that "
                 "referenced the old client_id",
                 prior_id.c_str(), fresh_id.c_str());
        auto stale = g_tray.pending_jobs.load();
        for (const auto& e : stale) {
            // Build a per-entry notification body — operator sees one
            // toast per orphaned job rather than a count summary so the
            // affected meeting is identifiable. The streaming-fallback
            // path additionally tries to dispatch a follow-up
            // `process.submit` carrying the same meeting_id.
            std::string body = "meeting_id=" + e.meeting_id
                             + " job_id=" + e.job_id
                             + " kind=" + e.kind;
            notify("recmeet: pending job orphaned by session expiry", body);
            log_warn("[tray] D.3 dropped journal entry: meeting_id=%s "
                     "job_id=%s kind=%s slot=%s",
                     e.meeting_id.c_str(), e.job_id.c_str(),
                     e.kind.c_str(), e.slot_kind.c_str());
        }
        // Wipe the journal — every entry referenced the old session by
        // construction. Save an empty list atomically.
        g_tray.pending_jobs.save({});
        // Clear the slot-queue in-flight set: the daemon-side jobs are
        // gone; we cannot drain by job_id any more. The retire matches
        // the journal wipe — a future submit gets a fresh slot.
        g_tray.slot_queues.postprocess.clear();
        g_tray.slot_queues.streaming.clear();
        g_tray.slot_queues.model_download.clear();
        // Phase D.4 follow-up — the per-slot phase/progress maps
        // referenced the prior session's jobs; clear them in lock-step
        // with the journal + slot_queues clear so the next reconnect
        // starts with empty per-slot rows. Disconnect alone does NOT
        // clear these maps (D.5 preserves slot_queues across reconnect
        // when the resume_token still resolves); only a fresh-id wipe
        // discards the stale per-slot state.
        g_tray.current_phase_by_slot.clear();
        g_tray.progress_percent_by_slot.clear();
    }
    // Update the snapshot regardless of fresh/same — the new id is now
    // the baseline for the NEXT reconnect's comparison.
    g_tray.last_client_id = fresh_id;

    // Step 4 — job.list re-sync. C.14 stamped `phase` + `progress` on
    // every entry so the tray's UI can populate synchronously from the
    // re-sync response without waiting on the next `progress.job`
    // event. Empty result on a fresh-id wipe (the daemon has no jobs
    // bound to the new client_id) is the expected path; we still call
    // job.list so the empty-array test seam observes a real wire
    // round-trip.
    IpcResponse list_resp;
    IpcError list_err;
    if (!g_tray.ipc.call("job.list", JsonMap{}, list_resp, list_err, 3000)) {
        log_warn("[tray] D.3 job.list re-sync failed: %s "
                 "(continuing without per-job dispatch)",
                 list_err.message.empty() ? "unknown" : list_err.message.c_str());
        return;
    }
    auto jit = list_resp.result.find("jobs");
    std::string jobs_arr_json = (jit != list_resp.result.end())
        ? json_val_as_string(jit->second)
        : std::string("[]");
    auto parsed = recmeet::parse_job_list_jobs(jobs_arr_json);
    log_info("[tray] D.3 job.list re-sync: %zu job(s) returned by daemon",
             parsed.size());

    // Phase D.4 follow-up — populate the per-slot phase/progress maps
    // from EVERY parsed entry that's still in-flight, not just the first.
    // Pre-fix this only updated the single shared global pair from the
    // first running entry, collapsing concurrent-slot UI state to one
    // value. With per-slot routing, each entry's `kind` ("postprocess",
    // "streaming", "model_download") maps directly to the matching
    // SlotKind bucket so the per-slot rows survive a reconnect with
    // multiple in-flight jobs (the primary C.7 concurrent-slot use case).
    for (const auto& j : parsed) {
        if (j.state == "running" || j.state == "queued"
            || j.state == "waiting_on_download"
            || j.state == "waiting_for_upload") {
            recmeet::SlotKind slot = recmeet::slot_kind_from_string(j.kind);
            if (!j.phase.empty()) g_tray.current_phase_by_slot[slot] = j.phase;
            if (j.progress >= 0)  g_tray.progress_percent_by_slot[slot] = j.progress;
        }
    }

    // Step 5 — per-job dispatch. The classifier collapses (state, kind)
    // into one of three actions. The Fetch + NotifyFailed branches
    // emit immediate wire calls; Monitor is a no-op (event pump
    // catches up).
    for (const auto& j : parsed) {
        auto cls = recmeet::classify_resynced_job(j.state, j.kind);
        switch (cls.action) {
            case recmeet::JobResyncAction::Fetch: {
                log_info("[tray] D.3 resync: job=%lld state=done — issuing process.fetch",
                         (long long)j.job_id);
                // Reuse the data_dir's staging path for the output;
                // production wiring routes fetched artifacts into the
                // meeting dir, but the resync path here is conservative
                // and just pulls them into a known location. The exact
                // destination is a D.4 follow-up (per-job UI affordances).
                fs::path out_dir = recmeet::data_dir() / "fetched"
                                 / std::to_string(j.job_id);
                std::error_code ec;
                fs::create_directories(out_dir, ec);
                IpcError ferr;
                auto written = g_tray.ipc.fetch_artifacts(
                    j.job_id, out_dir, ferr, 10000);
                if (written.empty()) {
                    log_warn("[tray] D.3 process.fetch(%lld) failed: %s",
                             (long long)j.job_id,
                             ferr.message.empty() ? "no artifacts"
                                                  : ferr.message.c_str());
                } else {
                    log_info("[tray] D.3 fetched %zu artifact(s) for job=%lld",
                             written.size(), (long long)j.job_id);
                    g_tray.pending_jobs.remove_by_job_id(std::to_string(j.job_id));
                }
                break;
            }
            case recmeet::JobResyncAction::NotifyFailed: {
                std::string body = "job_id=" + std::to_string(j.job_id)
                                 + " state=" + j.state
                                 + " kind=" + j.kind;
                if (!j.error.empty()) body += " error=" + j.error;
                notify("recmeet: job failed during reconnect", body);
                log_warn("[tray] D.3 resync: job=%lld state=%s kind=%s notify fired",
                         (long long)j.job_id, j.state.c_str(), j.kind.c_str());

                // Streaming-aborted convergence-pattern-2 fallback per
                // plan line 362-363: a streaming session that did not
                // survive the disconnect (failed/cancelled/unknown) is
                // not resumable — the tray falls back to batch upload
                // via process.submit carrying the same meeting_id. The
                // server's C.11.4 atomic-overwrite contract routes the
                // batch bytes to the same meeting dir, overwriting any
                // partial WAV the streaming session left behind.
                if (cls.streaming_aborted && !j.meeting_id.empty()) {
                    log_info("[tray] D.3 streaming aborted → falling back to "
                             "batch upload via process.submit (meeting_id=%s)",
                             j.meeting_id.c_str());
                    // Resolve the staging WAV by consulting the journal
                    // — the entry we wrote at process.stream return
                    // carries the local WAV path. We dispatch a fresh
                    // process.submit with the SAME meeting_id so the
                    // server's C.11.4 dedup contract routes the bytes
                    // back to the meeting dir the streaming session was
                    // writing into; the partial WAV is overwritten
                    // atomically.
                    auto journal_entries = g_tray.pending_jobs.load();
                    for (const auto& je : journal_entries) {
                        if (je.meeting_id == j.meeting_id
                            && !je.staging_wav_path.empty()) {
                            if (dispatch_streaming_fallback_submit(je)) {
                                log_info("[tray] D.3 streaming-fallback "
                                         "dispatched (meeting_id=%s)",
                                         j.meeting_id.c_str());
                            } else {
                                log_warn("[tray] D.3 streaming-fallback "
                                         "dispatch failed (meeting_id=%s) — "
                                         "journal entry preserved for retry",
                                         j.meeting_id.c_str());
                            }
                            break;
                        }
                    }
                }
                break;
            }
            case recmeet::JobResyncAction::Monitor: {
                log_debug("[tray] D.3 resync: job=%lld state=%s kind=%s — "
                          "event pump will catch up",
                          (long long)j.job_id, j.state.c_str(), j.kind.c_str());
                break;
            }
        }
    }
}

static gboolean try_reconnect(gpointer) {
    g_tray.reconnect_timer = 0;
    // Phase D.4 — the attempt is firing; drop the live countdown so the
    // menu's "reconnect in <N>s" line collapses to "reconnecting..." for
    // the brief window between attempt-start and either success
    // (Connected) or failure (re-schedule restarts the countdown).
    if (g_tray.reconnect_countdown_timer) {
        g_source_remove(g_tray.reconnect_countdown_timer);
        g_tray.reconnect_countdown_timer = 0;
    }
    g_tray.reconnect_scheduled_at = 0;
    g_tray.reconnect_jittered_secs = 0;

    log_debug("tray: D.3 reconnect attempt (nominal=%ds)", g_tray.reconnect_delay);
    if (connect_to_daemon()) {
        log_debug("tray: D.3 reconnected to daemon");
        // Successful connect — reset the nominal backoff and run the
        // post-reconnect resync (job.list pull + per-job dispatch +
        // client_id reconciliation). The resync runs before
        // build_menu() so the menu reflects the freshly-re-synced
        // status row.
        g_tray.reconnect_delay = 1;
        post_reconnect_resync();
        build_menu();
        return G_SOURCE_REMOVE;
    }
    log_debug("tray: D.3 reconnect failed — re-scheduling");
    schedule_reconnect_attempt();
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
    gtk_widget_set_tooltip_text(participants,
        "Tip: a line like \"Participants: Alice, Bob, Carol\" lets recmeet "
        "use the participant count as the speaker target during diarization.");
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
    // Phase D.5 — structured fields preserved for the sidecar v2 schema's
    // `context` block. The `context_inline` flatten above stays for
    // backward compatibility with the daemon's `process.submit` context
    // field; the structured fields below are used by save-for-later to
    // pre-fill the resume dialog on tray restart.
    std::string subject;
    std::vector<std::string> participants;
    std::string notes;
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

    // Phase D.5 — preserve the structured fields for the sidecar v2
    // context block (subject + participants list + notes). Split the
    // participants entry on commas + trim whitespace; matches the
    // operator-facing convention from the context dialog tooltip.
    result.subject = subject;
    result.notes   = notes;
    if (!participants.empty()) {
        size_t i = 0;
        while (i < participants.size()) {
            size_t j = participants.find(',', i);
            if (j == std::string::npos) j = participants.size();
            std::string p = participants.substr(i, j - i);
            size_t s = p.find_first_not_of(" \t");
            size_t e = p.find_last_not_of(" \t");
            if (s != std::string::npos)
                result.participants.push_back(p.substr(s, e - s + 1));
            i = j + 1;
        }
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

// Phase D.6 — build the set of staging WAV paths that are PROTECTED
// from auto-eviction because they are referenced by an in-flight journal
// entry (D.5 pending_jobs.json). Per-WAV `.pending` sidecar protection
// is checked inline by `scan_staging_dir`; this helper only handles the
// journal side. Called by both the synchronous-at-start sweep and the
// 10 min periodic sweep — kept tiny so both call sites pay the same
// small load cost on the journal file.
std::unordered_set<std::string> tray_collect_journal_protected_paths() {
    std::unordered_set<std::string> out;
    auto entries = g_tray.pending_jobs.load();
    for (const auto& e : entries) {
        if (!e.staging_wav_path.empty()) out.insert(e.staging_wav_path);
    }
    return out;
}

// Phase D.6 — periodic 10 min sweep handler. Registered at tray startup
// via `g_timeout_add_seconds(D6_PERIODIC_SWEEP_SECS, ...)`. Always returns
// G_SOURCE_CONTINUE so the timer survives across reconnects / config
// reloads — the budget cap stays enforced for the lifetime of the tray.
// Wraps the pure `sweep_staging` helper; logging happens inside.
gboolean tray_run_periodic_staging_sweep(gpointer) {
    fs::path staging = tray_capture::default_staging_dir();
    auto protected_paths = tray_collect_journal_protected_paths();
    (void)recmeet::sweep_staging(staging, protected_paths,
                                 g_tray.cfg.staging_max_bytes,
                                 /*extra_pending_bytes=*/0);
    return G_SOURCE_CONTINUE;
}

constexpr int D6_PERIODIC_SWEEP_SECS = 600;  // 10 min per plan line 406

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

    // Phase D.6 — synchronous disk-budget sweep at recording-start. The
    // projected total = current staging usage + expected max recording
    // size (460 MB; 4h × 16 kHz mono int16). If we are at or near the
    // configured budget, evict oldest safe-to-evict WAVs BEFORE the new
    // capture opens its file. Protected entries (journal-referenced
    // in-flight uploads or .pending sidecar save-for-later) are never
    // touched; if even unlinking every safe entry leaves us over budget
    // the sweep logs a warning and the recording proceeds — disk-full
    // detection on write is the OS's job, not the eviction policy's.
    {
        auto protected_paths = tray_collect_journal_protected_paths();
        (void)recmeet::sweep_staging(
            staging, protected_paths,
            g_tray.cfg.staging_max_bytes,
            recmeet::DEFAULT_MAX_RECORDING_BYTES);
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
    // Phase D.5 — mint the per-recording meeting_id EXACTLY ONCE, at
    // start_capture. This id is the stable handle the server-side
    // C.11.4 dedup contract uses; threading the SAME id through every
    // submit-retry is what lets a tray-crash mid-upload recover onto
    // the original meeting directory rather than allocating a fresh one.
    g_tray.capture_state.meeting_id    = recmeet::new_uuid_v4();
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
                    save_legacy_config_as_job_config(g_tray.cfg);
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
            // Phase D.5 — thread the per-recording meeting_id through
            // to the streaming session too, so a process.stream → batch
            // fallback (D.3) lands on the same meeting dir as the
            // batch-submit retry path.
            if (!g_tray.capture_state.meeting_id.empty()) {
                sp["meeting_id"] = g_tray.capture_state.meeting_id;
            }
            // Phase D.1 — admit a JobEntry to the streaming slot.
            // Today the streaming slot is always idle at this point
            // (only one live recording can be active), so admit lands
            // in-flight; the backlog path is reserved for future
            // multi-recording UI work. The admit happens BEFORE the
            // wire call so a failed `process.stream` round-trip rolls
            // back via `complete_in_flight()` and the slot stays
            // consistent.
            recmeet::JobEntry s_entry;
            s_entry.meeting_id       = g_tray.capture_state.meeting_id;
            s_entry.staging_wav_path = g_tray.capture_state.wav_path.string();
            s_entry.kind             = "stream";
            bool s_admitted =
                g_tray.slot_queues.streaming.admit(s_entry);
            if (!s_admitted) {
                // Backlog placement — the rare future-path where the
                // operator queues a second streaming session while one
                // is in-flight. Not exercised by the production UI
                // today; the [d1] tests cover the shape.
                log_info("[tray] D.1 streaming admit deferred to backlog "
                         "(backlog depth=%zu)",
                         g_tray.slot_queues.streaming.backlog_size());
            }

            IpcResponse sr; IpcError se;
            if (g_tray.ipc.call("process.stream", sp, sr, se, 5000)) {
                auto tit = sr.result.find("stream_token");
                auto jit = sr.result.find("job_id");
                std::string token = tit != sr.result.end()
                    ? recmeet::json_val_as_string(tit->second) : "";
                int64_t s_job_id = jit != sr.result.end()
                    ? recmeet::json_val_as_int(jit->second) : 0;
                if (!token.empty()) {
                    g_tray.capture_state.stream_token = token;
                    // Phase D.1 — backfill the in-flight entry's
                    // job_id (admit ran before the wire response).
                    if (s_admitted && s_job_id > 0) {
                        g_tray.slot_queues.streaming
                            .set_in_flight_job_id(s_job_id);
                    }
                    // Phase D.2 — journal write at the streaming-
                    // session reservation return. The plan body cites
                    // `process.stream.commit` as the canonical write
                    // point, but the tray does not call that verb
                    // today (the daemon's StreamingSession auto-
                    // commits during teardown). `process.stream`
                    // return is the equivalent client-observable
                    // "daemon now holds a reservation we may need to
                    // recover" boundary, so the journal entry lands
                    // here. Mid-stream tray crash recovery then
                    // surfaces the meeting_id; per D.3 the streaming
                    // resume falls back to batch via the same
                    // meeting_id (C.10a TCP-drop policy).
                    if (s_admitted && s_job_id > 0) {
                        recmeet::PendingJobsJournal::Entry sje;
                        sje.endpoint          = g_tray.resume_server_key;
                        sje.meeting_id        = s_entry.meeting_id;
                        sje.job_id            = std::to_string(s_job_id);
                        sje.staging_wav_path  = s_entry.staging_wav_path;
                        sje.kind              = s_entry.kind;
                        sje.slot_kind         = recmeet::to_string(
                                                    recmeet::SlotKind::Streaming);
                        sje.submitted_at_unix = static_cast<int64_t>(
                            std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now()
                                    .time_since_epoch()).count());
                        g_tray.pending_jobs.append(sje);
                    }
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
                    log_info("[tray] live-caption stream opened (token=%s job=%lld)",
                             token.c_str(), (long long)s_job_id);
                } else {
                    log_warn("[tray] process.stream returned no stream_token "
                             "— continuing without live captions");
                    // Roll back the streaming-slot admission on the
                    // no-token failure path.
                    if (s_admitted) {
                        g_tray.slot_queues.streaming.complete_in_flight();
                    }
                }
            } else {
                log_warn("[tray] process.stream failed (%s) — continuing "
                         "without live captions", se.message.c_str());
                // Roll back the streaming-slot admission on the wire-
                // call failure path.
                if (s_admitted) {
                    g_tray.slot_queues.streaming.complete_in_flight();
                }
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
    g_tray.capture_state.meeting_id.clear();
    g_tray.capture_state.waiting_disposition = false;
}

// Phase C.9 — submit the staged WAV to the daemon via `process.submit` +
// 0x01 upload-chunk frames. Pre-C.9 the tray called `record.start
// reprocess_dir=<staging-wav>` (Phase B.5 transitional glue) — the
// daemon's live-recording path is gone in C.9 so the tray now uploads
// the WAV in chunks over the IPC socket. Local-host only (the staging
// dir is also local, so the actual byte volume is small, but the wire
// shape mirrors what a future cross-host thin client will use).
//
// Wire sequence:
//   1. `process.submit { audio_size, format, sample_rate, channels,
//                        context, mode }`
//   2. Receive `{ job_id, upload_token, max_size }`. The job is reserved
//      in the postprocess slot but parked (`WaitingForUpload`) until
//      bytes_received == audio_size.
//   3. Stream the WAV in 64 KiB chunks via `send_upload_chunk` (each one
//      a 0x01 binary frame; the daemon writes into the staging file).
//   4. The daemon auto-finalizes when bytes_received hits audio_size —
//      the postprocess job becomes runnable and pp_worker drains it.
//   5. We return immediately; progress events arrive via the existing
//      event stream (subscribers update the postprocess indicator).
//
// Returns true on successful upload dispatch; false on connect / call /
// chunk-write failure. On failure the WAV stays in staging so the
// operator can pick Save-for-later or retry without re-recording.
bool apply_submit_with_context(const CapturedContext& ctx, std::string& err_msg) {
    if (!g_tray.daemon_connected) {
        if (!connect_to_daemon()) {
            err_msg = "daemon not running — start recmeet-daemon and try again";
            return false;
        }
    }

    // Resolve the WAV file size — `audio_size` is what the daemon uses to
    // know when the upload is complete (see UploadSession::feed_chunk).
    std::error_code ec;
    const auto wav_path = g_tray.capture_state.wav_path;
    auto wav_size = fs::file_size(wav_path, ec);
    if (ec || wav_size == 0) {
        err_msg = "cannot stat staged WAV: " +
                  (ec ? ec.message() : std::string("file is empty"));
        return false;
    }

    // Open the WAV file BEFORE issuing process.submit so a transient
    // I/O error here doesn't leave a half-reserved upload slot on the
    // daemon.
    std::ifstream wav_in(wav_path, std::ios::binary);
    if (!wav_in) {
        err_msg = "cannot open staged WAV for read: " + wav_path.string();
        return false;
    }

    // Step 1: process.submit. The staging WAV is the tray's
    // canonical PCM-16 mono recording (B.1 tray_capture writes
    // 44-byte RIFF header + S16LE mono samples). The daemon parses
    // it through libsndfile in the pp subprocess.
    JsonMap params;
    params["audio_size"]  = static_cast<int64_t>(wav_size);
    params["format"]      = std::string("wav");
    params["sample_rate"] = static_cast<int64_t>(SAMPLE_RATE);
    params["channels"]    = static_cast<int64_t>(1);
    params["mode"]        = std::string("transcribe");
    // Phase C.9: fold captured pre-recording context into the submit
    // call. Pre-C.9 the tray sent context via a follow-up `job.context`
    // verb after `record.start`; the verb is gone, and process.submit's
    // `context` field carries the same payload up-front (the daemon's
    // upload session freezes it into the per-job Config snapshot).
    if (!ctx.context_inline.empty()) {
        std::string combined = ctx.context_inline;
        if (!ctx.vocab_additions.empty()) {
            if (!combined.empty()) combined += "\n";
            combined += "Vocabulary: " + ctx.vocab_additions;
        }
        params["context"] = combined;
    }
    // Phase D.5 — thread the client-minted meeting_id through to the
    // daemon's process.submit handler. Empty is the legacy / pre-D.5
    // shape (server-side `is_valid_meeting_id("")` returns true as the
    // "no id" sentinel) but the post-D.5 tray always has a value here
    // because `start_capture` mints unconditionally. The same id is
    // re-used on the H-D3 retry path so C.11.4's atomic-overwrite
    // contract routes the bytes back to the original meeting dir.
    if (!g_tray.capture_state.meeting_id.empty()) {
        params["meeting_id"] = g_tray.capture_state.meeting_id;
    }

    // Phase D.1 — admit a JobEntry to the postprocess slot BEFORE the
    // submit round-trip. Today the slot is always idle at this point
    // (the disposition dialog only opens when the operator stops the
    // current recording; there is no concurrent postprocess submission
    // UI), so this admit always promotes the entry to in-flight. The
    // backlog path is exercised by the [d1] tests below — it becomes
    // load-bearing once D.4's per-slot UI surfaces a "queue another
    // submission" affordance.
    recmeet::JobEntry entry;
    entry.meeting_id       = g_tray.capture_state.meeting_id;
    entry.staging_wav_path = wav_path.string();
    entry.kind             = "submit";
    bool admitted_in_flight =
        g_tray.slot_queues.postprocess.admit(entry);
    if (!admitted_in_flight) {
        // Backlog placement — defer the submit + upload until the
        // current in-flight postprocess job terminates. The drain
        // worker (`handle_terminal_status`, below) will surface this
        // entry on `job.complete` / `job.failed` and the caller can
        // re-enter this path. Today the production tray never lands
        // here; the [d1] backlog test does.
        log_info("[tray] D.1 postprocess admit deferred to backlog "
                 "(in_flight job=%lld, backlog depth=%zu)",
                 (long long)g_tray.slot_queues.postprocess
                     .in_flight()->job_id,
                 g_tray.slot_queues.postprocess.backlog_size());
        return true;
    }

    IpcResponse resp;
    IpcError err;
    if (!g_tray.ipc.call("process.submit", params, resp, err, 10000)) {
        err_msg = err.message.empty() ? "process.submit failed" : err.message;
        // Admission rolled back — the entry never made it to the wire,
        // so clearing the slot keeps the drain-worker invariant
        // (in_flight always represents a server-side reservation).
        g_tray.slot_queues.postprocess.complete_in_flight();
        return false;
    }

    // The daemon's response carries the upload_token implicitly — every
    // subsequent 0x01 frame on this connection is routed to the
    // requesting client_id's pending upload session. The token's job is
    // to let `process.submit.cancel` reference the upload; we keep it
    // around (logged) but don't have to re-send it on each chunk.
    auto tok_it = resp.result.find("upload_token");
    auto job_it = resp.result.find("job_id");
    std::string upload_token =
        (tok_it != resp.result.end()) ? json_val_as_string(tok_it->second) : "";
    int64_t job_id =
        (job_it != resp.result.end()) ? json_val_as_int(job_it->second) : 0;
    if (upload_token.empty() || job_id <= 0) {
        err_msg = "process.submit response missing upload_token / job_id";
        g_tray.slot_queues.postprocess.complete_in_flight();
        return false;
    }
    log_info("[tray] process.submit OK (job=%lld token=%s size=%llu)",
             (long long)job_id, upload_token.c_str(),
             (unsigned long long)wav_size);
    // Phase D.1 — backfill the in-flight entry's job_id (admit ran
    // before we had it) so cancel paths and the drain worker can
    // resolve by id.
    g_tray.slot_queues.postprocess.set_in_flight_job_id(job_id);

    // Phase D.2 — write the journal entry immediately on submit return,
    // BEFORE the chunk upload begins. A mid-upload tray crash leaves a
    // journal entry with the meeting_id, so the restart path (H-D3) can
    // retry by re-submitting under the same meeting_id and the server's
    // C.11.4 dedup contract atomically overwrites whatever partial
    // bytes the previous run uploaded.
    {
        recmeet::PendingJobsJournal::Entry je;
        je.endpoint          = g_tray.resume_server_key;
        je.meeting_id        = entry.meeting_id;
        je.job_id            = std::to_string(job_id);
        je.staging_wav_path  = entry.staging_wav_path;
        je.kind              = entry.kind;
        je.slot_kind         = recmeet::to_string(
                                   recmeet::SlotKind::Postprocess);
        je.submitted_at_unix = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        g_tray.pending_jobs.append(je);
    }

    // Step 2: stream the file in chunks. 64 KiB is a comfortable
    // trade-off between IPC syscall overhead and tray responsiveness.
    // The daemon's UploadSession::feed_chunk caps each chunk against
    // `max_size` (the per-session limit echoed back in process.submit's
    // response), but on local-host loopback we never hit that.
    constexpr std::size_t CHUNK_BYTES = 64 * 1024;
    std::vector<char> buf(CHUNK_BYTES);
    std::uint64_t sent = 0;
    while (sent < wav_size) {
        std::size_t want = std::min<std::size_t>(CHUNK_BYTES, wav_size - sent);
        wav_in.read(buf.data(), static_cast<std::streamsize>(want));
        std::streamsize got = wav_in.gcount();
        if (got <= 0) {
            err_msg = "WAV read short at offset " + std::to_string(sent);
            return false;
        }
        std::string chunk(buf.data(), static_cast<std::size_t>(got));
        if (!g_tray.ipc.send_upload_chunk(chunk)) {
            err_msg = "send_upload_chunk failed at offset " + std::to_string(sent);
            return false;
        }
        sent += static_cast<std::uint64_t>(got);
    }
    log_info("[tray] submitted WAV via process.submit upload: %s (job=%lld)",
             wav_path.c_str(), (long long)job_id);

    // Step 3: the daemon's UploadSessionManager auto-finalizes once
    // bytes_received hits audio_size, transitioning the postprocess
    // job from `WaitingForUpload` to `Queued`. The pp_worker dequeues
    // it and the existing progress / job.complete event stream takes
    // over from there. No further IPC from this thread is needed.
    g_tray.capture_state.wav_path.clear();
    g_tray.capture_state.wav_timestamp.clear();
    g_tray.capture_state.wav_source.clear();
    g_tray.capture_state.meeting_id.clear();
    g_tray.capture_state.waiting_disposition = false;
    // The local staging WAV is now redundant (the daemon owns the
    // bytes in its own staging dir). Leave it on disk — the next
    // recording will overwrite it, and ops can find it for forensics
    // if a submit-then-pp-failure trace is needed.
    return true;
}

// Phase D.5 — leave the WAV in staging and drop a `.pending` sidecar v2
// so the tray-restart recovery path can re-surface the disposition flow
// with the captured meeting_id + context pre-populated. Atomic-write
// via `util::atomic_write_file`; on filesystem failure the sidecar is
// absent and the WAV is treated as orphaned (will be cleaned up by
// D.6's disk-budget sweep). Accepts an optional captured context — when
// empty (operator dismissed the dialog without filling fields) the
// `context` block is written with empty values and the resume dialog
// will re-prompt.
void apply_save_for_later(const CapturedContext& ctx) {
    tray_capture::PendingSidecarV2 payload;
    payload.meeting_id       = g_tray.capture_state.meeting_id;
    payload.wav_path         = g_tray.capture_state.wav_path.string();
    payload.timestamp        = g_tray.capture_state.wav_timestamp;
    payload.mic_source       = g_tray.capture_state.wav_source;
    payload.captions_enabled = g_tray.cap.captions_enabled_for_recording;
    payload.context.subject      = ctx.subject;
    payload.context.participants = ctx.participants;
    payload.context.notes        = ctx.notes;
    payload.context.language     = g_tray.cfg.language;
    if (!g_tray.cfg.vocabulary.empty()) {
        // Config carries vocabulary as a single newline-separated string;
        // sidecar v2 carries it as an array. Split on newlines + commas
        // to give the resume dialog a clean per-term list.
        std::string v = g_tray.cfg.vocabulary;
        std::string acc;
        for (char c : v) {
            if (c == '\n' || c == ',') {
                if (!acc.empty()) payload.context.vocabulary.push_back(acc);
                acc.clear();
            } else if (c != '\r') {
                acc += c;
            }
        }
        if (!acc.empty()) payload.context.vocabulary.push_back(acc);
    }

    try {
        tray_capture::write_pending_sidecar_v2(payload);
        log_info("[tray] saved WAV for later: %s (meeting_id=%s)",
                 g_tray.capture_state.wav_path.c_str(),
                 g_tray.capture_state.meeting_id.c_str());
    } catch (const std::exception& e) {
        log_warn("[tray] save-for-later: failed to write .pending sidecar "
                 "for %s: %s",
                 g_tray.capture_state.wav_path.c_str(), e.what());
    }
    // Clear the active staging fields — the WAV is now "saved",
    // outside the active capture lifecycle.
    g_tray.capture_state.wav_path.clear();
    g_tray.capture_state.wav_timestamp.clear();
    g_tray.capture_state.wav_source.clear();
    g_tray.capture_state.meeting_id.clear();
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
                // Phase C.9: pre-submit, fold captured context into the
                // submit call. The legacy two-step (record.start +
                // post-submit job.context) is gone — `process.submit`'s
                // `context` field carries the same payload up-front.
                std::string sub_err;
                if (!tray::apply_submit_with_context(captured_ctx, sub_err)) {
                    notify("Submit failed", sub_err);
                    // WAV remains in staging; tray stays in
                    // waiting_disposition so the operator can retry or
                    // pick Save-for-later. Re-present dialog state via
                    // build_menu so the indicator reflects reality.
                    build_menu();
                    return;
                }
                break;
            }
            case StopDisposition::Discard:
                tray::apply_discard();
                break;
            case StopDisposition::SaveForLater:
            case StopDisposition::Cancelled:
                // Phase D.5 — thread the captured context into the
                // sidecar v2 so the resume dialog can re-present submit
                // with the operator's subject/participants/notes
                // pre-populated.
                tray::apply_save_for_later(captured_ctx);
                break;
        }
        build_menu();
        return;
    }

    if (g_tray.postprocessing) {
        // Daemon-side postprocess cancel — same as the legacy on_cancel_pp.
        // Phase C.9: `record.stop target=postprocessing` is gone; use
        // `process.cancel { job_id }` (C.5) against the most-recent
        // job dispatched via process.submit. Phase D.1: the previous
        // scalar `last_pp_job_id` retired in favor of the postprocess
        // slot queue; we read the in-flight entry instead. If the slot
        // is idle the menu entry should not have been clickable — log
        // and skip rather than crash.
        const auto* pp = g_tray.slot_queues.postprocess.in_flight();
        if (!pp || pp->job_id <= 0) {
            log_warn("[tray] postprocess cancel requested but no in-flight job");
        } else {
            int64_t pp_job_id = pp->job_id;
            JsonMap params;
            params["job_id"] = pp_job_id;
            IpcResponse resp;
            IpcError err;
            if (!g_tray.ipc.call("process.cancel", params, resp, err, 5000)) {
                log_error("[tray] process.cancel (job=%lld) failed: %s",
                          (long long)pp_job_id, err.message.c_str());
            }
        }
    }
}

static void on_cancel_pp(GtkMenuItem*, gpointer) {
    if (!g_tray.postprocessing) return;

    // Phase C.9: `record.stop target=postprocessing` → `process.cancel`.
    // Phase D.1: postprocess in-flight tracking lives in the slot queue.
    const auto* pp = g_tray.slot_queues.postprocess.in_flight();
    if (!pp || pp->job_id <= 0) {
        log_warn("[tray] cancel-pp requested but no in-flight job");
        return;
    }
    int64_t pp_job_id = pp->job_id;
    JsonMap params;
    params["job_id"] = pp_job_id;

    IpcResponse resp;
    IpcError err;
    if (!g_tray.ipc.call("process.cancel", params, resp, err, 5000)) {
        log_error("[tray] cancel postprocessing (job=%lld) failed: %s",
                  (long long)pp_job_id, err.message.c_str());
    }
}

// --- Radio / checkbox callbacks ---

static void on_mic_selected(GtkCheckMenuItem* item, gpointer data) {
    if (!gtk_check_menu_item_get_active(item)) return;
    auto* name = static_cast<const char*>(data);
    g_tray.cfg.mic_source = name ? name : "";
    save_legacy_config_as_job_config(g_tray.cfg);
}

static void on_monitor_selected(GtkCheckMenuItem* item, gpointer data) {
    if (!gtk_check_menu_item_get_active(item)) return;
    auto* name = static_cast<const char*>(data);
    g_tray.cfg.monitor_source = name ? name : "";
    save_legacy_config_as_job_config(g_tray.cfg);
}

static void on_model_selected(GtkCheckMenuItem* item, gpointer data) {
    if (!gtk_check_menu_item_get_active(item)) return;
    auto* name = static_cast<const char*>(data);
    g_tray.cfg.whisper_model = name;
    save_legacy_config_as_job_config(g_tray.cfg);

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
    save_legacy_config_as_job_config(g_tray.cfg);
}

static void on_mic_only_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.mic_only = gtk_check_menu_item_get_active(item);
    save_legacy_config_as_job_config(g_tray.cfg);
}

static void on_no_summary_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.no_summary = gtk_check_menu_item_get_active(item);
    save_legacy_config_as_job_config(g_tray.cfg);
}

static void on_diarize_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.diarize = gtk_check_menu_item_get_active(item);
    save_legacy_config_as_job_config(g_tray.cfg);
}

static void on_vad_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.vad = gtk_check_menu_item_get_active(item);
    save_legacy_config_as_job_config(g_tray.cfg);
}

// Phase 5.4 — Live captions toggle. Persists to config and applies to the
// NEXT recording (captions are bound to record.start params; mid-recording
// toggles do not take effect — the menu's tooltip makes this explicit).
static void on_captions_enabled_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.captions_enabled = gtk_check_menu_item_get_active(item);
    save_legacy_config_as_job_config(g_tray.cfg);
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
        save_legacy_config_as_job_config(g_tray.cfg);
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

    save_legacy_config_as_job_config(g_tray.cfg);
    fetch_provider_models();
    build_menu();
}

static void on_api_model_selected(GtkCheckMenuItem* item, gpointer data) {
    if (!gtk_check_menu_item_get_active(item)) return;
    auto* model = static_cast<const char*>(data);
    if (model) {
        g_tray.cfg.api_model = model;
        save_legacy_config_as_job_config(g_tray.cfg);
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
        save_legacy_config_as_job_config(g_tray.cfg);
        build_menu();
    }
}

// --- Output path callbacks ---

static void on_set_output_dir(GtkMenuItem*, gpointer) {
    std::string path = run_folder_chooser("Select Output Directory");
    if (!path.empty()) {
        g_tray.cfg.output_dir = path;
        save_legacy_config_as_job_config(g_tray.cfg);
        build_menu();
    }
}

static void on_set_note_dir(GtkMenuItem*, gpointer) {
    std::string path = run_folder_chooser("Select Note Directory");
    if (!path.empty()) {
        g_tray.cfg.note_dir = path;
        save_legacy_config_as_job_config(g_tray.cfg);
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
        save_legacy_config_as_job_config(g_tray.cfg);

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

// Phase E.6.2 — open Speaker Management menu item.
//
// Pre-E.6.2 this forked an external web subprocess, polled the port
// until it answered, then xdg-open'd it. Post-E.6.2 the WebUI is a
// tray subsystem: start_web_listener() binds an embedded httplib::Server
// on a kernel-picked loopback port (idempotent — re-entry returns the
// already-bound port) and we xdg-open that URL directly. No subprocess
// supervision, no SIGCHLD reaper, no port probing.
static void on_open_speaker_ui(GtkMenuItem*, gpointer) {
    int port = recmeet::start_web_listener(g_tray.ipc);
    if (port <= 0) {
        notify("Speaker Management", "Failed to start embedded WebUI listener");
        return;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(port);
    const std::string cmd = "xdg-open '" + url + "' &";
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

// ---------------------------------------------------------------------------
// Phase D.5 — startup resume recovery
// ---------------------------------------------------------------------------

// Scan the tray's staging directory for `*.pending` sidecars and populate
// `g_tray.pending_resumes` with the parsed payloads. A malformed sidecar
// (read returns an empty payload) is skipped silently — its presence is
// logged but the file is left on disk for forensic inspection. The scan
// is non-recursive (staging is flat).
static void rescan_pending_sidecars() {
    g_tray.pending_resumes.clear();
    fs::path staging = tray_capture::default_staging_dir();
    std::error_code ec;
    if (!fs::is_directory(staging, ec) || ec) return;
    for (const auto& entry : fs::directory_iterator(staging, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        if (p.extension() != ".pending") continue;
        auto payload = tray_capture::read_pending_sidecar(p);
        if (payload.meeting_id.empty() && payload.wav_path.empty()) {
            log_warn("[tray] resume scan: skipping malformed sidecar %s",
                     p.string().c_str());
            continue;
        }
        g_tray.pending_resumes.push_back(std::move(payload));
    }
}

// Apply one resume-submit by index. Re-uses the existing submit pipeline
// by populating capture_state with the sidecar's recorded values, then
// invoking `apply_submit_with_context`. On success the sidecar is
// removed and the menu is rebuilt. On failure the sidecar stays on disk
// so the operator can retry. Threaded through the GTK main loop — there
// is no IPC background thread in the tray.
static void apply_resume_submit(size_t idx) {
    if (idx >= g_tray.pending_resumes.size()) return;
    auto p = g_tray.pending_resumes[idx];

    if (g_tray.recording || g_tray.capture_state.waiting_disposition) {
        notify("Cannot resume", "Stop the current recording first.");
        return;
    }

    g_tray.capture_state.wav_path      = fs::path(p.wav_path);
    g_tray.capture_state.wav_timestamp = p.timestamp;
    g_tray.capture_state.wav_source    = p.mic_source;
    g_tray.capture_state.meeting_id    = p.meeting_id;
    g_tray.cap.captions_enabled_for_recording = p.captions_enabled;

    CapturedContext ctx;
    if (!p.context.subject.empty())
        ctx.context_inline += "Subject: " + p.context.subject + "\n";
    if (!p.context.participants.empty()) {
        std::string joined;
        for (size_t i = 0; i < p.context.participants.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += p.context.participants[i];
        }
        ctx.context_inline += "Participants: " + joined + "\n";
        ctx.vocab_additions = joined;
    }
    if (!p.context.notes.empty()) {
        if (!ctx.context_inline.empty()) ctx.context_inline += "\n";
        ctx.context_inline += p.context.notes;
    }
    ctx.subject      = p.context.subject;
    ctx.participants = p.context.participants;
    ctx.notes        = p.context.notes;

    std::string err;
    if (!tray::apply_submit_with_context(ctx, err)) {
        notify("Resume submit failed", err);
        // Leave the sidecar in place; restore capture_state to idle so
        // the next recording lifecycle is unaffected.
        g_tray.capture_state.wav_path.clear();
        g_tray.capture_state.wav_timestamp.clear();
        g_tray.capture_state.wav_source.clear();
        g_tray.capture_state.meeting_id.clear();
        return;
    }
    // Success — remove the sidecar so the next rescan doesn't re-surface
    // it. apply_submit_with_context already cleared capture_state fields.
    fs::path sidecar = tray_capture::pending_sidecar_path(fs::path(p.wav_path));
    std::error_code ec;
    fs::remove(sidecar, ec);
    if (ec)
        log_warn("[tray] resume submit: failed to remove sidecar %s: %s",
                 sidecar.string().c_str(), ec.message().c_str());
    rescan_pending_sidecars();
    build_menu();
}

// Discard a pending resume — unlink both the sidecar and the staging
// WAV. Idempotent on missing files.
static void apply_resume_discard(size_t idx) {
    if (idx >= g_tray.pending_resumes.size()) return;
    auto p = g_tray.pending_resumes[idx];
    fs::path wav(p.wav_path);
    fs::path sidecar = tray_capture::pending_sidecar_path(wav);
    std::error_code ec;
    fs::remove(sidecar, ec);
    fs::remove(wav, ec);
    log_info("[tray] resume discard: %s", wav.string().c_str());
    rescan_pending_sidecars();
    build_menu();
}

// GTK menu-item callback. `user_data` carries the index of the entry in
// `g_tray.pending_resumes` (encoded via GINT_TO_POINTER).
extern "C" void on_resume_submit(GtkMenuItem*, gpointer ud) {
    apply_resume_submit(static_cast<size_t>(GPOINTER_TO_INT(ud)));
}
extern "C" void on_resume_discard(GtkMenuItem*, gpointer ud) {
    apply_resume_discard(static_cast<size_t>(GPOINTER_TO_INT(ud)));
}

// ---------------------------------------------------------------------------
// Phase D.5 — journal recovery (loaded entries dispatched via job.status)
// ---------------------------------------------------------------------------
//
// On tray startup, every journal entry is resolved via `job.status`:
//   - `unknown` / `expired-token`: per H-D3 retry by re-submitting with the
//     SAME `meeting_id` (C.11.4 dedup contract routes the bytes back to
//     the original meeting directory). Streaming entries fall back to
//     batch per D.3.
//   - `complete`: dispatch `process.fetch` to download artifacts; on
//     success remove the journal entry.
//   - `running`: monitoring is D.3 territory — leave the entry in place;
//     D.3 will subscribe to progress.job events.
//
// D.5 lands the dispatch shape; the actual `job.status` parsing + retry
// arms are minimal here (the call site only logs the resolved status
// and leaves recovery actions to D.2 / D.3). The retry shape is unit-
// tested in `test_tray_resume_recovery.cpp` against a mock IpcClient.
static void recover_pending_jobs_on_startup() {
    auto entries = g_tray.pending_jobs.load();
    if (entries.empty()) return;
    log_info("[tray] resume: %zu pending job(s) in journal", entries.size());
    for (const auto& e : entries) {
        if (!g_tray.daemon_connected) {
            log_info("[tray] resume: daemon not connected, deferring %s",
                     e.job_id.c_str());
            continue;
        }
        JsonMap params;
        if (!e.job_id.empty()) {
            // The wire shape accepts int64; the journal stores the id as a
            // string so the schema does not commit to a particular numeric
            // representation. Attempt a stoll; on failure skip (the entry
            // is malformed and the next save will leave it alone).
            try {
                params["job_id"] = static_cast<int64_t>(std::stoll(e.job_id));
            } catch (...) {
                log_warn("[tray] resume: skipping malformed job_id %s",
                         e.job_id.c_str());
                continue;
            }
        }
        IpcResponse resp;
        IpcError err;
        if (!g_tray.ipc.call("job.status", params, resp, err, 3000)) {
            log_warn("[tray] resume: job.status(%s) failed: %s",
                     e.job_id.c_str(), err.message.c_str());
            continue;
        }
        auto it = resp.result.find("status");
        std::string status = (it != resp.result.end())
            ? json_val_as_string(it->second) : "";
        log_info("[tray] resume: job_id=%s status=%s (meeting_id=%s)",
                 e.job_id.c_str(), status.c_str(), e.meeting_id.c_str());
        // D.2/D.3 wire the actual retry/fetch arms; D.5 only surfaces
        // the resolved status so the operator can see the recovered
        // entry in the tray status line.
    }
}

static void build_menu() {
    // Phase E.6.3 — headless: no indicator → no menu to attach. Every
    // build_menu() call site is reachable from event handlers that may
    // fire on the GLib main loop even in headless mode (reconnect
    // events, postprocess transitions, model-download completions),
    // so short-circuit here rather than guarding every call site.
    if (!g_tray.indicator) return;

    auto* menu = gtk_menu_new();
    bool is_idle = !g_tray.recording && !g_tray.postprocessing && !g_tray.downloading;
    bool can_record = !g_tray.recording && !g_tray.downloading;

    // --- Phase D.4 — connection state with jitter-aware reconnect
    //                 countdown.
    //
    // Replaces the pre-D.4 single-line "Status: ..." item (and its
    // `g_tray.status_menu_item` widget pointer that the inline phase /
    // progress handlers used to mutate). The per-slot rows below now
    // carry the live phase + progress data; this row's sole job is the
    // connection state + jitter-aware reconnect countdown the D.3
    // scheduler already armed.
    {
        bool armed = !g_tray.daemon_connected
                     && g_tray.reconnect_scheduled_at != 0
                     && g_tray.reconnect_jittered_secs > 0;
        int remaining = 0;
        if (armed) {
            time_t now = std::time(nullptr);
            int elapsed = static_cast<int>(now - g_tray.reconnect_scheduled_at);
            remaining = g_tray.reconnect_jittered_secs - elapsed;
            if (remaining < 0) remaining = 0;
        }
        std::string label = recmeet::render_reconnect_status_line(
            g_tray.daemon_connected, armed, remaining);
        auto* item = gtk_menu_item_new_with_label(label.c_str());
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    // --- Phase D.4 — per-server queue depth.
    //
    // Multi-server hook #5: the renderer iterates a std::vector<ServerView>
    // from day one, even though v1 has a length-1 list. We derive the
    // single entry locally rather than reading `Config::servers` because
    // the schema split is Phase E.2 (plan lines 416-425); D.4 must not
    // pre-empt that work.
    {
        std::size_t total = g_tray.slot_queues.postprocess.backlog_size()
                          + g_tray.slot_queues.streaming.backlog_size()
                          + g_tray.slot_queues.model_download.backlog_size();
        auto servers = recmeet::derive_single_server_view(
            g_tray.daemon_addr, total);
        for (const auto& sv : servers) {
            std::string label = recmeet::render_server_row(sv);
            auto* item = gtk_menu_item_new_with_label(label.c_str());
            gtk_widget_set_sensitive(item, FALSE);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }
    }

    // --- Phase D.4 — three per-slot status rows (postprocess, streaming,
    //                 model_download), in stable order matching C.7's
    //                 server-side typed-slot enum order.
    //
    // The in-flight view is populated from `slot_queues.<kind>.in_flight()`
    // (D.1) plus the per-slot phase + progress maps `current_phase_by_slot`
    // / `progress_percent_by_slot` (D.4 follow-up — pre-fix this was a
    // single shared pair of globals which collapsed concurrent slots
    // onto whichever event landed last). The `handle_ipc_event` "phase"
    // / "progress" handlers and `post_reconnect_resync` both route into
    // these maps by SlotKind; this consumer just reads the per-kind
    // entry (`.find(kind)` → empty/sentinel if absent, e.g. pre-first-
    // event window).
    //
    // For the postprocess slot we also fold the legacy `g_tray.postprocessing`
    // boolean into the "Working..." fallback so the row stays meaningful
    // for the brief window between admit and the first phase event. The
    // streaming row inherits the same fallback when `g_tray.recording` &&
    // captions are enabled. Model download is gated on `g_tray.downloading`.
    auto append_slot_row = [&](recmeet::SlotKind kind,
                               const char* fallback_phase,
                               bool fallback_active) {
        const auto& q = g_tray.slot_queues.select(kind);
        std::optional<recmeet::InFlightView> view;
        if (q.is_in_flight()) {
            recmeet::InFlightView v;
            v.job_id = q.in_flight()->job_id;
            // Per-slot phase + progress — D.4 follow-up: read the
            // entry for this slot (NOT the same shared pair three
            // times). `.find()` returns end() before the first phase
            // event for the slot, in which case the renderer's
            // "Working..." fallback applies via empty `v.phase`.
            auto pit = g_tray.current_phase_by_slot.find(kind);
            if (pit != g_tray.current_phase_by_slot.end()) {
                v.phase = pit->second;
            }
            auto qit = g_tray.progress_percent_by_slot.find(kind);
            v.progress_percent = (qit != g_tray.progress_percent_by_slot.end())
                ? qit->second : -1;
            view = v;
        } else if (fallback_active && fallback_phase) {
            recmeet::InFlightView v;
            v.phase = fallback_phase;
            view = v;
        }
        std::string label = recmeet::render_slot_row(
            kind, view, q.backlog_size());
        auto* item = gtk_menu_item_new_with_label(label.c_str());
        gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    };
    append_slot_row(recmeet::SlotKind::Postprocess,
                    "processing", g_tray.postprocessing);
    append_slot_row(recmeet::SlotKind::Streaming,
                    "recording",
                    g_tray.recording && g_tray.cap.captions_enabled_for_recording);
    append_slot_row(recmeet::SlotKind::ModelDownload,
                    g_tray.download_model.empty() ? "downloading"
                                                  : g_tray.download_model.c_str(),
                    g_tray.downloading);

    // --- Phase D.4 — streaming caption inline row.
    //
    // Shown IN ADDITION to the C.10a overlay (not instead of). We read
    // the same `g_tray.cap.state` buffer the overlay renders from —
    // `CaptionRenderState::latest_text()` is a thin accessor over the
    // most recent line. No duplicate caption-event subscription; the
    // existing `caption` event handler already triggers `build_menu()`
    // so this row refreshes alongside the overlay update.
    //
    // The row is suppressed entirely when there is no caption text to
    // show (no streaming session, or pre-first-caption window).
    if (g_tray.cap.captions_enabled_for_recording) {
        std::string latest = g_tray.cap.state.latest_text();
        std::string row = recmeet::render_caption_inline_row(latest);
        if (!row.empty()) {
            auto* item = gtk_menu_item_new_with_label(row.c_str());
            gtk_widget_set_sensitive(item, FALSE);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }
    }

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

    // --- Phase D.5: Resume Pending submenu ---
    //
    // Surfaced whenever there is at least one `.pending` sidecar in
    // staging. Per-entry submenu carries Submit + Discard actions; the
    // count in the parent label reflects the in-memory snapshot
    // (rebuilt by `rescan_pending_sidecars()` after every
    // submit/discard so the count updates without a tray restart).
    if (!g_tray.pending_resumes.empty()) {
        std::string label = "Resume Pending (" +
            std::to_string(g_tray.pending_resumes.size()) + ")";
        auto* parent = gtk_menu_item_new_with_label(label.c_str());
        auto* sub = gtk_menu_new();
        for (size_t i = 0; i < g_tray.pending_resumes.size(); ++i) {
            const auto& p = g_tray.pending_resumes[i];
            std::string entry_label = p.timestamp;
            if (!p.context.subject.empty())
                entry_label += " — " + p.context.subject;
            auto* entry_item = gtk_menu_item_new_with_label(entry_label.c_str());
            auto* entry_sub  = gtk_menu_new();

            auto* sub_item = gtk_menu_item_new_with_label("Submit");
            g_signal_connect(sub_item, "activate",
                             G_CALLBACK(on_resume_submit),
                             GINT_TO_POINTER(static_cast<int>(i)));
            gtk_widget_set_sensitive(sub_item, g_tray.daemon_connected);
            gtk_menu_shell_append(GTK_MENU_SHELL(entry_sub), sub_item);

            auto* disc_item = gtk_menu_item_new_with_label("Discard");
            g_signal_connect(disc_item, "activate",
                             G_CALLBACK(on_resume_discard),
                             GINT_TO_POINTER(static_cast<int>(i)));
            gtk_menu_shell_append(GTK_MENU_SHELL(entry_sub), disc_item);

            gtk_menu_item_set_submenu(GTK_MENU_ITEM(entry_item), entry_sub);
            gtk_menu_shell_append(GTK_MENU_SHELL(sub), entry_item);
        }
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(parent), sub);
        gtk_widget_set_sensitive(parent, !g_tray.recording);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), parent);
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

// Phase E.6.3 — headless mode runs the tray with no GTK status icon, no
// libnotify, and a `GMainLoop` (not `gtk_main`) for signal handling.
// The file-static loop pointer is wired up only on the headless path; the
// SIGTERM/SIGINT handlers (registered via `g_unix_signal_add`, so they run
// on the GLib main loop — async-signal-safe by construction) branch on
// whether `g_headless_loop` was populated.
static GMainLoop* g_headless_loop = nullptr;

namespace {

// Print the headless-startup help line. Stderr so it doesn't pollute any
// stdout-based smoke probe.
void print_headless_only_rejection() {
    fputs(
        "recmeet-tray: --headless requires --listen-now\n"
        "  (a headless tray with no HTTP listener has no way to be\n"
        "   interacted with — pass both flags, or drop --headless to\n"
        "   run a normal GUI tray)\n",
        stderr);
}

// Pre-parse loop: walks argv and detects --listen-now / --headless WITHOUT
// consuming or modifying argv. Called BEFORE `gtk_init` so we can skip
// `gtk_init` entirely in headless mode (gtk_init aborts the process via
// g_critical when no display can be opened, which would defeat the smoke
// gate on a CI host without DISPLAY/WAYLAND_DISPLAY).
//
// Returns 0 if the flags are an acceptable combination, nonzero exit code
// otherwise (e.g. --headless alone is rejected).
int preparse_runtime_flags(int argc, char* argv[],
                           bool& out_listen_now,
                           bool& out_headless) {
    out_listen_now = false;
    out_headless = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--listen-now") out_listen_now = true;
        else if (a == "--headless") out_headless = true;
    }
    if (out_headless && !out_listen_now) {
        print_headless_only_rejection();
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    // Phase E.6.3 — pre-parse `--headless` / `--listen-now` BEFORE
    // gtk_init. Calling gtk_init on a headless host (no DISPLAY,
    // no WAYLAND_DISPLAY) terminates the process via g_critical from
    // gtk_init_check; the smoke gate runs in exactly that shape.
    bool listen_now = false;
    bool headless = false;
    if (int rc = preparse_runtime_flags(argc, argv, listen_now, headless))
        return rc;

    g_tray.headless = headless;

    if (!headless) {
        gtk_init(&argc, &argv);
        notify_init();
    }

    // Parse remaining flags (GTK leaves unknown args in argv, and the
    // headless path skipped gtk_init entirely so every argv entry is
    // still here). --listen-now / --headless are accepted as no-ops here
    // (they were already consumed conceptually by preparse_runtime_flags);
    // --daemon ADDRESS is consumed.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--daemon" && i + 1 < argc) {
            g_tray.daemon_addr = argv[++i];
            continue;
        }
        if (a == "--listen-now") continue;
        if (a == "--headless")   continue;
    }
    // RECMEET_DAEMON_ADDR env var as fallback
    if (g_tray.daemon_addr.empty()) {
        if (const char* env = std::getenv("RECMEET_DAEMON_ADDR"))
            g_tray.daemon_addr = env;
    }
    // Set IPC address before first connect
    if (!g_tray.daemon_addr.empty())
        g_tray.ipc.set_address(g_tray.daemon_addr);

    g_tray.cfg = load_legacy_config_as_job_config();

    // Initialize logging (tray always logs to stderr — journald or interactive)
    auto log_level = parse_log_level(g_tray.cfg.log_level_str);
    log_init(log_level, g_tray.cfg.log_dir, g_tray.cfg.log_retention_hours, true);

    // Create indicator (GUI path only — headless has no system tray icon).
    if (!headless) {
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
    }

    // Connect to daemon (non-blocking — will retry via timer if not
    // running). Phase D.3: the first attempt at startup still goes
    // through the jittered-backoff scheduler so multi-tray hosts (rare
    // but possible: power-on of several user sessions at the same
    // moment) do not converge a thundering accept() burst on the daemon.
    if (!connect_to_daemon()) {
        g_tray.reconnect_delay = 1;
        schedule_reconnect_attempt();
    }

    // Phase D.5 — startup resume recovery.
    //   1. Scan the staging dir for `.pending` sidecars; populate the
    //      `Resume Pending (N)` submenu.
    //   2. Emit a SINGLE dbus notification if N > 0 (architecture-review
    //      checklist item #7: never per-entry; that path leads to
    //      notification spam).
    //   3. Load the journal and dispatch `job.status` per entry. The
    //      journal recovery loop is best-effort — D.2 writes the
    //      entries; D.3 wires the actual reconnect-with-jitter; D.5
    //      only surfaces the resolved status so the operator can see
    //      the recovered state.
    rescan_pending_sidecars();
    if (!g_tray.pending_resumes.empty()) {
        std::string msg = std::to_string(g_tray.pending_resumes.size()) +
            " saved recording(s) ready to resume — see tray menu.";
        notify("recmeet: pending recordings", msg);
    }
    recover_pending_jobs_on_startup();

    // Phase D.6 — register the periodic staging-sweep timer. Fires every
    // 10 min (D6_PERIODIC_SWEEP_SECS) for the lifetime of the tray
    // process. The synchronous-at-start sweep (in start_capture) covers
    // the common case where a new recording would push us over budget;
    // this periodic sweep covers the long-tail case where a sidecar
    // transitions to "fetched" between recordings and the now-evictable
    // WAV should not wait for the NEXT recording to be reclaimed.
    //
    // The timer only needs a `GMainLoop`, not GTK — so it is kept in
    // headless mode too (the periodic sweep is correctness-relevant
    // background behavior, not GUI behavior).
    g_timeout_add_seconds(D6_PERIODIC_SWEEP_SECS,
                          tray_run_periodic_staging_sweep, nullptr);

    if (!headless) {
        build_menu();
        fetch_provider_models();
    }

    // Phase E.6.2 — SIGCHLD handler retained as a safety net. The
    // pre-E.6.2 reason for this hook was reaping web-subprocess zombies,
    // which is gone (the WebUI is now an in-process httplib listener).
    // A no-op reaper still catches stray children spawned via `system()`
    // (xdg-open, $EDITOR, etc.) so they never linger as zombies.
    {
        struct sigaction sa{};
        sa.sa_handler = [](int) {
            while (waitpid(-1, nullptr, WNOHANG) > 0) {}
        };
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, nullptr);
    }

    // Phase E.6.3 — Handle SIGTERM/SIGINT gracefully via GLib main loop.
    // `g_unix_signal_add` dispatches the handler on the GLib main loop
    // thread (NOT in raw POSIX signal context), so calling
    // `g_main_loop_quit` from inside the handler is safe — unlike
    // `pthread_cond_signal` / `cv.notify_all` which would be async-
    // signal-unsafe. Same mechanism on both code paths; the handler
    // body chooses which loop to quit based on whether the file-static
    // `g_headless_loop` pointer was populated.
    g_unix_signal_add(SIGTERM, [](gpointer) -> gboolean {
        if (g_headless_loop) g_main_loop_quit(g_headless_loop);
        else gtk_main_quit();
        return G_SOURCE_REMOVE;
    }, nullptr);
    g_unix_signal_add(SIGINT, [](gpointer) -> gboolean {
        if (g_headless_loop) g_main_loop_quit(g_headless_loop);
        else gtk_main_quit();
        return G_SOURCE_REMOVE;
    }, nullptr);

    // Phase E.6.3 — `--listen-now` binds the embedded WebUI listener
    // BEFORE the main loop runs, so external probes (CI smoke gate,
    // operator debugging) can hit /api/* without first synthesizing a
    // menu click. Idempotent with the lazy on-menu-click start path:
    // `start_web_listener` short-circuits on re-entry.
    if (listen_now) {
        const int port = recmeet::start_web_listener(g_tray.ipc);
        if (port <= 0) {
            log_error("recmeet-tray: --listen-now requested but WebUI "
                      "listener failed to bind");
            return 2;
        }
    }

    log_info("recmeet-tray %s running (%s, %zu mic(s), %zu monitor(s), "
             "listener-port=%d)",
             RECMEET_VERSION,
             headless ? "headless" : "gui",
             g_tray.mics.size(), g_tray.monitors.size(),
             recmeet::get_listener_port());

    if (headless) {
        g_headless_loop = g_main_loop_new(nullptr, FALSE);
        g_main_loop_run(g_headless_loop);
        g_main_loop_unref(g_headless_loop);
        g_headless_loop = nullptr;
    } else {
        gtk_main();
    }

    // Cleanup — runs whether exited via on_quit, SIGTERM, or SIGINT.
    // Phase C.9: `record.stop` is gone. If a postprocess job is still in
    // flight we cancel it via `process.cancel { job_id }`; the local
    // capture state is torn down by tray destruction (PipeWireCapture
    // dtor stops the loop). Phase D.1: read the in-flight entry from
    // the postprocess slot queue (the pre-D.1 `last_pp_job_id` scalar
    // retired). If the slot is idle the recording is still local-only
    // and there is nothing on the wire to cancel.
    if (g_tray.postprocessing && g_tray.daemon_connected) {
        const auto* pp = g_tray.slot_queues.postprocess.in_flight();
        if (pp && pp->job_id > 0) {
            IpcResponse resp;
            IpcError err;
            JsonMap params;
            params["job_id"] = pp->job_id;
            g_tray.ipc.call("process.cancel", params, resp, err, 2000);
        }
    }
    teardown_ipc_watch();
    g_tray.ipc.close_connection();
    // Phase E.6.2 — stop the embedded WebUI listener (no-op if it was
    // never started). Replaces the pre-E.6.2 helper that SIGTERM'd
    // the supervised external web subprocess.
    recmeet::stop_web_listener();
    caption_overlay_destroy();   // Phase 5.1 — release overlay window/timer

    if (g_tray.reconnect_timer)
        g_source_remove(g_tray.reconnect_timer);

    log_shutdown();
    if (!headless) notify_cleanup();
    return 0;
}
