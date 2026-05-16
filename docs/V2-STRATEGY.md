# recmeet V2 Strategy and Development Process

This document captures the strategic decisions governing the transition from
recmeet V1 (the current architecture: monolithic daemon owns capture +
compute) to recmeet V2 (thin-client architecture: client owns capture, server
owns compute). The V2 implementation plan originally lived at
`agentctx/tasks/thin-client-recording-server.md`; as of iter 156 Phases A,
B, and C have all landed on `feat/v2-thin-client`. This document covers
**how the codebase forks, evolves, and stays interoperable**, not what V2
builds.

The feature roadmap proper lives in `docs/ROADMAP.md`. This document is its
process companion. For V2 operator-facing topics, see
`docs/ARCHITECTURE.md`, `docs/IPC-VERBS.md`,
`docs/IPC-WIRE-PROTOCOL.md`, and `docs/V2-DEPLOYMENT.md`.

---

## Implementation status as of iter 156

V2 Phases A, B, and C have all landed on `feat/v2-thin-client`:

- **Phase A (iter 138-152) — Security foundation.** PSK authentication
  gate on the TCP listener (`RECMEET_AUTH_TOKEN`); per-fd outbound queue;
  connection cap; `client_id` minting at accept; `protocol_version`
  handshake (current value: 3); `session.init` with subprocess credential
  merge. The daemon fail-stops on TCP bind without a PSK configured.
- **Phase B (iter 152) — Audio-capture migration to client.** Extracted
  `recmeet_capture` library with a fan-out subscriber API. The tray now
  owns capture; the daemon no longer links PipeWire or PulseAudio.
- **Phase C (iter 155) — Submit/process/fetch + server-side JobQueue +
  streaming.** Eleven sub-phases (C.1 → C.10b) across twelve commits:
  framed wire protocol (frame types `0x00` NDJSON, `0x01` binary upload,
  `0x02` binary artifact, `0x03` streaming PCM); state-machine
  `FrameReader`; `JobQueue` with three typed slots (postprocess,
  streaming, model_download — each capacity-1, independent);
  `process.submit` / `process.fetch` / `process.cancel` /
  `process.stream` / `process.stream.cancel` /
  `process.stream.commit` plus `job.status` / `job.list` verbs;
  `enroll.finalize` two-step dance; `record.start` removal (123 test
  sites migrated to `process.submit` / `process.stream`).
  `IPC_PROTOCOL_VERSION` bumped 1 → 3.

The V2 wire protocol is now stable: 14 V2 verbs, 5 V2 events, frame-typed
upload/download channel. The architectural-proof test
`tests/test_v2_thin_client_e2e.cpp` (Wave 1 of iter-156 stabilization)
exercises the real `recmeet-daemon` binary end-to-end over TCP — submit,
upload, process, fetch — and is wired into CI via `make integration-e2e`.

Operator-facing documentation is now V2-aware: `README.md`,
`QUICKSTART.md`, `docs/V2-DEPLOYMENT.md`, `docs/ARCHITECTURE.md`,
`docs/COMPONENT-DIAGRAMS.md`, `docs/IPC-VERBS.md`, and
`docs/IPC-WIRE-PROTOCOL.md`.

Outstanding before a `v2.0.0` tag:

- **Phase D — Client-side queueing + reconnect.** In-memory submission
  queue, drain worker with exponential backoff reconnect, persistent
  `(endpoint, job_id)` tuples in `pending_jobs.json` across tray
  restarts, server-restart notification, tray UI for queue depth and
  per-server view, save-for-later WAV persistence across tray restart.
- **Phase E — Cleanup, schema split, binary slimming, docs polish.**
  Split `config.yaml` into `daemon.yaml` (server-side keys) and
  `client.yaml` (client-side keys); strip remaining V1-only code paths;
  ldd assertions on the slimmed tray binary; final docs sweep.
- **Minor finding (Phase E candidate or earlier follow-up).** No PSK
  handshake deadline exists in the `IpcServer` poll loop today — a
  slowloris-class resource exhaustion vector against the `PendingPsk`
  slot pool. Surfaced as SUCCEED-with-INFO by the Wave 2 test
  additions; needs a `psk_deadline_ms` reaper in the poll loop.

---

## Context

