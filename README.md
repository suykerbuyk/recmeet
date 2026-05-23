# recmeet

A local-first meeting recorder for Linux. Records audio from any source (Zoom, Teams, Google Meet, VoIP, phone calls), transcribes with [whisper.cpp](https://github.com/ggerganov/whisper.cpp), identifies speakers with [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx), and summarizes with [llama.cpp](https://github.com/ggerganov/llama.cpp) or a cloud API — all on your own hardware. No cloud accounts, no subscriptions, no bots joining your meetings, no one else touching your data.

## Why this exists

Every commercial meeting transcription service — Otter, Fireflies, and the rest — demands access to your calendar, inserts a bot into every conversation, hosts your transcripts on their cloud, and charges a monthly fee to retain access to _your own data_.

Unacceptable.

recmeet was designed as a Python prototype in under two hours using local open-source models, proving that a single Linux machine can record, transcribe, and summarize any meeting without sending a byte off-box. It was then rewritten as a C++ binary anchored to three native-code inference engines (whisper.cpp + sherpa-onnx + llama.cpp), eliminating the Python/pip/venv dependency chain and producing a deployment that runs on hardware ranging from a 2015 server to a 2024 laptop.

The result: full transcriptions with speaker labels, professionally structured summaries with action items, output as standard Markdown files on your local filesystem. No subscriptions. No cloud uploads. No one between you and your data.

## What it does

- **Records** mic input and speaker output simultaneously via PipeWire/PulseAudio — captures both sides of any conversation regardless of platform
- **Transcribes** with whisper.cpp (tiny through large-v3 models, auto-downloaded on first use), with vocabulary hints to improve accuracy for names and domain-specific terms; auto-detects Vulkan GPU acceleration at build time (~4× real-time on whisper-medium / Radeon Pro W5500) with transparent CPU fallback
- **Identifies speakers** via sherpa-onnx diarization (Pyannote segmentation + 3D-Speaker embeddings), with cross-session speaker identification — enroll voices once, get real names in every transcript
- **Summarizes** via cloud API (xAI/OpenAI/Anthropic — all OpenAI-compatible) or a local GGUF model via llama.cpp — your choice
- **Outputs** timestamped transcripts, structured summaries, and meeting notes with YAML frontmatter (Obsidian-compatible)
- **System tray applet** for point-and-click control from swaybar or any system tray
- **Daemon architecture** — a background daemon owns all pipeline logic; the CLI and tray are thin IPC clients
- **MCP server** for IDE integration — exposes meeting data to Claude Code, Claude Desktop, and other MCP-compatible tools
- **AI agent CLI** for pre-meeting prep and post-meeting follow-up — searches past meetings, drafts briefings, and generates follow-up communications using Claude

## Local AI infrastructure

recmeet is anchored to four native-code inference engines linked into one C++ binary. Two engineering choices set the constraints that shape everything else: **a hard 16 GB host memory ceiling target** (4 hours of audio plus context notes, no OOM, no swap) and **a single-binary deployment** with GPU and per-ISA CPU compute backends loaded as `dlopen()` plugins — the same artifact runs on a 2015 server with SSE 4.2 and a 2024 laptop with AVX-512 and a Vulkan-capable GPU.

### Memory containment — 4 hours of audio on a 16 GB host

The target: process up to **4 hours of meeting audio plus context notes on a host with a hard 16 GB total memory ceiling**, end-to-end through transcription, diarization, speaker identification, and summarization, without OOM and without the operator pre-splitting the file. Validated end-to-end against a 60-minute real fixture under a `MemoryMax=8G` systemd cgroup gate.

Three load-bearing containment layers, each enforced independently:

1. **systemd cgroup caps** (`dist/recmeet-daemon.service.in`) — `MemoryHigh=10G`, `MemoryMax=14G`, `MemorySwapMax=0`. Cgroup-enforced by the kernel. Even an unrecoverable workload reaps only this unit, not the host.
2. **Subprocess isolation** — postprocessing runs as a `fork()`ed child. Crashes or cgroup hard-cap hits kill only the child. The daemon stays alive, the recorded audio is preserved, and the operator gets a precise error rather than a global SIGKILL.
3. **Chunked diarization** (next section) — when audio length exceeds ~17.5 minutes, the pipeline auto-engages a custom chunked algorithm so peak working set is bounded by chunk size rather than meeting length.

Below the chunk threshold the single-call path runs unchanged — short meetings pay no chunking overhead. The postprocessing subprocess also self-limits at 12 GB (`RECMEET_RSS_LIMIT_MB=12288`) as defense-in-depth behind the cgroup; on overflow it writes a precise error to stderr and exits cleanly without a deadlock or zombie.

### Long-audio diarization — chunked windows with centroid stitching

A custom algorithm built on top of sherpa-onnx that bounds per-chunk peak working set to ≤ 6 GB on 60-minute real audio (vs. ~11 GB working set for the equivalent un-chunked single-call path on the same fixture):

1. **Slice** the audio into ~15-minute windows with ~30-second overlap. Each chunk has a *core* (segment-ownership zone) and an *overlap* region (extra context across boundaries).
2. **Reuse one `DiarizeSession` and one `SpeakerEmbeddingSession`** across every chunk. sherpa's pyannote segmentation and 3D-Speaker embedding models (~45 MB combined) stay loaded for the whole run; only the cheap clustering object rebuilds per chunk via `set_clustering()`. Without session reuse, a 16-chunk run would reload ~720 MB of models — the chunking wall-clock budget falls apart.
3. **Extract one centroid per chunk-local speaker** with the shared embedding session.
4. **Stitch** chunk-local IDs into a global registry by cosine similarity on L2-normalized centroids (threshold `0.6` by default, matching the existing voiceprint match default). Centroids are stored *raw* (non-normalized) so the on-disk `MeetingSpeaker.embedding` byte-shape stays compatible with the single-call path.
5. **Own segments by midpoint-in-core, emit at full extent.** A boundary segment whose midpoint falls inside chunk *i*'s core is emitted by chunk *i* in full, even if its trailing edge spills into chunk *i+1*. The downstream `merge_speakers()` max-overlap rule absorbs the benign duplicate.
6. **Compact global IDs to `0..N-1` contiguous** after optional `num_speakers` ceiling enforcement. Without compaction a greedy-merge of `{0,1,2,3} → {0,1,3}` would surface as `Speaker_01, Speaker_02, Speaker_04` in transcripts.
7. **Bypass re-extraction in identify-speakers.** The chunked diarize has already produced one centroid per global cluster; the pipeline calls `identify_speakers_with_centroids(centroids, db, threshold)` instead of `identify_speakers(samples, ...)`. This skips the ~10 GB working-set spike the second extractor pass would otherwise cost on long audio.

Implementation in `src/diarize.cpp`; data-flow diagrams in `docs/ARCHITECTURE.md#diarization`.

### Inference stack at a glance

| Engine | Role | Format | Where it runs |
|---|---|---|---|
| **whisper.cpp** | Batch transcription | GGUF (75 MB – 3.1 GB) | CPU or Vulkan GPU |
| **sherpa-onnx** | Diarization (Pyannote segmentation + 3D-Speaker embeddings), voiceprint matching, Silero VAD, streaming Zipformer captions | ONNX | CPU |
| **llama.cpp** | Local LLM summarization | GGUF (~2 – 5 GB) | CPU |
| **onnxruntime** (vendored, built from source) | sherpa-onnx backend | shared library | CPU |

All four engines vendored and integrated at build time. No Python, no pip, no virtualenv. The main binary is C++; compute backends ship as `dlopen`-loaded `libggml-*.so` plugins so the same artifact runs across a wide ISA + GPU matrix without `DT_NEEDED` entries for any GPU library.

<details>
<summary><b>GPU plugin architecture — Vulkan + per-ISA CPU plugin discovery</b></summary>

`recmeet` builds with **Vulkan GPU acceleration** auto-detected at configure time whenever the host has the toolchain (`libvulkan` + headers + `glslc`). The build is a **single binary that uses GPU when available and silently falls back to CPU when not.** GPU backends ship as separate `libggml-vulkan.so` plugins loaded at startup; `ldd build/recmeet | grep vulkan` is empty even on a Vulkan-enabled build. Move the binary to a host with no GPU and it still runs.

The same plugin model carries per-ISA CPU variants. With `GGML_BACKEND_DL=ON` + `GGML_CPU_ALL_VARIANTS=ON`, ggml emits one CPU plugin per x86 ISA tier (`libggml-cpu-sse42.so`, `libggml-cpu-avx.so`, `libggml-cpu-avx2.so`, `libggml-cpu-skylakex.so`, `libggml-cpu-icelake.so`, and more — 10+ variants on a current Arch build). At startup each is scored against the host's CPU capability flags; the highest-supported variant wins. The same binary runs on a 2015 Sandy Bridge server and a 2024 Cooper Lake laptop.

Plugin discovery is deterministic. `recmeet::load_backends()` resolves the plugin directory from `/proc/self/exe`:

1. `$RECMEET_GGML_BACKEND_PATH` — test/dev override
2. `<exe-dir>/../lib` — production install layout (e.g. `~/.local/bin/recmeet` → `~/.local/lib/`)
3. `<exe-dir>/bin` — in-tree build layout (`build/recmeet` → `build/bin/`)
4. `<exe-dir>` — alternate co-located layout

We resolve the path ourselves rather than relying on ggml's built-in `$ORIGIN/../lib` macro because `dlopen()` does not expand `$ORIGIN` the way `ld.so` does for `RPATH` — ggml's default search ultimately falls through to a CWD scan (attacker-plantable). The compile-time `GGML_BACKEND_DIR=$ORIGIN/../lib` pin remains as defense in depth.

```bash
# Default — autodetect Vulkan if the toolchain is present, warn if missing, fall back to CPU
make build

# Required — fail configure if Vulkan can't be enabled
cmake -B build -G Ninja -DRECMEET_GGML_VULKAN=ON

# Force CPU-only (e.g. distro packagers shipping a universal artifact)
cmake -B build -G Ninja -DRECMEET_GGML_VULKAN=OFF
```

At daemon startup the **active-backend banner** prints on stderr (mirrored unconditionally, so it shows up under `journalctl --user -u recmeet-daemon.service` even at the default `error` log level):

    ggml: backend registry: CPU, Vulkan
    ggml: active backend: Vulkan (AMD Radeon Pro W5500 (RADV NAVI10), 8192 MB)

If a non-CPU backend registered but exposes zero devices (e.g. `libggml-vulkan.so` loaded on a host without a working Vulkan ICD), a `WARN` line surfaces the gap before showing the CPU-fallback active-backend line.

See [docs/BUILD.md](docs/BUILD.md#gpu-acceleration-vulkan) for the toolchain matrix, per-distro install hints, and the full plugin discovery flow.

</details>

<details>
<summary><b>Subprocess crash isolation and watchdog</b></summary>

Postprocessing runs as a `fork()`ed child of the daemon, not in-process. The child receives its config via a `--config-json` handoff (parent serializes, child reads), reports phase + progress events back over a stdout pipe as NDJSON, and writes the meeting note + speakers file before exiting.

This shape solves two real problems that long-audio postprocessing hits in production:

- **onnxruntime memory growth.** sherpa-onnx's underlying onnxruntime can leak working set across long-audio diarization runs. In-process this eventually OOMs the daemon. Forked, it eventually exits or the cgroup reaps it; the daemon stays alive.
- **onnxruntime thread-pool deadlock.** With sherpa-onnx threads uncapped on long audio, the onnxruntime thread pool can deadlock during diarization. Mitigated by capping sherpa-onnx to 4 threads, but the watchdog catches anything that slips through.

The watchdog is dual-timestamp:

- `last_progress_at` — updated on every progress event the child writes
- `last_heartbeat_at` — updated on every 10 s heartbeat the child's heartbeat thread writes

If `last_progress_at` lags by more than 300 s the daemon SIGTERMs the child (with SIGKILL escalation and cgroup-aware grace handling). Heartbeat-only liveness is explicitly *not* sufficient to suppress detection — that was the failure mode of the v1 watchdog that the iter-95 dual-timestamp rewrite fixed.

Defense in depth: the child also self-limits at `RECMEET_RSS_LIMIT_MB=12288` and writes a precise error to stderr on overflow. The cgroup at `MemoryMax=14G` is the real backstop; the self-limit is faster to fire under uncontended growth but stalls under uninterruptible kernel sleep.

</details>

<details>
<summary><b>Speaker identification — cross-session voiceprint matching</b></summary>

Diarization assigns generic `Speaker_01`, `Speaker_02`, … labels to clusters within a single recording. Speaker identification matches those clusters against a persistent voiceprint database so the same person gets their real name in every transcript.

**Auto-detect speaker count via context:** If your meeting context (typed in the pre-recording dialog, supplied via `--context-text`, or loaded from a saved `context.json` on reprocess) contains a line like:

```
Participants: Alice, Bob, Carol
```

recmeet will use the participant count (3 here) as the diarizer's target speaker count. This overrides the auto-detect default (capped at `max_auto_speakers`, default 8) but is itself overridden by explicit `--num-speakers N` on the CLI. Comma, ` and `, and ` & ` separators are all accepted; case is insensitive; multiple `Participants:` lines are summed.

When `--num-speakers N` is passed on the CLI, the count is enforced as **both ceiling and floor** — the diarize merge loop will neither over-create above N nor over-merge below N. Counts derived from the context's `Participants:` line are advisory (ceiling only).

**Embedding model:** sherpa-onnx 3D-Speaker `eres2net_base` — same model already loaded for diarization. No additional download required when speaker ID is enabled.

**Match algorithm:** cosine similarity on averaged speaker centroids. Each enrolled speaker has one or more 192-dimensional float embedding vectors stored as JSON; the in-memory `SpeakerEmbeddingManager` averages them internally. `GetBestMatches(mgr, embedding, threshold, 1)` returns the highest-scoring enrolled name above the threshold (`0.6` default, configurable via `--speaker-threshold`). Conflict resolution prevents two clusters from being assigned the same name — highest score wins.

**On-disk database:** `~/.local/share/recmeet/speakers/<Name>.json` — one file per person:

```json
{
  "name": "John",
  "created": "2026-03-08T10:00:00Z",
  "updated": "2026-03-09T14:30:00Z",
  "embeddings": [
    [0.12, -0.34, 0.56, ...],
    [0.11, -0.32, 0.58, ...]
  ]
}
```

Each embedding is ~2–4 KB. Multiple enrollments per speaker improve accuracy; sherpa-onnx averages them at match time.

**Feedback loop to transcription:** enrolled speaker names are automatically passed to whisper as `initial_prompt` vocabulary hints, biasing the decoder toward correct spellings. Enroll "John Suykerbuyk" once and whisper stops producing phonetic mangles like "John Seck-Rick" in every subsequent transcript.

Enroll from any past recording:

```bash
# Interactive — recmeet shows speakers with durations, you pick one
./build/recmeet --enroll "John" --from meetings/2026-03-08_14-30/

# Non-interactive — directly enroll speaker 1
./build/recmeet --enroll "John" --from meetings/2026-03-08_14-30/ --speaker 1
```

Test identification on a recording without modifying it:

```bash
./build/recmeet --identify meetings/2026-03-09_09-00/
```

Implementation in `src/speaker_id.cpp`; full data flow in `docs/ARCHITECTURE.md#speaker-identification`.

</details>

<details>
<summary><b>Vendored dependencies and build pipeline</b></summary>

The four inference engines are vendored via CMake `FetchContent`, integrated at build time, and shipped as part of the install set:

- **whisper.cpp + ggml** — `BUILD_SHARED_LIBS=ON` since v1.6.0; installs `libwhisper.so` + `libggml.so` + `libggml-base.so` to `<prefix>/lib/`
- **llama.cpp** — `BUILD_SHARED_LIBS=ON`; installs `libllama.so` to `<prefix>/lib/`
- **sherpa-onnx** — static lib (`libsherpa-onnx-c-api.so`), pulled from upstream `k2-fsa/sherpa-onnx@v1.12.27` with a small set of vendored patches (see `cmake/sherpa-onnx-patches/`)
- **onnxruntime** — *can* be built from source via `scripts/build-onnxruntime.sh` (~20 min on first build, see below); otherwise comes from the distro package

The onnxruntime build-from-source path solves a real problem on rolling-release distros: the system `onnxruntime-cpu` package can break unpredictably from protobuf version upgrades or GCC ABI changes (Arch's protobuf 33 → 34 jump produced SIGABRT during ONNX model parsing for several months). Building it locally with the host GCC and bundling protobuf internally eliminates both crash vectors.

The script also auto-patches onnxruntime 1.23.2 for GCC 15's stricter transitive-include behavior: `core/common/semver.h` no longer compiles cleanly because it relied on `<cstdint>` being pulled in by another header. The build script detects the missing include via `grep` and `sed`-injects `#include <cstdint>` after `#include "core/common/status.h"` before invoking the upstream `build.sh`. No operator action needed; the patch is documented so rolling-distro users on GCC 15+ know what the script is doing.

**Library split (iter 104) for thin-client deployment.** The C++ source compiles into two static libraries:

- `recmeet_ipc` — config, IPC client/server, util, NDJSON. **No ML deps.**
- `recmeet_core` — `recmeet_ipc` + the ML pipeline (whisper, sherpa-onnx, llama).

`recmeet-tray` links only `recmeet_ipc` — the tray applet does not pull in onnxruntime, whisper.cpp, or llama.cpp. This keeps the tray's binary size and resident memory low, and unblocks a future thin-client architecture (operator-host tray, compute on a separate daemon host).

**Other portability investigations:** `zig cc` + `musl` was investigated as a static-binary path (iter 100) and ruled out — libc++/libstdc++ ABI incompatibility, musl C++ exception handling bugs, no pre-built musl onnxruntime. Revisit when zig adds libstdc++ linking support (issue [#3936](https://github.com/ziglang/zig/issues/3936)).

</details>

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

2. **Transcribe**: whisper.cpp runs locally on CPU or Vulkan GPU, producing timestamped segments. Models are GGUF format, auto-downloaded from Hugging Face on first use (141 MB for `base`, up to 1.5 GB for `large-v3`). Vocabulary hints bias the decoder toward correct spellings of names and domain terms — enrolled speaker names are included automatically.

3. **Diarize** (optional, on by default): sherpa-onnx labels each segment with `Speaker_01`, `Speaker_02`, etc. using neural speaker embeddings and clustering. Configurable threshold for tuning speaker count detection. Long audio (over ~17 minutes at default settings) is automatically processed in overlapping chunks with shared diarize + embedding sessions and cosine-similarity centroid stitching, keeping peak memory bounded by chunk size rather than meeting length. See [Long-audio diarization](#long-audio-diarization--chunked-windows-with-centroid-stitching) above for the algorithm.

4. **Identify** (optional, on by default when speakers are enrolled): Matches diarization clusters against enrolled voiceprints using cosine similarity on 3D-Speaker embeddings. Enrolled speakers get their real names (`John`, `Alice`) instead of generic labels. Enroll speakers from past recordings with `recmeet --enroll "Name" --from meetings/DIR/`. See [Speaker identification](#local-ai-infrastructure) for the algorithm + database format.

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
./build/recmeet --model base --provider xai

# Switch providers
./build/recmeet --model base --provider openai
```

API keys are resolved in priority order: environment variable > per-provider entry in `config.yaml` > legacy `api_key` field.

### Local LLM (fully offline)

Uses llama.cpp to run a GGUF model on CPU. No API key, no network, no data leaves your machine.

```bash
# Download a GGUF model (e.g. from Hugging Face)
mkdir -p ~/.local/share/recmeet/models/llama/
# Place your .gguf file there, or use any path

# Run with local summarization
./build/recmeet --model base \
    --llm-model ~/.local/share/recmeet/models/llama/Qwen2.5-7B-Instruct-Q4_K_M.gguf
```

Or set it permanently in `~/.config/recmeet/config.yaml`:

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
~200–700 ms latency and are persisted as a `.vtt` sidecar alongside the
meeting audio.

Captions are **opt-in per recording** — they consume CPU that some users
want reserved for other workloads. The post-recording batch transcript
(produced by whisper) remains the authoritative record; live captions are
approximate and complementary.

### Enable

```bash
# CLI: opt in for this recording only
./build/recmeet --mic-only --show-captions

# CLI: choose a different streaming model (defaults to en-2023-06-26)
./build/recmeet --show-captions --caption-model en-small

# CLI: list cached + available streaming caption models
./build/recmeet --list-caption-models

# CLI: explicitly disable captions even if config has them on
./build/recmeet --no-captions
```

The tray applet has a **"Show Live Captions"** checkbox in its menu;
ticking it persists the preference in `~/.config/recmeet/config.yaml` and
opens a small caption overlay window during the next recording.

### Default model

`sherpa-onnx-streaming-zipformer-en-2023-06-26` (~74 MB int8,
Apache-2.0, English-only). Auto-downloaded on first use into
`~/.local/share/recmeet/models/sherpa/online/en-2023-06-26/`. The CLI / tray
prompts before downloading if the model isn't cached. A smaller
`en-small` variant is available via `--caption-model en-small`
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

### Source policy

In dual-mode recordings (both mic and monitor) live captions are wired to
the **monitor source** — the operator's own voice is recorded and
post-processed normally but does NOT appear in the live caption overlay.
This serves the hearing-comprehension use case (captioning remote speakers
with accents, hearing issues, or response latency). In `--mic-only`
recordings, captions fall back to the mic as a dictation preview.

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

## Output

### Directory structure

```
meetings/2026-02-20_14-30/
  audio_2026-02-20_14-30.wav      # Mixed mic + monitor (16kHz mono S16LE)
  context_2026-02-20_14-30.json   # Pre-recording context note (only if provided)
  speakers_2026-02-20_14-30.json  # Per-meeting speaker data (only if --diarize)
  captions.vtt                    # Live captions sidecar (only if --show-captions)
  Meeting_2026-02-20_14-30_Project_Kickoff.md  # Meeting note
```

Up to five files per meeting. The audio + meeting note are always present. The context file is written only when the user provided context (via the tray dialog, `--context-text`, or `--context-file`) — it persists the prompt across reprocess. The speakers file is written only when diarization runs. The captions sidecar is written only when live captions were enabled.

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

Filename includes an AI-derived title (e.g. `Meeting_2026-02-20_14-30_Project_Kickoff.md`). Contains YAML frontmatter with enriched metadata (date, time, type, domain, status, tags, participants, duration), a summary callout, action item checkboxes, and a foldable transcript section. Compatible with Obsidian Dataview.

## Cancelling a recording

For the common case where a recording was started but the meeting didn't happen (call not answered, test clip, wrong audio source), the tray exposes a **Cancel & Discard** action that stops the recording AND deletes its on-disk artifacts in one step. Normal **Stop Recording** is unchanged — it ends the capture and proceeds through transcription/diarization/summarization to a finished meeting note.

**How to invoke (tray).** While a recording is in progress the tray menu shows a **Cancel & Discard** item directly below **Stop Recording**. Click-to-confirm:

1. First click arms the item — the label flips to `Discard recording? click again to confirm` for 3 seconds.
2. Second click within that window sends the cancel verb to the daemon.
3. If you don't click again within 3 seconds, the label reverts to **Cancel & Discard** and nothing is sent.

On success, the tray fires a `Recording cancelled — discarded` notification and returns to idle.

**What gets deleted.** The entire `~/meetings/<timestamp>/` directory that the recording created — both raw captures (`mic.wav`, `monitor.wav`), the mixed `audio_<ts>.wav`, the live-captions `captions.vtt` if present, and any `context_<ts>.json` written by the daemon. The cancel branch in `run_recording` skips drain/write/validate entirely, so on-disk waste is minimal — only what the capture threads already flushed.

**Reprocess.** Cancel does not apply to reprocess: a `--reprocess` job has no live capture to abort, and signalling cancel from that context would point at the operator's source audio. The daemon refuses `record.cancel` during reprocess with `Cancel does not apply to reprocess; use record.stop target=postprocessing instead`. The tray hides the menu item during reprocess as defense-in-depth.

**Postprocessing.** To discard a job that has already left the recording loop and is in postprocessing, use the existing `record.stop` with `target=postprocessing` (existing behavior, unchanged). The tray's **Stop Recording** routes there automatically when the daemon is in the postprocessing phase.

**In-process CLI limitation.** `recmeet --mic-only` and similar standalone-mode invocations (no daemon) have no cancel surface — there is no IPC peer to receive `record.cancel`, and Ctrl-C fires the normal stop semantics. To discard a standalone recording, stop it with Ctrl-C and `rm -rf ~/meetings/<dir>/` the output directory manually. A CLI cancel surface (e.g. double-Ctrl-C or a `--cancel-on-interrupt` flag) is filed as a follow-up.

## Reprocess

Re-run the postprocessing pipeline (transcription, diarization, summarization, note generation) on an existing recording without re-recording. The audio file stays untouched; everything downstream is regenerated from the current model + config.

```bash
# Reprocess a single meeting directory
./build/recmeet --reprocess meetings/2026-02-21_17-34/ --num-speakers 2
```

Useful after upgrading whisper / diarization models, tweaking summary prompts, or recovering meetings whose original postprocessing failed (e.g. OOM on long audio before chunked diarization).

### Batch reprocess

To reprocess every meeting under a parent directory in one pass:

```bash
./build/recmeet --reprocess-batch ~/meetings/
```

Inspect what would be done first:

```bash
./build/recmeet --reprocess-batch ~/meetings/ --dry-run
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
./build/recmeet --reprocess-batch ~/meetings/ --diarize-chunk-minutes 12
```

This widens per-chunk RSS headroom at a small cost to per-chunk centroid quality — useful when reprocessing many long meetings back-to-back where library caches accumulate residual host overhead between iterations. The default of 15 min is the iter-121 quality/memory pick; 12 min is the documented stress-test value, not a regression. The chunked-diarize path is automatic — it engages whenever audio length exceeds `chunk_minutes·60 + chunk_overlap_sec + 120` seconds (≈17.5 min at default, ≈14.5 min at 12).

`--reprocess-batch` is mutually exclusive with `--reprocess`. To re-process a single meeting that already has a note, delete the note manually and re-run; v1 has no `--force` overwrite (frontmatter and manual body edits warrant a deliberate design — tracked as follow-up).

## Performance

All numbers below come from `scripts/bench-with-telemetry.sh` runs on the reference host. Per-bench artifacts (`amdgpu_top` JSONL stream, `vmstat`, raw sysfs CSV, and `summary.txt`) land in `bench-results/<workload>/` for direct inspection.

**Reference host:** AMD Radeon Pro W5500 (Navi14 / RDNA1 / 8 GB GDDR6 / Mesa RADV), 105 W TDP cap. Arch Linux, kernel 6.15.x. Vulkan-enabled `recmeet` build linked against locally-built onnxruntime 1.23.2.

### Whisper-medium transcription — Vulkan GPU

Real 47-minute meeting reprocessed via `recmeet --no-daemon --reprocess <wav> --model medium --no-summary --no-diarize`.

| Metric | Value |
|---|---|
| Audio length | 47 min |
| Wall-clock | 11.3 min |
| Real-time factor | **4.16× faster than real-time** |
| GPU shader activity (mean / peak) | **95.6% / 99%** |
| VRAM used (peak) | **4.1 GB / 8 GB** |
| Power draw (mean / peak) | 61 W / 108 W (58% of 105 W TDP avg) |
| Peak GPU temperature | 94 °C |
| Samples (1 Hz) | 674 |

Sustained 95.6% Shader Processor Interpolator activity is the headline: the GPU is being kept fed continuously, not waiting on data. VRAM peak well under the 8 GB ceiling leaves headroom for `whisper-large-v3` (~3.1 GB model) on the same hardware.

### Long-audio diarization under cgroup cap

The `[integration][slow][t2-1]` gate: chunked diarize end-to-end on a long-audio fixture, wrapped with `systemd-run --user --scope -p MemoryMax=8G -p MemorySwapMax=0` so the kernel SIGKILLs the bench process if the RSS cap is breached.

| Metric | Value |
|---|---|
| Wall-clock | 14.9 min |
| **Peak host RSS** | **4.4 GB** (44% of the 8 GB cgroup cap) |
| OOM kills | 0 |
| Swap consumption | 0 (cgroup forbids it) |
| GPU activity (mean) | 5.5% (CPU-bound workload) |
| VRAM used (peak) | 2.4 GB (desktop baseline; no GPU allocation by the bench) |

Memory containment intact: a long-audio chunked-diarize run that would have OOMed the un-chunked single-call path (≥ 11 GB peak working set) completes inside the cgroup with comfortable headroom. The 4-hours-on-16-GB target is the headline architectural claim; this run is the cgroup-enforced empirical validation.

### Benchmark suite — 12 cases, mixed CPU + GPU

| Metric | Value |
|---|---|
| Cases passed | 16 (2 skipped — uncached models, environmental) |
| Total wall-clock | 49 min |
| VRAM peak across suite | 7.6 GB (whisper-medium-on-Vulkan benchmark case) |
| GPU peak | 99% (during whisper-on-Vulkan cases) |
| GPU mean (whole suite) | 7.6% (most cases are CPU diarize / embedding-session parity benches) |
| Power peak | 111 W (brief headroom over the 105 W TDP) |

Five `[t2-0a]`/`[t2-0b]` parity benches in the suite verify that the session-reuse refactor (`DiarizeSession` + `SpeakerEmbeddingSession`) produces bit-identical output to the legacy direct-call path — the chunked-diarize correctness foundation.

### Cost in context

The CPU-only baseline for whisper-medium on the same 47-minute audio would be approximately 4–5 hours on this host (estimate from the iter-121 63-minute measurement of ~7 hours CPU). The 4.16× real-time GPU result represents ~22× speedup over the CPU baseline on this specific hardware, in line with the iter-121 validation that produced the ~26× number quoted in [docs/BUILD.md](docs/BUILD.md#gpu-acceleration-vulkan).

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

The install set includes the recmeet binaries, the vendored shared libraries (`libwhisper.so`, `libllama.so`, `libggml*.so`), the per-ISA CPU plugins (`libggml-cpu-*.so`), `libggml-vulkan.so` when Vulkan was enabled, `libonnxruntime.so.1` when built from source, and the systemd user units.

### Packages

```bash
make package-arch       # Arch Linux (split package: base + recmeet-vulkan companion)
make package-deb        # Debian/Ubuntu (.deb via CPack)
make package-rpm        # Fedora/RHEL (.rpm via CPack)
```

The Arch PKGBUILD is a split package — `recmeet-git` (always installed, CPU-capable, no GPU deps) plus an optional `recmeet-vulkan-git` companion that ships only `libggml-vulkan.so`. Missing ICD at runtime is a graceful fallback to CPU. See [docs/BUILD.md](docs/BUILD.md#packaging) for the full split-packaging mechanics and per-distro details.

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

The tray service is tied to `graphical-session.target` and restarts automatically on crash. On Sway, your config must activate that target — see [docs/BUILD.md](docs/BUILD.md) for details.

## CLI reference

The full CLI surface is large (64 flags across recording, reprocess, model management, daemon control, captioning, and speakers). The canonical reference is `recmeet --help`; the table below documents the full set for offline reading.

<details>
<summary><b>Full CLI flag table</b> (64 flags)</summary>

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
  --context-text TEXT  Inline pre-meeting context for summary
  --llm-model PATH     Local GGUF model for summarization (instead of API)
  --mmap               Use mmap for LLM model loading (faster load, may cause swap)
  --no-mmap            Disable mmap for LLM model loading (default, avoids swap thrashing)
  --no-diarize         Disable speaker diarization
  --num-speakers N     Number of speakers (0 = auto-detect, default: 0).
                       When passed explicitly on the CLI, N is enforced as
                       both ceiling and floor — the diarize merge loop will
                       neither over-create above N nor over-merge below N.
                       Counts derived from the context's `Participants:` line
                       are advisory (ceiling only).
                       For long audio that runs the chunked path, this is
                       enforced post-stitch as a global count limit via
                       sample-weighted greedy-merge (not per-chunk).
  --cluster-threshold F        Clustering distance threshold (default: 1.18; higher = fewer speakers)
  --diarize-chunk-minutes N    Chunked-diarize window (default: 15.0; auto-engages above ~17.5 min audio)
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
  --reprocess PATH     Reprocess existing recording directory or audio file
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
  --caption-model NAME  Streaming caption model name (default: en-2023-06-26)
  --list-caption-models  List available streaming caption models and exit
  --show-captions      Force-enable live captions for this recording
                       (V1: English only; ignored with --language != en)
  --no-captions        Force-disable live captions for this recording
  --progress-json      Emit machine-readable NDJSON progress on stdout (subprocess mode)
  --config-json FILE   Subprocess-mode config file (internal: parent-to-child handoff)
  -h, --help           Show this help
  -v, --version        Show version
```

### recmeet-daemon

```
Usage: recmeet-daemon [OPTIONS]

Run the recmeet daemon (IPC server for CLI and tray clients).

Options:
  --socket PATH        Unix socket path (default: $XDG_RUNTIME_DIR/recmeet/daemon.sock)
  --listen ADDRESS     Listen address: Unix socket path or host:port for TCP
  --log-level LEVEL    Log level: none, error, warn, info, debug (default: info)
  --log-dir DIR        Log file directory
  --log-retention HOURS  Hours of log history to keep (default: 4)
  -h, --help           Show this help
  -v, --version        Show version
```

</details>

## Configuration

`~/.config/recmeet/config.yaml` — all fields are optional, CLI flags override.

<details>
<summary><b>Full config schema</b></summary>

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

captions:
  enabled: false           # opt-in per recording or globally here
  model: "en-2023-06-26"   # default streaming Zipformer model
  # normalize_display: true  # lowercase + sentence-cap at render time (default)

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

web:
  port: 8384
  bind: "127.0.0.1"

logging:
  level: error # none, error, warn, info, debug
  # retention_hours: 4   # hours of log history to keep before rotation
  # directory: ~/.local/share/recmeet/logs/

general:
  threads: 0 # 0 = auto-detect (cores - 1)
```

</details>

## Build options

| Option                       | Default | Effect                                                                                        |
| ---------------------------- | ------- | --------------------------------------------------------------------------------------------- |
| `RECMEET_GGML_VULKAN`        | AUTO    | GPU acceleration via ggml Vulkan backend. AUTO probes; ON requires; OFF disables.             |
| `RECMEET_USE_SHERPA`         | ON      | Enable speaker diarization (fetches sherpa-onnx via CMake)                                    |
| `RECMEET_USE_LLAMA`          | ON      | Enable local LLM summarization via llama.cpp                                                  |
| `RECMEET_USE_NOTIFY`         | ON      | Enable desktop notifications via libnotify                                                    |
| `RECMEET_BUILD_TRAY`         | ON      | Build the system tray applet (requires GTK3 + AppIndicator)                                   |
| `RECMEET_BUILD_TESTS`        | ON      | Build the Catch2 test suite                                                                   |
| `RECMEET_BUILD_WEB`          | ON      | Build the `recmeet-web` speaker-management UI                                                 |
| `RECMEET_BUILD_GO_TOOLS`     | ON      | Build `recmeet-mcp` + `recmeet-agent` (requires Go 1.25+)                                     |
| `RECMEET_PATCH_SHERPA_ARENA` | ON      | Apply the vendored sherpa-onnx CPU memory arena patch (T1B memory containment fix)            |

```bash
# Example: headless build without tray, diarization, or Go tools
make RECMEET_BUILD_TRAY=OFF RECMEET_USE_SHERPA=OFF RECMEET_BUILD_GO_TOOLS=OFF

# Or manually:
# cmake -B build -G Ninja -DRECMEET_BUILD_TRAY=OFF -DRECMEET_USE_SHERPA=OFF -DRECMEET_BUILD_GO_TOOLS=OFF
```

## Testing

541 C++ unit test cases (2124 assertions) across 28 modules, plus 66 IPC integration cases (458 assertions), 16 reprocess-batch cases, 12 benchmark cases, 5 full-stack end-to-end cases, 1 long-audio cgroup integration gate, and a Go test suite covering the MCP server, agent CLI, and shared `meetingdata` library.

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

## Architecture

Four C++ binaries share two static libraries. The daemon owns all pipeline logic (audio capture, transcription, diarization, summarization). The CLI, tray, and web UI are thin IPC clients that communicate via a Unix domain socket (or TCP) using newline-delimited JSON. Two additional Go binaries provide AI-powered tooling.

```
recmeet_ipc   (static library — config, IPC, util — no ML deps)
recmeet_core  (static library — recmeet_ipc + ML pipeline)
    │
    ├── recmeet-daemon  (daemon — pipeline + IPC server)        ← links recmeet_core
    ├── recmeet         (CLI — dual-mode: client or standalone) ← links recmeet_core
    ├── recmeet-tray    (system tray — IPC client only)         ← links recmeet_ipc (no ML)
    ├── recmeet-web     (speaker-management web UI)             ← links recmeet_core
    └── recmeet_tests   (Catch2 test suite)                     ← links recmeet_core

tools/  (Go module — github.com/syketech/recmeet-tools)
    ├── recmeet-mcp    (MCP server — exposes meeting data to AI tools)
    └── recmeet-agent  (AI agent CLI — meeting prep + follow-up workflows)
```

### Daemon mode (recommended)

```
recmeet-daemon              recmeet (CLI)           recmeet-tray
      │                         │                        │
  [pipeline]  <── IPC ──>  [thin client]  <── IPC ──>  [thin client]
  [IPC server]             [auto-detect]              [GTK + g_io_watch]
      │
  Unix socket: $XDG_RUNTIME_DIR/recmeet/daemon.sock   (or TCP via --listen)
```

The CLI auto-detects a running daemon and operates as a client. Use `--no-daemon` to force standalone mode. The tray connects to the daemon and reconnects automatically with exponential backoff if the daemon restarts.

### IPC protocol

- **Transports**: Unix domain socket (default) or TCP — selected via `--listen` / `--daemon-addr`
- **Wire format**: Newline-delimited JSON (NDJSON)
- **Methods**: `record.start`, `record.stop`, `status.get`, `config.update`, `config.reload`, `sources.list`, `models.list`, `models.ensure`, `models.update`
- **Events** (server push): `phase`, `progress`, `state.changed`, `job.complete`, `model.downloading`, `caption`, `caption.degraded`

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full topology, state machine, IPC message types, and sequence diagrams. See [docs/BUILD.md](docs/BUILD.md) for the build system tutorial.

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

## Upstream contributions

recmeet vendors sherpa-onnx from upstream `k2-fsa/sherpa-onnx` at v1.12.27. Several fixes and CMake hygiene improvements developed in the course of recmeet's work have been submitted back upstream and accepted. Contributed by [@suykerbuyk](https://github.com/suykerbuyk):

| PR | Topic | Status |
|---|---|---|
| [k2-fsa/sherpa-onnx#3214](https://github.com/k2-fsa/sherpa-onnx/pull/3214) | v1.12.26 upgrade — `FetchContent_MakeAvailable` integration hooks | merged |
| [k2-fsa/sherpa-onnx#3215](https://github.com/k2-fsa/sherpa-onnx/pull/3215) | `wstring_convert` deprecation fix — manual UTF-8/UTF-32 codec with full RFC 3629 malformed-input handling | merged |
| [k2-fsa/sherpa-onnx#3216](https://github.com/k2-fsa/sherpa-onnx/pull/3216) | `FetchContent_Populate` deprecation fix for current CMake | merged |
| [k2-fsa/sherpa-onnx#3217](https://github.com/k2-fsa/sherpa-onnx/pull/3217) | CMake policy defaults for warning-free configure | merged |
| [k2-fsa/sherpa-onnx#3628](https://github.com/k2-fsa/sherpa-onnx/pull/3628) | `TopkIndex` off-by-one clamp + short-circuit (heap-buffer-overflow fix surfaced via ASAN) | under review |

## Project history

recmeet started as a Python prototype that proved the pipeline in a single session: `pw-record` and `parecord` for capture, `faster-whisper` for transcription, the `requests` library for Grok API calls. Within two hours the core concept was validated — a local Linux box could record, transcribe, and summarize any meeting without cloud dependencies.

The prototype also surfaced the key technical insight that shaped the architecture: `.monitor` sources (how PulseAudio exposes speaker output for recording) don't work with PipeWire's native `pw-record --target`. They produce silence. `parecord --device` works. This dual-capture strategy — PipeWire for the mic, PulseAudio for the monitor — carried through into the C++ rewrite.

The rewrite to C++ was motivated by deployment simplicity. The Python version required a virtualenv, pip-installed packages, system-site-packages for GTK bindings, and careful dependency management across machines. The C++ version compiles to one binary plus a small set of `dlopen`-loaded compute plugins. No Python runtime. No pip. No venv.

From there the project evolved through a sequence of named infrastructure pushes:

- **Doubling test coverage and extracting testable modules** (iters 1–10) — CLI parsing extraction, helper API promotion, 45 → 86 → 145+ tests.
- **Daemon architecture with Unix-socket IPC** (iter 42) — split the pipeline owner from its UI; CLI and tray became thin clients of a long-running compute daemon.
- **Vocabulary hints + cross-session speaker identification** (iters 56, 85) — 3D-Speaker voiceprint embeddings as a persistent on-disk DB, with the speaker-names → whisper-vocabulary-hints feedback loop closing the spelling-accuracy gap for unusual names.
- **Subprocess postprocessing with heartbeat watchdog** (iters 89–98) — forked-child execution so onnxruntime memory growth and thread-pool deadlocks no longer take down the daemon; dual-timestamp watchdog (progress vs. heartbeat) eliminates the false-negative class the v1 watchdog tripped on.
- **Vendored onnxruntime build pipeline + library split** (iters 99–104) — `scripts/build-onnxruntime.sh` with protobuf-bundled internally + GCC 15 transitive-include auto-patch eliminated the rolling-distro ABI-skew crash class. The `recmeet_ipc` / `recmeet_core` split unblocked thin-client deployment by removing ML deps from the tray.
- **Memory containment for 4-hour audio on 16 GB hosts** (iters 110–121) — the headline V1 target. Three-tier containment (cgroup caps + subprocess isolation + chunked-diarization algorithm with session reuse and centroid stitching), validated under `systemd-run --user --scope -p MemoryMax=8G`.
- **Live captioning V1 capstone** (iters 132–135, tag `v1.5.0`) — sherpa-onnx streaming Zipformer ASR worker, `.vtt` WebVTT sidecar, tray overlay + CLI stderr renderer, opt-in per recording. The V1 capstone before cutting the `v1-maintenance` branch.
- **Vulkan GPU acceleration with runtime-loadable backends** (iter 142, tag `v1.6.0`) — `GGML_BACKEND_DL=ON` + auto-detected Vulkan + per-ISA CPU plugin scoring at startup. Same binary runs on a 2015 server and a 2024 laptop with a Vulkan-capable GPU; 4.16× real-time on whisper-medium on the reference Radeon Pro W5500.

Several fixes from this work have been contributed back to upstream sherpa-onnx — see [Upstream contributions](#upstream-contributions) above. The full per-iteration narrative for those curious about the engineering archaeology lives in `agentctx/iterations.md`.

## License

Dual-licensed under [MIT and Apache 2.0](LICENSE), at your option.

Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
