// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "config.h"
#include "diarize.h"
#include "device_enum.h"
#include "log.h"
#include "model_manager.h"
#include "notify.h"
#include "pipeline.h"
#include "summarize.h"
#include "util.h"
#include "version.h"

#include <libayatana-appindicator/app-indicator.h>
#include <gtk/gtk.h>

#include <whisper.h>
#ifdef RECMEET_USE_LLAMA
#include <llama.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

using namespace recmeet;

// Icon names (standard icon theme)
static const char* ICON_IDLE       = "audio-input-microphone";
static const char* ICON_RECORDING  = "media-record";

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

struct BackgroundJob {
    int id;
    std::string out_dir;  // for display
};

struct TrayState {
    AppIndicator* indicator = nullptr;

    Config cfg;
    StopToken stop;
    std::thread pipeline_thread;

    enum State { IDLE, RECORDING } state = IDLE;

    std::atomic<int> next_job_id{1};  // monotonic counter
    std::vector<BackgroundJob> bg_jobs;  // GTK main thread only

    // Cached sources
    std::vector<AudioSource> mics;
    std::vector<AudioSource> monitors;

    // Cached API models for current provider
    std::vector<std::string> cached_models;
    std::mutex models_mutex;
    bool models_fetching = false;
    std::string models_provider; // which provider the cache is for
};

static TrayState g_tray;

// Forward declarations
static void build_menu();
static void refresh_sources();
static void fetch_provider_models();

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

// --- State transitions ---

static void set_state(TrayState::State new_state) {
    g_tray.state = new_state;
    const char* icon = (new_state == TrayState::RECORDING) ? ICON_RECORDING : ICON_IDLE;
    app_indicator_set_icon_full(g_tray.indicator, icon,
        new_state == TrayState::RECORDING ? "Recording" : "Idle");
    build_menu();
}

// --- Pipeline callbacks (must be called from GTK main thread via g_idle_add) ---

struct RecordingDoneData {
    int job_id;
    std::string out_dir;
};

static gboolean on_recording_done(gpointer data) {
    auto* d = static_cast<RecordingDoneData*>(data);
    if (d) {
        g_tray.bg_jobs.push_back({d->job_id, d->out_dir});
        delete d;
    }
    set_state(TrayState::IDLE);  // calls build_menu()
    return G_SOURCE_REMOVE;
}

struct PostprocessDoneData {
    int job_id;
    bool success;
    std::string error_msg;
    std::string out_dir;
};

static gboolean on_postprocess_done(gpointer data) {
    auto* d = static_cast<PostprocessDoneData*>(data);
    auto& jobs = g_tray.bg_jobs;
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
        [&](const BackgroundJob& j) { return j.id == d->job_id; }), jobs.end());
    if (!d->success)
        notify("Processing failed", d->out_dir + ": " + d->error_msg);
    build_menu();
    delete d;
    return G_SOURCE_REMOVE;
}

// --- Recording ---

static void on_record(GtkMenuItem*, gpointer) {
    if (g_tray.state != TrayState::IDLE) return;

    // Pre-check: verify a mic can be found before flipping to RECORDING
    if (g_tray.cfg.mic_source.empty()) {
        try {
            auto detected = detect_sources(g_tray.cfg.device_pattern);
            if (detected.mic.empty()) {
                std::string msg = "No microphone source detected.\n\nAvailable sources:";
                for (const auto& s : detected.all)
                    msg += "\n  " + s.description + " (" + s.name + ")";
                auto* dialog = gtk_message_dialog_new(
                    nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                    GTK_BUTTONS_OK, "%s", msg.c_str());
                gtk_dialog_run(GTK_DIALOG(dialog));
                gtk_widget_destroy(dialog);
                refresh_sources();
                build_menu();
                return;
            }
        } catch (const std::exception& e) {
            auto* dialog = gtk_message_dialog_new(
                nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                GTK_BUTTONS_OK, "Device detection failed: %s", e.what());
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            return;
        }
    }

    set_state(TrayState::RECORDING);
    g_tray.stop.reset();

    g_tray.pipeline_thread = std::thread([] {
        try {
            // Pre-check: download whisper model if needed
            if (!is_whisper_model_cached(g_tray.cfg.whisper_model)) {
                notify("Downloading model",
                       "Whisper '" + g_tray.cfg.whisper_model + "' — please wait...");
                ensure_whisper_model(g_tray.cfg.whisper_model);
                notify("Model ready",
                       "Whisper '" + g_tray.cfg.whisper_model + "' downloaded.");
            }

            // Pre-check: validate LLM model if configured
            if (!g_tray.cfg.no_summary && !g_tray.cfg.llm_model.empty()) {
                ensure_llama_model(g_tray.cfg.llm_model);
            }

#if RECMEET_USE_SHERPA
            // Pre-check: download sherpa models if diarization enabled
            if (g_tray.cfg.diarize && !is_sherpa_model_cached()) {
                notify("Downloading models",
                       "Speaker diarization models — please wait...");
                ensure_sherpa_models();
                notify("Models ready",
                       "Speaker diarization models downloaded.");
            }
#endif

            // Phase 1: Record + transcribe (blocks until stop)
            auto on_rec_phase = [](const std::string& phase) {
                log_info("[tray] Phase: %s", phase.c_str());
            };
            auto pp_input = run_recording(g_tray.cfg, g_tray.stop, on_rec_phase);

            // Snapshot config for background work (user may change settings)
            Config cfg_copy = g_tray.cfg;
            int job_id = g_tray.next_job_id++;

            // Return tray to IDLE + register background job
            g_idle_add(on_recording_done,
                       new RecordingDoneData{job_id, pp_input.out_dir.string()});

            // Phase 2: This thread becomes the background post-processing thread
            auto* done = new PostprocessDoneData{job_id, true, "", pp_input.out_dir.string()};
            try {
                auto on_pp_phase = [](const std::string& phase) {
                    log_info("[tray] BG Phase: %s", phase.c_str());
                };
                run_postprocessing(cfg_copy, pp_input, on_pp_phase);
            } catch (const std::exception& e) {
                done->success = false;
                done->error_msg = e.what();
            }
            g_idle_add(on_postprocess_done, done);

        } catch (const std::exception& e) {
            // Recording failed — no background job to spawn
            log_error("[tray] Recording error: %s", e.what());
            notify("Recording failed", e.what());
            g_idle_add(on_recording_done, nullptr);  // nullptr = no job to register
        }
    });
    g_tray.pipeline_thread.detach();
}

