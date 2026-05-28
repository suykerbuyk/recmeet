# Quick Start Guide

Step-by-step guide to installing, configuring, and using recmeet.

## V2 architecture at a glance

recmeet is a **thin-client / heavy-compute-server** system. The client (tray or CLI) captures audio locally via PipeWire/PulseAudio. The daemon (`recmeet-server`) does the heavy ML work — transcription, diarization, speaker identification, summarization, and streaming live captions. Clients talk to the daemon over a Unix socket (default, local-only) or TCP (`--listen` / `--daemon-addr` with PSK auth, for running the daemon on a separate machine).

Three operating modes are supported, in order of increasing setup cost:

1. **Standalone mode** — `recmeet-cli --no-daemon` runs the entire pipeline in the CLI process. No server, no IPC, no setup. Same flags as the legacy single-process build.
2. **Local server mode** — start `recmeet-server` (or use the systemd user unit) and let the tray / CLI auto-connect to its Unix socket. This is the default for desktop users.
3. **Remote server mode** — run `recmeet-server --listen 0.0.0.0:29991` on a beefy machine, connect from your laptop with `recmeet-cli --daemon-addr server:29991`. Requires a PSK (`RECMEET_AUTH_TOKEN`) and is intended for trusted networks only.

Sections 3–6 walk through each of these flows. For the architecture in detail, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); for multi-host deployment recipes, see [docs/V2-DEPLOYMENT.md](docs/V2-DEPLOYMENT.md); for the IPC verb reference, see [docs/IPC-VERBS.md](docs/IPC-VERBS.md).

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
| `recmeet-cli` | CLI tool (standalone or server client) |
| `recmeet-server` | Server daemon (owns pipeline logic) |
| `recmeet-client` | System tray applet (GTK3, server client) |
| `recmeet_tests` | Test suite |

`make build` also builds the Go-based AI tools (requires Go 1.25+):

| Binary | Purpose |
|--------|---------|
| `recmeet-mcp` | MCP server (exposes meeting data to AI tools) |
| `recmeet-agent` | AI agent CLI (meeting prep + follow-up) |

## 3. Mode A — Standalone (no daemon, fastest smoke test)

The simplest way to verify everything works — no daemon, no IPC, no API key:

```bash
./build/recmeet-cli --no-daemon --mic-only --no-summary --no-diarize --model tiny
# Speak into your mic, then press Ctrl+C to stop.
```

Output appears in `meetings/<timestamp>/`. The `--no-daemon` flag tells the CLI to skip daemon detection and run the entire pipeline (capture + transcribe + diarize + summarize) inside the CLI process. This is the legacy single-process flow and remains fully supported in V2.

Use standalone mode when:

- You don't want a daemon at all.
- You're on a host where the daemon isn't installed.
- You're scripting one-off CLI work and don't want shared state.

## 4. Mode B — Local daemon (recommended for desktop use)

The local server is the default desktop experience. It runs in the background, exposes a Unix socket at `$XDG_RUNTIME_DIR/recmeet-server/server.sock`, and serves the tray and CLI as thin clients. The server owns the heavy ML pipeline; the client owns audio capture.

### Start manually

```bash
./build/recmeet-server &
```

### Or install and use systemd

```bash
make install                                        # installs to ~/.local, downloads models, enables daemon
```

`make install` is turnkey — it downloads default models (whisper, sherpa, VAD), enables the daemon via systemd, and starts it. No further setup needed.

### Verify it's running

```bash
recmeet-cli --status
# Output: Server: running / State: idle
```

The CLI auto-detects the running daemon. The first command the client sends after connecting is `session.init`, which carries credentials and per-session preferences (provider, model, language, etc.) — so a single daemon can serve multiple clients with different settings.

## 5. Mode C — Remote daemon (V2 client/server over TCP)

V2 lets you run the daemon on one machine and the client(s) on another. Useful when you have a beefy desktop or server doing the heavy ML work and want to drive it from a laptop. **TCP listeners require PSK auth**; the daemon fail-stops if you start it on a TCP address without `RECMEET_AUTH_TOKEN` set.

### Generate a shared secret