V1 has reached operational maturity. The pipeline (capture → transcribe →
diarize → identify → summarize) is correct and well-tested across 451 unit
tests, 51 IPC integration tests, 3 full-stack tests, and the iter-121 work
landed the long-audio containment target (4-hour audio on a 16 GB host).
The remaining V1-shaped capstone is live captioning (`docs/ROADMAP.md`
Phase 2b). Beyond that, V1 is a useful product on its own terms: simple,
single-host, multi-provider LLM support including local self-hosted models.

V2 is a near-rewrite. The thin-client / heavy-server split changes where
audio capture happens, what the daemon links against, the wire protocol
(NDJSON + length-prefixed binary frames), the config schema (split into
`daemon.yaml` + `client.yaml`), the binary surface (tray drops onnxruntime
/ sherpa / whisper / llama / ggml), and the recording state machine
(client owns it). Calling V2 "almost a new product" is fair.

The realistic prediction: cross-version code overlap is low. Most V1 fixes
will have no V2 counterpart and vice versa, because the codepaths affected
do not exist on the other side. This shapes everything below.

---

## The split decision

**Single repo, V1 on a long-lived `v1-maintenance` branch from the
`v1.5.0` tag, V2 on `main`.**

### Considered alternatives

- **Separate repo (`recmeet-server` + keep `recmeet`).** Cleanest
  separation; each side picks its own dependency floor. Rejected because
  shared concerns (frontmatter, summarization, meeting-dir schema) would
  immediately need a vendored copy or git submodule, and the
  administrative overhead of two repos (two issue trackers, two CI
  setups, two release lines) is not justified by the marginal isolation
  gain.
- **Long-lived feature branch in same repo.** The usual antipattern
  argument (frequent two-way merges become unmanageable) does not apply
  here, because development is one-directional: V1 is in maintenance
  mode, not actively chasing parity with `main`. The maintenance-branch
  model that Linux kernel, Postgres, and Python use for stable lines is
  the right reference, not the long-lived feature branch model.
- **Monorepo with `v1/` and `v2/` top-level directories.** Atomic
  cross-version commits, single CI. Rejected because it requires
  restructuring the current repo layout for no clear benefit; the V1
  layout works and breaking it just to make room for V2 is gratuitous.

---

## Branch lifecycle and sequencing

1. **Live captioning lands on current `main` as the V1 capstone.** This
   is intentional: live captioning forces resolving streaming-ASR
   latency, model choice, and quality tradeoffs in the simpler V1
   single-process context before V2 inherits them in a more complex
   client/server context.
2. **Tag `v1.5.0` from `main`** once live captioning is shipped and any
   release-blocking polish lands. (1.5.0 follows the existing 1.3.x and
   1.4.x V1 lineage; it is the V1 capstone, not the inaugural V1 release.)
3. **Cut `v1-maintenance` from `v1.5.0`.** This branch is the V1 line
   forever after.
4. **`main` becomes V2 development trunk.** First V2 commit is the
   start of the thin-client work (Phase A: security foundation).
5. **V1 patch releases** tagged from `v1-maintenance` as `v1.5.x`. The
   maintenance policy is patches only; future minor bumps are
   intentionally not anticipated (see "Maintenance commitment" below).
6. **V2 releases** tagged from `main` as `v2.0.0`, `v2.1.0`, etc.

Per-branch build state is kept in distinct CMake build directories
(`build/` for whichever branch is checked out, or
`build-v1/` / `build-v2/` if you want to keep both warm) so branch
switching does not force a full reconfigure.

---

## Backport policy

**Direction: one-way only — `main` → `v1-maintenance` via
`git cherry-pick`.** Never the reverse.

**Arbiter: John (human architect).** No automatic backport rule.

### Candidates

- Dependency version bumps (whisper.cpp, sherpa-onnx, onnxruntime,
  ggml, llama.cpp) where the V1 build accepts the new version without
  source changes.
- Security fixes in shared third-party code or in code that exists on
  both sides verbatim.
- Bug fixes in shared logic that did not get rewritten in V2:
  frontmatter generation, summarization request shaping, meeting
  directory schema handling, model download / cache logic.
- **Meeting-directory schema fixes** — explicitly called out because
  cross-version data compatibility (see §"Cross-version data schema")
  depends on this staying in sync.

