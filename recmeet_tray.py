#!/usr/bin/env python3
"""recmeet_tray — System tray applet for recmeet meeting recorder."""

import os
import subprocess
import sys
import threading
from pathlib import Path

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("AyatanaAppIndicator3", "0.1")
from gi.repository import AyatanaAppIndicator3 as AppIndicator
from gi.repository import GLib, Gtk

from dotenv import load_dotenv

import recmeet
from recmeet_config import load_config, save_config, generate_initial_config

# Icon names (from standard icon theme)
ICON_IDLE = "audio-input-microphone-symbolic"
ICON_RECORDING = "media-record-symbolic"
ICON_PROCESSING = "document-edit-symbolic"

WHISPER_MODELS = ["tiny", "base", "small", "medium", "large-v3"]


class RecmeetTray:
    def __init__(self):
        load_dotenv()
        self.config = load_config()
        self.state = "idle"  # idle | recording | processing
        self.stop_event = None
        self.worker_thread = None
        self._sources_cache = None

        self.indicator = AppIndicator.Indicator.new(
            "recmeet",
            ICON_IDLE,
            AppIndicator.IndicatorCategory.APPLICATION_STATUS,
        )
        self.indicator.set_status(AppIndicator.IndicatorStatus.ACTIVE)

        self._refresh_sources()
        self._build_menu()

    # ── Source detection ──────────────────────────────────────────────

    def _refresh_sources(self):
        """Refresh the cached list of audio sources from pactl."""
        try:
            all_sources = recmeet._list_pactl_sources()
        except recmeet.DeviceError:
            all_sources = []

        self._sources_cache = {
            "mics": [s for s in all_sources if not s.endswith(".monitor")],
            "monitors": [s for s in all_sources if s.endswith(".monitor")],
        }

        # On first run, generate initial config with detected devices
        if all_sources:
            generate_initial_config(all_sources)

    # ── Menu construction ─────────────────────────────────────────────

    def _build_menu(self):
        menu = Gtk.Menu()

        # Record / Stop
        if self.state == "idle":
            item = Gtk.MenuItem(label="Record")
            item.connect("activate", self._on_record)
            menu.append(item)
        elif self.state == "recording":
            item = Gtk.MenuItem(label="Stop Recording")
            item.connect("activate", self._on_stop)
            menu.append(item)
        else:  # processing
            item = Gtk.MenuItem(label="Processing...")
            item.set_sensitive(False)
            menu.append(item)

        menu.append(Gtk.SeparatorMenuItem())

        # Mic Source submenu
        mic_menu = Gtk.Menu()
        mic_item = Gtk.MenuItem(label="Mic Source")
        mic_item.set_submenu(mic_menu)

        current_mic = self.config.get("mic_source", "")
        group = []

        auto_mic = Gtk.RadioMenuItem(label="Auto-detect")
        if not current_mic:
            auto_mic.set_active(True)
        auto_mic.connect("toggled", self._on_mic_selected, "")
        mic_menu.append(auto_mic)
        group = [auto_mic]

        for source in self._sources_cache.get("mics", []):
            item = Gtk.RadioMenuItem(label=source, group=group[0])
            if source == current_mic:
                item.set_active(True)
            item.connect("toggled", self._on_mic_selected, source)
            mic_menu.append(item)
            group.append(item)

        menu.append(mic_item)

        # Monitor Source submenu
        mon_menu = Gtk.Menu()
        mon_item = Gtk.MenuItem(label="Monitor Source")
        mon_item.set_submenu(mon_menu)

        current_mon = self.config.get("monitor_source", "")
        group = []

        auto_mon = Gtk.RadioMenuItem(label="Auto-detect")
        if not current_mon:
            auto_mon.set_active(True)
        auto_mon.connect("toggled", self._on_monitor_selected, "")
        mon_menu.append(auto_mon)
        group = [auto_mon]

        for source in self._sources_cache.get("monitors", []):
            item = Gtk.RadioMenuItem(label=source, group=group[0])
            if source == current_mon:
                item.set_active(True)
            item.connect("toggled", self._on_monitor_selected, source)
            mon_menu.append(item)
            group.append(item)

        menu.append(mon_item)

        # Model submenu
        model_menu = Gtk.Menu()
        model_item = Gtk.MenuItem(label="Model")
        model_item.set_submenu(model_menu)

        current_model = self.config.get("model", "base")
        group = []

        for i, model_name in enumerate(WHISPER_MODELS):
            if i == 0:
                item = Gtk.RadioMenuItem(label=model_name)
                group = [item]
            else:
                item = Gtk.RadioMenuItem(label=model_name, group=group[0])
            if model_name == current_model:
                item.set_active(True)
            item.connect("toggled", self._on_model_selected, model_name)
            model_menu.append(item)
            group.append(item)

        menu.append(model_item)

        menu.append(Gtk.SeparatorMenuItem())

        # Checkboxes
        mic_only = Gtk.CheckMenuItem(label="Mic Only")
        mic_only.set_active(self.config.get("mic_only", False))
        mic_only.connect("toggled", self._on_mic_only_toggled)
        menu.append(mic_only)

        no_summary = Gtk.CheckMenuItem(label="No Summary")
        no_summary.set_active(self.config.get("no_summary", False))
        no_summary.connect("toggled", self._on_no_summary_toggled)
        menu.append(no_summary)

        menu.append(Gtk.SeparatorMenuItem())

        # Edit Config
        edit_item = Gtk.MenuItem(label="Edit Config")
        edit_item.connect("activate", self._on_edit_config)
        menu.append(edit_item)

        # Refresh Devices
        refresh_item = Gtk.MenuItem(label="Refresh Devices")
        refresh_item.connect("activate", self._on_refresh_devices)
        menu.append(refresh_item)

        menu.append(Gtk.SeparatorMenuItem())

        # Quit
        quit_item = Gtk.MenuItem(label="Quit")
        quit_item.connect("activate", self._on_quit)
        menu.append(quit_item)

        menu.show_all()
        self.indicator.set_menu(menu)

    # ── State transitions ─────────────────────────────────────────────

    def _set_state(self, state):
        """Update state, icon, and rebuild menu. Must be called from GTK main thread."""
        self.state = state
        icons = {"idle": ICON_IDLE, "recording": ICON_RECORDING, "processing": ICON_PROCESSING}
        self.indicator.set_icon_full(icons[state], state)
        self._build_menu()

    # ── Recording pipeline ────────────────────────────────────────────

    def _on_record(self, _widget):
        if self.state != "idle":
            return

        self._set_state("recording")
        self.stop_event = threading.Event()
        self.worker_thread = threading.Thread(target=self._recording_pipeline, daemon=True)
        self.worker_thread.start()

    def _on_stop(self, _widget):
        if self.state == "recording" and self.stop_event:
            self.stop_event.set()

    def _recording_pipeline(self):
        """Run the full record-transcribe-summarize pipeline in a background thread."""
        try:
            # Reload config to pick up any file edits
            self.config = load_config()

            # Resolve sources
            mic_source = self.config.get("mic_source", "")
            monitor_source = self.config.get("monitor_source", "")
            mic_only = self.config.get("mic_only", False)
            pattern = self.config.get("device_pattern", recmeet.DEFAULT_DEVICE_PATTERN)

            if not mic_source or not monitor_source:
                detected = recmeet.detect_sources(pattern)
                if not mic_source:
                    mic_source = detected["mic"]
                if not monitor_source and not mic_only:
                    monitor_source = detected["monitor"]

            if not mic_source:
                GLib.idle_add(self._pipeline_error, "No mic source found.")
                return

            if mic_only:
                monitor_source = None

            # Resolve API key
            api_key = self.config.get("api_key") or os.getenv("XAI_API_KEY")
            no_summary = self.config.get("no_summary", False)

            # Create output directory
            output_dir = self.config.get("output_dir", "./meetings")
            out_dir = recmeet.create_output_dir(output_dir)

            def on_phase(phase):
                if phase == "transcribing":
                    GLib.idle_add(self._set_state, "processing")
                elif phase == "complete":
                    GLib.idle_add(self._set_state, "idle")

            result = recmeet.run_pipeline(
                out_dir=out_dir,
                mic_source=mic_source,
                monitor_source=monitor_source,
                model_name=self.config.get("model", "base"),
                api_key=api_key,
                no_summary=no_summary,
                stop_event=self.stop_event,
                on_phase=on_phase,
            )

            GLib.idle_add(self._pipeline_done, result)

        except Exception as e:
            GLib.idle_add(self._pipeline_error, str(e))

    def _pipeline_done(self, result):
        self._set_state("idle")
        recmeet.notify("Recording complete", str(result["out_dir"]))

    def _pipeline_error(self, message):
        self._set_state("idle")
        recmeet.notify("Recording failed", message)
        print(f"Error: {message}", file=sys.stderr)

    # ── Menu callbacks ────────────────────────────────────────────────

    def _on_mic_selected(self, widget, source):
        if widget.get_active():
            self.config["mic_source"] = source
            save_config(self.config)

    def _on_monitor_selected(self, widget, source):
        if widget.get_active():
            self.config["monitor_source"] = source
            save_config(self.config)

    def _on_model_selected(self, widget, model_name):
        if widget.get_active():
            self.config["model"] = model_name
            save_config(self.config)

    def _on_mic_only_toggled(self, widget):
        self.config["mic_only"] = widget.get_active()
        save_config(self.config)

    def _on_no_summary_toggled(self, widget):
        self.config["no_summary"] = widget.get_active()
        save_config(self.config)

    def _on_edit_config(self, _widget):
        from recmeet_config import CONFIG_PATH

        # Ensure config exists
        if not CONFIG_PATH.exists():
            self._refresh_sources()
            all_sources = (
                self._sources_cache.get("mics", [])
                + self._sources_cache.get("monitors", [])
            )
            generate_initial_config(all_sources)

        editor = os.environ.get("EDITOR", "nvim")
        terminal = os.environ.get("TERMINAL", "foot")
        try:
            subprocess.Popen([terminal, "-e", editor, str(CONFIG_PATH)])
        except FileNotFoundError:
            # Fallback to xdg-open
            try:
                subprocess.Popen(["xdg-open", str(CONFIG_PATH)])
            except FileNotFoundError:
                recmeet.notify("Cannot open config", f"Edit manually: {CONFIG_PATH}")

    def _on_refresh_devices(self, _widget):
        self._refresh_sources()
        self._build_menu()
        n_mics = len(self._sources_cache.get("mics", []))
        n_mons = len(self._sources_cache.get("monitors", []))
        recmeet.notify("Devices refreshed", f"{n_mics} mic(s), {n_mons} monitor(s)")

    def _on_quit(self, _widget):
        if self.state == "recording" and self.stop_event:
            self.stop_event.set()
            if self.worker_thread:
                self.worker_thread.join(timeout=10)
        Gtk.main_quit()


def main():
    tray = RecmeetTray()  # noqa: F841 — prevent GC of the tray object
    Gtk.main()


if __name__ == "__main__":
    main()