```bash
openssl rand -hex 32
# 7c2f9a... (paste into both env vars below)
```

### Server host

```bash
export RECMEET_AUTH_TOKEN="7c2f9a..."        # same secret on both sides
./build/recmeet-server --listen 0.0.0.0:29991
# recmeet-server: PSK auth enabled for TCP listener
# recmeet-server listening on 0.0.0.0:29991
```

To run under systemd, drop the token into an EnvironmentFile readable only by your user:

```bash
mkdir -p ~/.config/systemd/user/recmeet-server.service.d
cat >~/.config/systemd/user/recmeet-server.service.d/listen.conf <<'EOF'
[Service]
Environment=RECMEET_AUTH_TOKEN=7c2f9a...
ExecStart=
ExecStart=/usr/local/bin/recmeet-server --listen 0.0.0.0:29991
EOF
systemctl --user daemon-reload
systemctl --user restart recmeet-server.service
```

### Client host

```bash
export RECMEET_AUTH_TOKEN="7c2f9a..."        # same secret as the server
recmeet-cli --daemon-addr server.lan:29991 --mic-only --model base
```

Or, set the server address in your shell environment so every `recmeet-cli` invocation picks it up automatically:

```bash
export RECMEET_DAEMON_ADDR=server.lan:29991
```

There is no client-side config-file knob for this today; use either `--daemon-addr` per-invocation or `RECMEET_DAEMON_ADDR` in your shell profile.

Audio capture still happens **on the client host** — the daemon never opens PipeWire or PulseAudio. The raw WAV (or live PCM frames, for streaming captions) is uploaded over the IPC connection.

### Security caveats

- **Do not expose the daemon TCP port to the public internet.** The PSK gate is a fail-stop, not an encryption layer; the framed wire is plaintext NDJSON + binary blobs.
- For traffic crossing untrusted networks, put TLS in front (stunnel, an SSH `LocalForward`, a reverse proxy, or a WireGuard tunnel).
- Unix-socket listeners (the default) bypass the PSK check — they're already gated by filesystem permissions and kernel peer credentials.
- Keep `RECMEET_AUTH_TOKEN` out of shell history; use an EnvironmentFile (mode 0600) or a secret manager.

See [docs/DEPLOYMENT-THIN-CLIENT.md](docs/DEPLOYMENT-THIN-CLIENT.md) for the thin-client deployment playbook: PSK generation and rotation, the Tailscale + MagicDNS pattern (the most-recommended path for distributed teams), nginx/caddy TLS termination in front of the daemon, server sizing (CPU + RAM for whisper.cpp + sherpa-onnx + llama.cpp), and the forward-looking `recmeet-client` / `recmeet-server` package split. See [docs/V2-DEPLOYMENT.md](docs/V2-DEPLOYMENT.md) for the full multi-host deployment reference, including TLS-fronted setups, systemd templates, and the PSK-rotation + per-token revocation (`--evict`) operator workflow.

## 6. Configure summarization

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
# Option 2: Config file (~/.config/recmeet-client/client.yaml)
# (V2 keeps the api_keys block on the client side — same shape as V1.)
api_keys:
  xai: "xai-..."
  openai: "sk-..."
  anthropic: "sk-ant-..."
```

Environment variables always override config file keys. The tray stores keys entered via its prompt into the config file automatically.

To switch providers:

```bash
recmeet-cli --provider openai --model base
```

### Local LLM (fully offline)

Download a GGUF model and point recmeet-cli at it:

```bash
mkdir -p ~/.local/share/recmeet-server/models/llama
curl -L -o ~/.local/share/recmeet-server/models/llama/Qwen2.5-7B-Instruct-Q4_K_M.gguf \
    https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/resolve/main/qwen2.5-7b-instruct-q4_k_m.gguf

