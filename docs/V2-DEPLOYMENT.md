# V2 Deployment Guide

This guide shows how to deploy recmeet's V2 thin-client / daemon architecture on real hardware — single host, multi host, on a trusted LAN, or across networks. For the architectural overview see [`ARCHITECTURE.md`](ARCHITECTURE.md). For the wire-frame format see [`IPC-WIRE-PROTOCOL.md`](IPC-WIRE-PROTOCOL.md). For the per-verb reference see [`IPC-VERBS.md`](IPC-VERBS.md). For end-user "I just want to run it" steps see the project [`QUICKSTART.md`](../QUICKSTART.md), which links back here for anything past the default desktop install.

## Topology overview

V2 splits the workload between a thin client that owns local audio capture (the tray or the CLI in client mode) and a headless daemon that owns the ML pipeline. Three canonical topologies cover almost every deployment.

```
  Topology 1 — single-host local (default desktop install)
  ┌────────────────────────────────────────────────────────┐
  │  workstation                                           │
  │   ┌─────────┐ Unix socket ┌─────────────┐              │
  │   │  tray   ├────────────►│   daemon    │              │
  │   │  (cap)  │  peer creds │  (compute)  │              │
  │   └─────────┘             └─────────────┘              │
  └────────────────────────────────────────────────────────┘

  Topology 2 — single-host with remote tray (trusted LAN)
  ┌──────────────┐                   ┌──────────────────────┐
  │   laptop     │   TCP + PSK       │   server (LAN)       │
  │  ┌────────┐  │   port 29991      │   ┌─────────────┐    │
  │  │ tray   ├──┼──────────────────►│   │   daemon    │    │
  │  │ (cap)  │  │   trusted L2/L3   │   │  (compute)  │    │
  │  └────────┘  │                   │   └─────────────┘    │
  └──────────────┘                   └──────────────────────┘

  Topology 3 — multi-host with TLS or VPN
  ┌──────────────┐  TLS / WireGuard  ┌──────────────────────┐
  │   client     │   ════════════►   │   server (anywhere)  │
  │  ┌────────┐  │   stunnel /       │   ┌─────────────┐    │
  │  │ tray / │  │   tailscale /     │   │   daemon    │    │
  │  │  CLI   │  │   wireguard       │   │  (compute)  │    │
  │  └────────┘  │                   │   └─────────────┘    │
  └──────────────┘                   └──────────────────────┘
```

The wire format is identical in all three. Only the transport (Unix vs TCP) and what wraps it (nothing / nothing / TLS or VPN) change. PSK auth is mandatory on TCP — the daemon refuses to start a TCP listener without `RECMEET_AUTH_TOKEN` set.

## Single-host local (default)

The default desktop install. The tray and daemon run on the same machine and talk over a Unix socket at `$XDG_RUNTIME_DIR/recmeet/daemon.sock`. The kernel's peer-credential check is the only auth — no PSK is needed because the socket is mode `0700` and lives in the user's runtime directory.

### Install

```bash
make install
```

This:

1. Builds the four C++ binaries (`recmeet`, `recmeet-daemon`, `recmeet-tray`, `recmeet-web`).
2. Installs to `$DESTDIR$PREFIX` (default `/usr/local`).
3. Drops the systemd user units (`recmeet-daemon.service`, `recmeet-daemon.socket`, `recmeet-tray.service`) under `share/systemd/user/`.
4. Runs `recmeet --no-daemon --download-models` to seed the model cache.
5. Enables and starts `recmeet-daemon.service` via `systemctl --user enable --now`.

If `make install` runs under `DESTDIR` (i.e. you are packaging, not installing into a live session) it skips the model download and the `systemctl --user` step — the package's post-install scripts are expected to handle them on the target machine.

### Verify

```bash
recmeet --status
```

Expected output is a JSON-ish status line showing the daemon's composite state — one of `idle`, `postprocessing`, `streaming`, or `downloading`, plus per-slot booleans. If `recmeet --status` reports "daemon not running", check:

```bash
systemctl --user status recmeet-daemon.service
journalctl --user -u recmeet-daemon.service -n 50
```

The tray icon (ayatana-appindicator) should appear automatically if `recmeet-tray.service` is enabled or the desktop session autostarts `recmeet-tray.desktop`. Right-click the icon for the menu (Start / Stop / Reprocess / Settings).

### Where things live