### Non-candidates

- Anything touching V2's new IPC schema (binary frames, session
  handshake, `process.submit` / `process.fetch`).
- Anything in V2's capture-on-client codepath (`recmeet_capture` lib,
  client-side recording state machine).
- Anything in V2's server-side job queue or `(client_id, job_id)`
  routing.
- V2 binary slimming work (ldd assertions, dep removal).
- New V2 features outright (live captioning streaming, reconnect with
  backoff, persistence across tray restart, etc.).

When in doubt: the V1 codebase still has a working analog of the change?
Backport is a candidate. The change rewrote how that thing works? Not a
candidate.

---

## Architectural principles V2 inherits from V1

These are properties V1 has that V2 must preserve. They are not
negotiable design decisions in V2; they are constraints on the V2
plan.

1. **Capture is local-first, always.** Recording works without a server,
   without a network, without any external dependency. The audio bytes
   land on the local filesystem before any optional processing happens.
2. **Server features degrade gracefully.** Live captioning, batch
   transcription, summarization — all of these are additive layers on
   top of the local-first capture. If the server is offline, the client
   records anyway and the recording queues for later submission.
3. **The client never blocks on server reachability for the core
   record-and-keep-bytes flow.** Server outages, network partitions,
   restarts — none of these prevent the next recording from starting or
   completing.

These principles are why "stream audio to server, captions stream back"
is the V2 live-captioning answer rather than "client runs whisper for
captions": the streaming caption session is an additive optional layer
over the always-local capture, not a replacement for it.

---

## Coexistence on a single host

Both V1 and V2 binaries are installable side-by-side and both can run
concurrently. This is a deliberate goal — it makes A/B comparison
trivial during the V2 stabilization period.

### Binary naming

V1 binaries (unchanged):

- `recmeet` — CLI front-end (record, reprocess, speakers, vocab)
- `recmeet-daemon` — long-running daemon
- `recmeet-tray` — system tray applet
- `recmeet-web` — speaker management web UI (tray-spawned)
- `recmeet-mcp` — MCP server for AI agent integration
- `recmeet-agent` — agent CLI for prep/follow-up workflows

V2 binaries (proposed; subject to refinement during Phase A):

- `recmeet-server` — server admin CLI (status, config reload, speaker DB
  inspection)