recmeet-cli --llm-model ~/.local/share/recmeet-server/models/llama/Qwen2.5-7B-Instruct-Q4_K_M.gguf
```

### No summarization

```bash
recmeet-cli --no-summary
```

## 7. Record a meeting

The recording flow looks the same from the user's seat regardless of which mode you're in. Under the hood, daemon-mode clients capture audio locally and hand the WAV (or live PCM stream) to the daemon via `process.submit` / `process.stream`; standalone mode runs the pipeline in-process.

### Via CLI (with daemon running)

```bash
recmeet-cli --model base
# Recording starts. Press Ctrl+C to stop.
# Phases print as they happen: recording → transcribing → diarizing → summarizing → complete
```

The CLI auto-detects the server. If the server isn't running, it falls back to standalone mode (unless you pass `--daemon`, which makes server presence required).

### Via tray applet

```bash
recmeet-client                          # or: systemctl --user start recmeet-client.service
```

Click the tray icon, select **Record** to start, **Stop Recording** to finish. The tray connects to the daemon and shows status updates in real time. The tray is always a client — it does not have a standalone mode.

### Force standalone mode (no daemon)

```bash
recmeet-cli --no-daemon --model base
```

### Point the CLI at a remote server

```bash
export RECMEET_AUTH_TOKEN="<shared PSK>"
recmeet-cli --daemon-addr server.lan:29991 --model base
```

### Enable live captions (optional)

Live captions are an opt-in feature — they consume CPU that some users
want reserved. Captions display in real time during recording and are
saved as a `.vtt` sidecar; the post-recording batch transcript remains
the authoritative record.

In daemon mode, the client streams raw PCM frames (0x03 streaming
frames) to the daemon via `process.stream`; the daemon's stream-asr
engine emits routed `caption` events back over the IPC connection. In
standalone mode, the same engine runs in-process. The UX — toggle the
tray menu item or pass `--show-captions` — is identical.

**1. First-time setup — download the streaming model**

The default model is `sherpa-onnx-streaming-zipformer-en-2023-06-26`
(English-only, Apache-2.0). The tarball is ~310 MB on disk (ships both
int8 and fp32 weights); recmeet loads only the ~74 MB int8 set at
runtime. The CLI prompts before downloading on first use:

```bash
recmeet-cli --mic-only --show-captions
# Streaming caption model 'en-2023-06-26' (~310 MB) is not cached.
# Download now? [y/N]
```

The tray applet shows the same prompt the first time you tick "Show
Live Captions". The model is cached at
`~/.local/share/recmeet-server/models/sherpa/online/en-2023-06-26/`.

**2. Verify by recording a short test clip**

```bash
recmeet-cli --mic-only --show-captions --no-summary --model tiny
# Speak something. You should see captions appear on stderr in real
# time, e.g.:
#   [caption] hello and welcome to the meeting
#   [caption] let's start with the status update
# Press Ctrl+C to stop.
```

After the recording stops, check the meeting directory for the
sidecar:

```
meetings/2026-05-09_10-30/
  audio_2026-05-09_10-30.wav
  captions.vtt                    # WebVTT, finalized cues only
  Meeting_2026-05-09_10-30_*.md   # Standard meeting note
```

The `.vtt` file plays cleanly in VLC / browser `<track>` elements and
parses round-trip with ffmpeg.

**3. List available caption models**

```bash
recmeet-cli --list-caption-models
# en-2023-06-26  (~310 MB)  cached
# en-small       (~128 MB)  not cached
```

**4. Persist via config**

V2 splits the caption preference across two files:

```yaml
# ~/.config/recmeet-server/daemon.yaml — server liveness
captions:
  enabled: true
  model: en-2023-06-26
```

```yaml
# ~/.config/recmeet-client/client.yaml — client overlay visibility
captions:
  enabled: true
```

The server side gates whether the daemon stands up a streaming ASR
engine at all; the client side controls whether the tray renders the
overlay. Both default to `true`. See `docs/ARCHITECTURE.md` for the
full split.

The tray's "Show Live Captions" checkbox writes this for you.

**Limitations:** English-only in V1; partial captions are streamed but
not persisted; ALL-CAPS engine output is normalized at display time.
Sherpa-OFF builds compile but `--show-captions` is a no-op (the engine
emits a one-shot degraded event and recording continues without
captions).

## 8. View output

Each recording creates a directory under `meetings/`:

```
meetings/2026-03-05_10-30/
  audio_2026-03-05_10-30.wav                   # Mixed audio (16kHz mono)
  context_2026-03-05_10-30.json                # Pre-recording context (only if provided)
  speakers_2026-03-05_10-30.json               # Per-meeting speaker data (only if --diarize)
  Meeting_2026-03-05_10-30_Sprint_Review.md    # Meeting note
