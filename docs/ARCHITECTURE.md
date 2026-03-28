# Architecture

## Purpose

Recmeet records, transcribes, and summarizes meetings entirely on-device. Audio is captured via PipeWire/PulseAudio, transcribed with whisper.cpp, optionally diarized with sherpa-onnx, identified against enrolled voiceprints, and summarized either locally (llama.cpp) or through a cloud API. Everything runs on the user's machine — no audio or transcript data leaves the system unless the user explicitly configures a cloud summarization provider.

The system ships as three cooperating C++ binaries connected by a Unix socket IPC layer, plus a static library that contains all shared logic. Two additional Go binaries provide AI-powered meeting tooling — an MCP server for IDE integration and an agent CLI for automated meeting prep and follow-up.

## High-Level Architecture

```mermaid
graph TD
    CLI["recmeet<br/>(CLI)"]
    DAEMON["recmeet-daemon<br/>(IPC server)"]
    TRAY["recmeet-tray<br/>(system tray)"]
    CORE["recmeet_core<br/>(static library)"]
    SOCK["Unix socket<br/>$XDG_RUNTIME_DIR/recmeet/daemon.sock"]
    MCP["recmeet-mcp<br/>(MCP server)"]
    AGENT["recmeet-agent<br/>(AI agent CLI)"]
    MDATA["meetingdata<br/>(Go library)"]

    CLI -->|standalone| CORE
    CLI -->|client mode| SOCK
    TRAY --> SOCK
    SOCK --> DAEMON
    DAEMON --> CORE

    CORE --> PW["PipeWire"]
    CORE --> PA["PulseAudio"]
    CORE --> WHISPER["whisper.cpp"]
    CORE --> LLAMA["llama.cpp"]
    CORE --> SHERPA["sherpa-onnx"]
    CORE --> API["Cloud API<br/>(xAI / OpenAI / Anthropic)"]

    MCP --> MDATA
    AGENT --> MDATA
    AGENT --> CLAUDE["Anthropic API<br/>(Claude)"]
    AGENT --> BRAVE["Brave Search API<br/>(optional)"]
    MDATA -->|reads| NOTES["Meeting notes<br/>(Markdown files)"]
    MDATA -->|reads| SPKDB["Speaker DB<br/>(JSON files)"]
    MDATA -->|reads| CFG["config.yaml"]
```

## Build System and Binary Topology

CMake builds one static library (`recmeet_core`) from all shared sources, then links it into three C++ executables. Two additional Go binaries are built separately via `make go-build`.

| Target | Language | Source | Extra deps | Feature-gated |
|---|---|---|---|---|
| `recmeet` | C++ | `src/main.cpp` | — | — |
| `recmeet-daemon` | C++ | `src/daemon.cpp` | — | — |
| `recmeet-tray` | C++ | `src/tray.cpp` | GTK3, ayatana-appindicator3 | `RECMEET_BUILD_TRAY` |
| `recmeet-mcp` | Go | `tools/cmd/recmeet-mcp/main.go` | mcp-go | — |
| `recmeet-agent` | Go | `tools/cmd/recmeet-agent/main.go` | anthropic-sdk-go, cobra | — |

### Feature flags (CMake options)

| Flag | Default | Effect |
|---|---|---|
| `RECMEET_BUILD_TRAY` | ON | Build `recmeet-tray` |
| `RECMEET_USE_LLAMA` | ON | Link llama.cpp for local summarization |
| `RECMEET_USE_SHERPA` | ON | Link sherpa-onnx for diarization + VAD |
| `RECMEET_USE_NOTIFY` | ON | Link libnotify for desktop notifications |
| `RECMEET_BUILD_TESTS` | ON | Build Catch2 test suite |

### systemd units

| Unit | Type | Purpose |
|---|---|---|
| `recmeet-daemon.service` | simple | Runs the daemon, restarts on failure |
| `recmeet-daemon.socket` | socket | Socket activation at `%t/recmeet/daemon.sock` |
| `recmeet-tray.service` | simple | Runs the tray, `Wants=recmeet-daemon.service` |

