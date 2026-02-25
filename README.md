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
- **Identifies speakers** via sherpa-onnx diarization (Pyannote segmentation + 3D-Speaker embeddings)
- **Summarizes** via cloud API (xAI/OpenAI/Anthropic — all OpenAI-compatible) or a local GGUF model via llama.cpp — your choice
- **Outputs** timestamped transcripts, structured summaries, and meeting notes with YAML frontmatter (Obsidian-compatible)
- **System tray applet** for point-and-click control from swaybar or any system tray

## Quick start

### Dependencies (Arch Linux)

```bash
sudo pacman -S pipewire libpulse libsndfile curl libnotify \
               libayatana-appindicator gtk3 cmake ninja gcc
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

cmake -B build -G Ninja
ninja -C build
# Speaker diarization is ON by default. To disable:
# cmake -B build -G Ninja -DRECMEET_USE_SHERPA=OFF
```

### Run

```bash
# Quick test — mic only, no summary, no API key needed
./build/recmeet --mic-only --no-summary --no-diarize --model tiny

# Full pipeline with cloud summarization
export XAI_API_KEY=your-key-here
./build/recmeet --model base

# Full pipeline with local LLM (no API key, no network)
./build/recmeet --model base \
    --llm-model ~/.local/share/recmeet/models/llama/Qwen2.5-7B-Instruct-Q4_K_M.gguf

# System tray applet
./build/recmeet-tray

# Reprocess an old recording with speaker diarization
./build/recmeet --reprocess meetings/2026-02-21_17-34/ --num-speakers 2

# List available audio sources
./build/recmeet --list-sources
```

## How it works

```
Record ──► Transcribe ──► Diarize ──► Summarize ──► Output
(PipeWire)  (whisper.cpp)  (sherpa-onnx) (API/llama.cpp) (Markdown/Obsidian)
```

1. **Record**: PipeWire captures the mic via `pw_stream`; PulseAudio's `pa_simple` captures the speaker monitor (`.monitor` sources are a PulseAudio abstraction that PipeWire doesn't reliably handle, especially over Bluetooth). Both streams are mixed in-process into a single WAV.

2. **Transcribe**: whisper.cpp runs locally on CPU, producing timestamped segments. Models are GGUF format, auto-downloaded from Hugging Face on first use (141 MB for `base`, up to 1.5 GB for `large-v3`).

3. **Diarize** (optional, on by default): sherpa-onnx labels each segment with `Speaker_01`, `Speaker_02`, etc. using neural speaker embeddings and clustering. Configurable threshold for tuning speaker count detection.

4. **Summarize**: Either a cloud API call (xAI, OpenAI, or Anthropic — all use the same OpenAI-compatible endpoint) or a local GGUF model via llama.cpp. Same structured prompt for both paths. Dynamic context sizing with token-level truncation for long meetings.

5. **Output**: Timestamped transcript, structured Markdown summary with action items, and a meeting note with YAML frontmatter (Obsidian/Dataview-compatible).

## Output

### Directory structure

```
meetings/2026-02-20_14-30/
  mic.wav          # Raw mic recording
  monitor.wav      # Raw speaker/monitor recording
  audio.wav        # Mixed — used for transcription
  transcript.txt   # Timestamped transcript with speaker labels
  summary.md       # Structured summary
  Meeting_2026-02-20_14-30_Project_Kickoff.md  # Meeting note
```

### Transcript format

```
[00:00 - 00:05] Speaker_01: Hello everyone, thanks for joining.
[00:05 - 00:12] Speaker_02: Thanks. Let's start with the status update.
[00:12 - 00:30] Speaker_01: Sure. We shipped the new auth flow yesterday.
```

### Meeting note

Written alongside the transcript in the output directory. Filename includes an AI-derived title (e.g. `Meeting_2026-02-20_14-30_Project_Kickoff.md`). Contains YAML frontmatter with enriched metadata (date, time, type, domain, status, tags, summary), a summary callout, action item checkboxes, and a foldable transcript section. Compatible with Obsidian Dataview.

## CLI reference

