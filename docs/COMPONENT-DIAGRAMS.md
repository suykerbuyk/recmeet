# Component Interaction Diagrams

Detailed Mermaid diagrams documenting recmeet's architecture, component
interactions, and internal control flow. These diagrams reflect the actual
source code implementation, not aspirational design.

## Table of Contents

1. [Top-Level Component Interaction](#1-top-level-component-interaction)
2. [Build Topology and Library Dependencies](#2-build-topology-and-library-dependencies)
3. [Daemon Internals](#3-daemon-internals)
4. [IPC Server Poll Loop](#4-ipc-server-poll-loop)
5. [IPC Client Flow](#5-ipc-client-flow)
6. [IPC Protocol Wire Format](#6-ipc-protocol-wire-format)
7. [Recording Pipeline](#7-recording-pipeline)
8. [Postprocessing Pipeline](#8-postprocessing-pipeline)
9. [Tray Applet](#9-tray-applet)
10. [CLI Mode Selection](#10-cli-mode-selection)
11. [Subprocess Postprocessing](#11-subprocess-postprocessing)
12. [Audio Capture Subsystem](#12-audio-capture-subsystem)
13. [Go Tools Module](#13-go-tools-module)

---

## 1. Top-Level Component Interaction

All runtime communication paths between binaries, libraries, and external
systems. Solid lines are compile-time links; dashed lines are runtime IPC or
network calls.

```mermaid
graph TB
    subgraph "C++ Binaries"
        CLI["recmeet<br/>(CLI)"]
        DAEMON["recmeet-daemon<br/>(IPC server)"]
        TRAY["recmeet-tray<br/>(system tray)"]
        WEB["recmeet-web<br/>(REST API)"]
    end

    subgraph "Static Libraries"
        IPC["recmeet_ipc<br/>(config, IPC, util)"]
        CORE["recmeet_core<br/>(ML pipeline)"]
    end

    subgraph "Go Binaries"
        MCP["recmeet-mcp<br/>(MCP server)"]
        AGENT["recmeet-agent<br/>(AI agent CLI)"]
    end

    subgraph "Vendored C/C++ Deps"
        WHISPER["whisper.cpp"]
        LLAMA["llama.cpp"]
        SHERPA["sherpa-onnx"]
        ONNX["onnxruntime<br/>(shared lib)"]
        HTTPLIB["cpp-httplib"]
    end

    subgraph "System Libraries"
        PW["libpipewire"]
        PA["libpulse /<br/>libpulse-simple"]
        SNDFILE["libsndfile"]
        CURL["libcurl"]
        NOTIFY["libnotify"]
        GTK["GTK3"]
        APPIND["ayatana-<br/>appindicator3"]
    end

    subgraph "External Services"
        CLOUD["Cloud API<br/>(xAI / OpenAI / Anthropic)"]
        CLAUDE["Anthropic API<br/>(Claude)"]
        BRAVE["Brave Search API"]
    end

    subgraph "Data (Filesystem)"
        CONFIG["~/.config/recmeet/<br/>config.yaml"]
        MEETINGS["meetings/<br/>(WAV + notes)"]
        SPEAKERS["~/.local/share/recmeet/<br/>speakers/ (JSON)"]
        MODELS["~/.local/share/recmeet/<br/>models/"]
        LOGS["~/.local/share/recmeet/<br/>logs/"]
    end

    subgraph "IPC Transports"
        UNIX["Unix socket<br/>$XDG_RUNTIME_DIR/<br/>recmeet/daemon.sock"]
        TCP["TCP socket<br/>(configurable host:port)"]
    end

    %% Library links
    CORE -->|"links"| IPC
    CORE --> WHISPER
    CORE -.->|"if RECMEET_USE_LLAMA"| LLAMA
    CORE -.->|"if RECMEET_USE_SHERPA"| SHERPA
    SHERPA --> ONNX
    CORE --> PW
    CORE --> PA
    CORE --> SNDFILE
    IPC --> CURL
    IPC --> PA
    IPC -.->|"if RECMEET_USE_NOTIFY"| NOTIFY

    %% Binary links
    CLI -->|"links"| CORE
    DAEMON -->|"links"| CORE
    WEB -->|"links"| CORE
    WEB --> HTTPLIB
    TRAY -->|"links (thin client)"| IPC
    TRAY --> GTK
    TRAY --> APPIND

    %% IPC runtime paths
    CLI -.->|"client mode"| UNIX
    CLI -.->|"client mode"| TCP
    TRAY -.->|"always"| UNIX
    TRAY -.->|"remote"| TCP
    UNIX -.-> DAEMON
    TCP -.-> DAEMON

    %% Subprocess fork/exec
    DAEMON -.->|"fork/exec<br/>postprocessing"| CLI

    %% Data access
    CLI --> CONFIG
    CLI --> MEETINGS
    DAEMON --> CONFIG
    DAEMON --> MEETINGS
    DAEMON --> LOGS
    TRAY --> CONFIG
    WEB --> MEETINGS
    WEB --> SPEAKERS
    CORE --> MODELS
    CORE --> SPEAKERS

    %% Network
    CORE -.->|"summarize_http()"| CLOUD
    CORE -.->|"model download"| CURL
    AGENT -.-> CLAUDE
    AGENT -.-> BRAVE

    %% Go data access
    MCP --> MEETINGS
    MCP --> SPEAKERS
    MCP --> CONFIG
    AGENT --> MEETINGS
    AGENT --> SPEAKERS
    AGENT --> CONFIG
```

---

## 2. Build Topology and Library Dependencies

Exact source file inventory per CMake target and the link dependency graph.

```mermaid
graph TB
    subgraph "recmeet_ipc (static lib — no ML deps)"
        IPC_SRC["json_util.cpp<br/>api_models.cpp<br/>util.cpp<br/>log.cpp<br/>config.cpp<br/>notify.cpp<br/>device_enum.cpp<br/>http_client.cpp<br/>ipc_protocol.cpp<br/>config_json.cpp<br/>ipc_client.cpp<br/>ipc_server.cpp"]
    end

    subgraph "recmeet_core (static lib — ML pipeline)"
        CORE_SRC["audio_capture.cpp<br/>audio_monitor.cpp<br/>audio_file.cpp<br/>audio_mixer.cpp<br/>model_manager.cpp<br/>transcribe.cpp<br/>summarize.cpp<br/>note.cpp<br/>pipeline.cpp<br/>cli.cpp<br/>diarize.cpp<br/>speaker_id.cpp<br/>vad.cpp"]
    end

    subgraph "Executables"
        BIN_CLI["recmeet<br/>(main.cpp)"]
        BIN_DAEMON["recmeet-daemon<br/>(daemon.cpp)"]
        BIN_WEB["recmeet-web<br/>(web.cpp + httplib.cpp)"]
        BIN_TRAY["recmeet-tray<br/>(tray.cpp)"]
        BIN_TEST["recmeet_tests<br/>(tests/test_*.cpp × 26)"]
    end

    subgraph "Vendored"
        V_WHISPER["whisper.cpp<br/>(submodule)"]
        V_LLAMA["llama.cpp<br/>(submodule, gated)"]
        V_SHERPA["sherpa-onnx v1.12.27<br/>(FetchContent, gated)"]
        V_ONNX["onnxruntime<br/>(vendor/onnxruntime-local/)"]
        V_HTTPLIB["cpp-httplib<br/>(vendor/)"]
        V_CATCH["Catch2 v3.8.0<br/>(FetchContent)"]
    end

    subgraph "System (pkg-config)"
        SYS_PW["libpipewire-0.3"]
        SYS_PA["libpulse"]
        SYS_PAS["libpulse-simple"]
        SYS_SF["libsndfile"]
        SYS_CURL["libcurl"]
        SYS_NOTIFY["libnotify"]
        SYS_GTK["gtk+-3.0"]
        SYS_AI["ayatana-appindicator3"]
    end

    %% recmeet_ipc deps
    IPC_SRC --> SYS_PA
    IPC_SRC --> SYS_CURL
    IPC_SRC -.->|"RECMEET_USE_NOTIFY"| SYS_NOTIFY

    %% recmeet_core deps
    CORE_SRC -->|"PUBLIC link"| IPC_SRC
    CORE_SRC --> V_WHISPER
    CORE_SRC -.->|"RECMEET_USE_LLAMA"| V_LLAMA
    CORE_SRC -.->|"RECMEET_USE_SHERPA"| V_SHERPA
    V_SHERPA --> V_ONNX
    CORE_SRC --> SYS_PW
    CORE_SRC --> SYS_PAS
    CORE_SRC --> SYS_SF

    %% Binary links
    BIN_CLI --> CORE_SRC
    BIN_DAEMON --> CORE_SRC
    BIN_WEB --> CORE_SRC
    BIN_WEB --> V_HTTPLIB
    BIN_TRAY -->|"thin client"| IPC_SRC
    BIN_TRAY --> SYS_GTK
    BIN_TRAY --> SYS_AI
    BIN_TEST --> CORE_SRC
    BIN_TEST --> V_CATCH

    style BIN_TRAY fill:#e8f5e9,stroke:#2e7d32
    style IPC_SRC fill:#e3f2fd,stroke:#1565c0
    style CORE_SRC fill:#fff3e0,stroke:#e65100
```

---

## 3. Daemon Internals

### 3a. Daemon Startup Sequence

```mermaid
flowchart TD
    START["main(argc, argv)"] --> PARSE["Parse CLI args<br/>(--socket, --listen, --log-level)"]
    PARSE --> PID["Compute pid_path<br/>Unix: socket.pid<br/>TCP: daemon-tcp.pid"]
    PID --> FLOCK["open() + flock(LOCK_EX|LOCK_NB)"]
    FLOCK -->|"locked"| ABORT["stderr: 'Another instance running'<br/>return 1"]
    FLOCK -->|"acquired"| INIT["log_init()<br/>whisper_log_set(null)<br/>load_config()<br/>resolve_api_key()<br/>notify_init()"]
    INIT --> SELF["Resolve g_self_exe<br/>(/proc/self/exe → sibling 'recmeet')"]
    SELF --> SERVER["IpcServer server(socket_path)"]
    SERVER --> HANDLERS["Register method handlers:<br/>status.get, sources.list,<br/>config.reload, config.update,<br/>record.start, record.stop,<br/>job.context, speakers.*,<br/>models.list/ensure/update"]
    HANDLERS --> BIND["server.start()<br/>(bind + listen)"]
    BIND -->|"fail"| EXIT1["return 1"]
    BIND -->|"ok"| PPWORKER["Spawn g_pp_worker thread<br/>(pp_worker_loop — long-lived)"]
    PPWORKER --> SIGNALS["Install sigaction:<br/>SIGINT/SIGTERM → stop<br/>SIGHUP → reload config"]
    SIGNALS --> RUN["server.run()<br/>(blocks in poll loop)"]
    RUN --> SHUTDOWN["Shutdown:<br/>request all StopTokens<br/>SIGTERM child if alive<br/>g_queue_shutdown = true<br/>join all workers<br/>unlink pid + socket<br/>log_shutdown()"]
```

### 3b. Daemon State Machine

```mermaid
stateDiagram-v2
    [*] --> Idle

    Idle --> Recording : record.start (set g_recording)
    Idle --> Downloading : models.ensure (set g_downloading)

    Recording --> RecPP : run_recording() returns

    Recording --> Idle : pipeline error (clear g_recording)

    RecPP --> Postprocessing : rec_worker exits

    Recording --> ConcurrentRecPP : new record.start while PP active

    Postprocessing --> Idle : subprocess exits (clear g_postprocessing)

    Downloading --> Idle : complete or error (clear g_downloading)

    ConcurrentRecPP --> RecPP : run_recording() returns

    state RecPP {
        direction LR
        [*] --> handoff
        handoff : Atomic handoff under g_state_mu ―\ng_postprocessing = true THEN\ng_recording = false.\nNo transient idle broadcast.
    }

    state ConcurrentRecPP {
        direction LR
        [*] --> overlap
        overlap : g_recording = true\ng_postprocessing = true\n\nrecord.start guard checks\ng_recording or g_downloading\nbut NOT g_postprocessing\nso overlap is allowed.
    }

    note right of Recording
        Worker threads use lock_guard on g_state_mu
        for all multi-flag transitions.
        Bare atomic reads are fine without the mutex.
    end note
```

### 3c. Worker Thread Lifecycle

```mermaid
flowchart TB
    subgraph "rec_worker (one-shot per record.start)"
        RW_START["Spawned from record.start handler"]
        RW_RUN["run_recording(cfg, g_rec_stop, on_phase)"]
        RW_PHASE["on_phase callback:<br/>server.post(broadcast phase event)"]
        RW_OK{Success?}
        RW_ENQUEUE["Absorb pending context/vocab<br/>Enqueue PostprocessJob<br/>lock(g_state_mu):<br/>  g_postprocessing = true<br/>  g_recording = false<br/>g_queue_cv.notify_one()"]
        RW_ERR["lock(g_state_mu):<br/>  g_recording = false<br/>server.post(broadcast error state)"]

        RW_START --> RW_RUN
        RW_RUN --> RW_PHASE
        RW_RUN --> RW_OK
        RW_OK -->|"yes"| RW_ENQUEUE
        RW_OK -->|"exception"| RW_ERR
    end

    subgraph "pp_worker (long-lived, started at daemon init)"
        PP_WAIT["wait(g_queue_cv)<br/>until !queue.empty() || shutdown"]
        PP_SHUT{shutdown?}
        PP_DEQUEUE["Dequeue PostprocessJob"]
        PP_FLAG["if !already_flagged:<br/>  g_postprocessing = true<br/>  broadcast state"]
        PP_FORK["Fork/exec subprocess<br/>(see §11)"]
        PP_DONE["lock(g_state_mu):<br/>  g_postprocessing = false<br/>broadcast state"]

        PP_WAIT --> PP_SHUT
        PP_SHUT -->|"yes"| PP_EXIT["return"]
        PP_SHUT -->|"no"| PP_DEQUEUE
        PP_DEQUEUE --> PP_FLAG
        PP_FLAG --> PP_FORK
        PP_FORK --> PP_DONE
        PP_DONE --> PP_WAIT
    end

    subgraph "dl_worker (one-shot per models.ensure)"
        DL_START["Spawned from models.ensure handler"]
        DL_DOWNLOAD["Download models<br/>server.post(broadcast model events)"]
        DL_DONE["lock(g_state_mu):<br/>  g_downloading = false<br/>broadcast state"]

        DL_START --> DL_DOWNLOAD
        DL_DOWNLOAD --> DL_DONE
    end
```

### 3d. Signal Handling

```mermaid
flowchart LR
    SIG["Signal received"]
    SIG -->|"SIGHUP"| RELOAD["server.post(lambda:<br/>  load_config()<br/>  store under g_config_mu)"]
    SIG -->|"SIGINT / SIGTERM"| STOP["g_rec_stop.request()<br/>g_pp_stop.request()<br/>server.stop()<br/>(write 'X' to wakeup pipe)"]

    STOP --> POLL_EXIT["poll() returns →<br/>run() exits →<br/>shutdown sequence"]
```

---

## 4. IPC Server Poll Loop

```mermaid
flowchart TD
    RUN["server.run()"]
    BUILD["Build pollfd array:<br/>[0] wakeup_read_ (POLLIN)<br/>[1] listen_fd_ (POLLIN)<br/>[2..N] client fds (POLLIN)"]
    POLL["poll(fds, N, -1)<br/>(blocks indefinitely)"]
    EINTR{EINTR?}
    WAKE{"fds[0]<br/>wakeup?"}
    LISTEN{"fds[1]<br/>new conn?"}
    CLIENTS{"fds[2..N]<br/>client data?"}
    CHECK_RUN{running_?}

    RUN --> BUILD
    BUILD --> POLL
    POLL --> EINTR
    EINTR -->|"yes"| BUILD
    EINTR -->|"no"| WAKE

    WAKE -->|"POLLIN"| DRAIN["drain_wakeup()<br/>(read all bytes, discard)"]
    DRAIN --> POSTED["run_posted():<br/>swap posted_ under lock<br/>execute all lambdas"]
    POSTED --> CHECK_RUN
    CHECK_RUN -->|"false"| EXIT["return from run()"]
    CHECK_RUN -->|"true"| LISTEN
    WAKE -->|"no event"| LISTEN

    LISTEN -->|"POLLIN"| ACCEPT["accept_client():<br/>accept(), O_NONBLOCK<br/>TCP: TCP_NODELAY + SO_KEEPALIVE<br/>clients_[fd] = {}"]
    ACCEPT --> CLIENTS
    LISTEN -->|"no event"| CLIENTS

    CLIENTS -->|"POLLIN/HUP/ERR"| HANDLE["handle_client_data(fd)"]
    HANDLE --> READ_LOOP

    subgraph "handle_client_data(fd)"
        READ_LOOP["read(fd, buf, 4096)"]
        READ_LOOP -->|"n ≤ 0"| REMOVE["remove_client(fd):<br/>close(fd)<br/>clients_.erase(fd)"]
        READ_LOOP -->|"n > 0"| APPEND["Append to client read_buf"]
        APPEND --> NEWLINE{"Contains \\n?"}
        NEWLINE -->|"yes"| EXTRACT["Extract line up to \\n"]
        EXTRACT --> PARSE["parse_ipc_message(line)"]
        PARSE -->|"not Request"| SEND_ERR["send InvalidRequest error"]
        PARSE -->|"Request"| LOOKUP["Look up handlers_[method]"]
        LOOKUP -->|"not found"| SEND_ERR2["send MethodNotFound error"]
        LOOKUP -->|"found"| INVOKE["Invoke handler(req, resp, err)"]
        INVOKE -->|"true"| SEND_RESP["send_to(fd, serialize(resp))"]
        INVOKE -->|"false"| SEND_ERR3["send_to(fd, serialize(err))"]
        SEND_RESP --> NEWLINE
        SEND_ERR --> NEWLINE
        SEND_ERR2 --> NEWLINE
        SEND_ERR3 --> NEWLINE
        NEWLINE -->|"no"| DONE_CLIENT["done with this fd"]
    end

    CLIENTS --> BUILD

    subgraph "post() (called from worker threads)"
        POST_CALL["server.post(fn)"]
        POST_LOCK["lock(post_mu_)<br/>posted_.push_back(fn)"]
        POST_WAKE["write(wakeup_write_, 'P', 1)"]
        POST_CALL --> POST_LOCK --> POST_WAKE
    end

    subgraph "broadcast() (poll thread only)"
        BC_CALL["server.broadcast(event)"]
        BC_WIRE["wire = serialize(event) + '\\n'"]
        BC_SNAP["Snapshot client fd list"]
        BC_SEND["for each fd: send_to(fd, wire)"]
        BC_CALL --> BC_WIRE --> BC_SNAP --> BC_SEND
    end
```

---

## 5. IPC Client Flow

### 5a. Connection

```mermaid
flowchart TD
    CONNECT["client.connect()"]
    CONNECTED{"fd_ >= 0?"}
    CONNECTED -->|"yes"| OK["return true<br/>(already connected)"]
    CONNECTED -->|"no"| TRANSPORT{"addr_.transport?"}

    TRANSPORT -->|"Unix"| UNIX["socket(AF_UNIX)<br/>connect(sock_addr)"]
    UNIX -->|"ok"| FD["fd_ = socket"]
    UNIX -->|"fail"| FAIL["return false"]

    TRANSPORT -->|"TCP"| TCP_SOCK["socket(AF_INET)<br/>set O_NONBLOCK"]
    TCP_SOCK --> TCP_CONN["connect() → EINPROGRESS"]
    TCP_CONN --> TCP_POLL["poll(POLLOUT, 5000 ms)"]
    TCP_POLL -->|"timeout"| TCP_FAIL["close, return false"]
    TCP_POLL -->|"ready"| TCP_CHECK["getsockopt(SO_ERROR)"]
    TCP_CHECK -->|"error"| TCP_FAIL
    TCP_CHECK -->|"0"| TCP_OPTS["Restore blocking mode<br/>TCP_NODELAY<br/>SO_KEEPALIVE 30s/10s/3"]
    TCP_OPTS --> FD

    FD --> OK2["return true"]
```

### 5b. Blocking RPC (`call()`)

```mermaid
flowchart TD
    CALL["client.call(method, params, timeout)"]
    SEND["Serialize IpcRequest<br/>write(fd_, json + '\\n')"]
    SETUP["pending_id_ = req.id<br/>pending_done_ = false"]

    LOOP{"pending_done_?"}
    TIMEOUT_CHECK{"remaining > 0?"}
    RAD["read_and_dispatch(remaining_ms)"]
    RAD_OK{return value?}

    CALL --> SEND --> SETUP --> LOOP
    LOOP -->|"yes"| RESULT
    LOOP -->|"no"| TIMEOUT_CHECK
    TIMEOUT_CHECK -->|"no"| TIMEOUT["return InternalError<br/>'Timeout'"]
    TIMEOUT_CHECK -->|"yes"| RAD
    RAD --> RAD_OK
    RAD_OK -->|"false (disconnect)"| LOST["return InternalError<br/>'Connection lost'"]
    RAD_OK -->|"true"| LOOP

    RESULT{"pending_result_.type?"}
    RESULT -->|"Response"| RESP["resp = result<br/>return true"]
    RESULT -->|"Error"| ERR["err = result<br/>return false"]
```

### 5c. `read_and_dispatch()` Internal

```mermaid
flowchart TD
    ENTRY["read_and_dispatch(timeout_ms)"]
    DRAIN1{"read_buf_<br/>contains \\n?"}
    DRAIN1 -->|"yes"| PROCESS1["Extract line<br/>process_line()"]
    PROCESS1 --> CHECK1{"pending_done_?"}
    CHECK1 -->|"yes"| RET_TRUE["return true"]
    CHECK1 -->|"no"| DRAIN1
    DRAIN1 -->|"no"| FD_CHECK{"fd_ >= 0?"}
    FD_CHECK -->|"no"| RET_FALSE["return false"]
    FD_CHECK -->|"yes"| POLL["poll(fd_, POLLIN, timeout)"]
    POLL -->|"ret = 0"| RET_TRUE2["return true (timeout)"]
    POLL -->|"ret < 0"| RET_FALSE2["return false (error)"]
    POLL -->|"HUP/ERR"| RET_FALSE3["return false"]
    POLL -->|"POLLIN"| READ["read(fd_, buf, 4096)"]
    READ -->|"n ≤ 0"| CLOSE["close_connection()<br/>return false"]
    READ -->|"n > 0"| APPEND["Append to read_buf_"]
    APPEND --> DRAIN2{"Contains \\n?"}
    DRAIN2 -->|"yes"| PROCESS2["Extract line<br/>process_line()"]
    PROCESS2 --> DRAIN2
    DRAIN2 -->|"no"| RET_TRUE3["return true"]
```

### 5d. `process_line()` Dispatch

```mermaid
flowchart TD
    LINE["process_line(line)"]
    PARSE["parse_ipc_message(line)"]
    PARSE -->|"parse fail"| SKIP["return (skip)"]
    PARSE -->|"Event"| EV_CHECK{"until_event_<br/>matches?"}
    EV_CHECK -->|"yes"| MATCH["event_matched_ = true"]
    EV_CHECK -->|"no"| CB_CHECK{"event_cb_?"}
    MATCH --> CB_CHECK
    CB_CHECK -->|"yes"| CB_CALL["event_cb_(event)"]
    CB_CHECK -->|"no"| DONE["return"]
    CB_CALL --> DONE

    PARSE -->|"Response/Error"| PENDING{"pending_id_ > 0<br/>and id matches?"}
    PENDING -->|"yes"| STORE["pending_result_ = msg<br/>pending_done_ = true"]
    PENDING -->|"no"| DONE
```

---

## 6. IPC Protocol Wire Format

```mermaid
flowchart LR
    subgraph "Message Discrimination"
        MSG["JSON object"]
        MSG -->|"has 'method' key"| REQ["Request<br/>{id, method, params}"]
        MSG -->|"has 'result' key"| RESP["Response<br/>{id, result}"]
        MSG -->|"has 'error' key"| ERR["Error<br/>{id, error: {code, message}}"]
        MSG -->|"has 'event' key"| EV["Event<br/>{event, data}"]
    end

    subgraph "Value Types (JsonVal)"
        direction TB
        V_STR["string"]
        V_INT["int64_t"]
        V_DBL["double"]
        V_BOOL["bool"]
        V_NULL["null (monostate)"]
    end

    subgraph "Transport"
        direction TB
        T_UNIX["Unix socket<br/>$XDG_RUNTIME_DIR/<br/>recmeet/daemon.sock"]
        T_TCP["TCP socket<br/>host:port"]
        T_NDJSON["Wire: NDJSON<br/>(one JSON object per line)"]
    end

    subgraph "Address Parsing (parse_ipc_address)"
        ADDR["Input string"]
        ADDR -->|"empty"| DEFAULT["Unix: default_socket_path()"]
        ADDR -->|"host:port<br/>(digits after last ':')"| TCP_ADDR["TCP: {host, port}"]
        ADDR -->|"multiple ':'<br/>(bare IPv6)"| REJECT["Rejected"]
        ADDR -->|"path string"| UNIX_ADDR["Unix: path"]
    end
```

---

## 7. Recording Pipeline

`run_recording()` control flow with dual-capture and reprocess branching.

```mermaid
flowchart TD
    ENTRY["run_recording(cfg, stop, on_phase)"]
    MODE{"cfg.reprocess_dir<br/>non-empty?"}

    MODE -->|"yes"| REPROC_RESOLVE["Resolve path<br/>(absolute or relative to output_dir)"]
    REPROC_RESOLVE --> REPROC_VALIDATE["validate_reprocess_input(path)<br/>→ pp.audio_path"]
    REPROC_VALIDATE --> REPROC_DIR["pp.out_dir = output_dir_explicit ?<br/>  cfg.output_dir : parent(audio)"]
    REPROC_DIR --> REPROC_MKDIR["fs::create_directories(pp.out_dir)"]
    REPROC_MKDIR --> RETURN["Return PostprocessInput"]

    MODE -->|"no"| DETECT{"cfg.mic_source<br/>empty?"}
    DETECT -->|"yes"| AUTO["detect_sources(device_pattern)<br/>→ mic + monitor"]
    DETECT -->|"no"| USE_CFG["Use cfg.mic_source,<br/>cfg.monitor_source"]
    AUTO -->|"no mic"| THROW["throw DeviceError"]
    AUTO --> DUAL_CHECK
    USE_CFG --> DUAL_CHECK

    DUAL_CHECK{"dual_mode?<br/>(!mic_only && monitor)"}
    DUAL_CHECK -->|"no"| SINGLE

    DUAL_CHECK -->|"yes"| DUAL

    subgraph DUAL["Dual Capture Mode"]
        D_NOTIFY["notify('Recording started')"]
        D_MIC["PipeWireCapture mic(mic_source)<br/>mic.start()"]
        D_MON_CHECK{"source ends<br/>with .monitor?"}
        D_MON_CHECK -->|"yes"| D_MON_PA["PulseMonitorCapture(source)"]
        D_MON_CHECK -->|"no"| D_MON_PW["try PipeWireCapture(source, sink=true)"]
        D_MON_PW -->|"RecmeetError"| D_MON_PA
        D_MON_PW -->|"ok"| D_TIMER
        D_MON_PA --> D_TIMER

        D_TIMER["Spawn timer_thread<br/>(display_elapsed on stderr)"]
        D_LOOP["while !stop.stop_requested()<br/>  sleep_for(200ms)"]
        D_STOP["timer_stop → join timer<br/>mic.stop() → mon.stop()"]
        D_DRAIN["mic_samples = mic.drain()<br/>mon_samples = mon.drain()"]
        D_WRITE["Write mic.wav + monitor.wav"]
        D_VAL_MIC["validate_audio(mic.wav, 1.0s)<br/>→ fatal on fail"]
        D_VAL_MON["validate_audio(monitor.wav)<br/>→ non-fatal"]
        D_VAL_MON -->|"ok"| D_MIX["mix_audio(mic, mon)<br/>→ write audio_YYYY-MM-DD_HH-MM.wav"]
        D_VAL_MON -->|"AudioValidationError"| D_MIC_ONLY["Write mic-only<br/>→ audio_YYYY-MM-DD_HH-MM.wav"]
        D_MIX --> D_CLEANUP
        D_MIC_ONLY --> D_CLEANUP
        D_CLEANUP["if !keep_sources: remove raw WAVs"]

        D_NOTIFY --> D_MIC --> D_MON_CHECK
        D_TIMER --> D_LOOP --> D_STOP --> D_DRAIN
        D_DRAIN --> D_WRITE --> D_VAL_MIC --> D_VAL_MON
    end

    subgraph SINGLE["Single Mic Mode"]
        S_MIC["PipeWireCapture(mic_source)<br/>start()"]
        S_TIMER["Spawn timer_thread"]
        S_LOOP["while !stop.stop_requested()<br/>  sleep_for(200ms)"]
        S_STOP["stop() → drain()"]
        S_WRITE["Write audio_YYYY-MM-DD_HH-MM.wav"]

        S_MIC --> S_TIMER --> S_LOOP --> S_STOP --> S_WRITE
    end

    DUAL --> RETURN
    SINGLE --> RETURN
```

---

## 8. Postprocessing Pipeline

`run_postprocessing()` with memory scoping, cancellation, and all ML stages.

```mermaid
flowchart TD
    ENTRY["run_postprocessing(cfg, input,<br/>on_phase, on_progress, stop)"]

    PROMPT["Build initial_prompt:<br/>enrolled speaker names +<br/>cfg.vocabulary → comma-separated"]

    SKIP_TX{"input.transcript_text<br/>non-empty?"}

    ENTRY --> PROMPT --> SKIP_TX
    SKIP_TX -->|"yes"| CONTEXT
    SKIP_TX -->|"no"| AUDIO_OPEN

    subgraph AUDIO_SCOPE ["Audio Buffer Scope (freed before summarization)"]
        AUDIO_OPEN["samples = read_wav_float(audio_path)<br/>← vector&lt;float&gt; allocated"]

        subgraph WHISPER_SCOPE ["Whisper Model Scope (freed before diarization)"]
            MODEL_LOAD["model_path = ensure_whisper_model(cfg)<br/>WhisperModel model(model_path)"]

            VAD_CHECK{"SHERPA &&<br/>cfg.vad?"}
            VAD_CHECK -->|"yes"| VAD
            VAD_CHECK -->|"no"| TX_DIRECT

            subgraph VAD["VAD + Segmented Transcription"]
                VAD_PHASE["phase('detecting speech')"]
                VAD_RUN["detect_speech(samples, vad_cfg, threads)<br/>→ VadResult with segments"]
                TX_PHASE["phase('transcribing')"]
                TX_WEIGHTS["Compute seg_samples[] for<br/>weighted progress"]
                TX_LOOP["For each VAD segment:"]
                TX_CANCEL["check_cancel()"]
                TX_OPTS["TranscribeOptions:<br/>  initial_prompt, language,<br/>  on_progress = vad_weighted_progress()"]
                TX_CALL["transcribe(model, &samples[start],<br/>  n_samples, offset, opts)"]
                TX_APPEND["Append segments to result"]

                VAD_PHASE --> VAD_RUN --> TX_PHASE --> TX_WEIGHTS
                TX_WEIGHTS --> TX_LOOP --> TX_CANCEL --> TX_OPTS --> TX_CALL --> TX_APPEND
                TX_APPEND -->|"next segment"| TX_LOOP
            end

            subgraph TX_DIRECT["Direct Transcription (no VAD)"]
                TXD_PHASE["phase('transcribing')"]
                TXD_CALL["result = transcribe(model,<br/>  samples, n, 0.0, opts)"]
                TXD_PHASE --> TXD_CALL
            end

            AUDIO_OPEN --> MODEL_LOAD --> VAD_CHECK
        end

        WHISPER_FREE["← WhisperModel destructor:<br/>  whisper_free() called<br/>  (75 MB – 1.5 GB freed)"]

        DIAR_CHECK{"SHERPA &&<br/>cfg.diarize &&<br/>segments?"}
        DIAR_CHECK -->|"no"| AUDIO_FREE
        DIAR_CHECK -->|"yes"| DIAR

        subgraph DIAR["Diarization + Speaker ID"]
            DIAR_CANCEL["check_cancel()"]
            DIAR_PHASE["phase('diarizing')"]
            DIAR_RUN["diarize(samples, n, num_speakers,<br/>  threads, threshold, progress_cb)"]
            DIAR_DONE["→ DiarizeResult"]

            SPKID_CHECK{"cfg.speaker_id?"}
            SPKID_CHECK -->|"yes"| SPKID
            SPKID_CHECK -->|"no"| MERGE_NOID

            subgraph SPKID["Speaker Identification"]
                SPKID_DB["Load speaker DB"]
                SPKID_MODELS["ensure_sherpa_models()<br/>→ embedding model"]
                SPKID_PHASE["phase('identifying speakers')"]
                SPKID_RUN["identify_speakers(samples, diar,<br/>  db, model, threshold, threads)"]
                SPKID_SAVE["save_meeting_speakers(out_dir,<br/>  speakers with durations)"]

                SPKID_DB --> SPKID_MODELS --> SPKID_PHASE
                SPKID_PHASE --> SPKID_RUN --> SPKID_SAVE
            end

            MERGE["merge_speakers(segments, diar, names)<br/>→ 'John: Hello' / 'Speaker_01: Hello'"]
            MERGE_NOID["merge_speakers(segments, diar)<br/>→ 'Speaker_01: Hello'"]

            DIAR_CANCEL --> DIAR_PHASE --> DIAR_RUN --> DIAR_DONE --> SPKID_CHECK
            SPKID --> MERGE
        end

        AUDIO_FREE["← vector&lt;float&gt; freed<br/>  (~115 MB/hour of audio)"]

        VAD --> WHISPER_FREE
        TX_DIRECT --> WHISPER_FREE
        WHISPER_FREE --> DIAR_CHECK
        DIAR --> AUDIO_FREE
        MERGE_NOID --> AUDIO_FREE
    end

    TRANSCRIPT["transcript_text = result.to_string()<br/>[MM:SS - MM:SS] Speaker: text"]
    EMPTY_CHECK{"transcript<br/>empty?"}
    EMPTY_CHECK -->|"yes"| THROW["throw RecmeetError<br/>('No text produced')"]
    EMPTY_CHECK -->|"no"| CONTEXT

    AUDIO_FREE --> TRANSCRIPT --> EMPTY_CHECK

    subgraph CONTEXT["Context Resolution"]
        CTX_INLINE["1. cfg.context_inline"]
        CTX_FILE["2. cfg.context_file (read from disk)"]
        CTX_REPROC["3. load_meeting_context(out_dir)<br/>   (context.json fallback for reprocess)"]
        CTX_SAVE["Persist context.json if new recording"]
        CTX_INLINE --> CTX_FILE --> CTX_REPROC --> CTX_SAVE
    end

    SUMM_CHECK{"cfg.no_summary?"}
    CONTEXT --> SUMM_CHECK
    SUMM_CHECK -->|"yes"| NOTE

    subgraph SUMMARIZE["Summarization"]
        SUMM_PHASE["phase('summarizing')"]
        SUMM_SELECT{"LLAMA &&<br/>llm_model set?"}
        SUMM_SELECT -->|"yes"| LOCAL["ensure_llama_model(cfg.llm_model)<br/>summarize_local(transcript, model,<br/>  context, threads, mmap)"]
        SUMM_SELECT -->|"no"| CLOUD_CHECK{"api_key?"}
        CLOUD_CHECK -->|"yes"| CLOUD["Resolve URL from provider table<br/>summarize_http(transcript, url,<br/>  key, model, context)"]
        CLOUD_CHECK -->|"no"| SKIP_SUMM["log_warn: skip"]
        LOCAL --> META
        CLOUD --> META
        META["extract_meeting_metadata(summary)<br/>strip_metadata_block(summary)"]

        SUMM_PHASE --> SUMM_SELECT
    end

    SUMM_CHECK -->|"no"| SUMMARIZE

    subgraph NOTE["Note Generation"]
        NOTE_TIME["resolve_meeting_time(out_dir, audio)"]
        NOTE_BUILD["Build MeetingData struct"]
        NOTE_WRITE["write_meeting_note(cfg.note, data)<br/>→ Meeting_YYYY-MM-DD_HH-MM.md"]
        NOTE_COMPLETE["phase('complete')<br/>notify('Meeting processed')"]

        NOTE_TIME --> NOTE_BUILD --> NOTE_WRITE --> NOTE_COMPLETE
    end

    SUMMARIZE --> NOTE
    SKIP_SUMM --> NOTE

    RESULT["Return PipelineResult<br/>{note_path, output_dir, transcript_text}"]
    NOTE --> RESULT
```

---

## 9. Tray Applet

### 9a. Tray Startup and GTK Main Loop

```mermaid
flowchart TD
    MAIN["main(argc, argv)"]
    PARSE["Parse --daemon ADDRESS<br/>or RECMEET_DAEMON_ADDR env"]
    INIT["gtk_init()<br/>load_config()<br/>log_init(level, stderr=true)"]
    INDICATOR["app_indicator_new('recmeet-tray')<br/>Resolve icon theme path<br/>Set ACTIVE status"]
    SOURCES["refresh_sources()<br/>→ g_tray.mics, g_tray.monitors"]
    CONNECT["connect_to_daemon()"]
    CONNECT_OK{connected?}
    MENU["build_menu()<br/>fetch_provider_models()"]
    SIGNALS["SIGCHLD → reap recmeet-web zombies<br/>SIGTERM/SIGINT → gtk_main_quit"]
    GTK_MAIN["gtk_main()<br/>(blocks in GTK event loop)"]
    CLEANUP["Send record.stop if active<br/>teardown_ipc_watch()<br/>close IPC connection<br/>stop_web_server()<br/>log_shutdown()"]

    MAIN --> PARSE --> INIT --> INDICATOR --> SOURCES --> CONNECT
    CONNECT --> CONNECT_OK
    CONNECT_OK -->|"yes"| MENU
    CONNECT_OK -->|"no"| RECONNECT_SCHED["g_timeout_add_seconds(1,<br/>try_reconnect)"]
    RECONNECT_SCHED --> MENU
    MENU --> SIGNALS --> GTK_MAIN --> CLEANUP
```

### 9b. IPC Event Integration with GTK

```mermaid
flowchart TD
    subgraph "GTK Main Loop"
        GTK["gtk_main() running"]
        WATCH["GIOChannel watch on<br/>IPC socket fd<br/>(G_IO_IN | G_IO_HUP | G_IO_ERR)"]
    end

    WATCH -->|"G_IO_IN"| READ["ipc.read_and_dispatch(0)"]
    READ -->|"ok"| DISPATCH["Event dispatched to<br/>handle_ipc_event()"]
    READ -->|"fail"| DISCONN

    WATCH -->|"G_IO_HUP / G_IO_ERR"| DISCONN["teardown_ipc_watch()<br/>ipc.close_connection()<br/>update_state(false,false,false)"]
    DISCONN --> BACKOFF["Reset delay = 1s<br/>g_timeout_add_seconds<br/>(delay, try_reconnect)"]

    subgraph "handle_ipc_event(ev)"
        EV{"ev.event?"}
        EV -->|"phase"| PHASE["Update current_phase<br/>Clear progress_percent<br/>Patch status label in-place"]
        EV -->|"progress"| PROGRESS["Update phase + percent<br/>Patch status label in-place"]
        EV -->|"state.changed"| STATE["Check error → notify()<br/>Read {recording, postprocessing,<br/>downloading} booleans<br/>→ update_state()"]
        EV -->|"job.complete"| COMPLETE["notify() with note_path"]
        EV -->|"model.downloading"| MODEL["Update download_model<br/>rebuild_menu / notify"]
    end

    subgraph "update_state(rec, pp, dl)"
        US_CTX["Close context window<br/>if was_recording → !rec"]
        US_CLEAR["Clear progress/phase<br/>if all false"]
        US_ICON{"Which state?"}
        US_ICON -->|"recording"| ICON_REC["Set ICON_RECORDING"]
        US_ICON -->|"pp / dl"| ICON_PROC["Set ICON_PROCESSING"]
        US_ICON -->|"idle"| ICON_IDLE["Set ICON_IDLE"]
        US_REBUILD["build_menu()"]

        US_CTX --> US_CLEAR --> US_ICON
        ICON_REC --> US_REBUILD
        ICON_PROC --> US_REBUILD
        ICON_IDLE --> US_REBUILD
    end

    STATE --> US_CTX

    subgraph "Reconnection (exponential backoff)"
        TRY["try_reconnect()"]
        TRY --> TRY_CONN["connect_to_daemon()"]
        TRY_CONN -->|"success"| RESET["delay = 1s<br/>build_menu()"]
        TRY_CONN -->|"fail"| NEXT["delay = min(delay × 2, 30)<br/>g_timeout_add_seconds(delay,<br/>try_reconnect)"]
    end
```

### 9c. Recording Start/Stop Flow

```mermaid
flowchart TD
    subgraph "on_record()"
        REC_GUARD{"recording ||<br/>downloading?"}
        REC_GUARD -->|"yes"| REC_RET["return"]
        REC_GUARD -->|"no"| REC_CONN{"daemon_connected?"}
        REC_CONN -->|"no"| REC_RETRY["connect_to_daemon()"]
        REC_RETRY -->|"fail"| REC_NOTIFY["notify('Cannot connect')"]
        REC_RETRY -->|"ok"| REC_KEY
        REC_CONN -->|"yes"| REC_KEY

        REC_KEY["resolve_api_key(provider)"]
        REC_KEY --> REC_KEY_CHECK{"key empty &&<br/>!no_summary?"}
        REC_KEY_CHECK -->|"yes"| REC_PROMPT["prompt_api_key() GTK dialog"]
        REC_PROMPT --> REC_SAVE["Save to api_keys + config"]
        REC_KEY_CHECK -->|"no"| REC_BUILD
        REC_SAVE --> REC_BUILD

        REC_BUILD["Build run_cfg from g_tray.cfg<br/>config_to_map() → params"]
        REC_BUILD --> REC_CALL["ipc.call('record.start',<br/>params, 10s timeout)"]
        REC_CALL -->|"fail"| REC_ERR_NOTIFY["notify(error)"]
        REC_CALL -->|"ok"| REC_STATE["update_state(true, pp, false)"]
        REC_STATE --> REC_CTX["show_context_window()"]
    end

    subgraph "on_stop()"
        STOP_GUARD{"!recording &&<br/>!postprocessing?"}
        STOP_GUARD -->|"yes"| STOP_RET["return"]
        STOP_GUARD -->|"no"| STOP_CTX{"recording &&<br/>context window?"}
        STOP_CTX -->|"yes"| STOP_CAPTURE["capture_and_clear_context()<br/>→ {context_inline, vocab}"]
        STOP_CAPTURE --> STOP_JOB_CTX["ipc.call('job.context',<br/>{context_inline, vocabulary_append})"]
        STOP_CTX -->|"no"| STOP_TARGET
        STOP_JOB_CTX --> STOP_TARGET

        STOP_TARGET["params[target] =<br/>recording ? 'recording' : 'postprocessing'"]
        STOP_TARGET --> STOP_CALL["ipc.call('record.stop',<br/>params, 5s timeout)"]
    end
```

### 9d. Tray Menu Structure

```mermaid
graph TD
    MENU["GTK Menu"]
    MENU --> STATUS["(i) Status label<br/>(insensitive, patched in-place)"]
    MENU --> SEP1["─── separator ───"]
    MENU --> REC_SECTION["Record / Stop / Cancel<br/>(context-sensitive)"]
    MENU --> SEP2["─── separator ───"]
    MENU --> MIC["▶ Mic Source<br/>  ○ Auto-detect<br/>  ○ source1<br/>  ○ source2"]
    MENU --> MON["▶ Monitor Source<br/>  ○ Auto-detect<br/>  ○ monitor1"]
    MENU --> MODEL["▶ Whisper Model<br/>  ○ tiny / base / small /<br/>    medium / large-v3"]
    MENU --> LANG["▶ Language<br/>  ○ Auto-detect<br/>  ○ en / es / fr / ..."]
    MENU --> SEP3["─── separator ───"]
    MENU --> CHECKS["☑ Mic Only<br/>☑ No Summary<br/>☑ Speaker Diarization<br/>☑ VAD Segmentation"]
    MENU --> SEP4["─── separator ───"]
    MENU --> SUMMARY["▶ Summary<br/>  ▶ Provider<br/>    ○ xAI / OpenAI / Anthropic / Local<br/>  ▶ Model<br/>    ○ (fetched from API)"]
    MENU --> OUTPUT["▶ Output<br/>  (i) Output dir<br/>  (i) Note dir<br/>  Open Latest Session<br/>  Set Output Dir...<br/>  Set Note Dir...<br/>  Set LLM Model..."]
    MENU --> SEP5["─── separator ───"]
    MENU --> EDIT["Edit Config"]
    MENU --> SPEAKER["Speaker Management"]
    MENU --> REFRESH["Refresh Devices"]
    MENU --> UPDATE["Update Models"]
    MENU --> ABOUT["About"]
    MENU --> SEP6["─── separator ───"]
    MENU --> QUIT["Quit"]
```

---

## 10. CLI Mode Selection

```mermaid
flowchart TD
    ENTRY["main(argc, argv)"]
    PARSE["parse_cli(argc, argv) → CliResult"]

    PARSE --> VER{"--version?"}
    VER -->|"yes"| PRINT_VER["Print version, exit 0"]
    VER -->|"no"| HELP{"--help?"}
    HELP -->|"yes"| PRINT_HELP["Print help, exit 0"]
    HELP -->|"no"| ADDR_ENV{"RECMEET_DAEMON_ADDR<br/>env set?"}
    ADDR_ENV -->|"yes"| SET_ADDR["daemon_addr = env<br/>force DaemonMode::Force"]
    ADDR_ENV -->|"no"| STATUS_CHECK
    SET_ADDR --> STATUS_CHECK

    STATUS_CHECK{"--status?"}
    STATUS_CHECK -->|"yes"| CLIENT_STATUS["client_status(addr) → exit"]
    STATUS_CHECK -->|"no"| STOP_CHECK{"--stop?"}
    STOP_CHECK -->|"yes"| CLIENT_STOP["client_stop(addr) → exit"]
    STOP_CHECK -->|"no"| MODEL_DL{"--download-model?"}
    MODEL_DL -->|"yes"| DL_LOOP["Download models → exit"]
    MODEL_DL -->|"no"| VOCAB{"--list/add/remove/<br/>reset-vocab?"}
    VOCAB -->|"yes"| VOCAB_OPS["Vocab management<br/>+ save_config() → exit"]
    VOCAB -->|"no"| SPKR{"--speakers/reset/<br/>remove-speaker?"}
    SPKR -->|"yes"| SPKR_OPS["Speaker DB ops → exit"]
    SPKR -->|"no"| ENROLL{"--enroll NAME?"}
    ENROLL -->|"yes"| ENROLL_FLOW["Diarize → select speaker →<br/>extract embedding →<br/>save to DB → exit"]
    ENROLL -->|"no"| IDENTIFY{"--identify DIR?"}
    IDENTIFY -->|"yes"| ID_FLOW["Diarize → identify →<br/>print results → exit"]
    IDENTIFY -->|"no"| RECORD_FLOW

    subgraph RECORD_FLOW["Recording Flow"]
        REC_INIT["resolve_api_key()<br/>log_init()<br/>validate inputs"]
        DAEMON_MODE{"DaemonMode?"}
        DAEMON_MODE -->|"Force"| USE_DAEMON["use_daemon = true"]
        DAEMON_MODE -->|"Disable"| STANDALONE["use_daemon = false"]
        DAEMON_MODE -->|"Auto"| PROBE["daemon_running(addr)?"]
        PROBE -->|"yes"| USE_DAEMON
        PROBE -->|"no"| STANDALONE

        USE_DAEMON --> CLIENT_RECORD
        STANDALONE --> STANDALONE_MAIN

        subgraph CLIENT_RECORD["Client Mode"]
            CR_CONNECT["IpcClient::connect()"]
            CR_CB["Set event callback<br/>(phase, progress, state, complete)"]
            CR_START["call('record.start', params)"]
            CR_PRINT["Print 'Recording started.<br/>Press Ctrl+C to stop.'"]
            CR_SIGINT["Install SIGINT handler<br/>→ call('record.stop')"]
            CR_WAIT["read_events('job.complete')<br/>(blocks until done)"]

            CR_CONNECT --> CR_CB --> CR_START --> CR_PRINT --> CR_SIGINT --> CR_WAIT
        end

        subgraph STANDALONE_MAIN["Standalone Mode"]
            SM_SUBPROC{"--progress-json &&<br/>--config-json?"}
            SM_SUBPROC -->|"yes"| SUBPROCESS["subprocess_main()<br/>(see §11)"]
            SM_SUBPROC -->|"no"| SM_NORMAL

            subgraph SM_NORMAL["Normal Standalone"]
                SM_KEY["Validate API key"]
                SM_SIG["sigaction(SIGINT) → g_stop"]
                SM_MODELS["Interactive model checks:<br/>whisper Y/n, LLM, diarize, VAD"]
                SM_RUN["run_pipeline(cfg, g_stop)"]
                SM_DONE["log_shutdown()<br/>notify_cleanup()"]

                SM_KEY --> SM_SIG --> SM_MODELS --> SM_RUN --> SM_DONE
            end
        end

        REC_INIT --> DAEMON_MODE
    end
```

---

## 11. Subprocess Postprocessing

The daemon's `pp_worker_loop` fork/exec's the `recmeet` binary as a child
process for crash isolation. Communication is via NDJSON on stdout/stderr pipes.

### 11a. Fork/Exec Flow

```mermaid
flowchart TD
    JOB["PostprocessJob dequeued"]
    CFG_FIX["job.cfg.reprocess_dir = job.input.out_dir<br/>(ensures subprocess reprocesses,<br/>never starts live recording)"]
    CFG_WRITE["write_job_config() → temp JSON file<br/>/tmp/recmeet-pp-{job_id}.json"]
    ARGV["Build argv:<br/>[g_self_exe, --reprocess, out_dir,<br/> --config-json, cfg_path,<br/> --progress-json, --no-daemon]"]
    PIPES["pipe(stdout_pipe)<br/>pipe(stderr_pipe)"]
    FORK["fork()"]

    JOB --> CFG_FIX --> CFG_WRITE --> ARGV --> PIPES --> FORK

    FORK -->|"child (pid=0)"| CHILD
    FORK -->|"parent"| PARENT

    subgraph CHILD["Child Process"]
        C_SIG["Reset SIGINT/SIGTERM/SIGHUP<br/>to SIG_DFL"]
        C_DUP["dup2(stdout_pipe[1], STDOUT)<br/>dup2(stderr_pipe[1], STDERR)"]
        C_CLOSE["closefrom(3)<br/>(close ALL inherited fds:<br/>log fd, IPC sockets, pid lock)"]
        C_EXEC["execv(g_self_exe, argv)"]
        C_FAIL["_exit(127) on exec failure"]

        C_SIG --> C_DUP --> C_CLOSE --> C_EXEC --> C_FAIL
    end

    subgraph PARENT["Parent (pp_worker thread)"]
        P_CLOSE["Close write ends of both pipes"]
        P_PID["g_pp_child_pid.store(child_pid)"]
        P_POLL["Poll loop<br/>(see §11b)"]
        P_WAIT["waitpid(pid, &status, 0)"]
        P_CLEAR["g_pp_child_pid = -1<br/>Delete temp config JSON"]
        P_INTERPRET["Interpret exit status<br/>(see §11c)"]

        P_CLOSE --> P_PID --> P_POLL --> P_WAIT --> P_CLEAR --> P_INTERPRET
    end
```

### 11b. NDJSON Poll Loop and Watchdog

```mermaid
flowchart TD
    POLL["poll({stdout_fd, stderr_fd}, 2, 1000ms)"]

    POLL --> STDOUT{"stdout<br/>readable?"}
    STDOUT -->|"yes"| PARSE_OUT["Read lines, parse NDJSON"]

    PARSE_OUT --> EV_TYPE{"event type?"}
    EV_TYPE -->|"phase"| PHASE_BC["server.post(broadcast phase)<br/>Reset last_percent = -1<br/>Update last_progress timestamp"]
    EV_TYPE -->|"progress"| PROG_THROTTLE{"pct jump ≥ 10%<br/>or elapsed ≥ 120s?"}
    PROG_THROTTLE -->|"yes"| PROG_BC["server.post(broadcast progress)<br/>Update last_progress"]
    PROG_THROTTLE -->|"no"| SKIP["Skip (throttled)"]
    EV_TYPE -->|"job.complete"| CAPTURE["Capture note_path, output_dir"]
    EV_TYPE -->|"heartbeat"| HEARTBEAT["Update last_heartbeat only"]

    STDOUT -->|"no"| STDERR
    STDERR{"stderr<br/>readable?"}
    STDERR -->|"yes"| LOG_ERR["Log line, track last_stderr_line"]
    STDERR -->|"no"| WATCHDOG

    PHASE_BC --> WATCHDOG
    PROG_BC --> WATCHDOG
    SKIP --> WATCHDOG
    CAPTURE --> WATCHDOG
    HEARTBEAT --> WATCHDOG
    LOG_ERR --> WATCHDOG

    subgraph WATCHDOG["Dual-Timestamp Watchdog"]
        WD1{"last_heartbeat<br/>> 120s stale?"}
        WD1 -->|"yes"| KILL["kill(pid, SIGTERM)<br/>killed_stale = true<br/>Close both pipes<br/>Break poll loop"]
        WD1 -->|"no"| WD2{"last_progress<br/>> 300s stale?"}
        WD2 -->|"yes"| KILL
        WD2 -->|"no"| CANCEL_CHECK
    end

    CANCEL_CHECK{"g_pp_stop<br/>requested?"}
    CANCEL_CHECK -->|"yes"| CANCEL_KILL["kill(pid, SIGTERM)<br/>g_pp_stop.reset()"]
    CANCEL_CHECK -->|"no"| EOF_CHECK

    EOF_CHECK{"Both pipes<br/>closed?"}
    EOF_CHECK -->|"yes"| EXIT_POLL["Exit poll loop"]
    EOF_CHECK -->|"no"| POLL

    CANCEL_KILL --> POLL
```

### 11c. Exit Status Interpretation

```mermaid
flowchart LR
    EXIT{"Child exit status"}
    EXIT -->|"exit(0)"| OK["server.post(broadcast<br/>job.complete event)"]
    EXIT -->|"exit(2)"| CANCELLED["Log: 'Cancelled'<br/>(no notification)"]
    EXIT -->|"exit(127)"| LAUNCH_FAIL["'Failed to launch subprocess'"]
    EXIT -->|"killed_stale"| DEADLOCK["'Processing stalled<br/>(no progress) — likely<br/>onnxruntime deadlock'"]
    EXIT -->|"signal N"| CRASH["'Processing crashed<br/>(signal N: SIGNAME)'"]
    EXIT -->|"exit(N) other"| FAIL["'Processing failed<br/>(exit N): last_stderr_line'"]

    LAUNCH_FAIL --> NOTIFY["notify() + broadcast_state(error)"]
    DEADLOCK --> NOTIFY
    CRASH --> NOTIFY
    FAIL --> NOTIFY
```

### 11d. Subprocess Internal Flow

```mermaid
flowchart TD
    ENTRY["subprocess_main(cli)"]
    LOG_OFF["log_shutdown()<br/>(no file/stderr logging;<br/>NDJSON stdout only)"]
    READ_CFG["Read config JSON from<br/>cli.config_json_path"]
    PARSE["config_from_json(content) → cfg"]
    SUPPRESS["Suppress whisper log noise"]
    SIGNALS["sigaction(SIGINT/SIGTERM)<br/>→ g_stop.request()"]
    HEARTBEAT["Spawn heartbeat thread:<br/>write NDJSON {'event':'heartbeat'}<br/>every ~10s"]
    REC["run_recording(cfg, g_stop, on_phase)<br/>→ input (reprocess mode: returns immediately)"]
    PP["run_postprocessing(cfg, input,<br/>on_phase, on_progress, &g_stop)"]
    COMPLETE["Write NDJSON job.complete<br/>{note_path, output_dir}"]
    CLEANUP["Stop heartbeat thread<br/>Remove temp config JSON"]
    RET{"result?"}
    RET -->|"success"| EXIT0["return 0"]
    RET -->|"RecmeetError('Cancelled')"| EXIT2["return 2"]
    RET -->|"other error"| EXIT1["return 1"]

    ENTRY --> LOG_OFF --> READ_CFG --> PARSE --> SUPPRESS
    SUPPRESS --> SIGNALS --> HEARTBEAT --> REC --> PP --> COMPLETE --> CLEANUP --> RET
```

---

## 12. Audio Capture Subsystem

```mermaid
flowchart TD
    subgraph "Source Selection (run_recording)"
        SEL_START["Audio source names from Config"]
        SEL_EMPTY{"mic_source<br/>empty?"}
        SEL_EMPTY -->|"yes"| SEL_DETECT["detect_sources(pattern)<br/>→ mic + monitor"]
        SEL_EMPTY -->|"no"| SEL_USE["Use config values"]
        SEL_DETECT -->|"no mic"| SEL_ERR["throw DeviceError"]
        SEL_DETECT --> DUAL{"!mic_only &&<br/>monitor found?"}
        SEL_USE --> DUAL
    end

    DUAL -->|"yes"| DUAL_CAP
    DUAL -->|"no"| SINGLE_CAP

    subgraph DUAL_CAP["Dual Capture"]
        subgraph MIC_CAP["Mic Capture"]
            MIC_PW["PipeWireCapture(mic_source)<br/>S16LE mono 16kHz"]
            MIC_START["mic.start()"]
            MIC_PW --> MIC_START
        end

        subgraph MON_CAP["Monitor Capture"]
            MON_CHECK{".monitor suffix?"}
            MON_CHECK -->|"yes"| MON_PA["PulseMonitorCapture<br/>(pa_simple, own thread)"]
            MON_CHECK -->|"no"| MON_PW_TRY["try PipeWireCapture<br/>(capture_sink=true)"]
            MON_PW_TRY -->|"RecmeetError"| MON_PA
            MON_PW_TRY -->|"ok"| MON_OK["Monitor capturing"]
            MON_PA --> MON_OK
        end

        STOP_LOOP["while !stop.stop_requested()<br/>  sleep_for(200ms)"]
        DRAIN_BOTH["mic.stop() + mon.stop()<br/>mic_samples = mic.drain()<br/>mon_samples = mon.drain()"]
        WRITE_RAW["Write mic.wav + monitor.wav"]
        VALIDATE["validate_audio(mic.wav) — fatal<br/>validate_audio(monitor.wav) — non-fatal"]
        MIX_OR_SKIP{"Monitor valid?"}
        MIX_OR_SKIP -->|"yes"| MIX["mix_audio(mic, mon)<br/>→ averaged int16_t stream"]
        MIX_OR_SKIP -->|"AudioValidationError"| MIC_FALLBACK["Use mic-only"]

        MIC_START --> STOP_LOOP
        MON_OK --> STOP_LOOP
        STOP_LOOP --> DRAIN_BOTH --> WRITE_RAW --> VALIDATE --> MIX_OR_SKIP
        MIX --> WRITE_OUT
        MIC_FALLBACK --> WRITE_OUT
        WRITE_OUT["Write audio_YYYY-MM-DD_HH-MM.wav"]
    end

    subgraph SINGLE_CAP["Single Mic Capture"]
        S_PW["PipeWireCapture(mic_source)"]
        S_START["start()"]
        S_LOOP["while !stop.stop_requested()"]
        S_DRAIN["stop() → drain()"]
        S_WRITE["Write audio_YYYY-MM-DD_HH-MM.wav"]

        S_PW --> S_START --> S_LOOP --> S_DRAIN --> S_WRITE
    end

    subgraph "PipeWireCapture Internals (Pimpl)"
        PW_INIT["pw_init(), pw_main_loop_new()<br/>pw_stream_new()"]
        PW_PROPS["Properties:<br/>S16LE, 16kHz, mono<br/>capture_sink for loopback"]
        PW_CB["on_process callback (RT thread):<br/>dequeue buffer → memcpy to ring buffer<br/>(atomic flag, no mutex)"]
        PW_THREAD["pw_main_loop runs in own thread"]
        PW_STOP["stop(): set atomic flag<br/>pw_main_loop_quit()"]
        PW_DRAIN["drain(): move ring buffer out<br/>→ vector<int16_t>"]

        PW_INIT --> PW_PROPS --> PW_CB --> PW_THREAD
    end

    subgraph "PulseMonitorCapture Internals"
        PA_INIT["pa_simple_new(source, record)<br/>S16LE, 16kHz, mono"]
        PA_THREAD["Capture thread:<br/>pa_simple_read() in loop<br/>mutex-protected vector buffer"]
        PA_STOP["stop(): set StopToken<br/>join thread"]
        PA_DRAIN["drain(): move buffer out"]

        PA_INIT --> PA_THREAD
    end
```

---

## 13. Go Tools Module

### 13a. Module Structure and Data Flow

```mermaid
graph TB
    subgraph "Go Binaries"
        MCP_BIN["recmeet-mcp<br/>(cmd/recmeet-mcp/main.go)"]
        AGENT_BIN["recmeet-agent<br/>(cmd/recmeet-agent/main.go)"]
    end

    subgraph "meetingdata (shared library)"
        MD_CONFIG["config.go<br/>Parse config.yaml<br/>(flat YAML, matches C++)"]
        MD_MEETINGS["meetings.go<br/>Scan output dirs<br/>YYYY-MM-DD_HH-MM pattern"]
        MD_NOTES["notes.go<br/>Parse frontmatter +<br/>callout sections + search"]
        MD_ACTIONS["actionitems.go<br/>Parse ## Action Items<br/>- [ ] / - [x] format"]
        MD_SPEAKERS["speakers.go<br/>Load JSON profiles<br/>(strips embeddings)"]
    end

    subgraph "mcpserver"
        MCP_SERVER["server.go<br/>Setup + registration"]
        MCP_TOOLS["tools.go<br/>5 tool handlers"]
    end

    subgraph "agent"
        AG_CONFIG["config.go<br/>Agent-specific config"]
        AG_LOOP["loop.go<br/>Agentic loop (Claude API)"]
        AG_TOOLS["tools.go<br/>7 tool definitions"]
        AG_WORKFLOWS["workflows.go<br/>Prep + follow-up prompts"]
        AG_SEARCH["search.go<br/>Brave web search"]
        AG_FETCH["fetch.go<br/>HTML text extractor"]
        AG_WRITE["writefile.go<br/>File writer"]
    end

    subgraph "External APIs"
        CLAUDE_API["Anthropic API (Claude)"]
        BRAVE_API["Brave Search API"]
    end

    subgraph "Filesystem"
        FS_CONFIG["~/.config/recmeet/<br/>config.yaml"]
        FS_MEETINGS["meetings/<br/>(dirs + notes)"]
        FS_SPEAKERS["~/.local/share/recmeet/<br/>speakers/*.json"]
        FS_CONTEXT["~/.local/share/recmeet/<br/>context/"]
    end

    MCP_BIN --> MCP_SERVER
    MCP_SERVER --> MCP_TOOLS
    MCP_TOOLS --> MD_CONFIG
    MCP_TOOLS --> MD_NOTES
    MCP_TOOLS --> MD_ACTIONS
    MCP_TOOLS --> MD_SPEAKERS

    AGENT_BIN --> AG_WORKFLOWS
    AG_WORKFLOWS --> AG_LOOP
    AG_LOOP --> AG_TOOLS
    AG_TOOLS --> MD_CONFIG
    AG_TOOLS --> MD_NOTES
    AG_TOOLS --> MD_ACTIONS
    AG_TOOLS --> MD_SPEAKERS
    AG_TOOLS --> AG_SEARCH
    AG_TOOLS --> AG_FETCH
    AG_TOOLS --> AG_WRITE

    AG_LOOP --> CLAUDE_API
    AG_SEARCH --> BRAVE_API

    MD_CONFIG --> FS_CONFIG
    MD_MEETINGS --> FS_MEETINGS
    MD_NOTES --> FS_MEETINGS
    MD_ACTIONS --> FS_MEETINGS
    MD_SPEAKERS --> FS_SPEAKERS
    AG_WRITE --> FS_CONTEXT
    MCP_TOOLS -->|"write_context_file"| FS_CONTEXT
```

### 13b. MCP Server Tool Dispatch

```mermaid
flowchart TD
    CLIENT["MCP Client<br/>(Claude Code / Desktop / Cursor)"]
    STDIO["stdio (JSON-RPC)"]
    INIT["Redirect stdout → stderr<br/>(protect JSON-RPC stream)"]

    CLIENT -->|"tools/list"| STDIO
    STDIO --> INIT
    INIT --> REGISTER["Register 5 tools"]

    CLIENT -->|"tools/call"| DISPATCH{"Tool name?"}

    DISPATCH -->|"search_meetings"| SM["SearchNotes(noteDir, outputDir,<br/>query, filters)<br/>→ formatted results"]
    DISPATCH -->|"get_meeting"| GM["FindMeeting(outputDir, meeting_dir)<br/>ParseNote(path)<br/>→ full details"]
    DISPATCH -->|"list_action_items"| LA["ListActionItems(noteDir, outputDir,<br/>status, assignee, limit)<br/>→ filtered items"]
    DISPATCH -->|"get_speaker_profiles"| SP["LoadSpeakerProfiles(dbDir)<br/>→ profiles (no embeddings)"]
    DISPATCH -->|"write_context_file"| WC["Sanitize filename<br/>Write to context staging dir"]

    SM --> CLIENT
    GM --> CLIENT
    LA --> CLIENT
    SP --> CLIENT
    WC --> CLIENT
```

### 13c. Agent Agentic Loop

```mermaid
flowchart TD
    ENTRY["recmeet-agent prep|follow-up"]
    BUILD_PROMPT["Build system prompt +<br/>user message from workflow"]
    REGISTER["Register tools:<br/>search_meetings, get_meeting,<br/>list_action_items, get_speaker_profiles,<br/>web_search (if BRAVE_API_KEY),<br/>web_fetch, write_file"]

    SEND["Send to Claude API<br/>(messages + tool definitions)"]
    CHECK{"Stop reason?"}

    CHECK -->|"end_turn"| EXTRACT["Extract text response<br/>Print to stdout"]
    CHECK -->|"max_iterations (20)"| EXTRACT
    CHECK -->|"tool_use"| EXEC["For each tool_use block:"]

    EXEC --> TOOL_DISPATCH{"Tool name?"}
    TOOL_DISPATCH --> TOOL_EXEC["Execute tool<br/>Collect result string"]
    TOOL_EXEC --> APPEND["Append tool_result<br/>to conversation"]
    APPEND -->|"more tools"| TOOL_DISPATCH
    APPEND -->|"all done"| SEND

    ENTRY --> BUILD_PROMPT --> REGISTER --> SEND --> CHECK

    subgraph "Verbose Mode (--verbose)"
        V_CALL["stderr: tool name + params"]
        V_RESULT["stderr: result preview"]
    end
```
