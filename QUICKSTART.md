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

This produces four C++ binaries in `build/`:

| Binary | Purpose |
|--------|---------|
| `recmeet` | CLI tool (standalone or daemon client) |
| `recmeet-daemon` | Background daemon (owns pipeline logic) |
| `recmeet-tray` | System tray applet (GTK3, daemon client) |
| `recmeet_tests` | Test suite |

`make build` also builds the Go-based AI tools (requires Go 1.25+):

| Binary | Purpose |
|--------|---------|
| `recmeet-mcp` | MCP server (exposes meeting data to AI tools) |
| `recmeet-agent` | AI agent CLI (meeting prep + follow-up) |

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

Set an API key via environment variable or config file. The tray applet will also prompt you for a key on first recording if none is found.

```bash
# Option 1: Environment variable (highest priority)
export XAI_API_KEY=your-key-here       # xAI (default provider)
export OPENAI_API_KEY=your-key-here    # OpenAI
export ANTHROPIC_API_KEY=your-key-here # Anthropic
```

```yaml
# Option 2: Config file (~/.config/recmeet/config.yaml)
api_keys:
  xai: "xai-..."
  openai: "sk-..."
  anthropic: "sk-ant-..."
```

Environment variables always override config file keys. The tray stores keys entered via its prompt into the config file automatically.

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
  audio_2026-03-05_10-30.wav                   # Mixed audio (16kHz mono)
  Meeting_2026-03-05_10-30_Sprint_Review.md    # Meeting note
```

The meeting note contains:
- YAML frontmatter (date, participants, tags, duration)
- Summary with action items
- Full timestamped transcript with speaker labels (enrolled speakers use real names)

## 8. Configuration file

All CLI options can be set in `~/.config/recmeet/config.yaml`:

```yaml
audio:
  mic_only: false
  # mic_source: ""       # PipeWire/PulseAudio mic (auto-detect if omitted)
  # monitor_source: ""   # monitor/speaker source (auto-detect if omitted)

transcription:
  model: base
  language: ""           # empty = auto-detect
  # vocabulary: "John Suykerbuyk, PipeWire, Kubernetes"  # hints for whisper

diarization:
  enabled: true
  num_speakers: 0        # 0 = auto-detect
  cluster_threshold: 1.18

speaker_id:
  enabled: true            # auto-enabled when speakers are enrolled
  threshold: 0.6           # cosine similarity (higher = stricter matching)
  # database: ~/.local/share/recmeet/speakers/

vad:
  enabled: true
  threshold: 0.5
  min_silence: 0.5       # seconds of silence to end a speech segment
  min_speech: 0.25       # minimum speech duration (seconds)
  max_speech: 30.0       # maximum speech segment length (seconds)

summary:
  provider: xai
  model: grok-3
  # disabled: true       # uncomment to skip summarization
  # llm_mmap: false      # true = mmap model loading (faster load, may cause swap thrashing)

# Per-provider API keys (env vars always override these)
# api_keys:
#   xai: "xai-..."
#   openai: "sk-..."
#   anthropic: "sk-ant-..."

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

## 11. Speaker identification

recmeet can recognize recurring participants across meetings by matching their voice against enrolled voiceprints. Once enrolled, speakers get their real names in transcripts instead of generic `Speaker_01` labels.

### Enroll a speaker from a past recording

After a meeting, enroll yourself (or anyone) from the recording:

```bash
# Interactive — recmeet shows speakers with durations, you pick one
recmeet --enroll "John" --from meetings/2026-03-08_14-30/

# Non-interactive — directly enroll speaker 1
recmeet --enroll "John" --from meetings/2026-03-08_14-30/ --speaker 1
```

Each enrollment extracts a voiceprint (neural embedding) from the speaker's audio segments and saves it to `~/.local/share/recmeet/speakers/John.json`. Multiple enrollments from different recordings improve accuracy — the embeddings are averaged.

### List enrolled speakers

```bash
recmeet --speakers
# Output:
#   Enrolled speakers (2):
#     Alice                 3 enrollment(s)  updated: 2026-03-08T12:00:00Z
#     John                  2 enrollment(s)  updated: 2026-03-09T10:00:00Z
```

### Test identification on a recording (dry-run)

```bash
recmeet --identify meetings/2026-03-09_09-00/
# Output:
#   Speaker_01 → John  (45.2s)
#   Speaker_02 → Alice  (38.7s)
#   Speaker_03 → (unknown)  (12.1s)
```

### Remove a speaker

```bash
recmeet --remove-speaker "John"
```

### How it works

Speaker identification uses the same 3D-Speaker neural embedding model already used for diarization — no additional models to download. After diarization clusters speakers, recmeet extracts an embedding for each cluster and compares it against enrolled voiceprints using cosine similarity. Matches above the threshold (default: 0.6, configurable via `--speaker-threshold`) get the enrolled name; unmatched speakers keep their `Speaker_XX` labels.

### Configuration

Speaker identification is enabled by default when enrolled speakers exist. Override via config or CLI:

```yaml
# ~/.config/recmeet/config.yaml
speaker_id:
  enabled: true              # set to false to disable
  threshold: 0.6             # cosine similarity (higher = stricter matching)
  # database: /custom/path/  # default: ~/.local/share/recmeet/speakers/
```