```
Usage: recmeet [OPTIONS]

Audio:
  --source NAME          Mic source (auto-detect if omitted)
  --monitor NAME         Speaker/monitor source (auto-detect if omitted)
  --mic-only             Record mic only, skip monitor capture
  --device-pattern RE    Regex for source auto-detection

Transcription:
  --model NAME           Whisper model: tiny/base/small/medium/large-v3 (default: base)
  --language CODE        Force language (e.g. en, de, ja; default: auto-detect)

Diarization:
  --no-diarize           Disable speaker diarization
  --num-speakers N       Expected speaker count (0 = auto-detect)
  --cluster-threshold F  Clustering distance (default: 1.18; higher = fewer speakers)

Summarization:
  --provider NAME        API provider: xai, openai, anthropic (default: xai)
  --api-key KEY          API key (or set XAI_API_KEY / OPENAI_API_KEY / ANTHROPIC_API_KEY)
  --api-url URL          Custom API endpoint
  --api-model NAME       API model name
  --llm-model PATH       Local GGUF model (instead of API)
  --no-summary           Skip summarization entirely

Output:
  --output-dir DIR       Base output directory (default: ./meetings)
  --context-file PATH    Pre-meeting notes to include in summary prompt

Logging:
  --log-level LEVEL      Log level: none, error, warn, info (default: none)
  --log-dir DIR          Log file directory (default: ~/.local/share/recmeet/logs/)

General:
  --threads N            CPU threads for inference (0 = auto-detect)
  --reprocess DIR        Reprocess existing recording (full pipeline from audio.wav)
  --list-sources         List available audio sources and exit
  -h, --help             Show help
  -v, --version          Show version
```

## Configuration

`~/.config/recmeet/config.yaml` — all fields are optional, CLI flags override.

```yaml
transcription:
  model: base
  language: "" # empty = auto-detect

diarization:
  enabled: true
  num_speakers: 0 # 0 = auto-detect
  cluster_threshold: 1.18

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
cmake -B build -G Ninja -DRECMEET_BUILD_TRAY=OFF -DRECMEET_USE_SHERPA=OFF
```

## Testing

143 unit tests, 576 assertions across 14 modules, plus integration and benchmark suites.

```bash
# Unit tests (no hardware needed)
./build/recmeet_tests "~[integration]~[benchmark]"

# Integration tests (needs running PipeWire session)
./build/recmeet_tests "[integration]"

# Benchmark tests (needs whisper models + assets/)
./build/recmeet_tests "[benchmark]"

# Single module
./build/recmeet_tests "[cli]"
./build/recmeet_tests "[diarize]"
./build/recmeet_tests "[note]"
```

## Installing

```bash
sudo cmake --install build                       # install to /usr/local
cmake --install build --prefix /tmp/test-install  # or a custom prefix
```

### Packages

Pre-built packages for Arch Linux (PKGBUILD), Debian/Ubuntu (`.deb` via CPack), and Fedora/RHEL (`.rpm` via CPack) are supported. See [BUILD.md](BUILD.md) for packaging instructions and per-distro build dependencies.

### Autostart via systemd

recmeet-tray ships a systemd user service that starts the tray applet automatically with your graphical session:

```bash
systemctl --user enable --now recmeet-tray.service   # enable + start immediately
systemctl --user status recmeet-tray.service          # check status
systemctl --user disable --now recmeet-tray.service   # disable + stop
```

The service is tied to `graphical-session.target` and restarts automatically on crash. On Sway, your config must activate that target — see [BUILD.md](BUILD.md) for details.

## Project history

recmeet started as a Python prototype that proved the pipeline in a single session: `pw-record` and `parecord` for capture, `faster-whisper` for transcription, the `requests` library for Grok API calls. Within two hours the core concept was validated — a local Linux box could record, transcribe, and summarize any meeting without cloud dependencies.

The prototype also surfaced the key technical insight that shaped the architecture: `.monitor` sources (how PulseAudio exposes speaker output for recording) don't work with PipeWire's native `pw-record --target`. They produce silence. `parecord --device` works. This dual-capture strategy — PipeWire for the mic, PulseAudio for the monitor — carried through into the C++ rewrite.

The rewrite to C++ was motivated by deployment simplicity. The Python version required a virtualenv, pip-installed packages, system-site-packages for GTK bindings, and careful dependency management across machines. The C++ version compiles to a single static binary with whisper.cpp and llama.cpp linked in. No runtime dependencies beyond the system libraries that any desktop Linux already has. Copy the binary, run it.

From there, the project evolved through nine iterations: doubling test coverage and extracting testable modules, adding a full-featured system tray applet, model pre-download UX, fixing real bugs found during live end-to-end testing (whisper's `detect_language` trap, llama.cpp's chat template requirement), benchmark suites against reference audio, multi-provider API support, speaker diarization via sherpa-onnx, and hardening passes on error handling, context overflow, and clustering threshold tuning.

## Architecture

Three binaries share a common static library:

```
recmeet_core  (static library — 15 modules)
    |
    +-- recmeet        (CLI binary)
    +-- recmeet-tray   (system tray, GTK3 + AppIndicator)
    +-- recmeet_tests  (Catch2 test suite)
```

See [BUILD.md](BUILD.md) for a detailed build system tutorial.

## License

Dual-licensed under [MIT](LICENSE-MIT) and [Apache 2.0](LICENSE-APACHE), at your option.

Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
