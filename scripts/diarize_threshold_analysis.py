#!/usr/bin/env python3
# Copyright (c) 2026 John Suykerbuyk and SykeTech LTD
# SPDX-License-Identifier: MIT OR Apache-2.0
"""
Phase A — diarize centroid-dump threshold analysis.

Consumes the JSON artifacts produced by recmeet's --debug-dump-centroids flag
(see `dump_centroids_json` in src/diarize.cpp) and simulates the Phase B.1
unified greedy-merge loop offline for a sweep of cosine-similarity thresholds.

Output is a per-T global-count table; useful for picking the production
`cfg.collapse_threshold` without re-running the (~15 min) integration bench
once the dump exists.

Usage:
    scripts/diarize_threshold_analysis.py PATH_TO_DUMP.json
    scripts/diarize_threshold_analysis.py --tmin 0.50 --tmax 0.80 --tstep 0.05 PATH

Algorithm — mirrors the planned Phase B.1 unified greedy-merge loop:

    while len(globals) > 1:
        find pair (a, b) with highest cosine similarity in current centroid set
        if best_sim < T and (target_speakers <= 0 or len(globals) <= target):
            break
        merge a and b with sample-weighted mean
    return len(globals)

For the Phase A sweep we run with target_speakers=0 (no ceiling) so the only
termination is the threshold gate — the count reported at each T is the
"natural" cluster count for that threshold.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional


@dataclass
class Centroid:
    raw: List[float]
    count: int = 1


def cosine(a: List[float], b: List[float]) -> float:
    if len(a) != len(b) or not a:
        return 0.0
    dot = 0.0
    na = 0.0
    nb = 0.0
    for x, y in zip(a, b):
        dot += x * y
        na += x * x
        nb += y * y
    denom = math.sqrt(na) * math.sqrt(nb)
    if denom < 1e-12:
        return 0.0
    return dot / denom


def weighted_mean(a: List[float], ca: int, b: List[float], cb: int) -> List[float]:
    total = ca + cb
    if total <= 0:
        return [0.0] * len(a)
    return [(x * ca + y * cb) / total for x, y in zip(a, b)]


def simulate_greedy_merge(centroids: List[Centroid], threshold: float,
                          target_speakers: int = 0) -> int:
    """Run the Phase B.1 unified greedy-merge loop and return final count.

    target_speakers <= 0 disables the ceiling — only the threshold matters.
    """
    # Work on a copy so the caller can sweep multiple Ts against one dump.
    work = [Centroid(raw=list(c.raw), count=c.count) for c in centroids]

    while len(work) > 1:
        best_i = -1
        best_j = -1
        best_sim = -2.0
        for i in range(len(work)):
            for j in range(i + 1, len(work)):
                s = cosine(work[i].raw, work[j].raw)
                if s > best_sim:
                    best_sim = s
                    best_i = i
                    best_j = j

        ceiling_hit = target_speakers > 0 and len(work) > target_speakers
        threshold_ok = best_sim >= threshold

        if not ceiling_hit and not threshold_ok:
            break

        # Merge i and j: weighted-mean of raws, sum of counts. Keep the lower
        # index; drop the higher to preserve list invariants.
        new_raw = weighted_mean(work[best_i].raw, work[best_i].count,
                                work[best_j].raw, work[best_j].count)
        new_count = work[best_i].count + work[best_j].count
        work[best_i] = Centroid(raw=new_raw, count=new_count)
        del work[best_j]

    return len(work)


def load_dump(path: Path) -> List[Centroid]:
    with open(path) as f:
        doc = json.load(f)
    centroids_raw = doc.get("centroids", [])
    counts = doc.get("sample_counts", [])
    if not centroids_raw:
        return []
    out = []
    for i, raw in enumerate(centroids_raw):
        c = counts[i] if i < len(counts) else 1
        out.append(Centroid(raw=list(raw), count=int(c) if c else 1))
    return out


def sweep(centroids: List[Centroid], tmin: float, tmax: float, tstep: float,
          target_speakers: int = 0) -> List[tuple]:
    """Returns [(T, final_count), ...] across the half-open range [tmin, tmax]."""
    out = []
    t = tmin
    # Use a small epsilon to include tmax inclusively.
    while t <= tmax + 1e-9:
        n = simulate_greedy_merge(centroids, threshold=t,
                                  target_speakers=target_speakers)
        out.append((round(t, 4), n))
        t += tstep
    return out


def print_dump_summary(path: Path, doc_centroids: List[Centroid]) -> None:
    print(f"=== {path}")
    print(f"  globals at dump time: {len(doc_centroids)}")
    if doc_centroids:
        dim = len(doc_centroids[0].raw)
        print(f"  centroid dim:         {dim}")
        total = sum(c.count for c in doc_centroids)
        print(f"  total sample weight:  {total}")
        # Also dump pairwise sim distribution.
        sims = []
        for i in range(len(doc_centroids)):
            for j in range(i + 1, len(doc_centroids)):
                sims.append(cosine(doc_centroids[i].raw, doc_centroids[j].raw))
        if sims:
            sims_sorted = sorted(sims)
            print(f"  pairwise sim N:       {len(sims)}")
            print(f"  pairwise sim min:     {sims_sorted[0]:.4f}")
            print(f"  pairwise sim p25:     {sims_sorted[len(sims) // 4]:.4f}")
            print(f"  pairwise sim median:  {sims_sorted[len(sims) // 2]:.4f}")
            print(f"  pairwise sim p75:     {sims_sorted[3 * len(sims) // 4]:.4f}")
            print(f"  pairwise sim max:     {sims_sorted[-1]:.4f}")


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("dump", nargs="+", type=Path,
                   help="Path(s) to centroid dump JSON")
    p.add_argument("--tmin", type=float, default=0.50)
    p.add_argument("--tmax", type=float, default=0.80)
    p.add_argument("--tstep", type=float, default=0.05)
    p.add_argument("--target-speakers", type=int, default=0,
                   help="Ceiling for the unified merge loop (0 = no ceiling).")
    args = p.parse_args(argv)

    rc = 0
    for path in args.dump:
        if not path.exists():
            print(f"!! missing: {path}", file=sys.stderr)
            rc = 1
            continue
        centroids = load_dump(path)
        print_dump_summary(path, centroids)
        rows = sweep(centroids, args.tmin, args.tmax, args.tstep,
                     target_speakers=args.target_speakers)
        print()
        print("  | T     | count |")
        print("  |-------|-------|")
        for t, n in rows:
            print(f"  | {t:.2f} | {n:5d} |")
        print()
    return rc


if __name__ == "__main__":
    sys.exit(main())
