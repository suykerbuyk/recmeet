# recmeet

A local-first meeting recorder for Linux. Records audio from any source (Zoom, Teams, Google Meet, VoIP, phone calls), transcribes with [whisper.cpp](https://github.com/ggerganov/whisper.cpp), identifies speakers, and summarizes into structured Markdown notes — all on your own hardware. No cloud accounts, no subscriptions, no bots joining your meetings, no one else touching your data.

## Why this exists

Every commercial meeting transcription service — Otter, Fireflies, and the rest — demands access to your calendar, inserts a bot into every conversation, hosts your transcripts on their cloud, and charges a monthly fee to retain access to _your own data_.

Unacceptable.

recmeet was designed as a Python prototype in under two hours using local open-source models, proving that a single Linux machine can record, transcribe, and summarize any meeting without sending a byte off-box. It was then rewritten as a single C++ binary to eliminate the Python/pip/venv dependency chain entirely — one static binary, zero runtime dependencies, runs on any Linux box with PipeWire.

The result: full transcriptions with speaker labels, professionally structured summaries with action items, output as standard Markdown files on your local filesystem. No subscriptions. No cloud uploads. No one between you and your data.

## What it does

- **Records** mic input and speaker output simultaneously via PipeWire/PulseAudio — captures both sides of any conversation regardless of platform
- **Transcribes** with whisper.cpp (tiny through large-v3 models, auto-downloaded on first use), with vocabulary hints to improve accuracy for names and domain-specific terms; auto-detects Vulkan GPU acceleration at build time (~26× faster on whisper-medium / Radeon Pro W5500) with transparent CPU fallback
- **Identifies speakers** via sherpa-onnx diarization (Pyannote segmentation + 3D-Speaker embeddings), with cross-session speaker identification — enroll voices once, get real names in every transcript
- **Summarizes** via cloud API (xAI/OpenAI/Anthropic — all OpenAI-compatible) or a local GGUF model via llama.cpp — your choice
- **Outputs** timestamped transcripts, structured summaries, and meeting notes with YAML frontmatter (Obsidian-compatible)
- **System tray applet** for point-and-click control from swaybar or any system tray
- **Thin-client / heavy-compute-server architecture (V2)** — the client (tray or CLI) captures audio locally; a daemon (local Unix socket or remote TCP with PSK auth) runs transcription, diarization, identification, and summarization. Standalone in-process mode (`--no-daemon`) is still fully supported for CLI users who don't want a daemon.
- **MCP server** for IDE integration — exposes meeting data to Claude Code, Claude Desktop, and other MCP-compatible tools
- **AI agent CLI** for pre-meeting prep and post-meeting follow-up — searches past meetings, drafts briefings, and generates follow-up communications using Claude

## Architecture at a glance (V2)

recmeet is a **thin-client / heavy-compute-server** system:

- The **client** (`recmeet-client` tray applet or `recmeet-cli`) owns audio capture via the `recmeet_capture` library — PipeWire/PulseAudio live on the client host, not the server.
- The **server** (`recmeet-server`) owns the heavy ML pipeline (transcribe, diarize, identify, summarize, stream-asr captions) and routes jobs through a typed `JobQueue` with three concurrency slots.
- They communicate over **Unix socket** (default — local-only, trusted via kernel peer credentials) or **TCP with PSK auth** (`--listen host:port` + `RECMEET_AUTH_TOKEN` env var — for running the server on a separate machine).

The standalone in-process path (`recmeet-cli --no-daemon`) is also still supported — it runs the full pipeline directly, identical to the legacy single-process flow. See `docs/ARCHITECTURE.md` for the full V2 picture, `docs/MIGRATION.md` for the V1 → V2 / coexistence guide, and `docs/IPC-VERBS.md` for the IPC verb reference.

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

#### Local server mode (recommended for desktop use)

```bash
# Start the server (Unix socket — default)
./build/recmeet-server &

# CLI: connects to the running server automatically
export XAI_API_KEY=your-key-here
./build/recmeet-cli --model base

# Or use the system tray applet (also a server client)
./build/recmeet-client

# Headless WebUI mode — runs the client without a GUI status icon, but
# binds the embedded WebUI HTTP listener at startup (kernel-picked port
# on 127.0.0.1). Useful on a server box without a desktop, or for
# scripted access to the speaker/meeting management endpoints. Discover
# the bound port from the startup log line or via `ss -ltnp`. This
# combination subsumes the `recmeet-webd` use case that was deferred
# from the V2 binary plan.
./build/recmeet-client --headless --listen-now

# Server-control commands
./build/recmeet-cli --status              # query server state
./build/recmeet-cli --stop                # cancel a job in progress
```

#### Standalone mode (no server)

```bash
# Smoke test — no server, no API key, mic only
./build/recmeet-cli --no-daemon --mic-only --no-summary --no-diarize --model tiny

# Full pipeline in-process
export XAI_API_KEY=your-key-here
./build/recmeet-cli --no-daemon --model base
```

`--no-daemon` runs the entire pipeline in the CLI process, exactly as the legacy single-process build did. Use it when you don't want a server at all, or for one-off CLI work on a host where the server isn't installed.

#### Remote server (V2 client/server over TCP)

Run the server on one machine, drive it from another. The TCP listener **requires** PSK authentication via `RECMEET_AUTH_TOKEN`; the server fails to start without it.

```bash
# Server host (the box doing the heavy compute):
export RECMEET_AUTH_TOKEN="$(openssl rand -hex 32)"
./build/recmeet-server --listen 0.0.0.0:29991

# Client host (your laptop):
export RECMEET_AUTH_TOKEN="<same token as above>"
./build/recmeet-cli --daemon-addr server.lan:29991 --mic-only --model base
```

Do **not** expose the daemon TCP port directly to the public internet. The PSK gate is a fail-stop, not an encryption layer; put TLS (stunnel, an SSH tunnel, or a reverse proxy) in front if the traffic crosses untrusted networks. Unix-socket listeners bypass the PSK check because they're already gated by filesystem permissions and kernel peer credentials.

For the full thin-client deployment playbook — PSK generation and rotation, Tailscale/MagicDNS pattern, nginx/caddy TLS termination, server sizing for the ML workload, and future package-split guidance (`recmeet-client` vs `recmeet-server` PKGBUILDs) — see [docs/DEPLOYMENT-THIN-CLIENT.md](docs/DEPLOYMENT-THIN-CLIENT.md). For multi-host topologies and systemd templates, see [docs/V2-DEPLOYMENT.md](docs/V2-DEPLOYMENT.md).

#### Other handy commands

```bash
# Full pipeline with local LLM (no API key, no network)
./build/recmeet-cli --model base \
    --llm-model ~/.local/share/recmeet-server/models/llama/Qwen2.5-7B-Instruct-Q4_K_M.gguf

# Reprocess an old recording with speaker diarization
./build/recmeet-cli --reprocess meetings/2026-02-21_17-34/ --num-speakers 2

# List available audio sources (on the client host — the client owns capture)
./build/recmeet-cli --list-sources
```

See [QUICKSTART.md](QUICKSTART.md) for a step-by-step installation and usage guide, including the full standalone / local-daemon / remote-server walkthroughs.

## How it works

```
Record ──► Transcribe ──► Diarize ──► Identify ──► Summarize ──► Output
(PipeWire)  (whisper.cpp)  (sherpa-onnx) (voiceprint DB) (API/llama.cpp) (Markdown)
```

1. **Record**: The **client** (tray or CLI) captures audio via the `recmeet_capture` library — PipeWire `pw_stream` for the mic and PulseAudio `pa_simple` for the speaker monitor (`.monitor` sources are a PulseAudio abstraction that PipeWire doesn't reliably handle, especially over Bluetooth). Both streams are mixed client-side into a single WAV; in daemon mode the WAV is then handed off to the daemon via `process.submit` (or live-streamed PCM via `process.stream` for captions). The daemon itself no longer links PipeWire or PulseAudio — only the client does.

2. **Transcribe**: whisper.cpp runs locally on CPU, producing timestamped segments. Models are GGUF format, auto-downloaded from Hugging Face on first use (141 MB for `base`, up to 1.5 GB for `large-v3`). Vocabulary hints bias the decoder toward correct spellings of names and domain terms — enrolled speaker names are included automatically.

3. **Diarize** (optional, on by default): sherpa-onnx labels each segment with `Speaker_01`, `Speaker_02`, etc. using neural speaker embeddings and clustering. Configurable threshold for tuning speaker count detection. Long audio (over ~17 minutes at default settings) is automatically processed in overlapping chunks; chunk-local speaker IDs are stitched into a global registry by cosine-similarity matching on raw centroids, keeping peak memory bounded by chunk size rather than meeting length. See `docs/ARCHITECTURE.md#diarization` for the chunk + stitch flow.

4. **Identify** (optional, on by default when speakers are enrolled): Matches diarization clusters against enrolled voiceprints using cosine similarity on 3D-Speaker embeddings. Enrolled speakers get their real names (`John`, `Alice`) instead of generic labels. The speaker database lives at `~/.local/share/recmeet-server/speakers/` — one JSON file per person containing their averaged embedding vectors. Enroll speakers from past recordings with `recmeet-cli --enroll "Name" --from meetings/DIR/`. Enrolled names are automatically passed to whisper as vocabulary hints, improving transcription accuracy for unusual names.

5. **Summarize**: Either a cloud API call (xAI, OpenAI, or Anthropic — all use the same OpenAI-compatible endpoint) or a local GGUF model via llama.cpp. Same structured prompt for both paths. Dynamic context sizing with token-level truncation for long meetings.

6. **Output**: Timestamped transcript, structured Markdown summary with action items, and a meeting note with YAML frontmatter (Obsidian/Dataview-compatible).

## Summarization

recmeet supports two summarization backends — a cloud API or a local LLM. Both use the same structured prompt and produce identical output format.

### Cloud API (default)

Uses any OpenAI-compatible endpoint. Three providers are built in:

| Provider | Env var | Default model |
|----------|---------|---------------|
| xai | `XAI_API_KEY` | grok-3 |
| openai | `OPENAI_API_KEY` | gpt-4o |
| anthropic | `ANTHROPIC_API_KEY` | claude-sonnet-4-6 |

```bash
# Set an API key (env var, config file, or --api-key flag)
export XAI_API_KEY=your-key-here
./build/recmeet-cli --model base --provider xai

# Switch providers
./build/recmeet-cli --model base --provider openai
```

API keys are resolved in priority order: environment variable > per-provider entry in `config.yaml` > legacy `api_key` field.

### Local LLM (fully offline)

Uses llama.cpp to run a GGUF model on CPU. No API key, no network, no data leaves your machine.

```bash
# Download a GGUF model (e.g. from Hugging Face)
mkdir -p ~/.local/share/recmeet/models/llama/
# Place your .gguf file there, or use any path

# Run with local summarization
./build/recmeet-cli --model base \
    --llm-model ~/.local/share/recmeet-server/models/llama/Qwen2.5-7B-Instruct-Q4_K_M.gguf
```

Or set it permanently in `~/.config/recmeet-server/daemon.yaml` (see `docs/MIGRATION.md` for the V2 config-dir layout):

```yaml
summary:
  llm_model: "~/.local/share/recmeet/models/llama/Qwen2.5-7B-Instruct-Q4_K_M.gguf"
```

The tray applet also has a "Local LLM" radio button that opens a file chooser.

**Recommended models** for meeting summarization on CPU:

| Model | Size | Notes |
|-------|------|-------|
| Qwen2.5 7B Instruct Q4_K_M | ~4.5 GB | Good quality/speed balance, strong instruction following |
| Mistral 7B Instruct Q4_K_M | ~4 GB | Fast, reliable summaries |
| Llama 3 8B Instruct Q4_K_M | ~4.5 GB | Strong instruction following |
| Phi-3 Mini 3.8B Q4 | ~2.3 GB | Faster, smaller, decent quality for shorter meetings |

Any GGUF-format model compatible with llama.cpp will work. Download quantized versions from Hugging Face (search for "GGUF" in model repos).

**Constraints**: CPU-only inference, 32K token context window, 4096 token generation budget. Long transcripts are automatically truncated to fit (a log warning is emitted when this happens).

**Memory**: By default, recmeet disables mmap for LLM model loading (`--no-mmap`). This reads the model into heap memory instead of memory-mapping the file, which avoids swap thrashing that can freeze your system during summarization — even when free RAM is available. If you have plenty of RAM and want faster model loading, use `--mmap` or set `llm_mmap: true` in your config.

### Decision logic

1. If `--no-summary` is set: skip summarization entirely
2. If `--llm-model` is set: use local llama.cpp (cloud settings are ignored)
3. If an API key is available: use the configured cloud provider
4. If neither: skip with a warning

### Cloud vs local trade-offs

| | Cloud API | Local LLM |
|---|---|---|
| **Privacy** | Transcript sent to external API | Fully local — nothing leaves your machine |
| **Cost** | Per-API pricing | Free |
| **Speed** | Fast (API-side GPU inference) | Slower (CPU inference) |
| **Quality** | State-of-the-art models | Depends on model choice; 7B models are solid |
| **Setup** | API key | Download a ~4 GB GGUF file |
| **Network** | Required | Not required |

## Live Captioning

Real-time English captions during recording, displayed on the connected tray
client and (optionally) on stderr from the CLI. Captions appear with
~200-700 ms latency and are persisted as a `.vtt` sidecar alongside the
meeting audio.

Captions are **opt-in per recording** — they consume CPU that some users
want reserved for other workloads. The post-recording batch transcript
(produced by whisper) remains the authoritative record; live captions are
approximate and complementary.

### Enable

```bash
# CLI: opt in for this recording only
./build/recmeet-cli --mic-only --show-captions

# CLI: choose a different streaming model (defaults to en-2023-06-26)
./build/recmeet-cli --show-captions --caption-model en-small

# CLI: list cached + available streaming caption models
./build/recmeet-cli --list-caption-models

# CLI: explicitly disable captions even if config has them on
./build/recmeet-cli --no-captions
```

The tray applet has a **"Show Live Captions"** checkbox in its menu;
ticking it persists the preference in `~/.config/recmeet/config.yaml` and
opens a small caption overlay window during the next recording.

### Default model

`sherpa-onnx-streaming-zipformer-en-2023-06-26` (~74 MB int8,
Apache-2.0, English-only). Auto-downloaded on first use into
`~/.local/share/recmeet/models/sherpa/online/en-2023-06-26/`. The CLI / tray
prompts before downloading if the model isn't cached. A smaller
`en-small` (~28 MB) variant is available via `--caption-model en-small`
for low-end hosts.

### Output

The engine emits raw ALL-CAPS hypotheses with no punctuation
(architectural property of the streaming Zipformer). recmeet's clients
pass each caption through `normalize_caption()` before display, which
lowercases and capitalizes sentence boundaries — so what you see on the
tray overlay and on stderr is human-readable, not raw engine output.

Each finalized cue is appended to `<meeting_dir>/captions.vtt` as a
WebVTT block. Partial (mid-utterance) hypotheses are streamed to clients
but NOT persisted — the sidecar holds endpoint-finalized text only,
exactly the same shape ffmpeg / VLC / browser `<track>` elements expect.

```
WEBVTT

00:00:01.240 --> 00:00:04.020
hello and welcome to the meeting

00:00:04.020 --> 00:00:07.500
let's start with the status update
```

### Limitations (V1)

- **English-only.** Captions emit only when `language` is `en` (or unset
  with English audio). Setting `--language` to anything else disables
  captions with a warning.
- **Approximate.** The streaming Zipformer is smaller and less accurate
  than whisper-medium/large. Use the post-recording batch transcript for
  quotable text.
- **Partial captions are ephemeral.** Only finalized cues land in the
  `.vtt`. Mid-utterance hypotheses fly past the IPC and disappear.
- **Requires `RECMEET_USE_SHERPA=ON`.** Sherpa-OFF builds compile
  cleanly but `--show-captions` is a no-op (the engine reports a
  one-shot degraded event and recording continues without captions).

## GPU acceleration

`recmeet` builds with **Vulkan GPU acceleration** by default whenever the host has the toolchain installed (`libvulkan` + headers + `glslc`). On a Radeon Pro W5500 with Mesa RADV, whisper-medium on a 63-minute meeting runs in ~16 minutes on GPU vs ~7 hours on CPU — roughly 26× faster. Speaker diarization stays on CPU regardless (onnxruntime would need a separate ROCm/MIGraphX build).

The build is a **single binary that uses GPU when available and silently falls back to CPU when not.** GPU backends ship as separate `libggml-vulkan.so` plugins loaded at startup; `ldd build/recmeet | grep vulkan` is empty even on a Vulkan-enabled build. Move the binary to a host with no GPU and it still runs.

```bash
# Default — autodetect Vulkan if the toolchain is present, warn if missing pieces, fall back to CPU
make build

# Required — fail configure if Vulkan can't be enabled
cmake -B build -G Ninja -DRECMEET_GGML_VULKAN=ON

# Force CPU-only (e.g. distro packagers shipping a universal artifact)
cmake -B build -G Ninja -DRECMEET_GGML_VULKAN=OFF
```

At server startup, look for the **active-backend banner** on stderr or in `journalctl --user -u recmeet-server.service`:

    ggml: backend registry: CPU, Vulkan
    ggml: active backend: Vulkan (AMD Radeon Pro W5500 (RADV NAVI10), 8192 MB)

If you built with Vulkan but the GPU isn't being used (missing ICD driver, headless host, …), the banner emits a `WARN` line naming the gap before falling back to CPU. See [BUILD.md](docs/BUILD.md#gpu-acceleration-vulkan) for the full toolchain matrix, per-distro install hints, and VRAM measurements.

## Output

### Directory structure

```
meetings/2026-02-20_14-30/
  audio_2026-02-20_14-30.wav      # Mixed mic + monitor (16kHz mono S16LE)
  context_2026-02-20_14-30.json   # Pre-recording context note (only if provided)
  speakers_2026-02-20_14-30.json  # Per-meeting speaker data (only if --diarize)
  Meeting_2026-02-20_14-30.00_Project_Kickoff.md  # Meeting note (first attempt)
  Meeting_2026-02-20_14-30.01_Updated_Kickoff_Plan.md  # Reprocess attempt 1
```

Up to four files per meeting. The audio + meeting note are always present. The context file is written only when the user provided context (via the tray dialog, `--context-text`, or `--context-file`) — it persists the prompt across reprocess. The speakers file is written only when diarization runs.

Every artifact carries the meeting's `YYYY-MM-DD_HH-MM` timestamp suffix. Older meetings written before this convention used unsuffixed names (`audio.wav`, `context.json`, `speakers.json`); they continue to read correctly via legacy-name fallback, and reprocessing them writes the new per-instance filenames alongside.

The meeting note contains the transcript, summary, and action items — no separate `transcript.txt` or `summary.md`. Source WAVs (`mic.wav`, `monitor.wav`) are deleted after mixing; use `--keep-sources` to retain them.

### Transcript format

Embedded in the meeting note as a foldable section:

```
[00:00 - 00:05] John: Hello everyone, thanks for joining.
[00:05 - 00:12] Alice: Thanks. Let's start with the status update.
[00:12 - 00:30] John: Sure. We shipped the new auth flow yesterday.
```

When speakers are enrolled via `--enroll`, their real names replace generic `Speaker_XX` labels. Unenrolled speakers still appear as `Speaker_01`, `Speaker_02`, etc.

### Meeting note

Filename includes a zero-padded reprocess-attempt counter (`.00`, `.01`, ...) and an AI-derived title (e.g. `Meeting_2026-02-20_14-30.00_Project_Kickoff.md`). The counter is inserted between the timestamp and the title, sorts oldest-to-newest within a single recording, and increments by one each time the meeting is reprocessed — preserving every prior attempt's note for comparison instead of overwriting it. Existing un-numbered notes from earlier recmeet versions are migrated to the new form on the first new write (sorted by file mtime: oldest becomes `.00`, next `.01`, and so on; filename ascending as the tiebreak). Contains YAML frontmatter with enriched metadata (date, time, type, domain, status, tags, participants, duration), a summary callout, action item checkboxes, and a foldable transcript section. Compatible with Obsidian Dataview.

## Reprocess

Re-run the postprocessing pipeline (transcription, diarization, summarization, note generation) on an existing recording without re-recording. The audio file stays untouched; everything downstream is regenerated from the current model + config.

```bash
# Reprocess a single meeting directory
./build/recmeet-cli --reprocess meetings/2026-02-21_17-34/ --num-speakers 2
```

Useful after upgrading whisper / diarization models, tweaking summary prompts, or recovering meetings whose original postprocessing failed (e.g. OOM on long audio before chunked diarization).

### Batch reprocess

To reprocess every meeting under a parent directory in one pass:

```bash
./build/recmeet-cli --reprocess-batch ~/meetings/
```

Inspect what would be done first:

```bash
./build/recmeet-cli --reprocess-batch ~/meetings/ --dry-run
```

The batch driver:

- enumerates immediate subdirs matching `YYYY-MM-DD_HH-MM` (with optional `_N` suffix);
- skips any meeting that already has a `Meeting_<ts>*.md` note in `--note-dir` (or in the meeting dir itself if `--note-dir` is unset);
- skips any directory without a usable WAV file;
- runs all remaining meetings serially (diarization and transcription each saturate cores; parallelism would thrash);
- locks daemon vs. standalone dispatch once at start, so a daemon dying mid-batch aborts cleanly with a clear error rather than silently switching modes;
- prints a per-meeting status line and an end-of-batch summary;
- emits exactly one desktop notification (the summary) instead of one per meeting.

A single Ctrl-C aborts the current meeting, stops the loop, prints what completed, and exits 130. Per-meeting failures are reported in the summary but do not abort the batch; the exit code is 1 if any meeting failed, 0 otherwise.

**Memory headroom on 16 GB hosts.** For batch runs over a parent directory containing long meetings, pass `--diarize-chunk-minutes 12` to narrow the chunked-diarize window from the default 15 min:

```bash
./build/recmeet-cli --reprocess-batch ~/meetings/ --diarize-chunk-minutes 12
```

This widens per-chunk RSS headroom at a small cost to per-chunk centroid quality — useful when reprocessing many long meetings back-to-back where library caches accumulate residual host overhead between iterations. The default of 15 min is the iter-121 quality/memory pick; 12 min is the documented stress-test value, not a regression. The chunked-diarize path is automatic — it engages whenever audio length exceeds `chunk_minutes·60 + chunk_overlap_sec + 120` seconds (≈17.5 min at default, ≈14.5 min at 12).

`--reprocess-batch` is mutually exclusive with `--reprocess`. To re-process a single meeting that already has a note, delete the note manually and re-run; v1 has no `--force` overwrite (frontmatter and manual body edits warrant a deliberate design — tracked as follow-up).

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
  --vocab WORDS        Comma-separated vocabulary hints for transcription (names, terms)
  --list-vocab         List persistent vocabulary words and exit
  --add-vocab WORD     Add a word to persistent vocabulary and exit
  --remove-vocab WORD  Remove a word from persistent vocabulary and exit
  --reset-vocab        Clear all persistent vocabulary words and exit
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
  --mmap               Use mmap for LLM model loading (faster load, may cause swap)
  --no-mmap            Disable mmap for LLM model loading (default, avoids swap thrashing)
  --no-diarize         Disable speaker diarization
  --num-speakers N     Number of speakers (0 = auto-detect, default: 0)
  --cluster-threshold F  Clustering distance threshold (default: 1.18, higher = fewer speakers)
  --diarize-chunk-minutes N  Chunked-diarize window (default: 15.0; auto-engages above ~17.5 min audio)
  --diarize-chunk-overlap-sec N  Overlap between chunks (default: 30.0)
  --diarize-stitch-threshold F   Cosine similarity floor for cross-chunk centroid stitching (default: 0.6)
  --no-speaker-id      Disable speaker identification (voiceprint matching)
  --speaker-threshold F  Speaker identification similarity threshold (default: 0.6)
  --speaker-db DIR     Speaker database directory (default: ~/.local/share/recmeet/speakers/)
  --enroll NAME        Enroll a speaker from an existing recording (use with --from)
  --from DIR           Meeting directory for enrollment (use with --enroll)
  --speaker N          Speaker number to enroll (1-based; omit for interactive prompt)
  --speakers           List enrolled speakers and exit
  --remove-speaker NAME  Remove an enrolled speaker and exit
  --reset-speakers     Wipe the entire speaker database and exit
  --identify DIR       Identify speakers in a recording (dry-run) and exit
  --no-vad             Disable VAD segmentation (transcribe full audio)
  --vad-threshold F    VAD speech detection threshold (default: 0.5)
  --threads N          Number of CPU threads for inference (0 = auto-detect, default: 0)
  --reprocess DIR      Reprocess existing recording directory
  --reprocess-batch DIR  Reprocess every meeting subdir under DIR (skips meetings with existing notes)
  --dry-run            With --reprocess-batch: classify and tally only, don't run any pipeline work
  --log-level LEVEL    Log level: none, error, warn, info, debug (default: error)
  --log-dir DIR        Log file directory (default: ~/.local/share/recmeet/logs/)
  --log-retention HOURS  Hours of log history to keep (default: 4)
  --list-sources       List available audio sources and exit
  --download-models    Download required models and exit
  --update-models      Re-download all cached models and exit
  --daemon             Force client mode (require running daemon)
  --no-daemon          Force standalone mode (skip daemon detection)
  --daemon-addr ADDR   Daemon address override (Unix socket path or host:port for TCP)
  --status             Query daemon status and exit
  --stop               Stop daemon recording and exit
  --progress-json      Emit machine-readable NDJSON progress on stdout (subprocess mode)
  --config-json FILE   Subprocess-mode config file (internal: parent-to-child handoff)
  -h, --help           Show this help
  -v, --version        Show version
```

### recmeet-server

```
Usage: recmeet-server [OPTIONS]

Run the recmeet V2 server (IPC server for CLI and client tray).
The server owns the heavy ML pipeline; clients own audio capture.

Options:
  --socket PATH        Unix socket path (default: $XDG_RUNTIME_DIR/recmeet-server/server.sock)
  --listen ADDRESS     Listen address: Unix path or host:port for TCP
                       TCP requires RECMEET_AUTH_TOKEN (PSK auth, fail-stop)
  --log-level LEVEL    Log level: none, error, warn, info, debug (default: info)
  --log-dir DIR        Log file directory
  --log-retention HOURS  Hours of log history to keep (default: 4)
  -h, --help           Show this help
  -v, --version        Show version
```

`--socket` and `--listen` are aliases — both accept a Unix path or `host:port`. Use `--listen 0.0.0.0:29991` together with `RECMEET_AUTH_TOKEN=<secret>` to run the server as a remote host.

## Configuration

V2 splits config across two files:
- `~/.config/recmeet-server/daemon.yaml` — admin defaults for the server (transcription / diarization / summarization / captions).
- `~/.config/recmeet-client/client.yaml` — tray-remembered UI selections + per-user paths.

Both are optional; CLI flags override. See `docs/MIGRATION.md` for the V1 → V2 mapping.

```yaml
audio:
  # mic_source: ""       # PipeWire/PulseAudio mic (auto-detect if omitted)
  # monitor_source: ""   # monitor/speaker source (auto-detect if omitted)

transcription:
  model: base
  language: "" # empty = auto-detect
  # vocabulary: "John Suykerbuyk, PipeWire, Kubernetes"  # hints for whisper (enrolled speaker names are added automatically)

diarization:
  enabled: true
  num_speakers: 0           # 0 = auto-detect
  cluster_threshold: 1.18
  chunk_minutes: 15.0       # window width; auto-chunks audio above ~17.5 min
  chunk_overlap_sec: 30.0   # overlap between chunks (must satisfy chunk_minutes*60 > overlap+60)
  stitch_threshold: 0.6     # cosine similarity floor for cross-chunk centroid stitching

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
  # llm_model: "~/.local/share/recmeet/models/llama/Qwen2.5-7B-Instruct-Q4_K_M.gguf"  # local LLM (overrides provider)
  # llm_mmap: false     # true = mmap model loading (faster load, may cause swap thrashing)

# Per-provider API keys (env vars always override these)
# api_keys:
#   xai: "xai-..."
#   openai: "sk-..."
#   anthropic: "sk-ant-..."

output:
  directory: ./meetings

notes:
  domain: general

logging:
  level: error # none, error, warn, info, debug
  # retention_hours: 4   # hours of log history to keep before rotation
  # directory: ~/.local/share/recmeet/logs/

general:
  threads: 0 # 0 = auto-detect (cores - 1)
```

## Build options

| Option                | Default | Effect                                                                                        |
| --------------------- | ------- | --------------------------------------------------------------------------------------------- |
| `RECMEET_GGML_VULKAN` | AUTO    | GPU acceleration via ggml Vulkan backend. AUTO probes; ON requires; OFF disables. See below.  |
| `RECMEET_USE_SHERPA`  | ON      | Enable speaker diarization (fetches sherpa-onnx via CMake)                                    |
| `RECMEET_USE_LLAMA`   | ON      | Enable local LLM summarization via llama.cpp                                                  |
| `RECMEET_USE_NOTIFY`  | ON      | Enable desktop notifications via libnotify                                                    |
| `RECMEET_BUILD_TRAY`  | ON      | Build the system tray applet (requires GTK3 + AppIndicator)                                   |
| `RECMEET_BUILD_TESTS` | ON      | Build the Catch2 test suite                                                                   |

```bash
# Example: headless build without tray or diarization
make RECMEET_BUILD_TRAY=OFF RECMEET_USE_SHERPA=OFF

# Or manually:
# cmake -B build -G Ninja -DRECMEET_BUILD_TRAY=OFF -DRECMEET_USE_SHERPA=OFF
```

### Go tools (MCP server + AI agent)

`make build` also builds the Go tools (requires Go 1.25+). For coverage:

```bash
make coverage            # Go test coverage report
```

## Testing

451 C++ unit-test cases (1734 assertions) across 28 modules, plus 54 IPC integration cases (368 assertions), 16 reprocess-batch cases, 15 [t2-1] long-audio integration cases, 17 benchmark cases, 5 full-stack end-to-end cases, and 93 Go test functions across the MCP server and AI agent.

```bash
make test                # C++ unit + Go tests (no hardware needed)
make integration         # all [integration]-tagged tests (IPC + reprocess-batch + device + t2-1)
make integration-t2-1    # long-audio chunked-diarize gate under cgroup MemoryMax=8G (systemd-run)
make benchmark           # benchmark tests (needs whisper models + assets/)
make full-stack          # end-to-end pipeline tests (models + assets/)
make build-onnxruntime   # build vendored onnxruntime from source (~20 min, see docs/BUILD.md)

# Or run directly for more control:
./build/recmeet_tests "~[integration]~[benchmark]~[full-stack]"  # unit tests only
./build/recmeet_tests "[ipc][integration]"            # IPC integration
./build/recmeet_tests "[reprocess-batch]"             # reprocess-batch CLI
./build/recmeet_tests "[t2-1]"                        # long-audio chunked-diarize integration
./build/recmeet_tests "[benchmark]"                   # needs whisper models + assets/
./build/recmeet_tests "[full-stack]"                  # end-to-end pipeline (models + assets/)
./build/recmeet_tests "[cli]"                         # single module
```

- Long-running tests (`[full-stack]`, `[benchmark]`, `[slow]`, `[stress]`, `[verylong]`) automatically emit per-test announcements and a 30-second heartbeat to stderr. See `docs/BUILD.md` "Test progress reporting" for the env-var overrides (`RECMEET_TEST_ANNOUNCE`, `RECMEET_TEST_HEARTBEAT`, `RECMEET_TEST_HEARTBEAT_SECS`) and `RECMEET_TEST_PHASE_ECHO` for in-pipeline phase markers (set by `make benchmark` / `make full-stack` by default).

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

See [docs/BUILD.md](docs/BUILD.md) for packaging details and per-distro build dependencies.

### Autostart via systemd

The server and client each have a systemd user service. The client service depends on the server — enabling the client automatically pulls in the server.

```bash
# Server only (headless / CLI usage)
systemctl --user enable --now recmeet-server.service

# Server + client (desktop usage — server starts automatically)
systemctl --user enable --now recmeet-client.service

# Check status
systemctl --user status recmeet-server.service
systemctl --user status recmeet-client.service

# Stop and disable
systemctl --user disable --now recmeet-client.service
systemctl --user disable --now recmeet-server.service
```

Socket activation is also available — `recmeet-server.socket` starts the server on first connection:

```bash
systemctl --user enable --now recmeet-server.socket
```

The client service is tied to `graphical-session.target` and restarts automatically on crash. On Sway, your config must activate that target — see [docs/BUILD.md](docs/BUILD.md) for details.

### Postprocessing memory limits

**Target met (iter 121):** recmeet processes up to **4 hours of audio plus context notes on a host with a hard 16 GB memory ceiling**, end-to-end through transcription, diarization, speaker identification, and summarization. Validated against a 60-min real meeting and a 4-hour synthetic fixture under a `MemoryMax=16G` cgroup with no swap.

The architecture has three layers of containment, each load-bearing:

1. **systemd cgroup caps** (`dist/recmeet-server.service.in`): `MemoryHigh=10G` / `MemoryMax=14G` / `MemorySwapMax=0`. Cgroup-enforced so even an unrecoverable workload reaps only this unit, not the whole host.
2. **Subprocess isolation**: postprocessing runs as a forked child. Crashes or the cgroup hard-cap kill only the child; the daemon stays alive, the audio is preserved, the operator gets a precise error.
3. **Chunked diarization** (T2.1, iter 121): when audio exceeds `chunk_minutes·60 + chunk_overlap_sec + 120` seconds (≈17.5 min at default chunk_minutes=15), the pipeline auto-engages `diarize_chunked()` — slices audio into overlapping windows, reuses one `DiarizeSession` and one `SpeakerEmbeddingSession` across all chunks, stitches per-chunk centroids into a global registry by cosine similarity, then bypasses the second extractor pass via `identify_speakers_with_centroids()` (T2.2 H1). Per-chunk peak working set stays ≤ 6 GB on the 60-min fixture.

Below the chunk threshold the single-call `diarize()` path runs unchanged — short meetings pay no chunking overhead.

The postprocessing subprocess also self-limits at 12 GB (`RECMEET_RSS_LIMIT_MB=12288`); on overflow it writes `child RSS limit exceeded — split audio (ffmpeg) or raise RECMEET_RSS_LIMIT_MB` to stderr and exits cleanly. This is a defense-in-depth layer behind the cgroup; the cgroup remains the real backstop because RSS-self-limiting can stall under uninterruptible kernel sleep.

**Tunable for batch runs:** `--diarize-chunk-minutes 12` narrows the chunk window from the default 15 min, widening per-chunk RSS headroom for batch reprocessing of many long meetings back-to-back. See "Batch reprocess" above for the full recipe.

Forensic context for the original investigation that drove this design lives at [docs/history/DEADLOCK-INVESTIGATION.md](docs/history/DEADLOCK-INVESTIGATION.md).

## Project history

recmeet started as a Python prototype that proved the pipeline in a single session: `pw-record` and `parecord` for capture, `faster-whisper` for transcription, the `requests` library for Grok API calls. Within two hours the core concept was validated — a local Linux box could record, transcribe, and summarize any meeting without cloud dependencies.

The prototype also surfaced the key technical insight that shaped the architecture: `.monitor` sources (how PulseAudio exposes speaker output for recording) don't work with PipeWire's native `pw-record --target`. They produce silence. `parecord --device` works. This dual-capture strategy — PipeWire for the mic, PulseAudio for the monitor — carried through into the C++ rewrite.

The rewrite to C++ was motivated by deployment simplicity. The Python version required a virtualenv, pip-installed packages, system-site-packages for GTK bindings, and careful dependency management across machines. The C++ version compiles to a single static binary with whisper.cpp and llama.cpp linked in. No runtime dependencies beyond the system libraries that any desktop Linux already has. Copy the binary, run it.

From there, the project evolved through extensive iteration: doubling test coverage and extracting testable modules, adding a full-featured system tray applet, model pre-download UX, fixing real bugs found during live end-to-end testing (whisper's `detect_language` trap, llama.cpp's chat template requirement), benchmark suites against reference audio, multi-provider API support, speaker diarization via sherpa-onnx, packaging for Arch/Debian/Fedora, and hardening passes on error handling, context overflow, and clustering threshold tuning.

## Architecture

Four C++ binaries share a common static library. The **client** binaries (`recmeet-cli` and `recmeet-client`) own audio capture; the **server** (`recmeet-server`) owns the heavy ML pipeline (transcribe, diarize, identify, summarize, stream-asr captions). They communicate over Unix socket or TCP using a framed wire protocol (NDJSON control frames plus binary upload / artifact / streaming-PCM frames). Two additional Go binaries provide AI-powered tooling.

```
recmeet_core     (static library — pipeline + IPC)
recmeet_capture  (client-side capture library — PipeWire + PulseAudio)
    |
    +-- recmeet-server  (server daemon — pipeline + IPC; does NOT link PipeWire/PulseAudio)
    +-- recmeet-cli     (CLI — dual-mode: thin server client or standalone in-process)
    +-- recmeet-client  (system tray — server client, GTK3 + AppIndicator)
    +-- recmeet_tests   (Catch2 test suite)

tools/  (Go module — github.com/syketech/recmeet-tools)
    |
    +-- recmeet-mcp    (MCP server — exposes meeting data to AI tools)
    +-- recmeet-agent  (AI agent CLI — meeting prep + follow-up workflows)
```

### Server mode (recommended)

```
recmeet-client / recmeet-cli                 recmeet-server
        |                                          |
   [capture]                                   [JobQueue: 3 typed slots]
   (PipeWire / PulseAudio)  ── IPC ──►   [transcribe | diarize | summarize]
        |                                          |
   process.submit / process.stream         stream-asr, voiceprint match
        |                                          |
   Transport: Unix socket  (default, local-only)
              TCP + PSK    (--listen / --daemon-addr, RECMEET_AUTH_TOKEN)
```

The CLI auto-detects a running server and operates as a client. Use `--no-daemon` to force standalone in-process mode. The client tray connects to the server and reconnects automatically with exponential backoff if the server restarts. A per-client `session.init` handshake carries credentials and per-session preferences (provider, model, language, diarize, vad, etc.) so a single server can serve multiple clients with different settings.

### IPC protocol (V2)

- **Wire format**: Framed binary protocol — 0x00 NDJSON control, 0x01 binary upload (client→daemon), 0x02 binary artifact (daemon→client), 0x03 streaming PCM (client→daemon, used for live captions). `IPC_PROTOCOL_VERSION = 3`.
- **Transports**: Unix domain socket (default, local-only, kernel-peer-cred trusted) or TCP (`--listen host:port` + `RECMEET_AUTH_TOKEN`; daemon fail-stops without PSK on TCP).
- **Core verbs**: `session.init`, `session.update_credentials`, `session.update_prefs`, `process.submit`, `process.fetch`, `process.cancel`, `process.stream` / `process.stream.commit` / `process.stream.cancel`, `process.submit.cancel`, `job.status`, `job.list`, `enroll.finalize`, `auth.ok`, plus the unchanged `status.get`, `config.update`, `config.reload`, `models.list`, `models.ensure`, `models.update`.
- **Removed in V2** (gone in C.9): `record.start`, `record.stop`, `job.context`, `sources.list` — capture moved to the client, source enumeration is a client-local operation.
- **Events** (server push): `phase`, `progress`, `state.changed`, `job.complete`, `model.downloading`, `caption` (routed per-client).

See [docs/IPC-VERBS.md](docs/IPC-VERBS.md) for the per-verb reference (request/response shapes, error codes, framing rules) and [docs/IPC-WIRE-PROTOCOL.md](docs/IPC-WIRE-PROTOCOL.md) for the framing-level details. See [docs/BUILD.md](docs/BUILD.md) for a detailed build system tutorial.

## MCP server

`recmeet-mcp` is a [Model Context Protocol](https://modelcontextprotocol.io/) server that exposes your meeting data to AI tools — Claude Code, Claude Desktop, Cursor, or any MCP-compatible client. It runs over stdio and reads the same `config.yaml` as the rest of recmeet.

### Available tools

| Tool | Description |
|------|-------------|
| `search_meetings` | Search meeting notes by keyword, date range, and participants |
| `get_meeting` | Get full details for a specific meeting by directory name |
| `list_action_items` | List action items across all meetings, filtered by status or assignee |
| `get_speaker_profiles` | List enrolled speaker profiles from the voiceprint database |
| `write_context_file` | Write a pre-meeting context file for use in future recordings |

### Setup

Build the MCP server:

```bash
make build       # builds all binaries including recmeet-mcp + recmeet-agent
```

Add to your MCP client configuration. For Claude Code (`~/.claude.json`):

```json
{
  "mcpServers": {
    "recmeet": {
      "command": "/path/to/recmeet-mcp"
    }
  }
}
```

For Claude Desktop (`claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "recmeet": {
      "command": "/path/to/recmeet-mcp",
      "args": []
    }
  }
}
```

Once configured, you can ask your AI tool things like:
- "Search my meetings for discussions about the auth migration"
- "What action items are assigned to me?"
- "Show me the full notes from last Tuesday's standup"
- "Write a context file for tomorrow's sprint planning"

## AI agent

`recmeet-agent` is an AI-powered CLI that uses Claude to automate meeting preparation and follow-up. It searches your past meetings, fetches web content, and generates structured briefings or follow-up drafts.

### Prerequisites

```bash
export ANTHROPIC_API_KEY=your-key-here    # required
export BRAVE_API_KEY=your-key-here        # optional, enables web search
```

### Meeting preparation

Generate a briefing document before a meeting:

```bash
# Basic prep
recmeet-agent prep "Weekly sprint planning"

