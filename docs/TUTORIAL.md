<!--
Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
SPDX-License-Identifier: MIT OR Apache-2.0
-->

# Tutorial: First dual-source call (laptop client ↔ server)

A from-scratch, copy-paste walkthrough for the common V2 topology:

- **Server host** (here `bd770i`) — the capable box. Runs `recmeet-server`,
  which does all the heavy compute (transcribe / diarize / identify /
  summarize).
- **Client host** (here `s76`, a no-GPU PopOS laptop) — runs `recmeet-client`,
  the thin tray applet. It captures audio and ships a WAV to the server.

The goal is a **dual-source recording**: your microphone **and** the system
audio (the remote participants on a video call) mixed into one note. Mixing
happens entirely on the client; the server never sees two streams.

> Binary names are V2: `recmeet-server`, `recmeet-client`, `recmeet-cli`.
> (V1 used `recmeet-daemon` / `recmeet-tray` / `recmeet` — don't mix them up.)

For the deeper deployment reference (TLS, WireGuard, PSK rotation, capacity
planning) see [V2-DEPLOYMENT.md](V2-DEPLOYMENT.md). For build internals see
[BUILD.md](BUILD.md). This tutorial is the fast path.

---

## 0. Before you start

- Both hosts on the **same trusted LAN** (the wire is PSK-authenticated but
  **not encrypted** — fine for a LAN you own, not for the public internet).
- The server host can reach the internet for the summarization API (or you run
  local llama.cpp summarization — out of scope here).
- On the client laptop: **a microphone and a pair of headphones.** The
  headphones matter for testing (§5) — they're how you prove the *monitor*
  capture works rather than the mic just hearing the speakers.
- Pick a port (this guide uses `29991`) and generate one shared secret:

  ```bash
  openssl rand -hex 32      # 64 hex chars — your PSK. Keep it; both hosts need it.
  ```

---

## 1. Server on `bd770i`

### 1.1 Stop V1 (if running)

V2 uses separate XDG dirs so it *can* coexist, but for a first bring-up keep
it simple:

```bash
systemctl --user stop recmeet-daemon.service 2>/dev/null || true
systemctl --user disable recmeet-daemon.service 2>/dev/null || true
```

### 1.2 Build & install the server

The server needs the full ML stack. The onnxruntime build is the slow part
(~20 min) and only needs doing once:

```bash
cd ~/code/recmeet
git fetch && git checkout feat/v2-thin-client && git pull
make build-onnxruntime      # ~20 min, first time only → vendor/onnxruntime-local/
make build                  # builds recmeet-server, recmeet-cli, etc.
make install PREFIX="$HOME/.local"   # or sudo make install for /usr/local
```

### 1.3 Configure and run the daemon (TCP + PSK)

The server **refuses to start a TCP listener without `RECMEET_AUTH_TOKEN`** —
this is fail-fast by design. Quick manual run to confirm it works:

```bash
RECMEET_AUTH_TOKEN='<the 64-hex PSK from §0>' \
  recmeet-server --listen 0.0.0.0:29991 --log-level info
```

You should see `daemon: PSK auth enabled for TCP listener` in the log. Confirm
the socket is open (from another shell on `bd770i`):

```bash
ss -lnt | grep 29991
```

For a persistent install, use a systemd drop-in with a `0600` env file:

```bash
mkdir -p ~/.config/systemd/user/recmeet-server.service.d
cat > ~/.config/systemd/user/recmeet-server.service.d/listen.conf <<'EOF'
[Service]
EnvironmentFile=%h/.config/recmeet-server/daemon.env
ExecStart=
ExecStart=%h/.local/bin/recmeet-server --listen 0.0.0.0:29991
EOF

install -m 0600 /dev/stdin ~/.config/recmeet-server/daemon.env <<EOF
RECMEET_AUTH_TOKEN=<the 64-hex PSK from §0>
EOF

systemctl --user daemon-reload
systemctl --user enable --now recmeet-server.service
systemctl --user status recmeet-server.service
```

> The empty `ExecStart=` line is required — it clears the shipped command
> before the override takes effect.

Server-side files live under `~/.config/recmeet-server/` (config) and
`~/.local/share/recmeet-server/` (models, meetings).

---

## 2. Client on `s76` (no GPU)

### 2.1 Client-only build — skip the ML stack

The thin client links **none** of whisper / onnxruntime / llama / Vulkan, so
you don't build any of it. This recipe was verified to produce a 1.3 MB binary
that links only standard desktop libraries (PipeWire, PulseAudio, GTK3,
libappindicator, libsndfile, curl):

```bash
cd ~/code/recmeet
git fetch && git checkout feat/v2-thin-client && git pull

cmake -B build -G Ninja \
  -DRECMEET_USE_SHERPA=OFF \
  -DRECMEET_USE_LLAMA=OFF \
  -DRECMEET_BUILD_GO_TOOLS=OFF \
  -DRECMEET_BUILD_TESTS=OFF

ninja -C build recmeet-client        # ~tens of seconds — no onnxruntime, no whisper
```

`-DRECMEET_USE_SHERPA=OFF` is what drops the onnxruntime requirement entirely;
`ninja recmeet-client` builds only the client target and its deps
(`recmeet_ipc` + `recmeet_capture`), never `recmeet_core`/whisper.

Sanity-check the binary is a clean thin client (this should print **nothing**):

```bash
ldd build/recmeet-client | grep -Ei 'whisper|onnxruntime|ggml|llama|vulkan|sherpa'
```

Install the client binary, desktop entry, and icons:

```bash
sudo ninja -C build install
# or, minimal: cp build/recmeet-client ~/.local/bin/
```

PopOS runtime packages (if `ldd` shows anything "not found"): `pipewire`,
`pipewire-pulse`, `libpulse0`, `libsndfile1`, `libgtk-3-0`,
`libayatana-appindicator3-1`, `libcurl4`.

### 2.2 Point the client at the server

The client takes the server address via `--daemon` or the
`RECMEET_DAEMON_ADDR` env var, and the PSK via `RECMEET_AUTH_TOKEN`:

```bash
# ~/.bashrc or ~/.zshrc on s76 — treat this file like a credential
export RECMEET_DAEMON_ADDR='bd770i:29991'
export RECMEET_AUTH_TOKEN='<the same 64-hex PSK from §0>'
```

Then launch the tray:

```bash
recmeet-client          # picks up both env vars
# or explicitly:
recmeet-client --daemon bd770i:29991
```

On connect the client sends the PSK as its first frame; on success the tray's
connection indicator goes green. A bad token → `auth.error`, surfaced in the
tray with backoff retry. Quick CLI check of the link:

```bash
recmeet-cli --daemon-addr bd770i:29991 --status
```

Client-side files live under `~/.config/recmeet-client/` (`client.yaml`) and
`~/.local/share/recmeet-client/`.

---

## 3. Choose your audio sources

In the tray menu:

- **Mic Source ▸** — pick your microphone (or leave on auto-detect).
- **Monitor Source ▸** — pick the **`.monitor` of the sink your call audio
  plays to.** This is the loopback that captures the remote participants.
  Auto-detect chooses the default sink's monitor; if you route call audio to a
  specific output (e.g. headphones), select *that* sink's `.monitor`.
- **Mic Only** (checkbox) — leave **unchecked** for dual-source. Checking it
  forces single-mic capture (the pre-dual behavior).

Find the monitor source name on the laptop with:

```bash
pactl list short sources | grep monitor
```

> **Echo / self-capture rule:** the monitor must be the **output** sink's
> loopback, never your microphone's `.monitor`. The auto-detector enforces
> this; if you pick manually, choose the sink you're *listening to*.

---

## 4. Record

1. Tray ▸ **Start Recording**. The client opens *two* captures: the mic
   (PipeWire) and the monitor (PulseAudio `pa_simple` for `.monitor` names, or
   PipeWire `CAPTURE_SINK` with a `pa_simple` fallback for other sinks /
   Bluetooth).
2. Talk; let the remote audio play.
3. Tray ▸ **Stop Recording**. The client mixes mic + monitor **offline, once**
   into a single mono WAV and offers **Submit / Save for later / Discard**.
4. **Submit** sends the mixed WAV to `bd770i` for transcription, diarization,
   and summarization; the note comes back with the meeting.

If no monitor was found or it failed to open, the recording **continues
mic-only with a warning** — never a hard failure.

---

## 5. Acceptance test — prove dual-source actually works

The trap: if the "remote" audio plays through **speakers**, your mic hears it
too, so you can't tell whether the remote voice reached the note via the
*monitor capture* or just the mic. **Use headphones** — then the only path for
that audio into the recording is the monitor loopback. That's the real test.

Pick a recognizable "remote" clip: a podcast, a YouTube talk, or a TTS clip
with sentences you can later grep for in the transcript.

### Test 1 — verify the mix locally (no server needed)

The fastest feedback loop. You don't even need `bd770i` up.

1. Headphones on. Set Monitor Source to your headphone sink's `.monitor`.
2. Start Recording → play the clip for ~20 s **and** say a couple of sentences
   into the mic → Stop → choose **Save for later** (keeps the staging WAV).
3. Play the staged file back:

   ```bash
   STAGING="${XDG_DATA_HOME:-$HOME/.local/share}/recmeet-client/staging"
   paplay "$(ls -t "$STAGING"/audio_*.wav | head -1)"
   ```

   You should hear **both** your voice and the clip in that one mono file. If
   you do, dual capture + offline mix work. ✅

### Test 2 — monitor-only contribution (the discriminator)

1. Headphones on. Start Recording → play the clip → **stay completely silent
   on the mic** → Stop → Submit.
2. The note's transcript should contain the **clip's** speech. Since your mic
   was silent and the clip was on headphones, that text could *only* have come
   through the monitor capture. ✅ Proves the monitor path end-to-end.

### Test 3 — full dual + real meeting

1. A real Meet/Zoom call on `s76`, **headphones on**, with someone remote.
2. Record the call → Submit → confirm the note contains **both sides** of the
   conversation. This is the one criterion automated tests can't cover.

### Test 4 — graceful degradation

1. Set Monitor Source to a device you then unplug/remove (or a bogus name) →
   Start → Stop. The recording should complete **mic-only with a warning**, not
   crash. ✅

### Test 5 — Bluetooth fallback (optional)

Repeat Test 3 with a **Bluetooth** headset as the output. This exercises the
PipeWire `CAPTURE_SINK` → `pa_simple` fallback path that exists specifically
for Bluetooth sinks.

---

## 6. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| Tray indicator never goes green | Wrong `RECMEET_DAEMON_ADDR`, server not listening (`ss -lnt \| grep 29991` on `bd770i`), or firewall. |
| `auth.error` in tray / `recmeet-cli --status` fails | PSK mismatch. The `RECMEET_AUTH_TOKEN` must be byte-identical on both hosts. |
| Server won't start | TCP listener with no `RECMEET_AUTH_TOKEN` — it fail-fasts by design. Set the token (or use a Unix-socket `--listen` for local-only). |
| Note has only your voice, not the remote | Monitor Source not set / set to the wrong sink, or **Mic Only** is checked. Verify `pactl list short sources \| grep monitor` and the tray selection. |
| Remote voice present even with mic muted on speakers | Expected — speakers leak into the mic. Re-run with **headphones** for a clean result (§5). |
| `ldd build/recmeet-client` shows ML libs | You built the full tree, not the client-only recipe. Re-configure with `-DRECMEET_USE_SHERPA=OFF` (§2.1). |
| Build still compiles onnxruntime on `s76` | `RECMEET_USE_SHERPA` wasn't `OFF`, or you ran `ninja` (all targets) instead of `ninja recmeet-client`. |

---

## What this does *not* cover (by design, for V2.0.0)

- **Monitor-only recording** (no mic) — V1 had no such mode; out of scope.
- **Routing live captions to the monitor** in dual mode — captions stay on the
  mic (V1 parity). A future enhancement.
- **`--keep-sources`** retaining separate `mic.wav` / `monitor.wav` on the
  client — not yet wired in V2; use the staged-WAV check in Test 1 instead.
- **Per-source speaker identity** (who's local vs. remote) — tracked
  separately as `v2-source-aware-diarization`.