```bash
# CLI overrides
recmeet --no-speaker-id                    # disable for this run
recmeet --speaker-threshold 0.7            # stricter matching
recmeet --speaker-db /path/to/speakers/    # custom database location
```

## 12. Vocabulary hints (transcription accuracy)

Whisper can mangle unusual names and domain-specific terms. Vocabulary hints bias the transcription decoder toward correct spellings. Enrolled speaker names are included automatically — no configuration needed for names you've already enrolled.

### Automatic hints (speaker names)

When speakers are enrolled (section 11), their names are automatically passed to whisper as vocabulary hints during transcription. If you've enrolled "John Suykerbuyk", whisper will produce the correct spelling instead of phonetic approximations like "John Seck-Rick".

### Managing persistent vocabulary

For terms beyond speaker names — company names, product names, jargon — use the vocabulary management commands:

```bash
# Add words to persistent vocabulary
recmeet --add-vocab "SykeTech"
recmeet --add-vocab "PipeWire"
recmeet --add-vocab "Kubernetes"

# List current vocabulary
recmeet --list-vocab
# Output:
#   Vocabulary words (3):
#     SykeTech
#     PipeWire
#     Kubernetes

# Remove a word
recmeet --remove-vocab "Kubernetes"

# Clear all vocabulary words
recmeet --reset-vocab
```

Vocabulary is saved to `~/.config/recmeet/config.yaml` under `transcription.vocabulary` and persists across sessions.

### One-off vocabulary (CLI flag)

For a single recording, use `--vocab` to override the persistent vocabulary:

```bash
recmeet --vocab "John Suykerbuyk, PipeWire, recmeet"
```

### Configuration

```yaml
# ~/.config/recmeet/config.yaml
transcription:
  vocabulary: "SykeTech, PipeWire, Kubernetes"
```

Enrolled speaker names are always included alongside vocabulary hints — you don't need to add them manually. The combined list is passed to whisper's `initial_prompt` parameter, which biases the decoder toward the specified tokens.

## 13. MCP server (IDE integration)

The MCP server lets AI tools (Claude Code, Claude Desktop, Cursor) query your meeting data directly.


### Build

```bash
make build
```

### Configure your MCP client

For Claude Code, add to `~/.claude.json`:

```json
{
  "mcpServers": {
    "recmeet": {
      "command": "/path/to/build/recmeet-mcp"
    }
  }
}
```

### Available tools

| Tool | What it does |
|------|-------------|
| `search_meetings` | Search notes by keyword, date range, participants |
| `get_meeting` | Full meeting details by directory name |
| `list_action_items` | Action items filtered by status or assignee |
| `get_speaker_profiles` | List enrolled speaker voiceprints |
| `write_context_file` | Stage a context file for a future recording |

Once configured, ask your AI tool: "What action items came out of last week's meetings?" or "Search my meetings about the auth migration."

## 14. AI agent (meeting prep + follow-up)

The agent CLI uses Claude to automate pre-meeting research and post-meeting follow-up.

### Prerequisites

```bash
export ANTHROPIC_API_KEY=your-key-here    # required
export BRAVE_API_KEY=your-key-here        # optional — enables web search
make build
```

### Prepare for a meeting

```bash
# Basic prep — searches past meetings, checks open action items
recmeet-agent prep "Weekly sprint planning"

# With participants and agenda
recmeet-agent prep "Q2 roadmap review" \
    --participants "Alice,Bob" \
    --agenda-url "https://docs.example.com/agenda"
```

The agent writes a Markdown briefing file. Use it as context for your recording:

```bash
recmeet --context-file path/to/briefing.md
```

### Follow up after a meeting

```bash
recmeet-agent follow-up meetings/2026-03-15_14-30/Meeting_2026-03-15_14-30_Sprint_Review.md \
    --my-name "John"
```

The agent reads the meeting note, classifies action items by assignee, and drafts follow-up messages.

### Common flags

```bash
--model string    # LLM model (default: claude-sonnet-4-6)
--verbose         # Show tool calls and intermediate steps
--dry-run         # Print prompts without calling the API
```

## 15. Common workflows

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

**Summary skipped / no API key warning** — Set your API key in the environment (`XAI_API_KEY`, `OPENAI_API_KEY`, or `ANTHROPIC_API_KEY`), in `~/.config/recmeet/config.yaml` under `api_keys:`, or enter it when the tray prompts you. Alternatively, use `--llm-model` for local summarization, or `--no-summary` to suppress the warning.

**Postprocessing fails with `child RSS limit exceeded`** — onnxruntime
diarization grew the postprocessing subprocess past its 12 GB cap (typical for
meetings longer than ~45 minutes). The daemon survives via subprocess
isolation; the audio is preserved. Either split the audio with `ffmpeg` and
reprocess each chunk:

```bash
ffmpeg -i meetings/2026-04-29_19-00/audio_2026-04-29_19-00.wav -t 1800 -c copy chunk1.wav
ffmpeg -i meetings/2026-04-29_19-00/audio_2026-04-29_19-00.wav -ss 1800 -c copy chunk2.wav
recmeet --reprocess chunk1.wav --num-speakers 2
recmeet --reprocess chunk2.wav --num-speakers 2
```

…or raise `RECMEET_RSS_LIMIT_MB` and `MemoryMax` in
`dist/recmeet-daemon.service.in` and reinstall (`make install` then
`systemctl --user daemon-reload && systemctl --user restart
recmeet-daemon.service`).