## Binary: `recmeet` (CLI)

**Source:** `src/main.cpp`

The CLI operates in one of two modes, selected at startup:

1. **Client mode** — sends IPC requests to a running daemon.
2. **Standalone mode** — runs the full pipeline in-process (original single-binary behavior).

### Mode selection logic

```
if --daemon flag        → client mode (fail if daemon unreachable)
if --no-daemon flag     → standalone mode
if --status or --stop   → client mode (always)
else (auto)             → client mode if daemon_running(), otherwise standalone
```

In client mode, the CLI sends `record.start` with config overrides, installs a SIGINT handler that sends `record.stop`, and blocks on `read_events("job.complete")` until the daemon reports completion.

In standalone mode, the CLI runs `run_pipeline()` directly — model validation, audio capture, transcription, and summarization all happen in the same process.

## Binary: `recmeet-daemon`

**Source:** `src/daemon.cpp`

A long-running IPC server that owns the recording pipeline. Designed for headless or always-on operation under systemd.

### State machine

The daemon tracks a single atomic state with four values:

| State | Meaning |
|---|---|
| `Idle` | Ready to accept work |
| `Recording` | Audio capture in progress (worker thread) |
| `Postprocessing` | Transcription + summarization in progress (worker thread) |
| `Downloading` | Model download in progress (worker thread) |

State transitions use `compare_exchange_strong` — only one job runs at a time.

### Worker threads

Heavy work (recording, postprocessing, model downloads) runs on a detached `std::thread` (`g_worker`). The worker communicates results back to the poll thread via `server.post()`, which writes to a self-pipe to wake `poll()` and execute the callback on the main thread. This keeps all IPC I/O and broadcast calls single-threaded.

### PID locking

The daemon creates `<socket_path>.pid` and holds an `flock(LOCK_EX|LOCK_NB)` for its lifetime, preventing duplicate instances.

### Signal handling

| Signal | Behavior |
|---|---|
| `SIGHUP` | Reload config from disk via `server.post()` |
| `SIGINT` / `SIGTERM` | Request stop on active recording, then exit the poll loop |

## Binary: `recmeet-tray`

**Source:** `src/tray.cpp`

A GTK system tray applet using ayatana-appindicator. The tray is a pure IPC client — it never runs the pipeline directly.

### GTK + GIO integration

The tray wraps the IPC client's socket fd in a `GIOChannel` watched by the GTK main loop (`g_io_add_watch`). When the daemon pushes an event, `on_ipc_data()` fires, calls `ipc.read_and_dispatch(0)`, and the event callback updates the UI.

### Reconnection

On disconnect (`G_IO_HUP`), the tray tears down the watch, schedules `try_reconnect()` via `g_timeout_add_seconds`, and uses exponential backoff (1, 2, 4, 8, 16, 30, 30, ...) until the daemon reappears.

### Menu-driven config

The tray builds a GTK menu with radio groups for mic source, monitor source, whisper model, language, summary provider, and API model. Changes are persisted to `~/.config/recmeet/config.yaml` immediately. When the user selects a whisper model, the tray sends `models.ensure` to trigger a background download.

## IPC Protocol

### Wire format

Newline-delimited JSON (NDJSON) over a Unix stream socket at `$XDG_RUNTIME_DIR/recmeet/daemon.sock` (fallback: `/tmp/recmeet-<uid>/daemon.sock`).

### Message types

| Direction | Type | Discriminant field | Structure |
|---|---|---|---|
| client → server | Request | `"method"` | `{"id": N, "method": "...", "params": {...}}` |
| server → client | Response | `"result"` | `{"id": N, "result": {...}}` |
| server → client | Error | `"error"` | `{"id": N, "error": {"code": N, "message": "..."}}` |
| server → client | Event | `"event"` | `{"event": "...", "data": {...}}` |

### JSON value types

