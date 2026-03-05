# Quick Start Guide

Step-by-step guide to installing, configuring, and using recmeet.

## 1. Install dependencies

### Arch Linux

```bash
sudo pacman -S pipewire libpulse libsndfile curl libnotify \
               libayatana-appindicator gtk3 cmake ninja gcc onnxruntime-cpu
```

### Debian / Ubuntu

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
    libpipewire-0.3-dev libpulse-dev libsndfile1-dev libcurl4-openssl-dev \
    libnotify-dev libayatana-appindicator3-dev libgtk-3-dev
```

### Fedora / RHEL

```bash
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \
    pipewire-devel pulseaudio-libs-devel libsndfile-devel libcurl-devel \
    libnotify-devel libayatana-appindicator-gtk3-devel gtk3-devel
```

## 2. Build

```bash
git clone --recurse-submodules https://github.com/syketech/recmeet.git
cd recmeet
make build
```

This produces four binaries in `build/`:

| Binary | Purpose |
|--------|---------|
| `recmeet` | CLI tool (standalone or daemon client) |
| `recmeet-daemon` | Background daemon (owns pipeline logic) |
| `recmeet-tray` | System tray applet (GTK3, daemon client) |
| `recmeet_tests` | Test suite |

## 3. First recording (standalone, no setup needed)

The simplest way to verify everything works — no daemon, no API key:

```bash
./build/recmeet --no-daemon --mic-only --no-summary --no-diarize --model tiny
# Speak into your mic, then press Ctrl+C to stop
```

Output appears in `meetings/<timestamp>/`.

## 4. Set up the daemon

The daemon is the recommended way to run recmeet. It manages the full pipeline in the background while the CLI and tray act as lightweight clients.

### Start manually

```bash
./build/recmeet-daemon &
```

### Or install and use systemd

```bash
make install                                        # installs to ~/.local, downloads models, enables daemon
```

`make install` is turnkey — it downloads default models (whisper, sherpa, VAD), enables the daemon via systemd, and starts it. No further setup needed.

### Verify it's running

```bash
recmeet --status
# Output: Daemon: running / State: idle
```

## 5. Configure summarization

recmeet supports three cloud API providers (xAI, OpenAI, Anthropic) and local LLM summarization. Pick one.

### Cloud API (easiest)

Set an API key in your environment. recmeet checks provider-specific variables in order:

```bash
# xAI (default provider)
export XAI_API_KEY=your-key-here

# Or OpenAI
export OPENAI_API_KEY=your-key-here

# Or Anthropic
export ANTHROPIC_API_KEY=your-key-here
```

To switch providers:

```bash
recmeet --provider openai --model base
```

### Local LLM (fully offline)

Download a GGUF model and point recmeet at it:

```bash
mkdir -p ~/.local/share/recmeet/models/llama
curl -L -o ~/.local/share/recmeet/models/llama/Qwen2.5-7B-Instruct-Q4_K_M.gguf \
    https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/resolve/main/qwen2.5-7b-instruct-q4_k_m.gguf

recmeet --llm-model ~/.local/share/recmeet/models/llama/Qwen2.5-7B-Instruct-Q4_K_M.gguf
```

### No summarization

```bash
recmeet --no-summary
```

## 6. Record a meeting

### Via CLI (with daemon running)

```bash
recmeet --model base
# Recording starts. Press Ctrl+C to stop.
# Phases print as they happen: recording → transcribing → diarizing → summarizing → complete
```

The CLI auto-detects the daemon. If the daemon isn't running, it falls back to standalone mode.

### Via tray applet

```bash
recmeet-tray                          # or: systemctl --user start recmeet-tray.service
```

Click the tray icon, select **Record** to start, **Stop Recording** to finish. The tray connects to the daemon and shows status updates in real time.

### Force standalone mode (no daemon)

```bash
recmeet --no-daemon --model base
```

## 7. View output

Each recording creates a directory under `meetings/`:

```
meetings/2026-03-05_10-30/
  audio.wav                                    # Mixed audio (16kHz mono)
  Meeting_2026-03-05_10-30_Sprint_Review.md    # Meeting note
