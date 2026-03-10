// Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "config.h"
#include "config_json.h"
#include "device_enum.h"
#include "ipc_client.h"
#include "ipc_protocol.h"
#include "log.h"
#include "notify.h"
#include "summarize.h"
#include "util.h"
#include "version.h"

#include <libayatana-appindicator/app-indicator.h>
#include <gtk/gtk.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

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

    enum State { IDLE, RECORDING, REPROCESSING, POSTPROCESSING, DOWNLOADING } state = IDLE;
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

// --- Daemon connection ---

static void setup_ipc_watch();
static void teardown_ipc_watch();
static gboolean try_reconnect(gpointer);

static void set_state(TrayState::State new_state) {
    g_tray.state = new_state;
    if (new_state == TrayState::IDLE) {
        g_tray.progress_percent = -1;
        g_tray.current_phase.clear();
    }
    const char* icon = ICON_IDLE;
    const char* desc = "Idle";
    if (new_state == TrayState::RECORDING) {
        icon = ICON_RECORDING;
        desc = "Recording";
    } else if (new_state == TrayState::REPROCESSING) {
        icon = ICON_PROCESSING;
        desc = "Reprocessing";
    } else if (new_state == TrayState::POSTPROCESSING) {
        icon = ICON_PROCESSING;
        desc = "Processing";
    } else if (new_state == TrayState::DOWNLOADING) {
        icon = ICON_PROCESSING;
        desc = "Downloading";
    }
    app_indicator_set_icon_full(g_tray.indicator, icon, desc);
    build_menu();
}

static void handle_ipc_event(const IpcEvent& ev) {
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
        std::string state = json_val_as_string(ev.data.at("state"));
        auto err_it = ev.data.find("error");
        if (err_it != ev.data.end()) {
            std::string error = json_val_as_string(err_it->second);
            if (!error.empty())
                notify("Pipeline error", error);
        }
        if (state == "idle") set_state(TrayState::IDLE);
        else if (state == "recording") set_state(TrayState::RECORDING);
        else if (state == "reprocessing") set_state(TrayState::REPROCESSING);
        else if (state == "postprocessing") set_state(TrayState::POSTPROCESSING);
        else if (state == "downloading") set_state(TrayState::DOWNLOADING);
    } else if (ev.event == "job.complete") {
        std::string note = json_val_as_string(ev.data.at("note_path"));
        std::string dir = json_val_as_string(ev.data.at("output_dir"));
        if (!note.empty())
            notify("Meeting note ready", note);
        set_state(TrayState::IDLE);
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
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        log_warn("[tray] Daemon disconnected");
        g_tray.daemon_connected = false;
        teardown_ipc_watch();
        g_tray.ipc.close_connection();
        set_state(TrayState::IDLE);
        // Schedule reconnect with backoff
        g_tray.reconnect_delay = 1;
        g_tray.reconnect_timer = g_timeout_add_seconds(1, try_reconnect, nullptr);
        return G_SOURCE_REMOVE;
    }

    // Read and dispatch one round of data
    if (!g_tray.ipc.read_and_dispatch(0)) {
        log_warn("[tray] Daemon connection lost");
        g_tray.daemon_connected = false;
        teardown_ipc_watch();
        g_tray.ipc.close_connection();
        set_state(TrayState::IDLE);
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

    // Sync state
    IpcResponse resp;
    IpcError err;
    if (g_tray.ipc.call("status.get", resp, err, 2000)) {
        std::string state = json_val_as_string(resp.result["state"]);
        if (state == "recording") set_state(TrayState::RECORDING);
        else if (state == "reprocessing") set_state(TrayState::REPROCESSING);
        else if (state == "postprocessing") set_state(TrayState::POSTPROCESSING);
        else if (state == "downloading") set_state(TrayState::DOWNLOADING);
        else set_state(TrayState::IDLE);
    }

    log_info("[tray] Connected to daemon");
    return true;
}

static gboolean try_reconnect(gpointer) {
    g_tray.reconnect_timer = 0;
    if (connect_to_daemon()) {
        g_tray.reconnect_delay = 1;  // reset backoff
        build_menu();
        return G_SOURCE_REMOVE;
    }
    // Exponential backoff: 1, 2, 4, 8, 16, 30, 30, ...
    g_tray.reconnect_timer = g_timeout_add_seconds(
        g_tray.reconnect_delay, try_reconnect, nullptr);
    g_tray.reconnect_delay = std::min(g_tray.reconnect_delay * 2, 30);
    return G_SOURCE_REMOVE;
}

// --- Recording (via IPC) ---