Values are `string | int64 | double | bool | null`. Nested objects/arrays are stored as raw JSON strings in the flat `JsonMap`.

### Methods

| Method | Params | Result | Notes |
|---|---|---|---|
| `status.get` | — | `{state}` | Returns current daemon state name |
| `sources.list` | — | `{sources, count}` | JSON array of audio sources |
| `config.reload` | — | `{ok}` | Re-read config from disk |
| `config.update` | config key/values | `{ok}` | Merge into running config |
| `record.start` | config overrides | `{ok}` | Idle → Recording; error if busy |
| `record.stop` | — | `{ok}` | Signal stop; error if not recording |
| `models.list` | — | `{models}` | JSON array of cached model info |
| `models.ensure` | `{whisper_model?}` | `{ok}` | Download missing models; Idle → Downloading |
| `models.update` | — | `{ok}` | Re-download all cached models |

### Events (server → all clients)

| Event | Data | When |
|---|---|---|
| `state.changed` | `{state, error?}` | Any state transition |
| `phase` | `{name}` | Pipeline phase change (recording, transcribing, etc.) |
| `job.complete` | `{note_path, output_dir}` | Recording + postprocessing finished |
| `model.downloading` | `{model, status, error?}` | Model download progress |

### Error codes

| Code | Name | Meaning |
|---|---|---|
| -32600 | InvalidRequest | Malformed JSON |
| -32601 | MethodNotFound | Unknown method |
| -32602 | InvalidParams | Bad parameters |
| -32603 | InternalError | Server-side failure |
| 1 | AlreadyRecording | — |
| 2 | NotRecording | `record.stop` when idle |
| 3 | Busy | State is not Idle |

### Concurrency model

The IPC server runs a single-threaded `poll()` loop. All socket reads, writes, and broadcasts happen on this thread. Worker threads marshal results back via `server.post()` + self-pipe wakeup, ensuring no concurrent access to client fd state.

## Recording Pipeline

The pipeline has two phases, split at the point where audio capture completes:

1. **`run_recording()`** — audio capture (blocking on `StopToken`), WAV output, validation, mixing.
2. **`run_postprocessing()`** — transcription, diarization, speaker identification, summarization, note output.

In standalone mode, `run_pipeline()` calls both sequentially. In daemon mode, the worker thread calls them separately so it can broadcast `state.changed` between phases.

```mermaid
flowchart TD
    subgraph "Phase 1: run_recording()"
        DETECT["Detect audio sources"]
        CAPTURE["PipeWire/PulseAudio capture<br/>(mic + optional monitor)"]
        STOP["StopToken signaled"]
        DRAIN["Drain buffers → WAV"]
        VALIDATE["Validate audio"]
        MIX["Mix mic + monitor"]
    end

    subgraph "Phase 2: run_postprocessing()"
        LOAD_AUDIO["Load audio file → float[]"]
        VAD["VAD segmentation<br/>(sherpa-onnx, optional)"]
        TRANSCRIBE["Whisper transcription"]
        DIARIZE["Speaker diarization<br/>(sherpa-onnx, optional)"]
        IDENTIFY["Speaker identification<br/>(voiceprint matching, optional)"]
        FREE_AUDIO["Free audio buffer"]
        SUMMARIZE["Summarize<br/>(llama.cpp or cloud API)"]
        NOTE["Write meeting note"]
    end

    DETECT --> CAPTURE --> STOP --> DRAIN --> VALIDATE --> MIX
    MIX --> LOAD_AUDIO --> VAD --> TRANSCRIBE --> DIARIZE
    DIARIZE --> IDENTIFY --> FREE_AUDIO --> SUMMARIZE --> NOTE
```

### Memory scoping strategy

The postprocessing phase uses nested scopes to minimize peak memory:

1. **Audio buffer scope** — `samples` vector is alive during transcription and diarization, freed before summarization.
2. **Whisper model scope** — the `WhisperModel` object is freed after transcription completes, before diarization begins.

