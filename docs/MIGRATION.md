# Migrating from recmeet V1 to V2

V2 is a binary-rename + path-split release. V1 and V2 can be installed
side-by-side on the same machine at the same install prefix
(`~/.local` by default); they do not share binaries, sockets, systemd
units, or config/data directories. There is **no automatic config
migration** — V2 starts with sane defaults.

## What's different

V2 is a thin-client architecture:

- **`recmeet-server`** (was `recmeet-daemon`) does the heavy ML
  compute — transcription, diarization, speaker identification,
  summarization, live captions.
- **`recmeet-client`** (was `recmeet-tray`) is the system tray applet
  + WebUI bridge + local audio capture (PipeWire/PulseAudio).
- **`recmeet-cli`** (was `recmeet`) is the standalone / scripting
  CLI; it also runs the heavy ML pipeline in-process when invoked
  with `--no-daemon`.

Clients talk to the server over a Unix socket (default, local-only)
or TCP (`--listen` / `--daemon-addr` with PSK auth, for running the
server on a separate machine).

## V1 + V2 coexistence

V2 binds non-overlapping defaults so a V1 install can keep running
unmodified:

| Resource           | V1                                              | V2                                                          |
| ------------------ | ----------------------------------------------- | ----------------------------------------------------------- |
| Daemon binary      | `recmeet-daemon`                                | `recmeet-server`                                            |
| Tray / client binary | `recmeet-tray`                                | `recmeet-client`                                            |
| CLI binary         | `recmeet`                                       | `recmeet-cli`                                               |
| Config dir         | `~/.config/recmeet/`                            | `~/.config/recmeet-server/` and `~/.config/recmeet-client/` |
| Data dir           | `~/.local/share/recmeet/`                       | `~/.local/share/recmeet-server/` and `~/.local/share/recmeet-client/` |
| State dir          | (none — V1 used data dir)                       | `~/.local/state/recmeet-server/`                            |
| Default socket     | `$XDG_RUNTIME_DIR/recmeet/daemon.sock`          | `$XDG_RUNTIME_DIR/recmeet-server/server.sock`               |
| systemd unit (daemon) | `recmeet-daemon.service` + `.socket`         | `recmeet-server.service` + `.socket`                        |
| systemd unit (tray)   | `recmeet-tray.service`                        | `recmeet-client.service`                                    |
| Desktop file       | `recmeet-tray.desktop`                          | `recmeet-client.desktop`                                    |

The V1 install at `~/.config/recmeet/` and `~/.local/share/recmeet/`
is **read-never-written** by V2.

## No automatic migration

V2's first launch creates fresh `server.yaml` (under
`~/.config/recmeet-server/daemon.yaml`) and `client.yaml` (under
`~/.config/recmeet-client/client.yaml`) with the same admin defaults
that V1 shipped with. Operators wanting to carry forward V1 settings
should open `~/.config/recmeet/config.yaml` in one editor pane and the
two new V2 files in another and copy field-by-field. (The two-file
split mirrors the daemon-side / client-side split of the architecture:
ML / job-processing settings live on the server; UI selections live on
the client.)

## Sharing models (optional)

The whisper, sherpa, and llama model files cached under
`~/.local/share/recmeet/models/` total several GB. Disk-constrained
hosts can share the V1 cache with V2 via a one-line symlink before
the first V2 launch:

```sh
ln -s ~/.local/share/recmeet/models ~/.local/share/recmeet-server/models
```

V2 detects the existing cache and reuses it; `recmeet-cli --download-models`
remains a no-op when models are already present.

## Speaker database is NOT shared

Enrolled voiceprints under `~/.local/share/recmeet/speakers/` are
**not** symlinked or copied automatically. V2 starts with an empty
`~/.local/share/recmeet-server/speakers/`. Operators wanting to carry
voiceprints forward can copy the per-speaker JSON files manually:

```sh
cp -r ~/.local/share/recmeet/speakers/* ~/.local/share/recmeet-server/speakers/
```

Schema compatibility across V1 → V2 is not formally verified. Test
with a small set of speakers before bulk-copying a production
enrollment.

## Client UI state lives in client.yaml

Tray-remembered selections persist client-side, not server-side:

- `whisper_model` (last whisper model selected in the menu)
- `diarize` (last diarization toggle state)
- `vad` (last VAD toggle state)
- `captions_enabled` (last overlay-visible preference)

These live in `~/.config/recmeet-client/client.yaml` under the
`transcription:` and `captions:` blocks. Editing them directly works;
the next `recmeet-client` launch picks them up.

The server's own admin defaults for the same fields live in
`~/.config/recmeet-server/daemon.yaml`. When a connected client pushes
its remembered selection over the wire (via `session.init` and
`session.update_prefs`), the server uses the client value for that
session and falls back to the admin default for sessions that don't
supply one.

## Mid-session preference propagation

Three menu toggles propagate to the server **immediately**, mid-recording:

- Whisper model
- Diarize on/off
- VAD on/off

The remaining menu setters (mic source, monitor source, language,
mic-only, no-summary, llm-model, provider switch, api-model, output
dir, note dir) save to `client.yaml` but **do not** push to a
connected session. They take effect on the next reconnect or
`recmeet-server` restart.

This is a deliberate v2.0.0 scope decision — see the follow-up task
filed under `agentctx/tasks/` for the architectural target.

## systemd user-unit migration

If V1's systemd units are enabled and you want V2's units to handle
autostart instead:

```sh
# Disable V1
systemctl --user disable --now recmeet-daemon.service recmeet-tray.service
systemctl --user disable recmeet-daemon.socket

# Enable V2
systemctl --user daemon-reload
systemctl --user enable --now recmeet-server.service
systemctl --user enable --now recmeet-client.service
```

Both V1 and V2 units can be installed simultaneously; only the
`enable --now` decides which one autostarts. The non-enabled set can
be re-enabled later without reinstalling.

## Uninstalling V1 after V2 is running

V2 does not touch V1's install. Removing V1 is a clean
`make uninstall` from a V1 worktree (or distro-specific uninstall).
V2 keeps running.

If you also want to recover the disk used by V1's data / config:

```sh
rm -rf ~/.config/recmeet               # config + vocab
rm -rf ~/.local/share/recmeet          # models, logs, speakers
```

Models can be re-fetched on V2 via `recmeet-cli --download-models`.
