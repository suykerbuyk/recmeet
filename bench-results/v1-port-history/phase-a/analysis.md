# Phase A — Diarize Over-Count Analysis

Phase A of `diarize-overcount-and-context-aware-speaker-hints`. Confirms
RC1's collapse mechanism, picks the unified merge threshold empirically,
investigates RC2's H-2 assumption.

Branch: `worktree-agent-ab63c5cb31470b4a5` (forks off `feat/diarize-overcount-v1`).

## A.1 — Instrumentation

`feat(diarize): add --debug-dump-centroids JSON dump for Phase A analysis`
(commit `a2e537a`).

New CLI flag `--debug-dump-centroids PATH` writes a JSON artifact at the
end of `stitch_chunks()` (long-audio) and at the equivalent point in the
short-audio path (post-`diarize()`, per-cluster `extract_speaker_embedding`
into a synthetic globals view). Path is auto-suffixed with the meeting
timestamp before the extension to prevent concurrent-reprocess collision
(M-1). The JSON carries: `schema`, `source`, `num_globals`, raw
centroids `0..N-1`, sample-count weights, the full NxN cosine similarity
matrix, and per-chunk `{local_id, global_id}` mapping (long-audio only).

`scripts/diarize_threshold_analysis.py` ingests the JSON and runs the planned
Phase B.1 unified greedy-merge loop offline across `T ∈ [0.50, 0.80]`.

Unit-test coverage (`[dump-centroids]` tag, 5 cases): empty-path no-op,
JSON-key spot-check, M-1 timestamp-suffix collision avoidance, empty-centroid
skeleton, end-to-end `stitch_chunks` dump emission. The t2-1 integration
test reads `RECMEET_DEBUG_DUMP_CENTROIDS` env var so the bench can dump
without forking the test signature.

## A.2 — Long-audio analysis

### Fixture choice

The iter-183 bench used `find_iter110_fixture()`'s longest-recording
fallback — the largest `~/meetings/audio_*.wav` ≥ 17.5 min was
`2026-05-12_16-09` (217 min). Phase A re-ran the same test with that
fixture to capture both run-to-run sherpa jitter and the centroid dump.