This matters because whisper models (75 MB–1.5 GB) and audio buffers (16-bit, 16 kHz) can be large.

## Speaker Identification

Speaker identification matches diarization clusters against a persistent database of enrolled voiceprints, replacing generic `Speaker_XX` labels with real names across sessions.

### Architecture

The feature reuses the same 3D-Speaker embedding model (`eres2net_base`) already downloaded for diarization. No additional models are needed. The identification step runs inside the audio buffer scope, after diarization and before `merge_speakers()`.

**Source:** `src/speaker_id.h`, `src/speaker_id.cpp`

### Data flow

```mermaid
flowchart LR
    subgraph "Post-diarization (pipeline.cpp)"
        DIAR["diarize() returns<br/>DiarizeResult with<br/>N clusters"]
        LOAD_DB["Load speaker DB<br/>(~/.local/share/recmeet/speakers/)"]
        EXTRACT["For each cluster:<br/>extract centroid embedding<br/>(SpeakerEmbeddingExtractor)"]
        MATCH["Search enrolled speakers<br/>(SpeakerEmbeddingManager)<br/>cosine similarity ≥ threshold"]
        MAP["Build map&lt;int, string&gt;<br/>cluster_id → name"]
        MERGE["merge_speakers()<br/>uses name map"]
    end

    DIAR --> LOAD_DB --> EXTRACT --> MATCH --> MAP --> MERGE
```

### Enrollment flow

```mermaid
sequenceDiagram
    participant U as User
    participant CLI as recmeet --enroll
    participant D as Diarization
    participant E as Embedding Extractor
    participant DB as Speaker DB

    U->>CLI: --enroll "John" --from meetings/DIR/
    CLI->>CLI: Load audio file
    CLI->>D: diarize(samples)
    D-->>CLI: DiarizeResult (N speakers)

    alt Interactive mode (no --speaker)
        CLI-->>U: Show speakers with durations
        U->>CLI: Pick speaker number
    end

    CLI->>E: extract_speaker_embedding(samples, diar, speaker_id)
    E-->>CLI: float[] embedding vector

    CLI->>DB: Load existing profile (if any)
    CLI->>DB: Append embedding, save JSON
    CLI-->>U: "Enrolled 'John' (N embeddings total)"
```

### Speaker database

Each enrolled speaker is stored as a JSON file:

```
~/.local/share/recmeet/speakers/
├── John.json
├── Alice.json
└── Bob.json
```

**File format:**

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

Each embedding is a float vector (typically 192 dimensions for the eres2net model, ~2-4 KB per enrollment). Multiple embeddings per speaker improve accuracy — they are all registered with the sherpa-onnx `SpeakerEmbeddingManager`, which handles averaging internally during search.

### sherpa-onnx API usage

The speaker identification module uses two sherpa-onnx C APIs that are separate from the high-level diarization API:

| API | Purpose | Lifecycle |
|---|---|---|
| `SherpaOnnxSpeakerEmbeddingExtractor` | Extract embedding vectors from audio segments | Created per identification run |
| `SherpaOnnxSpeakerEmbeddingManager` | Register enrolled embeddings and search by cosine similarity | Created per identification run, populated from disk DB |

**Embedding extraction** feeds all audio segments belonging to a diarization cluster into a single `OnlineStream`, then calls `ComputeEmbedding()` to get the centroid vector.

**Speaker search** uses `GetBestMatches(mgr, embedding, threshold, 1)` to find the highest-scoring enrolled speaker above the similarity threshold. Conflict resolution ensures no two clusters are assigned the same enrolled name — the highest-scoring match wins.

### Configuration

| Config field | CLI flag | Default | Description |
|---|---|---|---|
| `speaker_id.enabled` | `--no-speaker-id` | `true` | Enable/disable identification |
| `speaker_id.threshold` | `--speaker-threshold` | `0.6` | Cosine similarity threshold |
| `speaker_id.database` | `--speaker-db` | `~/.local/share/recmeet/speakers/` | Database directory path |