static void on_stop(GtkMenuItem*, gpointer) {
    if (g_tray.state != TrayState::RECORDING) return;
    g_tray.stop.request();
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

#if RECMEET_USE_SHERPA
static void on_diarize_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.diarize = gtk_check_menu_item_get_active(item);
    save_config(g_tray.cfg);
}
#endif

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
    if (prov)
        g_tray.cfg.api_model = prov->default_model;

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

    std::thread([provider_name, base_url, fallback_key]() {
        const auto* prov = find_provider(provider_name);
        if (!prov) return;

        std::string key = resolve_api_key(*prov, fallback_key);
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

static void on_set_llm_model(GtkMenuItem*, gpointer) {
    choose_gguf_model();
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

static void on_refresh_devices(GtkMenuItem*, gpointer) {
    refresh_sources();
    build_menu();
    std::string msg = std::to_string(g_tray.mics.size()) + " mic(s), " +
                      std::to_string(g_tray.monitors.size()) + " monitor(s)";
    notify("Devices refreshed", msg);
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
        "See LICENSE-MIT and LICENSE-APACHE for full text.");
    gtk_about_dialog_set_wrap_license(GTK_ABOUT_DIALOG(dialog), TRUE);
    const char* authors[] = {"John Suykerbuyk (SykeTech LTD)", nullptr};
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);
    gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog),
        "audio-input-microphone");

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void on_quit(GtkMenuItem*, gpointer) {
    if (g_tray.state == TrayState::RECORDING) {
        g_tray.stop.request();
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!g_tray.bg_jobs.empty())
        log_warn("[tray] Quitting with %zu background job(s) still running. "
                 "Use 'recmeet --reprocess <dir>' to retry.",
                 g_tray.bg_jobs.size());
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
    bool is_idle = (g_tray.state == TrayState::IDLE);
    bool is_recording = (g_tray.state == TrayState::RECORDING);

    // --- Status label ---
    std::string status = is_recording ? "Status: Recording..." : "Status: Idle";
    if (!g_tray.bg_jobs.empty())
        status += "  (" + std::to_string(g_tray.bg_jobs.size()) + " processing)";
    auto* status_item = gtk_menu_item_new_with_label(status.c_str());
    gtk_widget_set_sensitive(status_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), status_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // --- Record / Stop ---
    if (is_idle) {
        auto* item = gtk_menu_item_new_with_label("Record");
        g_signal_connect(item, "activate", G_CALLBACK(on_record), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    } else {
        auto* item = gtk_menu_item_new_with_label("Stop Recording");
        g_signal_connect(item, "activate", G_CALLBACK(on_stop), nullptr);
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
#if RECMEET_USE_SHERPA
    {
        auto* item = gtk_check_menu_item_new_with_label("Speaker Diarization");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), g_tray.cfg.diarize);
        g_signal_connect(item, "toggled", G_CALLBACK(on_diarize_toggled), nullptr);
        if (!is_idle) gtk_widget_set_sensitive(item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
#endif

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

        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), gtk_separator_menu_item_new());

        // Chooser actions
        auto* set_out = gtk_menu_item_new_with_label("Set Output Dir...");
        g_signal_connect(set_out, "activate", G_CALLBACK(on_set_output_dir), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), set_out);

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
        auto* item = gtk_menu_item_new_with_label("Refresh Devices");
        g_signal_connect(item, "activate", G_CALLBACK(on_refresh_devices), nullptr);
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

    // Suppress vendor library log spam — the tray is a GUI app; GGML
    // debug output on stderr is never useful (and floods journald when
    // running as a systemd service).
    whisper_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);
#ifdef RECMEET_USE_LLAMA
    llama_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);
#endif

    g_tray.cfg = load_config();

    // Initialize logging
    auto log_level = parse_log_level(g_tray.cfg.log_level_str);
    log_init(log_level, g_tray.cfg.log_dir);

    // Create indicator
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    g_tray.indicator = app_indicator_new(
        "recmeet-tray", ICON_IDLE,
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    G_GNUC_END_IGNORE_DEPRECATIONS
    app_indicator_set_status(g_tray.indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(g_tray.indicator, "recmeet");

    refresh_sources();
    build_menu();
    fetch_provider_models();

    log_info("recmeet-tray %s running (%zu mic(s), %zu monitor(s))",
            RECMEET_VERSION, g_tray.mics.size(), g_tray.monitors.size());
    gtk_main();

    log_shutdown();
    notify_cleanup();
    return 0;
}
