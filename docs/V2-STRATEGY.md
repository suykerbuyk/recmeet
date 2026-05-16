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