### Integration with merge_speakers()

`merge_speakers()` accepts an optional `std::map<int, std::string>` mapping cluster IDs to enrolled names. For clusters with no match, it falls back to `format_speaker()` (`Speaker_XX`). This keeps the merge logic clean — identification is fully decoupled from label assignment.

```cpp
// Without speaker ID (original behavior)
result.segments = merge_speakers(result.segments, diar);
// → "Speaker_01: Hello"

// With speaker ID
auto names = identify_speakers(samples, diar, db, model_path, threshold);
result.segments = merge_speakers(result.segments, diar, names);
// → "John: Hello"
```

## Dependencies

### Vendored (compiled from source)

| Library | Purpose | CMake target |
|---|---|---|
| whisper.cpp | Speech-to-text transcription | `whisper` |
| llama.cpp | Local LLM summarization | `llama` (gated by `RECMEET_USE_LLAMA`) |
| sherpa-onnx | Speaker diarization, identification + VAD | `sherpa-onnx-c-api` (gated by `RECMEET_USE_SHERPA`) |

### Platform (pkg-config)

| Package | Purpose |
|---|---|
| `libpipewire-0.3` | Audio capture (primary) |
| `libpulse`, `libpulse-simple` | Monitor source fallback |
| `sndfile` | WAV read/write |
| `libcurl` | HTTP client (API calls, model downloads) |
| `libnotify` | Desktop notifications (optional) |
| `gtk+-3.0` | Tray UI (tray only) |
| `ayatana-appindicator3-0.1` | System tray indicator (tray only) |

### Runtime (not linked)

| Dependency | Purpose |
|---|---|
| PipeWire (running) | Audio routing |
| onnxruntime | sherpa-onnx backend (system package preferred) |

## Daemon State Machine

```mermaid
stateDiagram-v2
    [*] --> Idle

    Idle --> Recording : record.start<br/>(compare_exchange Idle→Recording)
    Idle --> Downloading : models.ensure / models.update<br/>(compare_exchange Idle→Downloading)

    Recording --> Postprocessing : StopToken signaled,<br/>run_recording() returns
    Recording --> Idle : pipeline error<br/>(catch block)

    Postprocessing --> Idle : run_postprocessing() completes<br/>(broadcasts job.complete)
    Postprocessing --> Idle : pipeline error<br/>(catch block)

    Downloading --> Idle : downloads complete
    Downloading --> Idle : download error

    note right of Recording : g_stop.request()<br/>via record.stop or SIGINT
```

## Lifecycle Diagrams

### Daemon-mode recording session

```mermaid
sequenceDiagram
    participant C as CLI / Tray
    participant D as Daemon (poll thread)
    participant W as Worker thread

    C->>D: record.start {params}
    D->>D: compare_exchange(Idle→Recording)
    D->>W: spawn worker
    D-->>C: {ok: true}
    D-->>C: event: state.changed {recording}

    W->>W: run_recording(cfg, stop)
    Note over W: Audio capture blocks on StopToken

    C->>D: record.stop
    D->>D: g_stop.request()
    D-->>C: {ok: true}

    W->>W: drain + validate + mix
    W->>D: post(state→Postprocessing)
    D-->>C: event: state.changed {postprocessing}

    W->>W: run_postprocessing(cfg, input)
    W->>D: post(broadcast job.complete)
    D-->>C: event: job.complete {note_path, output_dir}
    D-->>C: event: state.changed {idle}
```

### Standalone recording session

```mermaid
sequenceDiagram
    participant U as User
    participant CLI as recmeet (standalone)

    U->>CLI: recmeet [options]
    CLI->>CLI: Validate models (interactive prompts)
    CLI->>CLI: run_pipeline()
    Note over CLI: recording phase
    CLI->>CLI: PipeWire capture running
    U->>CLI: Ctrl+C (SIGINT)
    CLI->>CLI: StopToken signaled
    Note over CLI: postprocessing phase
    CLI->>CLI: Transcribe → Diarize → Identify → Summarize → Note
    CLI-->>U: "Done! Files in: ./meetings/..."
```

