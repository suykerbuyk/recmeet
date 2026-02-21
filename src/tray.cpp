#include "config.h"
#include "notify.h"
#include "pipeline.h"
#include "util.h"
#include "version.h"

#include <libayatana-appindicator/app-indicator.h>
#include <gtk/gtk.h>

#include <cstdio>
#include <memory>
#include <thread>

using namespace recmeet;

struct TrayState {
    AppIndicator* indicator = nullptr;
    GtkWidget* menu = nullptr;
    GtkWidget* item_record = nullptr;
    GtkWidget* item_stop = nullptr;
    GtkWidget* item_status = nullptr;
    GtkWidget* item_quit = nullptr;

    Config cfg;
    StopToken stop;
    std::thread pipeline_thread;
    bool recording = false;
};

static TrayState g_tray;

static void update_menu_state() {
    gtk_widget_set_sensitive(g_tray.item_record, !g_tray.recording);
    gtk_widget_set_sensitive(g_tray.item_stop, g_tray.recording);

    if (g_tray.recording) {
        app_indicator_set_icon_full(g_tray.indicator, "media-record", "Recording");
        gtk_menu_item_set_label(GTK_MENU_ITEM(g_tray.item_status), "Status: Recording...");
    } else {
        app_indicator_set_icon_full(g_tray.indicator, "audio-input-microphone", "Idle");
        gtk_menu_item_set_label(GTK_MENU_ITEM(g_tray.item_status), "Status: Idle");
    }
}

static gboolean on_pipeline_done(gpointer) {
    g_tray.recording = false;
    update_menu_state();
    return G_SOURCE_REMOVE;
}

static void on_record(GtkMenuItem*, gpointer) {
    if (g_tray.recording) return;
    g_tray.recording = true;
    g_tray.stop.reset();
    update_menu_state();

    // Reload config each time
    g_tray.cfg = load_config();

    g_tray.pipeline_thread = std::thread([] {
        try {
            auto on_phase = [](const std::string& phase) {
                fprintf(stderr, "[tray] Phase: %s\n", phase.c_str());
            };
            run_pipeline(g_tray.cfg, g_tray.stop, on_phase);
        } catch (const std::exception& e) {
            fprintf(stderr, "[tray] Pipeline error: %s\n", e.what());
            notify("Recording failed", e.what());
        }
        // Signal GTK main loop that we're done
        g_idle_add(on_pipeline_done, nullptr);
    });
    g_tray.pipeline_thread.detach();
}

static void on_stop(GtkMenuItem*, gpointer) {
    if (!g_tray.recording) return;
    g_tray.stop.request();
}

static void on_quit(GtkMenuItem*, gpointer) {
    if (g_tray.recording) {
        g_tray.stop.request();
        // Give pipeline a moment to finish
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    gtk_main_quit();
}

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);
    notify_init();

    g_tray.cfg = load_config();

    // Create indicator
    // All app_indicator constructors are marked deprecated in ayatana 0.5.x
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    g_tray.indicator = app_indicator_new(
        "recmeet-tray", "audio-input-microphone",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    G_GNUC_END_IGNORE_DEPRECATIONS
    app_indicator_set_status(g_tray.indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(g_tray.indicator, "recmeet");

    // Build menu
    g_tray.menu = gtk_menu_new();

    g_tray.item_status = gtk_menu_item_new_with_label("Status: Idle");
    gtk_widget_set_sensitive(g_tray.item_status, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_tray.menu), g_tray.item_status);

    gtk_menu_shell_append(GTK_MENU_SHELL(g_tray.menu), gtk_separator_menu_item_new());

    g_tray.item_record = gtk_menu_item_new_with_label("Record");
    g_signal_connect(g_tray.item_record, "activate", G_CALLBACK(on_record), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_tray.menu), g_tray.item_record);

    g_tray.item_stop = gtk_menu_item_new_with_label("Stop");
    g_signal_connect(g_tray.item_stop, "activate", G_CALLBACK(on_stop), nullptr);
    gtk_widget_set_sensitive(g_tray.item_stop, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_tray.menu), g_tray.item_stop);

    gtk_menu_shell_append(GTK_MENU_SHELL(g_tray.menu), gtk_separator_menu_item_new());

    g_tray.item_quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(g_tray.item_quit, "activate", G_CALLBACK(on_quit), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(g_tray.menu), g_tray.item_quit);

    gtk_widget_show_all(g_tray.menu);
    app_indicator_set_menu(g_tray.indicator, GTK_MENU(g_tray.menu));

    fprintf(stderr, "recmeet-tray %s running\n", RECMEET_VERSION);
    gtk_main();

    notify_cleanup();
    return 0;
}