| Path | Contents |
|---|---|
| `$XDG_RUNTIME_DIR/recmeet/daemon.sock` | The Unix socket the daemon listens on. Fallback: `/tmp/recmeet-<uid>/daemon.sock` when `XDG_RUNTIME_DIR` is unset. |
| `$XDG_RUNTIME_DIR/recmeet/daemon.sock.pid` | The daemon's PID file (Unix listener). For a TCP listener, `daemon-tcp.pid` in the same directory. |
| `~/.config/recmeet/config.yaml` | Operator config — sections include `[audio]`, `[transcription]`, `[summary]`, `[diarization]`, `[speaker_id]`, `[vad]`, `[captions]`, `[general]`, `[logging]`, `[notes]`, `[ipc]`, `[server]`, `[web]`. See [config schema reference](#config-schema-reference) below. |
| `~/.local/share/recmeet/models/whisper/` | Cached whisper.cpp GGUF models (base, large-v3, etc.). |
| `~/.local/share/recmeet/models/sherpa/` | sherpa-onnx diarization (`segmentation/`, `embedding/`), VAD (`vad/silero_vad.onnx`), streaming caption models (`online/`). |
| `~/.local/share/recmeet/models/llama/` | Local llama.cpp summarization models (optional). |
| `~/.local/share/recmeet/logs/` | Daemon + client logs, named `recmeet-YYYY-MM-DD-HH.log`. Retention defaults to 4 hours; tune via `--log-retention` or `[logging] retention_hours`. |
| `~/.local/share/recmeet/speakers/` | Voiceprint database (JSON files). Override via `[speaker_id] database` or `--speaker-db`. |
| Operator's `note_dir` | Where meeting notes are written. Set via `[notes] directory` or `-n / --note-dir`. The daemon does not pick a default — if unset, notes land alongside the audio in the staging output dir. |

## Single-host with remote tray (LAN)

This topology fits the common case of a headless server box (decent CPU/GPU, no display, runs the daemon) and a laptop on the same LAN running the tray. The daemon listens on TCP; the laptop's tray connects with the PSK.

**Threat model:** this configuration assumes a trusted L2/L3 network. The wire is unencrypted. PSK gives you authentication, not confidentiality. If you do not own the LAN — or anyone on it can sniff — skip ahead to [Multi-host with TLS or VPN](#multi-host-with-tls-or-vpn).

### Set up the daemon (server side)

Decide on a listening port (29991 used throughout this guide for examples) and a PSK. Then either pass `--listen` directly, or use the systemd unit with an `EnvironmentFile`.

**Manual run** (debugging):

```bash
RECMEET_AUTH_TOKEN=$(openssl rand -hex 32) \
  recmeet-daemon --listen 0.0.0.0:29991 --log-level info
```

The daemon will fail-fast and refuse to start if `--listen` parses as TCP but `RECMEET_AUTH_TOKEN` is unset — there is no warn-and-continue path. The error message is `recmeet-daemon: refusing to start TCP listener without RECMEET_AUTH_TOKEN set.` on stderr, logged as `daemon: refusing TCP startup — RECMEET_AUTH_TOKEN unset` in the log file.

**systemd user unit** (production):

Override the shipped `recmeet-daemon.service` via a drop-in:

```bash
mkdir -p ~/.config/systemd/user/recmeet-daemon.service.d
cat > ~/.config/systemd/user/recmeet-daemon.service.d/listen.conf <<'EOF'
[Service]
EnvironmentFile=%h/.config/recmeet/daemon.env
ExecStart=
ExecStart=/usr/local/bin/recmeet-daemon --listen 0.0.0.0:29991
EOF

install -m 0600 /dev/stdin ~/.config/recmeet/daemon.env <<EOF
RECMEET_AUTH_TOKEN=$(openssl rand -hex 32)
EOF

systemctl --user daemon-reload
systemctl --user restart recmeet-daemon.service
systemctl --user status recmeet-daemon.service
```

The empty `ExecStart=` first line is required by systemd to clear the unit's shipped command before the override takes effect.

Verify the listener is up from the daemon host:

```bash
ss -lnt | grep 29991
journalctl --user -u recmeet-daemon.service | grep "PSK auth enabled"
```

You should see `daemon: PSK auth enabled for TCP listener` in the log on each start.

### Generate a PSK

```bash
openssl rand -hex 32
```

That is the canonical way — 32 bytes of cryptographic randomness, 64 hex characters. Anything weaker than that becomes the practical security floor for the deployment, since the daemon compares it constant-time and any TCP client that knows the token can submit unlimited postprocess jobs.

A few things to know:

- The token is never logged. Successful auths log `ipc_server: TCP auth ok (peer=..., client_id=...)`; failures log `peer=..., reason=invalid_token` or `reason=auth_required`. The token itself does not appear in any log file.
- Comparison is constant-time (`ct_equals` in `src/ipc_server.cpp`) so a hostile peer cannot use response timing to recover the prefix.
- The token is sent literally in the first `0x00` NDJSON frame as `{"type":"auth.token","token":"..."}`. Anyone on the wire sees it in cleartext. This is fine for a trusted LAN; it is not fine for the public internet.

### Distribute the PSK

Three patterns, with their tradeoffs.

**A. systemd EnvironmentFile (preferred for production):**

```ini
[Service]
EnvironmentFile=%h/.config/recmeet/daemon.env
```

…with `daemon.env` at mode `0600`. The token never lands in shell history or process listings. Use this on the daemon side.

**B. Per-user shell env var (acceptable for client side):**

```bash
# ~/.bashrc or ~/.zshrc on the laptop
export RECMEET_AUTH_TOKEN='...64 hex chars...'
export RECMEET_DAEMON_ADDR='server:29991'
```

The tray and the CLI both honor `RECMEET_AUTH_TOKEN` (via the IPC client) and `RECMEET_DAEMON_ADDR` (as fallback for `--daemon-addr`). Caveat: the token sits in your shell rc file; treat that file like a credential.

**C. Shared filesystem secret (multi-user host):**

```bash
install -m 0600 -o $USER -g $USER /dev/stdin ~/.config/recmeet/daemon.env <<EOF
RECMEET_AUTH_TOKEN=...
EOF
```

`sourced` from each user's shell rc, or referenced via `EnvironmentFile` in a per-user systemd unit. Per-user isolation only — if a single token is shared by many users, every user can impersonate every other user on the daemon.

### Connect a client

From the laptop:

```bash
# CLI client, one-shot reprocess of a directory containing audio.wav
recmeet --daemon-addr server:29991 --reprocess ~/audio/2026-05-15

# CLI client, query daemon status
recmeet --daemon-addr server:29991 --status

# Tray, configured via env var
RECMEET_DAEMON_ADDR=server:29991 recmeet-tray
```

`--daemon-addr` implicitly enables client mode (it sets `DaemonMode::Force`) so you do not need `--daemon` alongside it. If `RECMEET_DAEMON_ADDR` is set in the environment it acts as the fallback when `--daemon-addr` is not on the command line.

When the client connects it sends `{"type":"auth.token","token":"..."}` as its first frame. The daemon replies with either `{"type":"auth.ok","client_id":"...","protocol_version":3}` or `{"type":"auth.error","reason":"invalid_token"|"auth_required"}`. The client aborts on `auth.error` and exits with a non-zero status; the tray surfaces the failure via its connection-status indicator and retries with backoff.

### Caveats

- **Do not expose this directly to the public internet.** The wire is unencrypted. A PSK gives you authentication, not confidentiality — a passive observer who captures one auth handshake replays the token forever.
- **One token per deployment.** The daemon takes exactly one `RECMEET_AUTH_TOKEN`. There is no per-client identity. If you need per-user revocation, run one daemon per user or wait for the future admin-verb surface.
- **No transport upgrade mid-session.** Rotating the token requires a daemon restart (see [PSK lifecycle](#psk-lifecycle)).

## Multi-host with TLS or VPN

When client and daemon are on different networks — or even on the same network but you do not own it — wrap the TCP transport in TLS or a VPN. The daemon does not do TLS itself; pick one of the patterns below.

### Pattern A: stunnel TLS terminator

stunnel sits in front of the daemon, listens on a public port with a valid certificate, and forwards plaintext to the daemon on localhost. The daemon stays bound to `127.0.0.1` and never sees the network directly.

**Daemon side** (`/etc/stunnel/recmeet.conf`):

```ini
[recmeet-daemon]
accept = 0.0.0.0:29992
connect = 127.0.0.1:29991
cert = /etc/letsencrypt/live/recmeet.example.org/fullchain.pem
key  = /etc/letsencrypt/live/recmeet.example.org/privkey.pem
sslVersionMin = TLSv1.3
ciphers = TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256
```

Run the daemon bound to loopback only:

```bash
recmeet-daemon --listen 127.0.0.1:29991
```

**Client side** (`/etc/stunnel/recmeet.conf`):

```ini
[recmeet-client]
client = yes
accept = 127.0.0.1:29991
connect = recmeet.example.org:29992
verifyChain = yes
CAfile = /etc/ssl/certs/ca-certificates.crt
checkHost = recmeet.example.org
```

Then point the recmeet client at the local stunnel:

```bash
recmeet --daemon-addr 127.0.0.1:29991 --status
```

The recmeet client thinks it is talking to a local TCP daemon; stunnel does the TLS wrap transparently. Certificate lifecycle (renewal, distribution of the CA bundle, host verification) is the operator's responsibility.

### Pattern B: WireGuard mesh

If you can give every participating machine a WireGuard interface, the mesh itself is the security boundary. The daemon listens on its WireGuard interface address; the PSK is defense in depth in case the mesh is misconfigured or a peer is compromised.

**Daemon side** (`/etc/wireguard/wg0.conf`):

```ini
[Interface]
Address = 10.42.0.1/24
ListenPort = 51820
PrivateKey = <server-private-key>

[Peer]
PublicKey = <laptop-public-key>
AllowedIPs = 10.42.0.2/32
```

Then bind the daemon to the mesh address:

```bash
RECMEET_AUTH_TOKEN=$(openssl rand -hex 32) \
  recmeet-daemon --listen 10.42.0.1:29991
```

**Client side** (`/etc/wireguard/wg0.conf`):

```ini
[Interface]
Address = 10.42.0.2/24
PrivateKey = <laptop-private-key>

[Peer]
PublicKey = <server-public-key>
Endpoint = recmeet.example.org:51820
AllowedIPs = 10.42.0.1/32
PersistentKeepalive = 25
```

And connect:

```bash
recmeet --daemon-addr 10.42.0.1:29991 --status
```

Tailscale, Nebula, or any other mesh VPN works the same way — substitute the mesh's interface address for `10.42.0.x`. The PSK is still required; treat it as belt-and-suspenders against a compromised mesh peer or a misconfigured `AllowedIPs`.

## PSK lifecycle

### Rotation

The daemon does NOT hot-reload `RECMEET_AUTH_TOKEN`. `SIGHUP` reloads `daemon.yaml` but does not re-read the environment — the token is captured at startup and held for the process lifetime. Rotation procedure:

1. Generate the new token: `openssl rand -hex 32`.
2. Update the `EnvironmentFile` (or wherever the token is sourced from) on the daemon host.
3. Distribute the new token to every connecting client.
4. `systemctl --user restart recmeet-daemon.service` — or `kill -TERM $(cat ~/.local/share/recmeet/runtime/daemon-tcp.pid)` if you are running it directly.
5. Verify with `journalctl --user -u recmeet-daemon.service | tail -n 20` — look for `daemon: PSK auth enabled for TCP listener`.

Plan rotations during a quiet window. A restart drops every in-flight TCP connection and aborts any non-terminal job — postprocess, streaming, or model download.

### Revocation

Revocation is the same operation as rotation: there is no per-client revocation today. Restart with a new token and every old client gets `auth.error: invalid_token` on its next connect attempt. Track this limitation if you are considering a multi-tenant deployment.

### Audit

The daemon logs successful and failed PSK attempts at `info` level. The token itself never appears in any log line.

```bash
# Successful auths
grep "TCP auth ok" ~/.local/share/recmeet/logs/recmeet-*.log

# Failed auths
grep "TCP auth refused" ~/.local/share/recmeet/logs/recmeet-*.log
```

Failure reasons are `auth_required` (client sent something other than an `auth.token` frame as its first message) and `invalid_token` (token did not match). The peer's address is logged in both cases.

## systemd integration

### User service (default install)

`make install` drops three units under `$PREFIX/share/systemd/user/`:

| Unit | Type | Purpose |
|---|---|---|
| `recmeet-daemon.service` | simple | Runs the daemon; restarts on failure (`RestartSec=5`). Memory caps via `MemoryHigh=10G` / `MemoryMax=14G`. See `dist/recmeet-daemon.service.in`. |
| `recmeet-daemon.socket` | socket | Defines `ListenStream=%t/recmeet/daemon.sock` with `SocketMode=0700`. Present in the install tree but the daemon does not currently consume systemd-passed fds (it binds its own listen socket). |
| `recmeet-tray.service` | simple | Optional — runs the tray applet under the user's graphical session. |

After install, the daemon is enabled and started automatically:

```bash
systemctl --user is-enabled recmeet-daemon.service
systemctl --user status   recmeet-daemon.service
journalctl --user -u recmeet-daemon.service -f
```

### System-wide service (multi-user host)

The shipped unit is a `--user` unit and runs as the invoking user. To run a single daemon for multiple users, write a custom system unit:

```ini
# /etc/systemd/system/recmeet-daemon.service
[Unit]
Description=Recmeet Daemon (system-wide)

[Service]
Type=simple
User=recmeet
Group=recmeet
EnvironmentFile=/etc/recmeet/daemon.env
ExecStart=/usr/local/bin/recmeet-daemon --listen 0.0.0.0:29991
Restart=on-failure
RestartSec=5

# Memory caps (see shipped unit for rationale)
Environment=MALLOC_ARENA_MAX=2
Environment=RECMEET_RSS_LIMIT_MB=12288
MemoryHigh=10G
MemoryMax=14G
MemorySwapMax=0

[Install]
WantedBy=multi-user.target
```

With:

```bash
useradd --system --create-home --shell /usr/sbin/nologin recmeet
install -m 0600 -o recmeet -g recmeet /dev/stdin /etc/recmeet/daemon.env <<EOF
RECMEET_AUTH_TOKEN=$(openssl rand -hex 32)
EOF
systemctl daemon-reload
systemctl enable --now recmeet-daemon.service
```

Caveats:

- The daemon's `$HOME` becomes `/home/recmeet`, so `~/.config/recmeet/config.yaml`, `~/.local/share/recmeet/models/`, and the meeting `note_dir` all resolve under that user. Either pre-populate them, or set `XDG_CONFIG_HOME` / `XDG_DATA_HOME` in the unit to point somewhere else.
- Speaker DB and meeting notes belong to whoever owns the daemon process, not to the originating client. The current daemon has no per-client artifact isolation past the per-job staging dir. If that matters, run one daemon per user.

### Socket activation

`recmeet-daemon.socket` is installed alongside the service unit but **not consumed**. The daemon does its own `bind()` + `listen()` (see `src/daemon.cpp` `--listen` handling). Enabling `recmeet-daemon.socket` does nothing useful today. Track this if socket activation matters to you; the unit is shipped as a placeholder for future wiring.

## Storage and capacity planning

### Model storage

| Model | Approximate size |
|---|---|
| whisper.cpp `tiny` | ~75 MB |
| whisper.cpp `base` | ~150 MB |
| whisper.cpp `small` | ~500 MB |
| whisper.cpp `medium` | ~1.5 GB |
| whisper.cpp `large-v3` | ~3 GB |
| sherpa-onnx segmentation + embedding | ~50 MB |
| sherpa-onnx streaming caption (en, 2023-06-26) | ~30 MB |
| sherpa-onnx VAD (silero) | ~2 MB |
| llama.cpp summarization model | varies (3-8 GB typical) |

Plan for 4-5 GB if you want `base` + `large-v3` + the full sherpa stack cached. Plan for 10-15 GB if you also want local summarization (which avoids cloud API egress entirely but costs disk).

Models live under `~/.local/share/recmeet/models/` (subdirs `whisper/`, `sherpa/`, `llama/`).

### Meeting storage

Meeting notes (Markdown files plus per-meeting audio WAVs) are written to the operator-configured `note_dir`:

- Per-session: the client sends `session.init` with `preferences: { note_dir: ... }` or `output_dir: ...`.
- Daemon default: `[notes] directory` in `~/.config/recmeet/config.yaml`.

The daemon does **not** garbage-collect old meeting directories. Disk usage grows monotonically. Operators should either:

- Mount `note_dir` on a partition with `logrotate` / `tmpwatch` / a cron cleanup task; or
- Periodically archive completed meetings off the daemon host.

### Per-job staging

Postprocess uploads stage under `fs::temp_directory_path()` (effectively `/tmp` on Linux) as `recmeet-upload-<job_id>-<tok8>/`. Streaming sessions stage their accumulating WAV at `<tmp>/recmeet-stream-<job_id>-<token>.wav`. Subprocess config files live at `<tmp>/recmeet-pp-<job_id>.json`.

Cleanup is best-effort on subprocess exit and `UploadSession` destruction. Orphans can accumulate if the daemon crashes, the kill-grace machine fails to clean up its child, or `tmpfs` is not auto-reaped on reboot. Plan for one of:

- Periodic daemon restart (the unit's `RestartSec` and a daily `systemctl --user restart recmeet-daemon.service` cron).
- Mounting `/tmp` as `tmpfs` so it clears on reboot (the default on most modern distros).
- A cron sweep: `find /tmp -maxdepth 1 -name 'recmeet-*' -mtime +1 -exec rm -rf {} +`.

## Health checks and monitoring

### Liveness

```bash
recmeet --daemon-addr server:29991 --status
```

The verb is `status.get`. The response is:

```json
{
  "state": "idle" | "postprocessing" | "streaming" | "downloading",
  "postprocessing": false,
  "streaming": false,
  "downloading": false,
  "queue_depth": 0
}
```

`state` is a composite name with priority order `postprocessing > streaming > downloading > idle`; the three per-slot booleans are independent and any combination can be true at once (the JobQueue has three independent typed slots — see `ARCHITECTURE.md` Job Queue).

An exit code of zero with a parseable result means the daemon is alive, authenticated, and serving. A nonzero exit means one of: daemon down, network unreachable, auth failed, protocol version mismatch.

### Readiness

There is no separate readiness probe today. Liveness is readiness — the daemon either accepts auth + IPC, or it does not. The first job after a fresh start may take longer because the postprocess subprocess has to load whisper.cpp from cold cache, but the daemon itself does not have a "warming" state.

### Metrics

No first-class metrics endpoint today. Workarounds:

- `recmeet --daemon-addr ... --status` for state polling.
- `job.list` (via the CLI's planned `--list-jobs` surface; for now via a custom client) to enumerate per-client jobs and inspect their states.
- Parse `~/.local/share/recmeet/logs/recmeet-*.log` for completion / error patterns.

A Prometheus exporter is on the wishlist; not in V2 scope.

## Config schema reference

Per-section keys observed by the daemon (see `src/config.cpp`). All sections are optional; missing keys take struct defaults.

| Section | Keys |
|---|---|
| `[audio]` | `device_pattern`, `mic_source`, `monitor_source` |
| `[transcription]` | `model`, `language`, `vocabulary` |
| `[summary]` | `provider`, `api_url`, `api_key`, `model`, `llm_model` |
| `[api_keys]` | one key per provider name (xai, openai, anthropic, ...) |
| `[diarization]` | `num_speakers`, `cluster_threshold`, `chunk_minutes`, `chunk_overlap_sec`, `stitch_threshold` |
| `[speaker_id]` | `threshold`, `database` |
| `[vad]` | `threshold`, `min_silence`, `min_speech`, `max_speech` |
| `[captions]` | `model` |
| `[general]` | `threads` |
| `[logging]` | `level`, `directory`, `retention_hours` |
| `[output]` | `directory` |
| `[notes]` | `domain`, `directory` |
| `[web]` | `port`, `bind` |
| `[ipc]` | `max_message_bytes`, `max_clients` |
| `[server]` | `max_upload_bytes`, `slot.postprocess`, `slot.streaming`, `slot.model_download`, `allow_client_downloads`, `diarization_cache_ttl_secs` |

Defaults of note:

- `[ipc] max_clients = 16` (TCP backlog is `2 * max_clients`).
- `[server] slot.postprocess = 1`, `slot.streaming = 1`, `slot.model_download = 1` — capacity-1 typed slots; raising any of these is unsupported and untested.
- `[server] allow_client_downloads = true` — set to `false` to refuse `models.ensure` from clients (server-side download policy).

## Troubleshooting

**`recmeet-daemon: refusing to start TCP listener without RECMEET_AUTH_TOKEN set.`**
Set `RECMEET_AUTH_TOKEN` in the environment before `recmeet-daemon --listen <host:port>`. There is no warn-and-continue; either set the token or switch to a Unix-socket listener via `--listen /path/to/daemon.sock`.

**Client aborts with `auth.error: invalid_token`**
The token the client sent does not match the daemon's `RECMEET_AUTH_TOKEN`. Common causes: token rotated on the server but not pushed to the client; client picked up the token from a stale `.env`; client and server reading from different shell rc files.

**Client aborts with `auth.error: auth_required`**
The client's first frame was something other than `{"type":"auth.token","token":"..."}`. Either the client is configured for Unix (which skips PSK) but is connecting to a TCP listener, or its build predates Phase A.1.

**Connection drops immediately after `auth.ok`, log mentions protocol version**
Client and daemon were built from different protocol revisions. `IPC_PROTOCOL_VERSION = 3` is stamped into every `auth.ok` frame; mismatch aborts the connection on the client side. Rebuild both ends from the same revision. Useful diagnostic: `recmeet --version` and `recmeet-daemon --version`.

**`Method not found` on a `record.start` request**
The client is V1; the daemon is V2 (or vice versa). `record.start`, `record.stop`, `job.context`, `sources.list`, and `config.update` were removed in Phase C.9 / A.6. V2 clients submit work via `process.submit` (file uploads) or `process.stream` (live audio). Update the client.

**`server_full` error frame on connect, fd closes immediately**
The daemon's `[ipc] max_clients` cap is hit. Default is 16. Raise it in `~/.config/recmeet/config.yaml`:

```yaml
ipc:
  max_clients: 64
```

…and restart the daemon. The TCP backlog is sized as `2 * max_clients` so raising the cap also widens the accept backlog.

**`process.submit` returns `Busy`**
The relevant slot is occupied AND the slot's FIFO is also at its policy limit. In V2 the postprocess slot queues additional submissions rather than rejecting them, so `Busy` is rare — when it does fire it usually means the streaming slot or model-download slot is already in use and the verb's target slot has a hard reject policy. Check `job.list` to see what is in flight.

**Subprocess crash mid-postprocess**
Each postprocess job runs in a fresh `recmeet --subprocess-mode` child for crash isolation against onnxruntime heap corruption (see `ARCHITECTURE.md` Postprocess subprocess isolation, iter 90). On crash:
- The daemon's `pp_worker_loop` logs the subprocess exit status.
- The job ends in `failed` state in the job registry.
- The originating client gets a `progress.job { phase: "failed" }` event.
- The staging audio is preserved (the upload finalized before the subprocess ran), so the client can re-submit.

**Long-running streaming session, RAM looks fine but `/tmp` grows**
This is by design. The streaming session is disk-backed — incoming `0x03` PCM frames go straight to `<tmp>/recmeet-stream-<job_id>-<token>.wav` rather than buffering in RAM. RSS stays flat; `/tmp` usage grows at PCM rate. Plan disk accordingly (rough rate: 16 kHz mono S16LE = 32 kB/sec = 115 MB/hour; stereo doubles it).

**`recmeet --status` from the client says "daemon not running" but the server's `systemctl status` is green**
Check that `--daemon-addr` (or `RECMEET_DAEMON_ADDR`) on the client side points to the right host and port. Check that the daemon's `--listen` is on a reachable interface (not `127.0.0.1` if the client is remote). Check that there is no firewall (`ufw`, `iptables`, security group) blocking the port on the daemon side.

## Migration from V1 (forward-looking)

V2 has no client-side V1 backcompat. The verbs `record.start`, `record.stop`, `job.context`, `sources.list`, and `config.update` were removed in Phase C.9 / A.6 — a stray request returns `MethodNotFound`. The IPC protocol version bumped to `3`; mismatched clients abort on `auth.ok`.

When the `v1-maintenance` branch is cut, the migration path will be:

1. **All clients in lockstep.** Operators must upgrade every connecting client at the same time the daemon is upgraded. There is no protocol-bridge daemon.
2. **Mixed environments stay on v1-maintenance.** Hosts that cannot upgrade in lockstep stay on the `v1-maintenance` branch until they can.
3. **Speaker DB migration is automatic.** The on-disk JSON shape is stable; V2 daemons read V1 speaker databases unchanged.
4. **Meeting notes are stable.** V1 and V2 produce the same Markdown shape; no per-meeting migration is required.

This section will be tightened once the v1-maintenance branch is cut and the migration has been exercised in practice.

## See also

- [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) — architectural overview (the authoritative document on V2).
- [`docs/IPC-VERBS.md`](IPC-VERBS.md) — per-verb IPC reference.
- [`docs/IPC-WIRE-PROTOCOL.md`](IPC-WIRE-PROTOCOL.md) — frame-layer spec.
- [`docs/V2-STRATEGY.md`](V2-STRATEGY.md) — strategy doc explaining the V1 → V2 migration plan.
- [`QUICKSTART.md`](../QUICKSTART.md) — end-user quickstart; links here for past-the-default-install steps.
- [`README.md`](../README.md) — project overview.