```

The meeting note contains:
- YAML frontmatter (date, participants, tags, duration)
- Summary with action items
- Full timestamped transcript with speaker labels

## 8. Configuration file

All CLI options can be set in `~/.config/recmeet/config.yaml`:

```yaml
audio:
  mic_only: false

transcription:
  model: base
  language: ""           # empty = auto-detect

diarization:
  enabled: true
  num_speakers: 0        # 0 = auto-detect
  cluster_threshold: 1.18

summary:
  provider: xai
  model: grok-3
  # disabled: true       # uncomment to skip summarization

output:
  directory: ./meetings

notes:
  domain: general        # used in meeting note frontmatter

logging:
  level: none            # none, error, warn, info

general:
  threads: 0             # 0 = auto (cores - 1)
```

CLI flags override config file values. The daemon reloads config on `SIGHUP`:

```bash
kill -HUP $(pidof recmeet-daemon)
```

## 9. Daemon management

### Makefile shortcuts

```bash
make daemon-start        # build + start daemon in background
make daemon-stop         # stop running daemon
make daemon-status       # query daemon status
```

### CLI commands

```bash
recmeet --status         # show daemon state (idle/recording/postprocessing)
recmeet --stop           # stop a recording in progress
recmeet --daemon         # force client mode (fail if no daemon)
recmeet --no-daemon      # force standalone mode (ignore daemon)
```

### systemd

```bash
# Daemon only
systemctl --user enable --now recmeet-daemon.service

# Daemon + tray (tray automatically starts daemon)
systemctl --user enable --now recmeet-tray.service

# Socket activation (daemon starts on first connection)
systemctl --user enable --now recmeet-daemon.socket

# Logs
journalctl --user -u recmeet-daemon.service -f
```

## 10. Model management

Models (whisper, sherpa-onnx, VAD) are auto-downloaded on first use. You can also manage them explicitly.

### Pre-download all models

```bash
recmeet --download-models
```

This downloads whisper (for the configured model size), sherpa-onnx (segmentation + embedding), and VAD models. Skips any that are already cached.

### Update cached models

```bash
recmeet --update-models
```

Re-downloads all currently cached models to get the latest versions.

### Via the tray

The tray auto-downloads models when you select a new whisper model from the menu. Use the **Update Models** menu item to re-download all cached models.

## 11. Common workflows

### Record with specific audio sources

```bash
# List available sources
recmeet --list-sources

# Use specific mic and monitor
recmeet --source alsa_input.usb-Blue_Yeti --monitor alsa_output.pci-0000.monitor
```

### Reprocess an old recording

```bash
recmeet --reprocess meetings/2026-02-21_17-34/ --num-speakers 2
```

### Mic-only recording (no speaker output)

```bash
recmeet --mic-only
```

### Tune speaker detection

```bash
# Lower threshold = more speakers detected
recmeet --cluster-threshold 0.9

# Force a known number of speakers
recmeet --num-speakers 3
```

## Troubleshooting

**"Daemon: not running"** — Start the daemon: `recmeet-daemon &` or `systemctl --user start recmeet-daemon.service`

**"Another recmeet-daemon is already running"** — A daemon is already active. Use `recmeet --status` to check, or `pkill recmeet-daemon` to kill a stale instance.

**No audio captured** — Verify PipeWire is running: `pw-cli info`. List sources with `recmeet --list-sources` and check that your mic appears.

**Whisper model not found** — Models are auto-downloaded on first use. If behind a proxy, download manually (see [BUILD.md](BUILD.md)).

**Tray shows "Disconnected"** — The tray reconnects automatically when the daemon starts. Check `systemctl --user status recmeet-daemon.service`.

**Summary skipped / no API key warning** — Set your API key in the environment (`XAI_API_KEY`, `OPENAI_API_KEY`, or `ANTHROPIC_API_KEY`), or use `--llm-model` for local summarization, or `--no-summary` to suppress the warning.