### Startup sequence

```mermaid
sequenceDiagram
    participant S as systemd
    participant D as recmeet-daemon
    participant T as recmeet-tray

    S->>D: ExecStart (recmeet-daemon.service)
    D->>D: flock(PID file)
    D->>D: Load config, resolve API key
    D->>D: Create IpcServer, bind socket
    D->>D: Install signal handlers
    D->>D: server.run() (poll loop)

    S->>T: ExecStart (recmeet-tray.service, After=daemon)
    T->>T: gtk_init, load config
    T->>T: Create AppIndicator
    T->>T: refresh_sources()
    T->>D: connect (Unix socket)
    T->>D: status.get
    D-->>T: {state: "idle"}
    T->>T: Sync UI state
    T->>T: fetch_provider_models() (background thread)
    T->>T: gtk_main()
```

### Shutdown sequence

```mermaid
sequenceDiagram
    participant S as systemd
    participant D as recmeet-daemon
    participant W as Worker thread

    S->>D: SIGTERM
    D->>D: g_stop.request()
    D->>D: server.stop() (write to self-pipe)
    D->>D: poll loop exits

    alt Worker is active
        D->>W: join (blocks until worker finishes)
        W->>W: StopToken causes early exit
        W-->>D: thread returns
    end

    D->>D: unlink PID file, close fds
    D->>D: unlink socket
    D->>D: log_shutdown, notify_cleanup
```

### Model download flow

```mermaid
sequenceDiagram
    participant C as CLI / Tray
    participant D as Daemon (poll thread)
    participant W as Worker thread

    C->>D: models.ensure {whisper_model: "small"}
    D->>D: compare_exchange(Idle→Downloading)
    D->>W: spawn worker
    D-->>C: {ok: true}

    W->>D: post(state.changed {downloading})
    D-->>C: event: state.changed {downloading}

    W->>W: is_whisper_model_cached("small")?
    alt Not cached
        W->>D: post(model.downloading {whisper/small, downloading})
        D-->>C: event: model.downloading {...}
        W->>W: ensure_whisper_model("small")
        W->>D: post(model.downloading {whisper/small, complete})
        D-->>C: event: model.downloading {...}
    end

    W->>D: post(state→Idle, state.changed {idle})
    D-->>C: event: state.changed {idle}
```

## Go Tools Module

The `tools/` directory contains a self-contained Go module (`github.com/syketech/recmeet-tools`) that provides AI-powered meeting tooling. It reads the same `config.yaml` and meeting output files as the C++ binaries but has no compile-time or runtime dependency on them.

### Module structure

```
tools/
├── go.mod                              # Module: github.com/syketech/recmeet-tools
├── cmd/
│   ├── recmeet-mcp/main.go            # MCP server entry point
│   └── recmeet-agent/main.go          # Agent CLI entry point
├── meetingdata/                         # Shared data access library
│   ├── config.go                       # Config parsing (matches C++ parser)
│   ├── meetings.go                     # Meeting directory discovery
│   ├── notes.go                        # Note parsing + search
│   ├── actionitems.go                  # Action item extraction
│   └── speakers.go                     # Speaker profile loading
├── mcpserver/                           # MCP tool implementations
│   ├── server.go                       # Server setup + registration
│   └── tools.go                        # Tool definitions + handlers
└── agent/                               # Agent internals
    ├── config.go                       # Agent-specific configuration
    ├── loop.go                         # Agentic loop (Claude API)
    ├── tools.go                        # Tool registry + definitions
    ├── workflows.go                    # Prep + follow-up workflows
    ├── search.go                       # Brave web search tool
    ├── fetch.go                        # Web page fetcher
    └── writefile.go                    # File writing tool
```

### Key dependencies