```

All artifacts share the meeting's `YYYY-MM-DD_HH-MM` timestamp suffix.

The meeting note contains:
- YAML frontmatter (date, participants, tags, duration)
- Summary with action items
- Full timestamped transcript with speaker labels (enrolled speakers use real names)

## 9. Configuration file

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
  num_speakers: 0           # 0 = auto-detect
  cluster_threshold: 1.18
  chunk_minutes: 15.0       # window width; auto-chunks audio above ~17.5 min
  chunk_overlap_sec: 30.0   # overlap between chunks
  stitch_threshold: 0.6     # cosine similarity floor for cross-chunk centroid stitching

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
  level: error           # none, error, warn, info, debug
  # retention_hours: 4   # hours of log history to keep before rotation

general:
  threads: 0             # 0 = auto (cores - 1)
```

CLI flags override config file values. The daemon reloads config on `SIGHUP`:

```bash
kill -HUP $(pidof recmeet-server)
```

## 10. Daemon management

### Makefile shortcuts

```bash
make daemon-start        # build + start daemon in background
make daemon-stop         # stop running daemon
make daemon-status       # query daemon status
```

### CLI commands

```bash
recmeet --status                      # show daemon state (idle/recording/postprocessing)
recmeet --stop                        # cancel a job in progress
recmeet --daemon                      # force client mode (fail if no daemon)
recmeet --no-daemon                   # force standalone mode (ignore daemon)
recmeet --daemon-addr host:port       # point at a remote daemon (TCP, requires RECMEET_AUTH_TOKEN)
recmeet --daemon-addr /path/to/sock   # point at a non-default Unix socket
```

### Listen address (daemon side)

```bash
recmeet-server                                       # default: $XDG_RUNTIME_DIR/recmeet/daemon.sock
recmeet-server --listen /tmp/recmeet.sock            # custom Unix path
RECMEET_AUTH_TOKEN=<secret> recmeet-server --listen 0.0.0.0:29991   # TCP (PSK required)
```

TCP listeners fail-stop without `RECMEET_AUTH_TOKEN`. Unix-socket listeners bypass the PSK check (they're already gated by filesystem permissions and kernel peer credentials).

### systemd

```bash
# Daemon only
systemctl --user enable --now recmeet-server.service

# Daemon + tray (tray automatically starts daemon)
systemctl --user enable --now recmeet-client.service

# Socket activation (daemon starts on first connection)
systemctl --user enable --now recmeet-server.socket

# Logs
journalctl --user -u recmeet-server.service -f
```

## 11. Model management

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

## 12. Speaker identification

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

## 13. Vocabulary hints (transcription accuracy)

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

## 14. MCP server (IDE integration)

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

## 15. AI agent (meeting prep + follow-up)

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

## 16. Common workflows

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

To reprocess every meeting under a parent directory in one pass, with a
dry-run preview first:

```bash
recmeet --reprocess-batch ~/meetings/ --dry-run
recmeet --reprocess-batch ~/meetings/
```

See README "Batch reprocess" for full semantics (skip rule, partial-failure
behaviour, and the Ctrl-C contract).

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

**"Daemon: not running"** — Start the daemon: `recmeet-server &` or `systemctl --user start recmeet-server.service`

**"Another recmeet-server is already running"** — A daemon is already active. Use `recmeet --status` to check, or `pkill recmeet-server` to kill a stale instance.

**No audio captured** — Verify PipeWire is running: `pw-cli info`. List sources with `recmeet --list-sources` and check that your mic appears.

**Whisper model not found** — Models are auto-downloaded on first use. If behind a proxy, download manually (see [docs/BUILD.md](docs/BUILD.md)).

**Tray shows "Disconnected"** — The tray reconnects automatically when the daemon starts. Check `systemctl --user status recmeet-server.service`.

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
`dist/recmeet-server.service.in` and reinstall (`make install` then
`systemctl --user daemon-reload && systemctl --user restart
recmeet-server.service`).