static void on_record(GtkMenuItem*, gpointer) {
    if (g_tray.state != TrayState::IDLE) return;

    if (!g_tray.daemon_connected) {
        if (!connect_to_daemon()) {
            notify("Daemon not running",
                   "Start recmeet-daemon first, or use recmeet CLI in standalone mode.");
            return;
        }
    }

    // Resolve API key from env before sending to daemon
    if (g_tray.cfg.llm_model.empty()) {
        const auto* prov = find_provider(g_tray.cfg.provider);
        if (prov) {
            std::string key = resolve_api_key(*prov, g_tray.cfg.api_key);
            if (!key.empty()) g_tray.cfg.api_key = key;
        }
    }

    // Send record.start with current config (now includes api_key)
    JsonMap params = config_to_map(g_tray.cfg);

    IpcResponse resp;
    IpcError err;
    if (!g_tray.ipc.call("record.start", params, resp, err, 10000)) {
        std::string msg = err.message.empty() ? "Unknown error" : err.message;
        notify("Recording failed", msg);
        log_error("[tray] record.start failed: %s", msg.c_str());
        return;
    }

    set_state(g_tray.cfg.reprocess_dir.empty() ? TrayState::RECORDING : TrayState::REPROCESSING);
}

static void on_stop(GtkMenuItem*, gpointer) {
    if (g_tray.state != TrayState::RECORDING && g_tray.state != TrayState::REPROCESSING
        && g_tray.state != TrayState::POSTPROCESSING) return;

    IpcResponse resp;
    IpcError err;
    if (!g_tray.ipc.call("record.stop", resp, err, 5000)) {
        log_error("[tray] record.stop failed: %s", err.message.c_str());
        return;
    }

    // Optimistic UI update — daemon will confirm via state.changed event
    // Skip if already postprocessing (cancel case — wait for daemon to confirm idle)
    if (g_tray.state != TrayState::POSTPROCESSING)
        set_state(TrayState::POSTPROCESSING);
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
    if (g_tray.daemon_connected && g_tray.state == TrayState::IDLE) {
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

#if RECMEET_USE_SHERPA
static void on_diarize_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.diarize = gtk_check_menu_item_get_active(item);
    save_config(g_tray.cfg);
}

static void on_vad_toggled(GtkCheckMenuItem* item, gpointer) {
    g_tray.cfg.vad = gtk_check_menu_item_get_active(item);
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
    if (g_tray.state != TrayState::IDLE) return;

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
    if (g_tray.state != TrayState::IDLE && g_tray.state != TrayState::DOWNLOADING
        && g_tray.daemon_connected) {
        IpcResponse resp;
        IpcError err;
        g_tray.ipc.call("record.stop", resp, err, 2000);
    }
    teardown_ipc_watch();
    g_tray.ipc.close_connection();

    // Terminate managed web server
    if (g_tray.web_server_pid > 0) {
        log_info("[tray] stopping recmeet-web (pid %d)", g_tray.web_server_pid);
        kill(g_tray.web_server_pid, SIGTERM);
        for (int i = 0; i < 10; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            int status;
            if (waitpid(g_tray.web_server_pid, &status, WNOHANG) != 0) {
                g_tray.web_server_pid = -1;
                break;
            }
        }
        if (g_tray.web_server_pid > 0) {
            log_warn("[tray] recmeet-web did not exit, sending SIGKILL");
            kill(g_tray.web_server_pid, SIGKILL);
            waitpid(g_tray.web_server_pid, nullptr, 0);
            g_tray.web_server_pid = -1;
        }
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
    bool is_active = (g_tray.state != TrayState::IDLE && g_tray.state != TrayState::DOWNLOADING);

    // --- Status label ---
    std::string status;
    if (!g_tray.daemon_connected)
        status = "Status: Disconnected";
    else if (g_tray.state == TrayState::RECORDING)
        status = "Status: Recording...";
    else if (g_tray.state == TrayState::REPROCESSING)
        status = "Status: Reprocessing...";
    else if (g_tray.state == TrayState::POSTPROCESSING) {
        status = "Status: Processing...";
        if (g_tray.progress_percent >= 0 && !g_tray.current_phase.empty()) {
            status = "Status: " + g_tray.current_phase + "... "
                     + std::to_string(g_tray.progress_percent) + "%";
            // Capitalize first letter of phase
            if (status.size() > 8)
                status[8] = static_cast<char>(toupper(static_cast<unsigned char>(status[8])));
        }
    } else if (g_tray.state == TrayState::DOWNLOADING) {
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
    if (is_active) {
        const char* label = (g_tray.state == TrayState::RECORDING) ? "Stop Recording" : "Cancel";
        auto* item = gtk_menu_item_new_with_label(label);
        g_signal_connect(item, "activate", G_CALLBACK(on_stop), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    } else {
        auto* item = gtk_menu_item_new_with_label("Record");
        g_signal_connect(item, "activate", G_CALLBACK(on_record), nullptr);
        gtk_widget_set_sensitive(item, is_idle && g_tray.daemon_connected);
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
    {
        auto* item = gtk_check_menu_item_new_with_label("VAD Segmentation");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), g_tray.cfg.vad);
        g_signal_connect(item, "toggled", G_CALLBACK(on_vad_toggled), nullptr);
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

    log_info("recmeet-tray %s running (%zu mic(s), %zu monitor(s))",
            RECMEET_VERSION, g_tray.mics.size(), g_tray.monitors.size());
    gtk_main();

    teardown_ipc_watch();
    if (g_tray.reconnect_timer)
        g_source_remove(g_tray.reconnect_timer);

    log_shutdown();
    notify_cleanup();
    return 0;
}