| Library | Purpose |
|---|---|
| `mark3labs/mcp-go` | Model Context Protocol server (stdio transport) |
| `anthropics/anthropic-sdk-go` | Claude API client for the agentic loop |
| `spf13/cobra` | CLI framework for the agent |
| `golang.org/x/net/html` | HTML parsing for web_fetch |

### `meetingdata` package

The shared data access layer. Both the MCP server and agent import this package to read meeting data from disk.

**Config loading** — Parses `~/.config/recmeet/config.yaml` using a line-based YAML parser that matches the C++ parser's behavior (flat sections with indented key-value pairs, not full YAML spec). Resolves `$XDG_CONFIG_HOME` and `$XDG_DATA_HOME` for paths.

**Meeting discovery** — Scans the output directory for directories matching `YYYY-MM-DD_HH-MM`, finds audio files (timestamped or legacy `audio.wav`), and locates corresponding note files across multiple directory structures (meeting dir, `YYYY/MM/` subdirs, note dir root).

**Note parsing** — Extracts YAML frontmatter, callout sections (summary, context, transcript using `> [!type]` syntax), and action items. Search supports keyword matching against title, summary, tags, and participants, with date range and participant filters.

**Action items** — Parsed from `## Action Items` sections (not inside callouts). Format: `- [ ] **[Assignee]** - description` or `- [x]` for completed items. Supports cross-meeting listing with status and assignee filters.

**Speaker profiles** — Loads JSON files from the speaker database directory. Strips embedding vectors before returning profiles (privacy — only name, creation date, update date, and embedding count are exposed).

### Binary: `recmeet-mcp` (MCP server)

**Source:** `tools/cmd/recmeet-mcp/main.go`

A Model Context Protocol server that exposes meeting data to AI tools (Claude Code, Claude Desktop, Cursor, and other MCP-compatible clients). Communicates over stdio using JSON-RPC.

**Critical implementation detail:** stdout is redirected to stderr at startup. MCP uses stdout exclusively for the JSON-RPC stream — any stray output (log messages, fmt.Println) would corrupt the protocol. All logging goes to stderr.

#### MCP tools

| Tool | Params | Description |
|---|---|---|
| `search_meetings` | `query`, `date_from`, `date_to`, `participants[]`, `limit` | Search notes by keyword, date range, participants |
| `get_meeting` | `meeting_dir` (required) | Full meeting details by directory name |
| `list_action_items` | `status`, `assignee`, `limit` | Action items filtered by status/assignee |
| `get_speaker_profiles` | — | List enrolled speaker profiles |
| `write_context_file` | `filename` (required), `content` (required) | Write pre-meeting context to staging dir |

All read tools are annotated as read-only and non-destructive. `write_context_file` sanitizes filenames to prevent directory traversal and rejects hidden files.

#### Data flow

```mermaid
sequenceDiagram
    participant Client as MCP Client<br/>(Claude Code / Desktop)
    participant MCP as recmeet-mcp<br/>(stdio)
    participant MD as meetingdata
    participant FS as Filesystem<br/>(meetings/, speakers/, config.yaml)

    Client->>MCP: tools/list (JSON-RPC)
    MCP-->>Client: 5 tool definitions

    Client->>MCP: tools/call search_meetings {query: "auth"}
    MCP->>MD: SearchNotes(noteDir, outputDir, "auth", filters)
    MD->>FS: Scan meeting dirs + parse notes
    FS-->>MD: Matching notes
    MD-->>MCP: []SearchResult
    MCP-->>Client: Formatted results (text/plain)
```

### Binary: `recmeet-agent` (AI agent CLI)

**Source:** `tools/cmd/recmeet-agent/main.go`

An AI agent CLI powered by Claude that automates meeting preparation and follow-up. Uses Cobra for CLI parsing and the Anthropic SDK for the agentic loop.

#### Commands

| Command | Args | Description |
|---|---|---|
| `prep` | `description` (positional, required) | Research past meetings and generate a briefing |
| `follow-up` | `note-path` (positional, required) | Read meeting notes and draft follow-up messages |

