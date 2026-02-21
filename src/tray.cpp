#include "config.h"
#include "device_enum.h"
#include "notify.h"
#include "pipeline.h"
#include "util.h"
#include "version.h"

#include <libayatana-appindicator/app-indicator.h>
#include <gtk/gtk.h>

#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace recmeet;

// Icon names (standard icon theme)
static const char* ICON_IDLE       = "audio-input-microphone";
static const char* ICON_RECORDING  = "media-record";
static const char* ICON_PROCESSING = "document-edit-symbolic";

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
    StopToken stop;
    std::thread pipeline_thread;

    enum State { IDLE, RECORDING, PROCESSING } state = IDLE;

    // Cached sources
    std::vector<AudioSource> mics;
    std::vector<AudioSource> monitors;
};

static TrayState g_tray;

// Forward declarations
static void build_menu();
static void refresh_sources();

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
    const char* icon = ICON_IDLE;
    if (new_state == TrayState::RECORDING) icon = ICON_RECORDING;
    else if (new_state == TrayState::PROCESSING) icon = ICON_PROCESSING;
    app_indicator_set_icon_full(g_tray.indicator, icon,
        new_state == TrayState::RECORDING ? "Recording" :
        new_state == TrayState::PROCESSING ? "Processing" : "Idle");
    build_menu();
}

// --- Pipeline callbacks (must be called from GTK main thread via g_idle_add) ---

static gboolean on_pipeline_done(gpointer) {
    set_state(TrayState::IDLE);
    return G_SOURCE_REMOVE;
}

static gboolean on_pipeline_processing(gpointer) {
    if (g_tray.state == TrayState::RECORDING)
        set_state(TrayState::PROCESSING);
    return G_SOURCE_REMOVE;
}

// --- Recording ---

static void on_record(GtkMenuItem*, gpointer) {
    if (g_tray.state != TrayState::IDLE) return;
    set_state(TrayState::RECORDING);
    g_tray.stop.reset();

    g_tray.pipeline_thread = std::thread([] {
        try {
            auto on_phase = [](const std::string& phase) {
                fprintf(stderr, "[tray] Phase: %s\n", phase.c_str());
                if (phase == "transcribing" || phase == "summarizing")
                    g_idle_add(on_pipeline_processing, nullptr);
            };
            run_pipeline(g_tray.cfg, g_tray.stop, on_phase);
        } catch (const std::exception& e) {
            fprintf(stderr, "[tray] Pipeline error: %s\n", e.what());
            notify("Recording failed", e.what());
        }
        g_idle_add(on_pipeline_done, nullptr);
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

// --- Summary backend callbacks ---

static void choose_gguf_model();

static void on_summary_api(GtkCheckMenuItem* item, gpointer) {
    if (!gtk_check_menu_item_get_active(item)) return;
    g_tray.cfg.llm_model.clear();
    save_config(g_tray.cfg);
}

static void on_summary_local(GtkCheckMenuItem* item, gpointer) {
    if (!gtk_check_menu_item_get_active(item)) return;
    if (g_tray.cfg.llm_model.empty())
        choose_gguf_model();
    if (g_tray.cfg.llm_model.empty()) {
        // User cancelled — revert to API. Rebuild menu to reset radio state.
        build_menu();
        return;
    }
    save_config(g_tray.cfg);
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

static void on_set_obsidian_vault(GtkMenuItem*, gpointer) {
    std::string path = run_folder_chooser("Select Obsidian Vault");
    if (!path.empty()) {
        g_tray.cfg.obsidian.vault_path = path;
        g_tray.cfg.obsidian_enabled = true;
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

static void on_quit(GtkMenuItem*, gpointer) {
    if (g_tray.state == TrayState::RECORDING) {
        g_tray.stop.request();
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
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
        fprintf(stderr, "[tray] Source enumeration failed: %s\n", e.what());
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
    const char* status_text = is_idle ? "Status: Idle" :
        is_recording ? "Status: Recording..." : "Status: Processing...";
    auto* status_item = gtk_menu_item_new_with_label(status_text);
    gtk_widget_set_sensitive(status_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), status_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // --- Record / Stop ---
    if (is_idle) {
        auto* item = gtk_menu_item_new_with_label("Record");
        g_signal_connect(item, "activate", G_CALLBACK(on_record), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    } else if (is_recording) {
        auto* item = gtk_menu_item_new_with_label("Stop Recording");
        g_signal_connect(item, "activate", G_CALLBACK(on_stop), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    } else {
        auto* item = gtk_menu_item_new_with_label("Processing...");
        gtk_widget_set_sensitive(item, FALSE);
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

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // --- Summary backend submenu ---
    {
        auto* item = gtk_menu_item_new_with_label("Summary");
        auto* submenu = gtk_menu_new();
        GSList* group = nullptr;

        bool use_local = !g_tray.cfg.llm_model.empty();

        std::string api_label = "API (" + g_tray.cfg.api_model + ")";
        auto* api_radio = gtk_radio_menu_item_new_with_label(group, api_label.c_str());
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(api_radio));
        if (!use_local)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(api_radio), TRUE);
        g_signal_connect(api_radio, "toggled", G_CALLBACK(on_summary_api), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), api_radio);

        std::string local_label = "Local LLM";
        if (use_local) {
            fs::path p(g_tray.cfg.llm_model);
            local_label += " (" + p.filename().string() + ")";
        } else {
            local_label += " (not configured)";
        }
        auto* local_radio = gtk_radio_menu_item_new_with_label(group, local_label.c_str());
        if (use_local)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(local_radio), TRUE);
        g_signal_connect(local_radio, "toggled", G_CALLBACK(on_summary_local), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), local_radio);

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

        // Current obsidian vault label
        std::string vault_label = "Obsidian Vault: ";
        if (g_tray.cfg.obsidian_enabled && !g_tray.cfg.obsidian.vault_path.empty())
            vault_label += g_tray.cfg.obsidian.vault_path.string();
        else
            vault_label += "(not configured)";
        auto* vault_info = gtk_menu_item_new_with_label(vault_label.c_str());
        gtk_widget_set_sensitive(vault_info, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), vault_info);

        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), gtk_separator_menu_item_new());

        // Chooser actions
        auto* set_out = gtk_menu_item_new_with_label("Set Output Dir...");
        g_signal_connect(set_out, "activate", G_CALLBACK(on_set_output_dir), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), set_out);

        auto* set_vault = gtk_menu_item_new_with_label("Set Obsidian Vault...");
        g_signal_connect(set_vault, "activate", G_CALLBACK(on_set_obsidian_vault), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), set_vault);

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

    g_tray.cfg = load_config();

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

    fprintf(stderr, "recmeet-tray %s running (%zu mic(s), %zu monitor(s))\n",
            RECMEET_VERSION, g_tray.mics.size(), g_tray.monitors.size());
    gtk_main();

    notify_cleanup();
    return 0;
}
