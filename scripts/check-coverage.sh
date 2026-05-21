#!/usr/bin/env bash
# scripts/check-coverage.sh — C++ coverage no-regression gate (Phase 1).
#
# Compares current gcovr per-file line coverage (read from the build/
# directory populated by `make cxx-coverage`) against the committed
# baseline in `tests/coverage-baseline.txt`.
#
# Default behavior: WARN-ONLY. Exits 0 even when files regress, but
# emits GitHub Actions `::warning::` lines naming every regressed file.
# This is Phase 1's "warn-only for the first CI run, then
# gate-no-regression" policy.
#
# TODO(phase 1.5): flip the default to enforce. The plan calls for the
# first CI run to land warn-only so the baseline can settle, then for a
# follow-up commit to drop the warn-only behavior and make regressions
# block the build. The `--enforce` flag below is the explicit version of
# that future-default behavior; the CI workflow does NOT pass it today.
#
# Usage:
#   scripts/check-coverage.sh             # warn-only (Phase 1 default)
#   scripts/check-coverage.sh --enforce   # fail on regression (future default)
#
# Tolerance: a per-file drop of <0.1 percentage point is treated as
# noise (float-formatting jitter). Anything larger is a regression.
#
# Prereqs: `make cxx-coverage` has been run; build/ contains .gcda files.

set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

BASELINE="tests/coverage-baseline.txt"
BUILD_DIR="${BUILD_DIR:-build}"
TOLERANCE="0.1"   # percentage-point noise floor

ENFORCE=0
for arg in "$@"; do
    case "$arg" in
        --enforce) ENFORCE=1 ;;
        -h|--help)
            sed -n '1,/^$/p' "$0"
            exit 0
            ;;
        *)
            echo "check-coverage.sh: unknown arg: $arg" >&2
            exit 2
            ;;
    esac
done

if [ ! -f "$BASELINE" ]; then
    echo "check-coverage.sh: baseline missing: $BASELINE" >&2
    echo "  Generate via: make cxx-coverage  &&  (copy summary into baseline)" >&2
    exit 2
fi

if ! command -v gcovr >/dev/null 2>&1; then
    echo "check-coverage.sh: gcovr not found on PATH." >&2
    echo "  Install: pacman -S gcovr | apt install gcovr | pip install gcovr" >&2
    exit 2
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "check-coverage.sh: build dir missing: $BUILD_DIR" >&2
    echo "  Run 'make cxx-coverage' first." >&2
    exit 2
fi

# Generate current coverage CSV.
CURRENT_CSV="$(mktemp -t recmeet-cov-XXXXXX.csv)"
trap 'rm -f "$CURRENT_CSV"' EXIT

gcovr --root . \
      --filter 'src/.*' \
      --gcov-ignore-parse-errors \
      --csv "$CURRENT_CSV" \
      "$BUILD_DIR" >/dev/null 2>&1 || {
    echo "check-coverage.sh: gcovr failed" >&2
    exit 2
}

# Compare baseline vs current. Baseline format: "<path>: NN.N%" (one per
# line; comments and TOTAL line ignored). Tolerance handled in awk.
python3 - "$BASELINE" "$CURRENT_CSV" "$TOLERANCE" "$ENFORCE" <<'PY'
import csv
import sys

baseline_path, current_path, tol_s, enforce_s = sys.argv[1:5]
tolerance = float(tol_s)
enforce = enforce_s == "1"

baseline = {}
baseline_total = None
with open(baseline_path) as f:
    for raw in f:
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if ':' not in line:
            continue
        path, val = line.rsplit(':', 1)
        pct = float(val.strip().rstrip('%'))
        if path.strip() == 'TOTAL':
            baseline_total = pct
        else:
            baseline[path.strip()] = pct

current = {}
current_total_lines = 0
current_covered_lines = 0
with open(current_path) as f:
    for row in csv.DictReader(f):
        path = row['filename']
        total = int(row['line_total'])
        covered = int(row['line_covered'])
        current_total_lines += total
        current_covered_lines += covered
        if total == 0:
            current[path] = 100.0
        else:
            current[path] = (covered / total) * 100.0

current_total = (current_covered_lines / current_total_lines * 100.0) if current_total_lines else 0.0

regressions = []
for path, base_pct in baseline.items():
    cur_pct = current.get(path)
    if cur_pct is None:
        # File was removed. Don't treat as regression but note it.
        print(f"::warning file={path}::coverage baseline entry has no current data (file removed?)")
        continue
    if base_pct - cur_pct > tolerance:
        regressions.append((path, base_pct, cur_pct))

# TOTAL gate
total_regressed = False
if baseline_total is not None and (baseline_total - current_total) > tolerance:
    total_regressed = True

if regressions:
    for path, base_pct, cur_pct in regressions:
        msg = (f"coverage regression: {path} {cur_pct:.1f}% "
               f"(baseline {base_pct:.1f}%, delta -{base_pct - cur_pct:.1f}pp)")
        # GitHub Actions ::warning:: annotation (recognized in PR diff view)
        print(f"::warning file={path}::{msg}")
        print(f"  {msg}")

if total_regressed:
    msg = (f"TOTAL coverage regression: {current_total:.1f}% "
           f"(baseline {baseline_total:.1f}%, "
           f"delta -{baseline_total - current_total:.1f}pp)")
    print(f"::warning::{msg}")
    print(f"  {msg}")

print()
print(f"Baseline TOTAL: {baseline_total:.1f}%  Current TOTAL: {current_total:.1f}%")
print(f"Per-file regressions (>{tolerance}pp): {len(regressions)}")
print(f"TOTAL regression: {'yes' if total_regressed else 'no'}")

if enforce and (regressions or total_regressed):
    print()
    print("check-coverage.sh: --enforce set; failing on regression.", file=sys.stderr)
    sys.exit(1)

sys.exit(0)
PY