- `recmeet-server-daemon` — long-running server process (replaces
  `recmeet-daemon`'s compute role)
- `recmeet-client` — client CLI (record from terminal, submit, fetch)
- `recmeet-client-tray` — client tray applet (capture, submit/discard
  UX, queue)
- `recmeet-client-web` — speaker management web UI (tray-spawned, V2
  plan keeps this tray-bundled)
- `recmeet-client-mcp` — MCP server, client-side (open question: is MCP
  client-only, or does it need a server-side counterpart?)

### Path namespacing

For coexistence, V2 must not write to paths V1 owns. Required
namespacing:

| Concern | V1 path | V2 path |
|---|---|---|
| Config | `~/.config/recmeet/config.yaml` | `~/.config/recmeet-v2/daemon.yaml` + `client.yaml` |
| Unix socket | `/run/user/$UID/recmeet/recmeet.sock` | `/run/user/$UID/recmeet-v2/server.sock` |
| TCP listen port | configurable (V1 default: off) | configurable (V2 default: off, distinct port number) |
| systemd units | `recmeet-tray.service` | `recmeet-client-tray.service`, `recmeet-server-daemon.service` |
| Logs | `~/.local/share/recmeet/logs/` | `~/.local/share/recmeet-v2/logs/` |
| Speaker DB | `~/.local/share/recmeet/speakers/` | `~/.local/share/recmeet-v2/server/speakers/` |
| Client staging | (V1 has no client staging) | `~/.local/share/recmeet-v2/client/staging/` |
| Pending jobs | (V1 has no pending jobs) | `~/.local/share/recmeet-v2/client/pending_jobs.json` |

### Shareable across versions

- **Model cache** (`~/.local/share/recmeet/models/` or via env var) —
  read-only, safe to share if both versions accept the same model
  versions. Saves multi-GB of duplicate downloads. If model versions
  diverge, fall back to per-version model dirs.
- **Meeting output dir** (`~/meetings/`) — see §"Cross-version data
  schema" below.

### Concurrency caveats

- Only one process can capture from the default mic at a time. If both
  V1 tray and V2 client tray try to record simultaneously, the second
  will fail. This is the same constraint as any two audio-capture apps
  on the same host.
- If both versions' web UIs run, they must bind to distinct ports.

---

## Cross-version data schema (the `~/meetings/` contract)

This is the most important shared interface between V1 and V2. Both
versions write into and read from `~/meetings/{name}_{YYYY-MM-DD}/`
using the same on-disk schema. This makes the meeting directory a
genuine portable artifact: a recording made by V1 can be reprocessed
by V2 and vice versa.

### Shared on-disk shape

- `audio_YYYY-MM-DD_HH-MM.wav` — 16 kHz mono PCM. V2 client capture
  matches V1 daemon capture format byte-for-byte.
- `context.json` (or per-instance `context_HH-MM.json` after iter
  125-126).
- `speakers.json` (per-meeting; lists who-spoke; iter 75 batch
  reidentify writes here).
- Meeting note in markdown with YAML frontmatter (written to
  `note_dir`, which can equal `output_dir` or be separate).

### Compatibility rules

- **Schema evolution is additive only.** New fields can be added; old
  fields cannot be renamed or removed without coordinated change to
  both lines.
- **Both versions ignore unknown fields.** A V2-written `context.json`
  with new keys must still load cleanly in V1.
- **V2 reprocess accepts a V1-recorded `~/meetings/foo/` directory** as
  input via the client-side file picker → `process.submit` flow.
- **V1 reprocess accepts the artifacts V2 produces** via existing
  `--reprocess` machinery.

### What does not share across versions

- **Voiceprint embeddings** are model-version-dependent. If V2 ships a
  newer embedding extractor, the same speaker has different vectors
  under each version. Each version maintains its own speaker DB; no
  attempt to share voiceprints across versions.
- **Live capture from the same mic** — see "Concurrency caveats" above.
- **New artifact types** — if V2 introduces new files into the meeting
  dir (e.g., live caption transcripts, streaming session logs), V1
  ignores them. V1's reprocess does not need them; V2's does not depend
  on V1 producing them.

### Implication

The `~/meetings/` schema is the de facto V1↔V2 wire format for
recordings at rest. **Any change to it is a backport candidate** —
adding a field on one side without porting the parser to the other
side breaks the shared-recording use case. This is one of the few
places where V2-side work routinely flows back to `v1-maintenance`.

---

## Deployment story

V2 collapses naturally into three canonical topologies. The full
operator-facing walk-through (config files, systemd units, PSK
generation, firewall posture) lives in `docs/V2-DEPLOYMENT.md`; this
section captures the strategy-level decisions only.

### Topology 1 — Single-host, local Unix socket

Daemon and tray run on the same machine. The daemon listens on the
per-user Unix socket only (TCP disabled). No PSK, no firewall rule,
no network exposure. This is the closest analog to V1's
single-binary-set deployment and the default for laptop users who
record on the same host that processes.

### Topology 2 — Single-host server, remote tray over the LAN / tailnet

Daemon runs on a headless host (home server, NUC, lab box); tray runs
on the operator's laptop or workstation. The daemon binds TCP on a
LAN address (or a Tailscale IP) and requires a PSK. Capture happens
on the tray host; bytes stream to the daemon over the framed wire
protocol; results flow back the same way. The tailnet case is the
recommended path because Tailscale provides encryption and identity;
without it, the PSK is the only auth layer and the operator must take
responsibility for transport-level confidentiality.

### Topology 3 — Multi-host, multi-user with TLS or VPN

Multiple tray clients connect to one daemon. Wire-level isolation is
provided by the transport (TLS terminator in front of the daemon, or
a shared VPN); the daemon's per-client `client_id` minting and
JobQueue per-client routing handle the rest. True multi-tenant work
(per-client auth tokens, quotas, isolation) is deferred to v3; in v2
the PSK is shared across all tray clients of a given daemon.

### PSK lifecycle

The PSK is operator-managed, file-on-disk or env-var, with no
rotation protocol baked into V2. Rotation is a restart-coordinated
event: change the secret on the daemon, restart it, push the new
secret to each tray. Forward-looking work (per-client tokens, OIDC,
mTLS) is explicitly v3 territory.

### Session identity and lifecycle

V2 introduces a **server-issued resumption token** as the first per-client
persistent credential. The PSK above gates connection; the resume_token
gates *re-association* with prior server-side state (`client_id`, session
credentials, session preferences, owned jobs) across a TCP reconnect.

On first connect after PSK auth, the server stamps `auth.ok` with a fresh
`client_id` (ephemeral, server-minted) and a `resume_token` (32 bytes of
real entropy via `getrandom(2)`, hex-encoded, constant-time compare on
lookup). The client persists the token to
`~/.local/share/recmeet/session.token` (0600). On reconnect, the client
re-sends both PSK and `resume_token`; the server's lookup table maps the
token to the prior `client_id` and rebinds owned jobs to the live
connection. Token-not-found and token-expired both fall through to a
fresh-connect path with new `client_id` + new `resume_token`.

The token is opaque — no JWT, no claims, no signature. It is a routing
key whose authority derives entirely from the lookup table on the
daemon. Rotating the PSK silently invalidates every outstanding
resume_token on next reconnect (PSK check happens first), giving
operators a coarse "log everyone out" lever without per-token revocation
machinery in V2.

#### Garbage collection (two half-lives)

The resume-token map is unbounded otherwise: every disconnect that never
returns leaks one `(resume_token → client_id → session state → owned
jobs)` entry, and every job those dead clients owned sits in the
JobQueue's per-job client binding consuming registry slots and
event-routing decisions on every emission. V2 specifies two distinct
half-lives, with different drivers, swept by a single periodic GC
thread inside the daemon (default cadence 5 min):

| Object | Driver | Default TTL | Knob (`[server]`) |
|---|---|---|---|
| Resume-token → session binding (creds, prefs, owned-jobs list) | "Operator closed the laptop, will they come back?" | **24 h** post-last-seen | `resume_token_ttl_hours` |
| In-flight jobs (`queued`/`running`/`downloading_model`) owned by a disconnected client | "A slot is being held hostage by a job nobody will fetch" | **1 h** post-disconnect — converts to `failed` and frees the slot; the WAV stays on disk for operator forensics | `inflight_orphan_ttl_minutes` |
| Terminal jobs (`complete`/`failed`/`cancelled`) owned by a disconnected client | "Holding artifacts for the client's eventual fetch on next reconnect" | **24 h** — matches the session TTL and C.8's already-planned 24 h diarization-retention window so the two don't fight | (same as resume_token_ttl_hours) |
| GC sweep interval | "How often we walk the maps" | 5 min | `gc_interval_minutes` |

Subtlety: completed-job artifacts on disk (transcript, summary,
frontmatter, sidecar WAV) outlive the registry entry. After the
terminal-job TTL the operator can still find them via filesystem; what
they lose is the ability to refetch through the IPC. This is the
correct tradeoff — the IPC registry is a routing layer, not a
long-term archive.

#### Operator escape hatch

`recmeet-daemon --evict <resume_token_prefix>` forces immediate eviction
of a specific session — the revocation lever for suspected token
compromise without waiting for TTL. Small CLI surface, load-bearing for
the multi-client deployment story.

#### V3 evolution path

V2's resume_token is opaque-and-server-issued. V3's pre-provisioned
per-client identities (operator pre-mints `(client_id, secret)` pairs
out-of-band, daemon loads from `clients.yaml`, replaces the global PSK
with per-client tokens) is a strictly richer model with the same wire
shape — the v2-to-v3 upgrade replaces the lookup table with a
credential verifier, no client-side change beyond shipping new tokens.
Specifying the resume_token as opaque now (rather than baking in a
JWT-shaped surface "for future-proofing") keeps the v3 path clean.

### Meeting identity and the client-server audio contract

V2 distinguishes **session identity** (the resume_token above — an IPC
routing key for connection continuity) from **meeting identity** (a
content key for a specific recorded conversation). The two are
orthogonal: one operator session produces many meetings; one meeting
flows through many sessions over its lifetime (live-streamed under one
session, batch-reuploaded under a later one, server-side reprocessed
under a third — see the flow patterns below).

The canonical identity for a meeting is a **client-minted UUID v4
(`meeting_id`)** stamped at recording start. It lives in the existing
`context.json` as a new additive field (per the V1↔V2 compatibility
rules above — V1 ignores unknown fields, so this stays backport-safe)
and in the V2-specific `.pending` sidecar. The directory naming
(`~/meetings/{name}_{YYYY-MM-DD}/`) stays unchanged — `meeting_id` is
the IPC-layer key, not a path component. The server maintains an
in-memory `meeting_id → meeting_dir_path` index that survives daemon
restart via lazy rebuild (walk `~/meetings/`, read each `context.json`,
populate the map — cost amortized against startup).

Why introduce a UUID when `(name, YYYY-MM-DD, HH-MM)` already identifies
a meeting at rest? Three load-bearing reasons:

1. **Operator-side renames must not break the link.** Meeting subjects
   are mutable in the tray UI; the directory name is derived from
   subject and would change with it. The UUID survives renames.
2. **Multi-host scenarios (bd770i + s76 → same daemon) need a stable
   key the client can mint without coordinating clocks** to
   sub-minute precision against the server.
3. **The v3 inter-daemon voiceprint sync path** (already in the v1
   out-of-scope list) needs a portable meeting key that doesn't depend
   on any single daemon's on-disk layout.

#### Convergence principle

The client always retains its own copy of a meeting's audio in
`~/.local/share/recmeet/staging/`. The server is the long-term source
of truth and the repository visible to all of the operator's machines.
The two copies converge by **client-initiated overwrite**: any
`process.submit` or `process.stream.commit` carrying a `meeting_id`
the server has already seen overwrites the server's audio atomically
(write to `*.tmp` + `fsync` + `rename` → no half-state visible to a
concurrent reader). The client's copy is always authoritative on
overwrite. This is one well-defined dedup rule rather than a thicket
of naming conventions, and the laptop-on-an-unreliable-network
scenario degrades cleanly: keep recording locally, reupload-to-overwrite
the partial when the network returns.

#### The four flow patterns

Every recorded conversation maps to one of these:

| # | Pattern | Client copy | Server copy | When server copy converges |
|---|---|---|---|---|
| 1 | Live-stream → commit | full (B.1 fan-out) | full (streaming accumulator) | at `process.stream.commit` |
| 2 | Live-stream → disconnect mid-call → batch-upload after | full | partial (cut off at disconnect) | at follow-up `process.submit` (overwrites partial) |
| 3 | Offline record (intentional or no network) → batch-upload later | full | absent until upload | at `process.submit` (first write) |
| 4 | Server-side reprocess of resident meeting | full | full (already there from a prior pattern) | no upload needed; `process.reprocess` operates on server-resident audio |

Patterns 2 and 4 are the load-bearing additions the V1 architecture
couldn't express. Pattern 2 is the laptop-cafe-disconnect case; pattern
4 is the "rerun this meeting with different summarization settings"
case. Both reduce to "`meeting_id` is the key; operator intent
determines whether to upload-and-overwrite or operate-on-server-copy."

#### On-disk layout

**Client** (each tray host):

```
~/.local/share/recmeet/staging/
  audio_2026-05-16_14-30.wav
  audio_2026-05-16_14-30.wav.pending   # sidecar (extended H-D2 schema)
```

The `.pending` sidecar v2 schema carries everything needed to
reconstruct a submission across tray restart or machine change:

```json
{
  "meeting_id":       "<UUID v4>",
  "wav_path":         "<absolute path>",
  "timestamp":        "YYYY-MM-DD_HH-MM",
  "mic_source":       "<pulse source name>",
  "captions_enabled": false,
  "context": {
    "subject":      "<string>",
    "participants": ["..."],
    "notes":        "<string>",
    "language":     "<bcp47>",
    "vocabulary":   ["..."]
  }
}
```

**Server** (daemon host) — unchanged from V1 except for the additive
`meeting_id` field in `context.json`:

```
~/meetings/{name}_{YYYY-MM-DD}/
  audio_YYYY-MM-DD_HH-MM.wav   # overwritable on reupload of same meeting_id
  context.json                 # NEW additive field: meeting_id
  transcript.txt
  diarization.json
  summary.md
  frontmatter.json
  speakers.json                # per-meeting speaker mapping
```

The directory naming preserves the V1↔V2 cross-version reprocess
contract (the `~/meetings/` schema above). The server-side index
(`meeting_id → meeting_dir_path`) is V2-only operational state; V1
doesn't need it because V1 has no `meeting_id` concept.

#### IPC implications

Three verbs carry `meeting_id` in V2:

- **`process.submit { meeting_id, audio_size, format, sample_rate,
  channels, context, mode, speaker_hints? }`** — extended from C.2.
  Server uses `meeting_id` as the dedup key: known meeting_id ⇒
  overwrite path; unknown meeting_id ⇒ allocate new meeting directory
  using `context.subject` + `timestamp`.
- **`process.stream { meeting_id, format, sample_rate, channels,
  context, language, captions_enabled, latency_budget_ms,
  speaker_hints? }`** — extended from C.10a. The streaming accumulator
  writes directly into the meeting directory keyed by `meeting_id`;
  `process.stream.commit` does not need to rename a temp path because
  the canonical path was the target from frame zero.
- **`process.reprocess { meeting_id, transcribe?, diarize?, identify?,
  summarize?, summary_style?, vocabulary?, ... }`** — **new verb,
  not in the current Phase C plan.** Operates on the server's resident
  copy of the meeting. Returns a `job_id` for progress monitoring. The
  per-stage flags let the operator rerun individual pipeline stages
  without redoing the whole thing (e.g., re-summarize without
  re-transcribing).

`process.reprocess` is the load-bearing addition that makes pattern 4
work without re-upload. It is small to spec but materially expands
client capability — without it, every reprocess scenario requires
re-uploading the audio, defeating the "server as repository" framing.

#### Voiceprint DB residency

The global voiceprint database (speaker enrollment, cross-meeting
identification) is daemon-resident. Both bd770i and s76 connecting to
the same daemon see the same speaker inventory — this is the V2
multi-host benefit relative to the V1 single-host model. Two separate
daemons mean two separate voiceprint DBs; cross-daemon voiceprint sync
stays in v3 (already in the v1 out-of-scope list).

Per-meeting `speakers.json` (under each meeting's directory) is a
snapshot of which voiceprints matched in that meeting — produced by
the postprocess job, consumed by `process.reprocess` when the operator
wants different identification settings.

#### Live captions stay server-side

The streaming-ASR + diarization pipeline is heavy enough that doing it
client-side would balloon the thin-client back into a thick client. V2
accepts the ~500 ms client→server→client round-trip as the cost of the
thin-client model. The B.1 fan-out callback (landed iter 152) means
the local WAV is always being written in parallel during live
captioning, so a mid-stream disconnect degrades cleanly to pattern 2
(keep recording locally, reupload-to-overwrite later) without any
extra client logic.

#### Disk-space implications

The server's per-meeting `audio_*.wav` slot is overwritten on each
upload-of-same-`meeting_id`, so the same meeting never duplicates
server-side. But meetings accumulate: at the 4-hour audio target
(~460 MB raw PCM per meeting), an active operator producing
~5 meetings/week consumes ~2.3 GB/week of server storage. V2 ships
without an automatic retention policy — operators manage `~/meetings/`
with standard filesystem tools. V3 may add an operator-configurable
retention policy (e.g., "drop audio after N days, keep
transcript/summary/frontmatter indefinitely").

### V1 → V2 client compatibility

V2 has **no backwards compatibility for `record.start`** or any other
V1 verb that moved off the daemon. A V1 tray cannot talk to a V2
daemon and vice versa. In a mixed-environment migration, V1 hosts
must stay on `v1-maintenance` and connect to V1 daemons until they
are upgraded as a unit. The cross-version interop story is at the
data-at-rest layer (the `~/meetings/` schema; see above), not at the
IPC layer.

---

## Validation milestones

Two validation milestones gate the maintenance-branch model — one V1-side
(shipped) and one V2-side (shipped, iter 156 stabilization Wave 1).

### V1 validation: live captioning (shipped iter 135, `v1.5.0`)

Live captioning was the V1 capstone: daemon-captures-audio,
daemon-runs-streaming-ASR, daemon-broadcasts-captions. The corresponding
V2 shape (client-captures-audio, optional-stream-to-server,
server-runs-streaming-ASR, server-routes-captions-to-originating-client)
shares the streaming-ASR engine choice and the model picker but
essentially nothing else.

Outcome: the V1→V2 port is a near-rewrite, as predicted. The
maintenance-branch model is validated — codepaths really do diverge,
backports really are rare, and the one-way cherry-pick policy is
correctly tuned.

### V2 validation: thin-client e2e architectural-proof test (shipped iter 156)

`tests/test_v2_thin_client_e2e.cpp` is the V2 validation test: it stands
up the real `recmeet-daemon` binary on a loopback TCP listener with a
PSK, drives a full client-side recording → submit → process → fetch
round-trip across the framed wire protocol, and asserts on the
artifacts produced. Seventeen assertions cover the handshake, the
upload framing, the postprocess JobQueue slot, the artifact-fetch
return channel, and the final on-disk meeting directory. The test is
wired into the `test.yml` CI workflow via `make integration-e2e`.

This is the architectural proof that the Phase A + B + C IPC reshape is
end-to-end correct against the real binary, not just against unit-level
mocks. Failures of this test gate any future protocol-version bump.

---

## V1 end-of-life

**No fixed date.** V1 sunsets organically when V2 becomes more
compelling than V1 along every axis that matters: stability, feature
set, single-host UX, multi-host UX, deployment story.

V1 retains standalone value: simple, single-binary-set deployment,
multi-provider LLM support including local self-hosted models, no
network configuration required, no PSK to manage, no separate server to
keep alive. For users whose use case is "record on this laptop, process
on this laptop, save the note here," V1 may remain the better choice
indefinitely.

**Maintenance commitment** for `v1-maintenance`: dep bumps and security
fixes for as long as upstream support for the dependency stack
continues. Feature work does not happen on `v1-maintenance` after
`v1.5.0` ships.

---

## Open questions

- **MCP server scope in V2.** Is `recmeet-mcp` purely a client-side
  binary that talks to the server via the V2 IPC? Or does the server
  expose its own MCP surface for agent-driven server administration?
  *STILL OPEN — Phase E.* Phase A did not need to settle this; the MCP
  binary is unchanged on `feat/v2-thin-client`. Decision deferred to
  the Phase E binary-naming and surface-finalization pass.
- **Binary name ergonomics.** `recmeet-server-daemon` is verbose;
  `recmeetd-server` or `recmeet-srv` may read better. *STILL OPEN —
  Phase E.* Phase A through C kept the existing `recmeet-daemon` /
  `recmeet-tray` names to minimize churn; the rename is mechanical
  and scheduled for Phase E alongside the schema split.
- **Shared library extraction.** If `recmeet_ipc` (already a separate
  CMake target as of iter 104) turns out to need parallel maintenance
  on both branches, consider extracting it to a small shared repo or
  vendoring it across both lines. *STILL OPEN — revisit after `v2.0.0`
  ships.* The V2-side `recmeet_ipc` is now substantially different
  from V1's (frame types, JobQueue, FrameReader), so shared-library
  extraction is less attractive than it looked pre-Phase-C. No action
  needed unless backport friction becomes a real problem.
- **Web UI fate.** V1's `recmeet-web` (speaker management) and V2's
  `recmeet-client-web` are likely identical in scope. Worth deciding
  whether they stay separate or share a codebase via a shared static
  asset directory. *STILL OPEN — Phase E or post-2.0.* No urgency
  while V2's speaker DB lives server-side and the WebUI surface has
  not yet been re-pointed at the V2 IPC.
- **PSK handshake deadline.** *NEW, STILL OPEN — Phase E candidate.*
  Wave 2 of the iter-156 stabilization surfaced (as SUCCEED-with-INFO)
  the fact that the `IpcServer` poll loop has no per-connection
  deadline for completing the PSK handshake. A malicious peer that
  opens a TCP connection and never sends bytes occupies a `PendingPsk`
  slot indefinitely; with the connection cap, this is a slowloris-class
  resource exhaustion vector. Fix is a `psk_deadline_ms` reaper in
  the poll loop. See `docs/IPC-WIRE-PROTOCOL.md` for the handshake
  shape.
