# recmeet

A local-first meeting recorder for Linux. Records audio from any source (Zoom, Teams, Google Meet, VoIP, phone calls), transcribes with [whisper.cpp](https://github.com/ggerganov/whisper.cpp), identifies speakers, and summarizes into structured Markdown notes — all on your own hardware. No cloud accounts, no subscriptions, no bots joining your meetings, no one else touching your data.

## Why this exists

Every commercial meeting transcription service — Otter, Fireflies, and the rest — demands access to your calendar, inserts a bot into every conversation, hosts your transcripts on their cloud, and charges a monthly fee to retain access to _your own data_.

Unacceptable.

recmeet was designed as a Python prototype in under two hours using local open-source models, proving that a single Linux machine can record, transcribe, and summarize any meeting without sending a byte off-box. It was then rewritten as a single C++ binary to eliminate the Python/pip/venv dependency chain entirely — one static binary, zero runtime dependencies, runs on any Linux box with PipeWire.

The result: full transcriptions with speaker labels, professionally structured summaries with action items, output as standard Markdown files on your local filesystem. No subscriptions. No cloud uploads. No one between you and your data.

## What it does

- **Records** mic input and speaker output simultaneously via PipeWire/PulseAudio — captures both sides of any conversation regardless of platform
- **Transcribes** with whisper.cpp (tiny through large-v3 models, auto-downloaded on first use)
- **Identifies speakers** via sherpa-onnx diarization (Pyannote segmentation + 3D-Speaker embeddings), with cross-session speaker identification — enroll voices once, get real names in every transcript
- **Summarizes** via cloud API (xAI/OpenAI/Anthropic — all OpenAI-compatible) or a local GGUF model via llama.cpp — your choice
- **Outputs** timestamped transcripts, structured summaries, and meeting notes with YAML frontmatter (Obsidian-compatible)
- **System tray applet** for point-and-click control from swaybar or any system tray
- **Daemon architecture** — a background daemon owns all pipeline logic; the CLI and tray are thin IPC clients

## Quick start

### Dependencies (Arch Linux)

```bash
sudo pacman -S pipewire libpulse libsndfile curl libnotify \
               libayatana-appindicator gtk3 onnxruntime-cpu cmake ninja gcc
```

### Dependencies (Debian / Ubuntu)

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
    libpipewire-0.3-dev libpulse-dev libsndfile1-dev libcurl4-openssl-dev \
    libnotify-dev libayatana-appindicator3-dev libgtk-3-dev
```

### Dependencies (Fedora / RHEL)

```bash
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \
    pipewire-devel pulseaudio-libs-devel libsndfile-devel libcurl-devel \
    libnotify-devel libayatana-appindicator-gtk3-devel gtk3-devel
```

### Build

```bash
git clone --recurse-submodules https://github.com/syketech/recmeet.git
cd recmeet
make

# Or manually:
# cmake -B build -G Ninja && ninja -C build
```

### Run

```bash
# Quick test — standalone mode, mic only, no summary, no API key needed
./build/recmeet --no-daemon --mic-only --no-summary --no-diarize --model tiny

# Full pipeline with cloud summarization (standalone)
export XAI_API_KEY=your-key-here
./build/recmeet --model base

# Or use the daemon for persistent background operation
./build/recmeet-daemon &              # start the daemon
./build/recmeet --model base          # CLI auto-detects daemon and uses it
./build/recmeet --status              # check daemon state
./build/recmeet --stop                # stop a recording in progress

# System tray applet (connects to daemon automatically)
./build/recmeet-tray

# Full pipeline with local LLM (no API key, no network)
./build/recmeet --model base \
    --llm-model ~/.local/share/recmeet/models/llama/Qwen2.5-7B-Instruct-Q4_K_M.gguf

# Reprocess an old recording with speaker diarization
./build/recmeet --reprocess meetings/2026-02-21_17-34/ --num-speakers 2

# List available audio sources
./build/recmeet --list-sources
```

See [QUICKSTART.md](QUICKSTART.md) for a step-by-step installation and usage guide.

## How it works

```
Record ──► Transcribe ──► Diarize ──► Identify ──► Summarize ──► Output
(PipeWire)  (whisper.cpp)  (sherpa-onnx) (voiceprint DB) (API/llama.cpp) (Markdown)
```

1. **Record**: PipeWire captures the mic via `pw_stream`; PulseAudio's `pa_simple` captures the speaker monitor (`.monitor` sources are a PulseAudio abstraction that PipeWire doesn't reliably handle, especially over Bluetooth). Both streams are mixed in-process into a single WAV.

2. **Transcribe**: whisper.cpp runs locally on CPU, producing timestamped segments. Models are GGUF format, auto-downloaded from Hugging Face on first use (141 MB for `base`, up to 1.5 GB for `large-v3`).

3. **Diarize** (optional, on by default): sherpa-onnx labels each segment with `Speaker_01`, `Speaker_02`, etc. using neural speaker embeddings and clustering. Configurable threshold for tuning speaker count detection.

4. **Identify** (optional, on by default when speakers are enrolled): Matches diarization clusters against enrolled voiceprints using cosine similarity on 3D-Speaker embeddings. Enrolled speakers get their real names (`John`, `Alice`) instead of generic labels. The speaker database lives at `~/.local/share/recmeet/speakers/` — one JSON file per person containing their averaged embedding vectors. Enroll speakers from past recordings with `recmeet --enroll "Name" --from meetings/DIR/`.

5. **Summarize**: Either a cloud API call (xAI, OpenAI, or Anthropic — all use the same OpenAI-compatible endpoint) or a local GGUF model via llama.cpp. Same structured prompt for both paths. Dynamic context sizing with token-level truncation for long meetings.

6. **Output**: Timestamped transcript, structured Markdown summary with action items, and a meeting note with YAML frontmatter (Obsidian/Dataview-compatible).

## Output

### Directory structure

```
meetings/2026-02-20_14-30/
  audio.wav        # Mixed mic + monitor (16kHz mono S16LE)
  Meeting_2026-02-20_14-30_Project_Kickoff.md  # Meeting note
```

Two files per meeting. The meeting note contains the transcript, summary, and action items — no separate `transcript.txt` or `summary.md`. Source WAVs (`mic.wav`, `monitor.wav`) are deleted after mixing; use `--keep-sources` to retain them.

### Transcript format

Embedded in the meeting note as a foldable section:

```
[00:00 - 00:05] John: Hello everyone, thanks for joining.
[00:05 - 00:12] Alice: Thanks. Let's start with the status update.
[00:12 - 00:30] John: Sure. We shipped the new auth flow yesterday.
```

When speakers are enrolled via `--enroll`, their real names replace generic `Speaker_XX` labels. Unenrolled speakers still appear as `Speaker_01`, `Speaker_02`, etc.

### Meeting note

Filename includes an AI-derived title (e.g. `Meeting_2026-02-20_14-30_Project_Kickoff.md`). Contains YAML frontmatter with enriched metadata (date, time, type, domain, status, tags, participants, duration), a summary callout, action item checkboxes, and a foldable transcript section. Compatible with Obsidian Dataview.

## CLI reference

### recmeet (CLI)

```
Usage: recmeet [OPTIONS]

Record, transcribe, and summarize meetings.

Options:
  --source NAME        PipeWire/PulseAudio mic source (auto-detect if omitted)
  --monitor NAME       Monitor/speaker source (auto-detect if omitted)
  --mic-only           Record mic only (skip monitor capture)
  --keep-sources       Keep separate mic.wav and monitor.wav after mixing
  --model NAME         Whisper model: tiny/base/small/medium/large-v3 (default: base)
  --language CODE      Force whisper language (e.g. en, de, ja; default: auto-detect)
  --output-dir DIR     Base directory for outputs (default: ./meetings)
  --note-dir DIR       Directory for meeting notes (default: same as audio)
  --provider NAME      API provider: xai, openai, anthropic (default: xai)
  --api-key KEY        API key (default: from provider env var or config)
  --api-url URL        API endpoint override (default: derived from provider)
  --api-model NAME     API model name (default: provider's default model)
  --no-summary         Skip summarization (record + transcribe only)
  --device-pattern RE  Regex for device auto-detection
  --context-file PATH  Pre-meeting notes to include in summary prompt
  --llm-model PATH     Local GGUF model for summarization (instead of API)
  --no-diarize         Disable speaker diarization
  --num-speakers N     Number of speakers (0 = auto-detect, default: 0)
  --cluster-threshold F  Clustering distance threshold (default: 1.18, higher = fewer speakers)
  --no-speaker-id      Disable speaker identification (voiceprint matching)
  --speaker-threshold F  Speaker identification similarity threshold (default: 0.6)
  --speaker-db DIR     Speaker database directory (default: ~/.local/share/recmeet/speakers/)
  --enroll NAME        Enroll a speaker from an existing recording (use with --from)
  --from DIR           Meeting directory containing audio.wav for enrollment
  --speaker N          Speaker number to enroll (1-based; omit for interactive prompt)
  --speakers           List enrolled speakers and exit
  --remove-speaker NAME  Remove an enrolled speaker and exit
  --identify DIR       Identify speakers in a recording (dry-run) and exit
  --no-vad             Disable VAD segmentation (transcribe full audio)
  --vad-threshold F    VAD speech detection threshold (default: 0.5)
  --threads N          Number of CPU threads for inference (0 = auto-detect, default: 0)
  --reprocess DIR      Reprocess existing recording from audio.wav
  --log-level LEVEL    Log level: none, error, warn, info (default: none)
  --log-dir DIR        Log file directory (default: ~/.local/share/recmeet/logs/)
  --list-sources       List available audio sources and exit
  --download-models    Download required models and exit
  --update-models      Re-download all cached models and exit
  --daemon             Force client mode (require running daemon)
  --no-daemon          Force standalone mode (skip daemon detection)
  --status             Query daemon status and exit
  --stop               Stop daemon recording and exit
  -h, --help           Show this help
  -v, --version        Show version
```

### recmeet-daemon

```
Usage: recmeet-daemon [OPTIONS]

Run the recmeet daemon (IPC server for CLI and tray clients).

Options:
  --socket PATH     Unix socket path (default: $XDG_RUNTIME_DIR/recmeet/daemon.sock)
  --log-level LEVEL Log level: none, error, warn, info (default: info)
  --log-dir DIR     Log file directory
  -h, --help        Show this help
  -v, --version     Show version
```

## Configuration

`~/.config/recmeet/config.yaml` — all fields are optional, CLI flags override.

```yaml
audio:
  # mic_source: ""       # PipeWire/PulseAudio mic (auto-detect if omitted)
  # monitor_source: ""   # monitor/speaker source (auto-detect if omitted)

transcription:
  model: base
  language: "" # empty = auto-detect

diarization:
  enabled: true
  num_speakers: 0 # 0 = auto-detect
  cluster_threshold: 1.18

speaker_id:
  enabled: true            # auto-enabled when speakers are enrolled
  threshold: 0.6           # cosine similarity threshold (higher = stricter)
  # database: ~/.local/share/recmeet/speakers/

vad:
  enabled: true
  threshold: 0.5
  min_silence: 0.5      # seconds of silence to end a speech segment
  min_speech: 0.25      # minimum speech duration (seconds)
  max_speech: 30.0      # maximum speech segment length (seconds)

summary:
  provider: xai
  model: grok-3
  # api_key: ...         # prefer env vars instead

output:
  directory: ./meetings

notes:
  domain: general

logging:
  level: none # none, error, warn, info
  # directory: ~/.local/share/recmeet/logs/

general:
  threads: 0 # 0 = auto-detect (cores - 1)
```

## Build options

| Option                | Default | Effect                                                      |
| --------------------- | ------- | ----------------------------------------------------------- |
| `RECMEET_USE_SHERPA`  | ON      | Enable speaker diarization (fetches sherpa-onnx via CMake)  |
| `RECMEET_USE_LLAMA`   | ON      | Enable local LLM summarization via llama.cpp                |
| `RECMEET_USE_NOTIFY`  | ON      | Enable desktop notifications via libnotify                  |
| `RECMEET_BUILD_TRAY`  | ON      | Build the system tray applet (requires GTK3 + AppIndicator) |
| `RECMEET_BUILD_TESTS` | ON      | Build the Catch2 test suite                                 |

```bash
# Example: headless build without tray or diarization
make RECMEET_BUILD_TRAY=OFF RECMEET_USE_SHERPA=OFF

# Or manually:
# cmake -B build -G Ninja -DRECMEET_BUILD_TRAY=OFF -DRECMEET_USE_SHERPA=OFF
```

## Testing

213 unit tests, 831 assertions across 23 modules, plus integration and benchmark suites.

```bash
make test                # unit tests (no hardware needed)
make benchmark           # benchmark tests (needs whisper models + assets/)

# Or run directly for more control:
./build/recmeet_tests "~[integration]~[benchmark]"   # unit tests
./build/recmeet_tests "[integration]"                 # needs running PipeWire session
./build/recmeet_tests "[benchmark]"                   # needs whisper models + assets/
./build/recmeet_tests "[cli]"                         # single module
```

## Installing

```bash
make install                                         # install to ~/.local (default, no sudo)
sudo make install PREFIX=/usr/local                  # or system-wide
make install PREFIX=/tmp/test-install                # or a custom prefix

# Or manually:
# cmake --install build --prefix ~/.local
# sudo cmake --install build
```

`make install` also downloads default models (whisper, sherpa, VAD) and enables the daemon via systemd. Use `make uninstall` to reverse everything.

### Packages

```bash
make package-arch       # Arch Linux (PKGBUILD)
make package-deb        # Debian/Ubuntu (.deb via CPack)
make package-rpm        # Fedora/RHEL (.rpm via CPack)
```

See [BUILD.md](BUILD.md) for packaging details and per-distro build dependencies.

### Autostart via systemd

The daemon and tray each have a systemd user service. The tray service depends on the daemon — enabling the tray automatically pulls in the daemon.

```bash
# Daemon only (headless / CLI usage)
systemctl --user enable --now recmeet-daemon.service

# Daemon + tray (desktop usage — daemon starts automatically)
systemctl --user enable --now recmeet-tray.service

# Check status
systemctl --user status recmeet-daemon.service
systemctl --user status recmeet-tray.service

# Stop and disable
systemctl --user disable --now recmeet-tray.service
systemctl --user disable --now recmeet-daemon.service
```

Socket activation is also available — `recmeet-daemon.socket` starts the daemon on first connection:

```bash
systemctl --user enable --now recmeet-daemon.socket
```

The tray service is tied to `graphical-session.target` and restarts automatically on crash. On Sway, your config must activate that target — see [BUILD.md](BUILD.md) for details.

## Project history

recmeet started as a Python prototype that proved the pipeline in a single session: `pw-record` and `parecord` for capture, `faster-whisper` for transcription, the `requests` library for Grok API calls. Within two hours the core concept was validated — a local Linux box could record, transcribe, and summarize any meeting without cloud dependencies.

The prototype also surfaced the key technical insight that shaped the architecture: `.monitor` sources (how PulseAudio exposes speaker output for recording) don't work with PipeWire's native `pw-record --target`. They produce silence. `parecord --device` works. This dual-capture strategy — PipeWire for the mic, PulseAudio for the monitor — carried through into the C++ rewrite.

The rewrite to C++ was motivated by deployment simplicity. The Python version required a virtualenv, pip-installed packages, system-site-packages for GTK bindings, and careful dependency management across machines. The C++ version compiles to a single static binary with whisper.cpp and llama.cpp linked in. No runtime dependencies beyond the system libraries that any desktop Linux already has. Copy the binary, run it.

From there, the project evolved through extensive iteration: doubling test coverage and extracting testable modules, adding a full-featured system tray applet, model pre-download UX, fixing real bugs found during live end-to-end testing (whisper's `detect_language` trap, llama.cpp's chat template requirement), benchmark suites against reference audio, multi-provider API support, speaker diarization via sherpa-onnx, packaging for Arch/Debian/Fedora, and hardening passes on error handling, context overflow, and clustering threshold tuning.

## Architecture

Four binaries share a common static library. The daemon owns all pipeline logic (audio capture, transcription, diarization, summarization). The CLI and tray are thin IPC clients that communicate via a Unix domain socket using newline-delimited JSON.

```
recmeet_core  (static library — 23 modules)
    |
    +-- recmeet-daemon  (daemon — pipeline + IPC server)
    +-- recmeet         (CLI — dual-mode: IPC client or standalone)
    +-- recmeet-tray    (system tray — IPC client, GTK3 + AppIndicator)
    +-- recmeet_tests   (Catch2 test suite)
```

### Daemon mode (recommended)

```
recmeet-daemon              recmeet (CLI)           recmeet-tray
      |                         |                        |
  [pipeline]  <── IPC ──>  [thin client]  <── IPC ──>  [thin client]
  [IPC server]             [auto-detect]              [GTK + g_io_watch]
      |
  Unix socket: $XDG_RUNTIME_DIR/recmeet/daemon.sock
```

The CLI auto-detects a running daemon and operates as a client. Use `--no-daemon` to force standalone mode (the CLI runs the full pipeline directly, as in previous versions). The tray connects to the daemon and reconnects automatically with exponential backoff if the daemon restarts.

### IPC protocol

- **Transport**: Unix domain socket
- **Wire format**: Newline-delimited JSON (NDJSON)
- **Methods**: `record.start`, `record.stop`, `status.get`, `config.update`, `config.reload`, `sources.list`, `models.list`, `models.ensure`, `models.update`
- **Events** (server push): `phase`, `state.changed`, `job.complete`, `model.downloading`

See [BUILD.md](BUILD.md) for a detailed build system tutorial.

## License

Dual-licensed under [MIT and Apache 2.0](LICENSE), at your option.

Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