# With participant context and agenda
recmeet-agent prep "Q2 roadmap review" \
    --participants "Alice,Bob" \
    --agenda-url "https://docs.example.com/q2-roadmap"

# Custom output path
recmeet-agent prep "1:1 with manager" --output prep-notes.md
```

The agent searches past meetings involving the participants, checks for open action items, optionally fetches and analyzes the agenda URL, and compiles everything into an organized Markdown briefing. Use the briefing as a `--context-file` when recording:

```bash
recmeet --context-file prep-notes.md
```

### Post-meeting follow-up

Draft follow-up communications from meeting notes:

```bash
recmeet-agent follow-up meetings/2026-03-15_14-30/Meeting_2026-03-15_14-30_Sprint_Review.md \
    --my-name "John"
```

The agent reads the meeting note, classifies action items by assignee and urgency, and drafts follow-up messages for each participant.

### Agent CLI reference

```
Usage: recmeet-agent <command> [flags]

Commands:
  prep        Prepare context for an upcoming meeting
  follow-up   Process meeting notes and draft follow-up communications

Common flags:
  --model string    LLM model (default: claude-sonnet-4-6)
  --verbose         Show tool calls and intermediate steps
  --dry-run         Print prompts without calling the API
  --config string   Config file path (default: ~/.config/recmeet/config.yaml)
```

## License

Dual-licensed under [MIT and Apache 2.0](LICENSE), at your option.

Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
