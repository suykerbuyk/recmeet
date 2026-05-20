#!/usr/bin/env bash
# bench-with-telemetry.sh
#
# Run a labeled bench command with parallel GPU/CPU/power telemetry capture
# for the Radeon Pro W5500 (PCI 0000:03:00.0). Writes per-run artifacts to
# bench-results/<name>/ alongside a parseable summary.txt.
#
# Usage:
#   scripts/bench-with-telemetry.sh <name> -- <cmd...>
#
# Outputs (under bench-results/<name>/):
#   amdgpu_top.jsonl   amdgpu_top -J stream (1 Hz, per-GPU-engine breakdown)
#   vmstat.txt         vmstat 1 stream (host CPU load + memory)
#   sysfs.csv          space-separated columns:
#                          epoch_ns gpu_busy_pct vram_used_bytes power_uw temp_mc
#   cmd-stdout.txt     bench command stdout
#   cmd-stderr.txt     bench command stderr
#   start.txt          ISO-8601 start
#   end.txt            ISO-8601 end
#   summary.txt        parseable key=value (wall, GPU%, VRAM, watts, peak temp)

set -u -o pipefail

PROG="$(basename "$0")"
NAME="${1:-}"
[ -n "$NAME" ] || { echo "usage: $PROG <name> -- <cmd...>" >&2; exit 2; }
shift
[ "${1:-}" = "--" ] || { echo "usage: $PROG <name> -- <cmd...>" >&2; exit 2; }
shift

# Discover W5500 hwmon and DRM card. W5500 is PCI 0000:03:00.0 (vendor 1002:7341).
W5500_HWMON=""
for h in /sys/class/hwmon/hwmon*; do
    [ -r "$h/name" ] || continue
    [ "$(cat "$h/name" 2>/dev/null)" = "amdgpu" ] || continue
    [ -r "$h/power1_average" ] || continue
    W5500_HWMON="$h"; break
done
W5500_CARD=""
for c in /sys/class/drm/card*; do
    [ -e "$c/device/uevent" ] || continue
    grep -q '^PCI_SLOT_NAME=0000:03:00.0$' "$c/device/uevent" 2>/dev/null && { W5500_CARD="$c"; break; }
done

OUTDIR="$(pwd)/bench-results/$NAME"
mkdir -p "$OUTDIR"

echo "=== bench-with-telemetry: $NAME ==="
echo "    hwmon=$W5500_HWMON"
echo "    card=$W5500_CARD"
echo "    output=$OUTDIR"
echo "    cmd=$*"

date -Iseconds > "$OUTDIR/start.txt"
START_EPOCH=$(date +%s)

# amdgpu_top JSONL (1 Hz default refresh)
amdgpu_top -J > "$OUTDIR/amdgpu_top.jsonl" 2> "$OUTDIR/amdgpu_top.err" &
AMDGPU_PID=$!

# vmstat 1
vmstat 1 > "$OUTDIR/vmstat.txt" 2>&1 &
VMSTAT_PID=$!

# Direct sysfs sampler (1 Hz). Robust to amdgpu_top format drift.
(
  while true; do
    ts=$(date +%s.%N)
    gpu="?"; vram="?"; pow="?"; temp="?"
    [ -n "$W5500_CARD" ]  && gpu=$(cat "$W5500_CARD/device/gpu_busy_percent" 2>/dev/null   || echo "?")
    [ -n "$W5500_CARD" ]  && vram=$(cat "$W5500_CARD/device/mem_info_vram_used" 2>/dev/null || echo "?")
    [ -n "$W5500_HWMON" ] && pow=$(cat "$W5500_HWMON/power1_average" 2>/dev/null            || echo "?")
    [ -n "$W5500_HWMON" ] && temp=$(cat "$W5500_HWMON/temp1_input" 2>/dev/null              || echo "?")
    echo "$ts $gpu $vram $pow $temp"
    sleep 1
  done
) > "$OUTDIR/sysfs.csv" 2>/dev/null &
SYSFS_PID=$!

# Run the bench (don't fail-fast — we always want to stop loggers).
set +e
"$@" > "$OUTDIR/cmd-stdout.txt" 2> "$OUTDIR/cmd-stderr.txt"
EXIT=$?
set -e

END_EPOCH=$(date +%s)
date -Iseconds > "$OUTDIR/end.txt"

# Stop loggers.
kill "$AMDGPU_PID" "$VMSTAT_PID" "$SYSFS_PID" 2>/dev/null || true
wait "$AMDGPU_PID" "$VMSTAT_PID" "$SYSFS_PID" 2>/dev/null || true

# Post-process sysfs.csv into a parseable summary.
WALL=$((END_EPOCH - START_EPOCH))
awk -v wall="$WALL" -v exit_code="$EXIT" -v name="$NAME" '
  $2 != "?" {
    n++
    g_sum += $2
    if ($2 > g_max) g_max = $2
    if ($3 != "?") { v_sum += $3; if ($3 > v_max) v_max = $3; vn++ }
    if ($4 != "?") { p_sum += $4; if ($4 > p_max) p_max = $4; pn++ }
    if ($5 != "?" && $5+0 > t_max+0) t_max = $5
  }
  END {
    printf "name=%s\n", name
    printf "exit_code=%s\n", exit_code
    printf "wall_clock_seconds=%s\n", wall
    printf "samples=%d\n", n
    if (n > 0) {
      printf "gpu_busy_mean_pct=%.1f\n", g_sum/n
      printf "gpu_busy_peak_pct=%d\n", g_max
    }
    if (vn > 0) {
      printf "vram_mean_mb=%.0f\n", v_sum/vn/1024/1024
      printf "vram_peak_mb=%.0f\n", v_max/1024/1024
    }
    if (pn > 0) {
      printf "power_mean_w=%.2f\n", p_sum/pn/1000000
      printf "power_peak_w=%.2f\n", p_max/1000000
    }
    if (t_max+0 > 0) printf "temp_peak_c=%.1f\n", t_max/1000
  }
' "$OUTDIR/sysfs.csv" > "$OUTDIR/summary.txt"

echo
echo "--- $NAME summary ---"
cat "$OUTDIR/summary.txt"
echo

exit "$EXIT"
