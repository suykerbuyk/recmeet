# Thin-Client Deployment Guide

This guide is the operational playbook for deploying recmeet's V2 thin-client architecture across more than one machine — laptop running the tray, headless server running the daemon. The guide is opinionated; it picks the deployment patterns that have shipped successfully in practice and skips the ones that work in theory but bite in production.

For the architectural overview see [`ARCHITECTURE.md`](ARCHITECTURE.md). For the wire-frame format see [`IPC-WIRE-PROTOCOL.md`](IPC-WIRE-PROTOCOL.md). For the multi-host deployment reference (stunnel, WireGuard, systemd unit examples) see [`V2-DEPLOYMENT.md`](V2-DEPLOYMENT.md). The guide you are reading focuses on the **thin-client** half of the deployment story: PSK lifecycle, the recommended overlay-network pattern (Tailscale), the recommended TLS pattern (reverse proxy with automatic certs), server sizing for the ML workload, and the forward-looking package split.

---

## Table of contents

1. [PSK setup](#psk-setup)
2. [Tailscale + MagicDNS (recommended)](#tailscale--magicdns-recommended)
3. [TLS via reverse proxy (nginx or caddy + certbot)](#tls-via-reverse-proxy-nginx-or-caddy--certbot)
4. [Server sizing](#server-sizing)
5. [Package split guidance (`recmeet-client` vs `recmeet-server`)](#package-split-guidance-recmeet-client-vs-recmeet-server)
6. [See also](#see-also)

---

## PSK setup

The PSK (`RECMEET_AUTH_TOKEN`) gates every TCP connection to the daemon. Unix-socket listeners bypass the PSK because the socket lives in the user's runtime directory and is gated by filesystem permissions and kernel peer credentials. Once you cross a machine boundary, the PSK is the only authentication layer on the wire — treat it like any other long-lived shared credential.

### Generate

```bash
openssl rand -hex 32
# 7c2f9a3e8b1d4f5c... (64 hex characters, 32 bytes of cryptographic randomness)
```

That is the canonical generator. Anything weaker (a memorable phrase, a short hex string, a UUID rather than `getrandom(2)` output) becomes the practical security floor of the deployment — any TCP client that knows the token can submit unlimited postprocess jobs. The daemon constant-time-compares the token (`ct_equals` in `src/ipc_server.cpp`) so response timing does not leak the prefix length.

### Distribute

Three patterns, in descending order of robustness.

**A. Out-of-band over a secure channel (preferred).** Hand the token to each client host via a channel that is already authenticated and encrypted — SSH, Signal, an existing password manager's shared vault, a `pass` git repo, sops-encrypted YAML in your dotfiles repo. The token never appears in shell history, never lands in a config file in plaintext on a shared host, and never traverses email or Slack.

**B. systemd `EnvironmentFile` on the server side, environment variable on the client side.** The recommended file-on-disk pattern. On the daemon:

```bash
install -m 0600 /dev/stdin ~/.config/recmeet/daemon.env <<EOF
RECMEET_AUTH_TOKEN=$(openssl rand -hex 32)
EOF
```

…then point the systemd unit at it (see [`V2-DEPLOYMENT.md`](V2-DEPLOYMENT.md) for the full unit example). On each client laptop, the operator pastes the token into their shell rc:

```bash
# ~/.bashrc or ~/.zshrc
export RECMEET_AUTH_TOKEN='7c2f9a3e8b1d4f5c...'
export RECMEET_DAEMON_ADDR='server.tailnet-name.ts.net:29991'
```

The tray and CLI both honor both env vars.

**C. Shared filesystem secret (multi-user host only).** Already covered in [`V2-DEPLOYMENT.md`](V2-DEPLOYMENT.md#distribute-the-psk); use this only when a single daemon serves multiple users on the same host and you have configured per-user shell rc files.

### Rotate

The daemon does **not** hot-reload the PSK — `SIGHUP` reloads `daemon.yaml` but not the environment. Rotation is a restart-coordinated event:

```bash
# 1. Mint the new token
NEW=$(openssl rand -hex 32)

# 2. Update the daemon's EnvironmentFile and restart
sed -i "s/^RECMEET_AUTH_TOKEN=.*/RECMEET_AUTH_TOKEN=$NEW/" ~/.config/recmeet/daemon.env
systemctl --user restart recmeet-daemon.service

# 3. Distribute the new token to each client (out-of-band)
# 4. Each client updates ~/.bashrc / ~/.zshrc and re-sources it
```

Phase D.3 (`58d660d`, iter 168) makes this less disruptive than it sounds. The reconnect-with-jitter path means trays come back without a synchronized storm, and the `resume_token` re-association rebinds prior in-flight jobs to the new connection so a tray that was mid-upload resumes from where it parked rather than starting over.

Plan rotations during a quiet window — a restart still drops every in-flight TCP connection and aborts any non-terminal job that the `resume_token` cannot recover (typically: jobs whose audio has not yet been fully uploaded). The on-disk staging sidecar (Phase D.5) preserves the recording itself, so even aborted submissions can be retried by hand after the restart finishes.

### Per-session revocation (`--evict`)

For the "this laptop was lost, kill its session without disturbing other clients" case, use the `recmeet-daemon --evict <resume_token_prefix>` CLI added in C.13:

```bash
# Find the suspect session in the journal (prefixes are logged, full tokens never are)
journalctl --user -u recmeet-daemon.service | grep "client_id=" | grep "peer=10.42.0.7"

# Evict by token prefix (8+ hex chars)
recmeet-daemon --evict abcd1234
```

This is a surgical operation — it does not require a restart and does not affect any other client's session. Full PSK rotation remains the way to "log everyone out" coarsely.

---

## Tailscale + MagicDNS (recommended)

For most distributed teams the cleanest path is Tailscale: every participating machine joins the tailnet, the daemon binds to its tailnet address, and clients reach it via MagicDNS by hostname. Tailscale provides encryption and identity at the transport layer; the recmeet PSK becomes belt-and-suspenders against a compromised tailnet peer or a misconfigured ACL.

### Why Tailscale

- **WireGuard under the hood** — modern, audited, fast.
- **Zero firewall config** — Tailscale handles NAT traversal; no port forwards.
- **MagicDNS** — connect to `daemon-host.tailnet-name.ts.net` instead of an IP.
- **ACLs** — restrict which tailnet members can reach the daemon's port at the Tailscale layer, on top of the PSK gate.
- **Cross-platform clients** — works the same on Linux laptops, macOS, iOS, NixOS, etc.

### Setup

On the daemon host:

```bash
# Install (Arch example; see https://tailscale.com/download for your distro)
sudo pacman -S tailscale
sudo systemctl enable --now tailscaled
sudo tailscale up --hostname daemon-host

# Note the tailnet address
tailscale ip -4
# 100.64.0.5
```

Bind the recmeet daemon to the tailnet interface — either by tailnet IP or by `0.0.0.0` with a Tailscale ACL restricting access:

```bash
# Bind to the tailnet IP directly (safest)
RECMEET_AUTH_TOKEN=$(openssl rand -hex 32) \
  recmeet-daemon --listen 100.64.0.5:29991

# Or, for the systemd unit:
sed -i 's|ExecStart=.*|ExecStart=/usr/local/bin/recmeet-daemon --listen 100.64.0.5:29991|' \
  ~/.config/systemd/user/recmeet-daemon.service.d/listen.conf
systemctl --user daemon-reload
systemctl --user restart recmeet-daemon.service
```

On each client laptop:

```bash
sudo tailscale up
export RECMEET_AUTH_TOKEN='<same token as daemon>'
export RECMEET_DAEMON_ADDR='daemon-host.tailnet-name.ts.net:29991'
recmeet --status
# expect: { "state": "idle", "postprocessing": false, ... }
```

That is the whole setup. Audio captured on the laptop streams to the daemon over the encrypted tailnet, and live captions / progress events route back the same way. The `resume_token` (Phase D.3) means a laptop that closes its lid in a coffee shop and reopens on a different network keeps its session — Tailscale silently re-establishes the WireGuard tunnel and recmeet's tray drains its queued submissions through the recovered connection.

### Tailscale ACL example

In your tailnet's ACL (`https://login.tailscale.com/admin/acls`):

```jsonc
{
  "acls": [
    // recmeet daemon only reachable by trusted laptops on its TCP port
    {
      "action": "accept",
      "src":    ["tag:recmeet-client"],
      "dst":    ["daemon-host:29991"]
    }
  ],
  "tagOwners": {
    "tag:recmeet-client": ["autogroup:admin"]
  }
}
```

Tag your laptops with `tag:recmeet-client` and only they can reach the daemon port. This is defence in depth on top of the PSK.

---

## TLS via reverse proxy (nginx or caddy + certbot)

If a tailnet is not an option — fixed-IP server with public DNS, on-prem deployment behind a corporate edge, contractor laptops that cannot join the tailnet — terminate TLS in a reverse proxy in front of the daemon and let the daemon listen on loopback only. Caddy is the path of least resistance because it does automatic Let's Encrypt; nginx + certbot is the path most ops teams already understand.

### Pattern A: Caddy (automatic certs)

`/etc/caddy/Caddyfile`:

```caddy
recmeet.example.org {
    reverse_proxy 127.0.0.1:29991
}
```

That's it. Caddy fetches a Let's Encrypt cert automatically, renews it on its own, terminates TLS on port 443, and forwards plaintext NDJSON + binary frames to the daemon on loopback. Wire framing is binary-safe inside the TLS tunnel — Caddy treats the connection as opaque bytes.

Run the daemon bound to loopback only:

```bash
RECMEET_AUTH_TOKEN=$(openssl rand -hex 32) \
  recmeet-daemon --listen 127.0.0.1:29991
```

Clients point at the public hostname over HTTPS — but **wait**: recmeet's wire protocol is not HTTP. The reverse proxy must run in **TCP/stream mode**, not HTTP mode. Caddy's default `reverse_proxy` directive in an HTTPS site assumes HTTP — which corrupts the binary framing. The correct shape uses the [`layer4`](https://github.com/mholt/caddy-l4) plugin to terminate TLS without parsing HTTP:

```caddy
{
    layer4 {
        :443 {
            tls
            route {
                proxy {
                    upstream 127.0.0.1:29991
                }
            }
        }
    }
}
```

Caddy with the layer4 plugin needs a custom build (`xcaddy build --with github.com/mholt/caddy-l4`). For production, the nginx pattern below is more battle-tested.

### Pattern B: nginx stream + certbot (recommended for production)

nginx's `stream` module proxies raw TCP and is binary-safe by construction. certbot fetches and renews the Let's Encrypt cert; nginx terminates TLS on port 443 (or any other port) and forwards plaintext to the daemon on loopback.

`/etc/nginx/nginx.conf` (excerpt):

```nginx
stream {
    upstream recmeet_daemon {
        server 127.0.0.1:29991;
    }

    server {
        listen 443 ssl;

        ssl_certificate     /etc/letsencrypt/live/recmeet.example.org/fullchain.pem;
        ssl_certificate_key /etc/letsencrypt/live/recmeet.example.org/privkey.pem;

        ssl_protocols       TLSv1.3;
        ssl_ciphers         TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256;

        proxy_pass          recmeet_daemon;
        proxy_timeout       300s;
        proxy_connect_timeout 30s;
    }
}
```

certbot setup (one-time):

```bash
sudo apt install certbot          # Debian/Ubuntu; equivalent on other distros
sudo certbot certonly --standalone -d recmeet.example.org
# Add a renewal hook to reload nginx after each renewal:
echo 'systemctl reload nginx' | sudo tee /etc/letsencrypt/renewal-hooks/post/reload-nginx.sh
sudo chmod +x /etc/letsencrypt/renewal-hooks/post/reload-nginx.sh
```

certbot installs a systemd timer for automatic renewal — verify with `systemctl list-timers | grep certbot`.

Daemon bound to loopback only:

```bash
RECMEET_AUTH_TOKEN=$(openssl rand -hex 32) \
  recmeet-daemon --listen 127.0.0.1:29991
```

Clients connect to `recmeet.example.org:443` and the recmeet CLI / tray runs through the TLS layer transparently. The wire format never changes; nginx just wraps the bytes.

> **Caveat — client-side TLS termination.** The recmeet client does not currently speak TLS natively. To use this pattern, the operator runs a local stunnel (or nginx in `stream`-client mode) on the client laptop that wraps the recmeet client's loopback TCP in TLS to the public endpoint. See [`V2-DEPLOYMENT.md` Pattern A: stunnel TLS terminator](V2-DEPLOYMENT.md#pattern-a-stunnel-tls-terminator) for the full client-side stunnel config. Native client TLS is on the post-`v2.0.0` wishlist.

---

## Server sizing

The daemon does all the heavy compute — transcription with whisper.cpp, diarization with sherpa-onnx, optional summarization with llama.cpp. Sizing depends on which models you intend to run and whether you have a GPU.

### CPU and RAM

| Workload | Whisper model | RAM budget | CPU notes |
|---|---|---|---|
| Light single-user (one tray) | `base` (150 MB) | 4 GB | 4+ cores; transcription is the bottleneck |
| Standard single-user | `small` (500 MB) | 6 GB | 6+ cores; comfortable for typical 1-hour meetings |
| Power user / multi-tray | `medium` (1.5 GB) | 10 GB | 8+ cores; daemon's `MemoryMax=14G` cgroup cap is the safety net |
| Maximum quality | `large-v3` (3 GB) | 14 GB | 12+ cores or GPU strongly recommended |
| With local summarization | + Qwen2.5-7B-Q4 (4.5 GB) | + 6 GB | llama.cpp is also CPU-saturating; expect overlapping load |

The daemon ships with cgroup memory caps (`MemoryHigh=10G` / `MemoryMax=14G` / `MemorySwapMax=0` — see `dist/recmeet-daemon.service.in`) tuned for a host with a 16 GB physical floor. Chunked diarization (T2.1, iter 121) keeps per-chunk RSS under 6 GB on a 60-minute meeting, so a 16 GB host can comfortably handle multi-hour audio without swapping.

Multi-tray planning: each tray streams to the daemon over the same wire, and the daemon's three typed slots (`postprocess`, `streaming`, `model_download`) are capacity-1, so two trays submitting at the same time queue serially behind a single `postprocess` slot. This is intentional — concurrent transcription would thrash. Plan capacity by aggregate meeting-hours-per-day, not by client count.

### GPU acceleration

Whisper benefits massively from Vulkan acceleration when the host has a GPU and the driver stack. On a Radeon Pro W5500 with Mesa RADV, whisper-medium on a 63-minute meeting runs ~26× faster on GPU (~16 min) than on CPU (~7 h). Speaker diarization stays on CPU regardless (onnxruntime would need a separate ROCm/MIGraphX build); summarization with llama.cpp also benefits from Vulkan when enabled.

The build is **single-binary with automatic CPU fallback** — GPU backends ship as separate `libggml-vulkan.so` plugins loaded at startup. If the host has no GPU, `recmeet-daemon` still runs unmodified and emits a clean `ggml: active backend: CPU` banner.

```bash
# Default — autodetect Vulkan if the toolchain is present
make build

# Force Vulkan ON at configure time (fail if unavailable)
cmake -B build -G Ninja -DRECMEET_GGML_VULKAN=ON

# Force CPU-only (distro packagers shipping universal artifacts)
cmake -B build -G Ninja -DRECMEET_GGML_VULKAN=OFF
```

At daemon startup, look for the active-backend banner in the journal:

```
ggml: backend registry: CPU, Vulkan
ggml: active backend: Vulkan (AMD Radeon Pro W5500 (RADV NAVI10), 8192 MB)
```

If you built with Vulkan but the GPU is not being used, the banner emits a `WARN` line naming the gap (missing ICD driver, headless host, no `/dev/dri/renderD*`, etc.) before falling back to CPU. See [`BUILD.md`](BUILD.md#gpu-acceleration-vulkan) for the full toolchain matrix and VRAM measurements.

**Override the backend path** for non-standard layouts via `RECMEET_GGML_BACKEND_PATH` — the daemon resolves this env var when looking for the Vulkan plugin so distro packagers shipping the plugin in a non-default directory can wire it up without rebuilding.

### Disk

Model cache: plan for 4–5 GB if you want `base` + `large-v3` + the full sherpa stack; 10–15 GB if you also want local summarization. Models live under `~/.local/share/recmeet/models/`.

Meeting storage: each per-meeting WAV is 16 kHz mono S16LE (115 MB / hour). At a typical user's pace (~5 meetings/week of ~1 h), that is ~2.3 GB/week. The daemon does not garbage-collect old meetings; operators manage `~/meetings/` with standard filesystem tools.

Staging: postprocess uploads stage under `fs::temp_directory_path()` (typically `/tmp`) and are cleaned on subprocess exit. Streaming sessions accumulate their disk-backed WAV at the same PCM rate (~115 MB / hour) — plan `/tmp` accordingly, or mount it as `tmpfs` so it clears on reboot. See [`V2-DEPLOYMENT.md` Per-job staging](V2-DEPLOYMENT.md#per-job-staging) for cleanup recipes.

### Network

The TCP wire carries: occasional NDJSON control frames (~KB), one full WAV upload per `process.submit` (~115 MB / hour of recorded audio), continuous `0x03` PCM frames during a live-caption session (~32 KB/sec sustained), and artifact downloads (~MB per finished meeting). For a single-user single-server deployment on a typical LAN (Gigabit Ethernet, Wi-Fi 6) bandwidth is never the bottleneck. For a remote daemon over a 100 Mbit/sec uplink, uploading a 1-hour WAV (~115 MB) takes ~10 seconds; live captioning's sustained 32 KB/sec is also trivially within budget.

---

## Package split guidance (`recmeet-client` vs `recmeet-server`)

V2 currently ships as a single `recmeet-git` package on Arch (and equivalent universal `.deb` / `.rpm`), which installs every binary plus every dependency on every host. This is the simplest distribution model and stays the default through `v2.0.x`. A future split into two packages is planned but **not yet shipped** — flagged here so the architecture is visible to packagers and operators planning multi-host deployments.

### Forward plan: two PKGBUILDs

- **`recmeet-client-git`** — slim client install for laptops. Ships `recmeet`, `recmeet-tray`, `recmeet-mcp`, `recmeet-agent`. Pulls in PipeWire + PulseAudio + GTK3 + ayatana-appindicator — no ML deps. `ldd $(recmeet-tray) | grep -E 'whisper|sherpa|llama|onnx|ggml'` should be empty (Phase E.3 binary slimming guarantees this). Install size: small.
- **`recmeet-server-git`** — full server install for daemon hosts. Ships `recmeet-daemon`, `recmeet-web`. Pulls in whisper.cpp + sherpa-onnx + llama.cpp + onnxruntime + libcurl. Does **not** depend on PipeWire or PulseAudio — `ldd $(recmeet-daemon) | grep -E 'pipewire|pulse'` is empty (Phase E.4 binary slimming guarantees this).
- **`recmeet-vulkan-git`** — optional GPU acceleration plugin. Installs `libggml-vulkan.so` under the server's plugin path. Only needed on daemon hosts with a Vulkan-capable GPU. Picked up automatically by `recmeet-daemon` at startup (see the active-backend banner above).

Until the split lands, packagers and operators can still get the slim shape on a client host by building with the right CMake flags:

```bash
# Client-only build: skip the daemon, web, and ML deps
cmake -B build -G Ninja \
    -DRECMEET_BUILD_TRAY=ON \
    -DRECMEET_BUILD_WEB=OFF \
    -DRECMEET_USE_SHERPA=OFF \
    -DRECMEET_USE_LLAMA=OFF
ninja -C build recmeet-tray recmeet
```

```bash
# Server-only build: headless, no tray, full ML stack
cmake -B build -G Ninja \
    -DRECMEET_BUILD_TRAY=OFF \
    -DRECMEET_BUILD_WEB=ON \
    -DRECMEET_USE_SHERPA=ON \
    -DRECMEET_USE_LLAMA=ON
ninja -C build recmeet-daemon recmeet-web
```

The corresponding install steps are obvious — copy the built binaries plus their systemd units into the target host's filesystem. See [`BUILD.md`](BUILD.md) for the full build-flag matrix.

---

## See also

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — the authoritative architectural overview.
- [`V2-DEPLOYMENT.md`](V2-DEPLOYMENT.md) — multi-host deployment reference (single-host, LAN, stunnel, WireGuard, systemd units, troubleshooting). This guide complements that one; deploy via that doc first, then layer in the patterns above.
- [`V2-STRATEGY.md`](V2-STRATEGY.md) — V1 → V2 migration strategy + Phase status table.
- [`IPC-WIRE-PROTOCOL.md`](IPC-WIRE-PROTOCOL.md) — frame-layer spec (0x00/0x01/0x02/0x03 opcodes) + job state machine + resume-token re-association.
- [`IPC-VERBS.md`](IPC-VERBS.md) — per-verb IPC reference.
- [`BUILD.md`](BUILD.md) — build-system tutorial + per-distro dependency lists + Vulkan toolchain notes.
- [`../README.md`](../README.md) — project overview.
- [`../QUICKSTART.md`](../QUICKSTART.md) — end-user quickstart.