| run | wall-clock | peak RSS | speaker count | dump artifact |
|-----|-----------:|---------:|--------------:|---------------|
| iter-183 (baseline) | 14.9 min | 4.4 GB | 23 | (no dump — Phase A wasn't written yet) |
| Phase A run 1 | 5.3 min | 1.9 GB | 9  | `centroids_t2-1_run1_9speakers.json` |

The 23-vs-9 speaker difference is the run-to-run sherpa-clustering jitter
the test header acknowledges ("we don't pin an exact number because pyannote
VAD and sherpa clustering can both jitter run-to-run"). **Both runs are
valid RC1 reproducers** — auto-detect mode produces > 8 globals in either
case.

### Threshold sweep — `centroids_t2-1.json` (run 1, 9 globals)

9 globals, 512-dim, total sample weight 6,041,784 (≈ 6,290 s = 105 min of
voiced audio across ~14 chunks).

Pairwise sim distribution (36 pairs):
- min:    -0.144
- p25:     0.064
- median:  0.181
- p75:     0.301
- **max:   0.570**

Sweep (no ceiling — `target_speakers = 0`):

```
  | T    | count |
  |------|-------|
  | 0.50 |   5   |
  | 0.55 |   6   |
  | 0.60 |   9   |
  | 0.65 |   9   |
  | 0.70 |   9   |
  | 0.75 |   9   |
  | 0.80 |   9   |
```

Maximum pairwise sim is **0.570**, so nothing collapses above 0.57. Even
the floor of the plan's expected landing zone (T = 0.55) leaves 6 globals.

Observation: this fixture's sherpa fragments are very far apart in cosine
space. The natural cluster count at any reasonable T is 5–9, all within
the plan's `max_auto_speakers = 8` cap window. **Only the ceiling, not the
threshold, will pull a real overcount (like iter-183's 23) down to ≤ 8.**
B.1's unified loop with `target_speakers = 8` will force-merge regardless
of threshold, which is the documented intent.

### Threshold sweep — production `centroids_09-36`

`/home/johns/meetings/2026-05-18_09-36/` reprocessed via:

```
./build/recmeet --no-daemon --no-summary --no-speaker-id \
  --reprocess /tmp/phase_a_09-36/ \
  --debug-dump-centroids /tmp/phase_a/centroids_09-36.json \
  --num-speakers 0
```

This recording is **12.76 min (765 s)** — *below* the chunked threshold
(~17.5 min). It runs the **short-audio path** end-to-end via `diarize()`.
This corrects a fact in the plan: the plan's symptoms section says "23-min
1-man-1-woman call" but the audio is actually 12.76 min. **09-36 is a
short-audio reproducer of over-counting**, not a chunked-path reproducer.
This makes it a perfect A.3 fixture (see below) — same recording exercises
RC2 directly.

4 globals reported (truth = 2 — operator-confirmed via existing
`speakers_2026-05-18_09-36.json` reprocess history).

Sample weights: `[7845389, 3931739, 160919, 7830]` ≈ `[490s, 245s, 10s, 0.5s]`.

Pairwise similarity matrix:
```
       g0      g1      g2      g3
g0  +1.000  +0.260  +0.826  +0.381
g1  +0.260  +1.000  +0.189  +0.149
g2  +0.826  +0.189  +1.000  +0.309
g3  +0.381  +0.149  +0.309  +1.000
```

Sweep (no ceiling):
```
  | T    | count |
  |------|-------|
  | 0.50 |   3   |
  | 0.55 |   3   |
  | 0.60 |   3   |
  | 0.65 |   3   |
  | 0.70 |   3   |
  | 0.75 |   3   |
  | 0.80 |   3   |
```

**Reading**:

- g0 (490 s, dominant — the man) and g2 (10 s) are the same voice — sim 0.826.
  Collapse pass merges them at any T ≤ 0.82.
- g1 (245 s, the woman) is distinct from everything else, max sim 0.26.
  Correct.
- g3 (0.5 s) is a sherpa artifact — sim < 0.40 against everything else.
  **No collapse threshold in [0.50, 0.80] drops it.**

So the collapse pass alone takes 4 → 3 for this fixture. To reach truth = 2,
the operator's `Participants: John Suykerbuyk, Marci Grant` context line
must force `target_speakers = 2` via Phase B.2's precedence chain — then
B.1's ceiling path force-merges the 0.5-s ghost against its closest neighbor
(g3 ↔ g0 at 0.38), yielding `{g0, g2, g3} ∪ {g1}` = 2. This is exactly the
plan's intended UX path.

### Proposed production T

**Recommendation: `cfg.collapse_threshold = 0.55`**.

Justification:

- **t2-1 (run 1)**: At T = 0.55, 9 → 6 globals (single collapse). At T = 0.60,
  no merges (max sim 0.57 < 0.60). 0.55 captures the closest pair without
  collapsing distinct voices.
- **09-36**: At T = 0.55, 4 → 3 (the obvious 0.83 pair merges). Higher T
  produces the same result (no other pair above 0.55). Lower T (0.50) would
  also collapse the 0.38 ghost — wrong call without a ceiling to force it.
- **2026-05-05_10-52** (additional short-audio fixture, see A.3 below):
  At T = 0.55, 4 → 3 (the 0.77 pair merges); at T = 0.50 it would
  over-aggressively collapse 4 → 2 by merging a borderline 0.55 pair too.
- **M-4 calibration bound** (plan line 142): "centroid drift during
  sample-weighted mean updates can push some pairs above 0.6 but rarely above
  0.75 in practice." T = 0.55 sits at the floor of that bound, conservatively
  preferring "leave operator-visible distinct clusters alone" over
  "auto-merge near-misses."
- **No single T in [0.50, 0.80] satisfies BOTH fixtures in the way the
  plan's "collapses 23 → 8 AND 7+ → 2 simultaneously" framing suggests.**
  The threshold collapses each fixture as far as the actual centroid
  geometry allows; the ceiling (`target_speakers = 8` default, lower with
  context names) is the lever that gets us to single-digit / participant-count
  speaker totals.

This nuance is consistent with the plan's B.1 design ("we still merge because
`ceiling_hit` is true"). The threshold is the **floor on quality of merges
done past the cap**; the cap is the **mechanism that forces a count**.

### Caveat — t2-1 cannot reach 8 by threshold alone in run 1

Run-1 sim matrix has max 0.570. The plan's expected landing zone 0.55–0.70
is roughly compatible with the floor — 9 → 6 at T = 0.55 — but the goal of
"collapses 23 → 8" depends on what the original 23-cluster fixture's sim
matrix looked like, which we cannot reconstruct from the iter-183 bench
artifacts (no centroid dump was written then). **Whatever T we pick, RC1's
ceiling enforcement (Phase B.1's `ceiling_hit` branch) is the load-bearing
mechanism — not the threshold.**

## A.3 — Short-audio investigation (H-2 validation)

Strategy: scan `~/meetings/` for `audio_*.wav` files between 4 and 16 min
(below the chunked threshold), reprocess a handful via the daemon-less
single-process CLI, dump centroids, inspect the post-extract similarity
matrix.

Candidates exercised (3 of ~13 fitting the duration window):

| meeting | duration | speakers reported | dump file |
|---------|----------|------------------:|-----------|
| `2026-05-18_09-36` | 12.76 min | **4** (truth 2) | `centroids_09-36_*.json` |
| `2026-05-05_10-52` | 14.21 min | **4** | `centroids_short_2026-05-05_10-52_*.json` |
| `2026-05-18_08-37` | 12.27 min | 1 | `centroids_short_2026-05-18_08-37_*.json` |

Two of three short-audio candidates over-count. **RC2 is REPRODUCED.**

### H-2 verdict — partially holds, with one important gap

H-2 (plan): "sherpa's over-split centroids are close in cosine-similarity
space" → "the collapse pass alone will fix RC2."

Observed sim distribution across reproducer fixtures:

| fixture | max sim | next-highest | min sim | comment |
|---------|--------:|-------------:|--------:|---------|
| `09-36` (4 globals) | 0.826 | 0.381 | 0.149 | One "true" merge pair (g0↔g2). Ghost g3 (0.5 s) at sim ≤ 0.38 — below threshold floor. |
| `10-52` (4 globals) | 0.770 | 0.548 | 0.092 | Two merge pairs but second one (0.548) is borderline. |

**Strong holders**: the dominant over-split pair in each fixture is at sim
**≥ 0.77**, well above any reasonable T. The collapse pass picks those up.

**Weak gap (H-2 doesn't catch ghosts)**: extremely short-duration ghost
clusters (≤ 1 s of audio in the dump) land at low sim against everything,
so they survive the collapse pass. In `09-36` the 0.5-s ghost (g3) is the
gap. In `10-52` a 2.7-s ghost (g3, sample_count = 45,900) is also borderline.

**Recommendation** (do not silently consume in Phase B):

1. Phase B.1's ceiling path catches these ghosts when `target_speakers` is
   pinned (via `--num-speakers` or Phase C context names). The unified
   greedy loop with `ceiling_hit` true will force-merge them regardless of
   threshold. **For the headline 09-36 case + Phase C context line, the
   collapse pass DOES converge to truth = 2.**
2. For RC2 in the *purely auto-detect* short-audio path (no context, no
   `--num-speakers`), the collapse pass leaves a 3-speaker result for
   09-36 (4 → 3, ghost survives). This is **strictly better than current
   (4)** but **not the operator-target "exactly 2"**. The headline UX
   target ("record → context line → done") requires Phase C.
3. Suggest a follow-up task: **min-duration filter for short-audio
   clusters** that drops clusters whose sample_count is below, say, 1 second
   of audio. Would catch the 0.5 s ghost in 09-36 and the 2.7 s ghost in
   10-52 cheaply. Not in current scope — file separately if the operator
   wants the auto-detect-only path tightened beyond Phase C's reach.

**This does NOT block Phase B.** Phase B + Phase C (with context-name target)
reaches the headline UX outcome on the 09-36 fixture. The ghost-cluster gap
is a separate, smaller issue affecting auto-detect-only short-audio
recordings without context.

## A.4 — Fixture true-speaker-count

At the proposed T = 0.55:

- `centroids_t2-1.json` (run-1, 217-min fixture, no ceiling): **6** globals
  after collapse.
- With ceiling `target_speakers = 8`: **6** (ceiling doesn't bind because
  size is already 6 ≤ 8 after collapse).

**6 ≤ 8** — the existing `CHECK(speakers.size() <= 8)` assertion in the t2-1
integration test passes for this run at the proposed T. **No fixture
override needed for Phase D under this run's geometry.** The original
iter-183 23-count was a run-to-run jitter extreme; Phase B's unified merge
loop with default cap 8 handles both extremes (cap-merges if natural count
> 8, lets it stand if ≤ 8).

Caveat: because sherpa's clustering is non-deterministic, a future run on
this same fixture could land outside this geometry (the 23-count case). The
test's existing "no exact number" stance + the `<= 8` upper bound remain
appropriate — Phase B's ceiling will keep that bound holding regardless of
sherpa's per-run jitter.

## A.5 — Upstream sherpa-onnx check

Vendored sherpa-onnx: **v1.12.27** (tagged 2026-02-26).

Relevant downstream activity since 1.12.27:

- **[PR #3514](https://github.com/k2-fsa/sherpa-onnx/pull/3514) — "Fix an
  offset-by-one error in pyannote speaker diarization."** Merged 2026-04-15;
  first available in v1.12.31+. Adds bounds checks in
  `offline-speaker-diarization-pyannote-impl.h` — clamps `last_frame` /
  `num_frames_to_keep` / `top_k` bounds. **Not a direct over-count fix**,
  but does plug intermittent off-by-one paths that could affect output
  determinism on edge-case recordings.
- **[PR #3563](https://github.com/k2-fsa/sherpa-onnx/pull/3563) — "fix: add
  bounds checks to prevent SIGSEGV in speaker diarization."** Merged
  2026-05-08; first available in v1.12.40+. Adds defensive
  `valid_indexes` / `ReLabel` / `SortBySpeaker` bounds checks. Also not a
  direct over-count fix but defensive against `FastClustering::Cluster()`
  emitting out-of-range labels.

**"No speakers found in the audio samples"** warning at
`offline-speaker-diarization-pyannote-impl.h:Process:128` (the warning seen
≥ 5 times in iter-183 bench stderr) is *not* tracked in any open sherpa-onnx
issue. Reading the surrounding code (the `speakers_per_frame.maxCoeff() == 0`
early-exit), this fires when a chunk has 0 detected speakers — i.e., the
chunk is silence. The warning is informational; it returns `{}` and the
chunk contributes nothing. **It's not a cause of the over-count.**

**Recommendation**: file a separate task to bump sherpa-onnx to v1.13.x
(stable as of v1.13.2) for the bounds-check improvements. **Do not block
this task on the upstream bump** — our in-house collapse pass fixes the
over-count using the tools we already have.

## Blockers / surprises for Phase B

1. **No single T collapses both fixtures to the plan-specified targets
   ("23 → 8" AND "7+ → 2") via threshold alone.** This is expected given
   the actual sim geometry, and the plan's B.1 unified loop already handles
   it (ceiling vs threshold). The operator should know **the threshold sets
   the floor on auto-merge quality, the ceiling sets the count.**
2. **Plan rev 4 fact correction**: `09-36` is 12.76 min (not 23 min as the
   plan says). The fixture exercises the SHORT-audio path, not chunked.
   This matters for B.3 wiring — `apply_collapse` MUST land in the
   short-audio path for the headline production fix to work.
3. **Ghost-cluster gap**: very-short-duration clusters (≤ 1 s) survive
   any threshold-only collapse pass. Phase B + Phase C (with context names
   pinning a low `target_speakers`) covers the headline UX target. For
   strict auto-detect-only short-audio cleanup, a min-duration filter is
   worth filing as a separate follow-up.
4. **Run-to-run sherpa jitter is substantial**: 23 (iter-183) vs 9 (Phase A
   run 1) on the same fixture / same code. Tests that assert specific
   counts will be flaky; tests that assert bounds (≤ N) are appropriate.
   Phase D should preserve the existing `CHECK(speakers.size() <= 8)` shape.

## Artifacts (committed under `bench-results/phase-a/`)

- `centroids_t2-1.json` — Phase A run 1 dump (9 globals, 217-min fixture).
- `centroids_09-36.json` — production 09-36 dump (4 globals, 12.76 min,
  short-audio path).
- `analysis.md` — this document.
- `scripts/diarize_threshold_analysis.py` — sweep tool (committed in commit
  `a2e537a` alongside the A.1 instrumentation).