#### Tool registry

The agent exposes tools to Claude via the Anthropic tool-use API. Each tool implements a `Definition()` + `Execute()` interface.

| Tool | Source | Description |
|---|---|---|
| `search_meetings` | meetingdata | Search past meetings by keyword/date/participants |
| `get_meeting` | meetingdata | Get full meeting details by date (+ optional time) |
| `list_action_items` | meetingdata | List action items with status/assignee filters |
| `get_speaker_profiles` | meetingdata | List enrolled speakers |
| `web_search` | Brave API | Web search (only registered if `BRAVE_API_KEY` is set) |
| `web_fetch` | net/html | Fetch and extract text from a URL (10K char limit) |
| `write_file` | os | Write content to a file path |

#### Agentic loop

```mermaid
flowchart TD
    START["System prompt +<br/>user message"] --> CALL["Send to Claude API<br/>(with tool definitions)"]
    CALL --> CHECK{Stop reason?}
    CHECK -->|end_turn| DONE["Extract text response"]
    CHECK -->|tool_use| EXEC["Execute tool calls"]
    EXEC --> FEED["Append tool results<br/>as user message"]
    FEED --> CALL
    CHECK -->|max iterations| DONE
```

The loop runs up to 20 iterations (configurable). Each iteration sends the conversation history to Claude with the registered tools. If Claude returns `tool_use` blocks, the agent executes each tool, collects results, and feeds them back. The loop terminates when Claude returns `end_turn` or the iteration limit is reached.

**Verbose mode** (`--verbose`) logs each tool call and result to stderr for debugging.

**Dry-run mode** (`--dry-run`) prints the system prompt and user message without calling the API.

#### Prep workflow

```mermaid
sequenceDiagram
    participant U as User
    participant A as recmeet-agent prep
    participant C as Claude API
    participant T as Tools

    U->>A: prep "Sprint planning"<br/>--participants "Alice,Bob"
    A->>C: System: "You are a meeting prep assistant..."<br/>User: meeting description + participants
    C->>A: tool_use: search_meetings {participants: ["Alice","Bob"]}
    A->>T: Execute search_meetings
    T-->>A: Past meeting results
    A->>C: Tool results
    C->>A: tool_use: list_action_items {assignee: "Alice"}
    A->>T: Execute list_action_items
    T-->>A: Open action items
    A->>C: Tool results
    C->>A: tool_use: write_file {path: "...", content: briefing}
    A->>T: Execute write_file
    T-->>A: Written successfully
    A->>C: Tool results
    C->>A: end_turn: "Briefing written to ..."
    A-->>U: Output path
```

#### Follow-up workflow

```mermaid
sequenceDiagram
    participant U as User
    participant A as recmeet-agent follow-up
    participant C as Claude API
    participant T as Tools

    U->>A: follow-up meeting-note.md<br/>--my-name "John"
    A->>A: Parse note (frontmatter, summary, action items)
    A->>C: System: "You are a follow-up assistant..."<br/>User: note content + action items
    C->>A: tool_use: write_file {path: "followup-alice.md", content: draft}
    A->>T: Execute write_file
    T-->>A: Written
    A->>C: Tool results
    C->>A: end_turn: summary of drafted communications
    A-->>U: Result summary
```

#### Configuration

The agent reads the standard recmeet `config.yaml` for meeting paths, speaker DB location, and API keys. Agent-specific settings are resolved from environment variables and CLI flags:

| Setting | Source | Default |
|---|---|---|
| Anthropic API key | `ANTHROPIC_API_KEY` env var, then `api_keys.anthropic` in config | — (required) |
| Brave API key | `BRAVE_API_KEY` env var | — (optional, enables web_search) |
| Model | `--model` flag | `claude-sonnet-4-6` |
| Max iterations | hardcoded | 20 |
| Context staging dir | `$XDG_DATA_HOME/recmeet/context/` | `~/.local/share/recmeet/context/` |
