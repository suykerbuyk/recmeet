#!/usr/bin/env bash
# scripts/smoke.sh — end-to-end recmeet smoke gate (Phase E.6).
#
# Spawns recmeet-daemon + recmeet-tray (--listen-now --headless) in an
# isolated sandbox and exercises the embedded WebUI's HTTP surface
# against the live daemon. Permanent CI artifact — extend the assertion
# list as new phases land. Currently covers Phase E.6 (tray-bundled
# WebUI).
#
# Invariants:
#   - NO `set -e`: assertions tally pass/fail and the suite runs to
#     completion so the operator sees the full picture on a failed run.
#   - `XDG_CONFIG_HOME` exported ONCE at the top so BOTH binary spawns
#     inherit it (rev-2 finding C1; prior rev set it inline on the tray
#     only, silently bypassing the gate on the daemon).
#   - `XDG_DATA_HOME` is NOT overridden — model cache reuse is desirable
#     for speed + bandwidth + determinism.
#   - Daemon log grep matches uppercase `[ERROR]` / `[WARN]` (rev-2 C2).
#   - Sandbox daemon.yaml schema uses `speaker_id.database`, NOT
#     `server.speaker_db` (rev-2 C3, verified at src/config.cpp:340-343).
#   - On failure (rc != 0) the cleanup trap dumps the daemon log tail
#     BEFORE nuking the sandbox so the operator sees the failure context
#     (rev-2 M3).
#   - Cleanup wait is bounded by a 5s watchdog that escalates to
#     SIGKILL so the script can never hang on a wedged tray (rev-2 M2).
#   - Pre-flight stops the V1 systemd daemon if it was running; cleanup
#     restarts it ONLY if it was active at start.
#
# Exit codes:
#   0  All assertions passed.
#   1  Pre-flight failure (missing build artefact, missing tool).
#   2  Daemon failed to bind its IPC socket within the poll window.
#   3  Tray failed to bind its WebUI listener within the poll window.
#   non-zero N — at least one assertion failed; cleanup printed the
#               daemon log tail.

set -uo pipefail
# NOTE: deliberately no `set -e`. Each assertion records pass/fail; we
# exit on the FAIL_COUNT at the end so the operator sees every failure
# rather than the first one.

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------

test -x build/recmeet-daemon || { echo "scripts/smoke.sh: build/recmeet-daemon missing — run 'make build' first." >&2; exit 1; }
test -x build/recmeet-tray   || { echo "scripts/smoke.sh: build/recmeet-tray missing — run 'make build' first." >&2; exit 1; }

# Freshness gate — refuse to run against a stale binary. `make smoke`
# already depends on `build`, so this primarily fires when smoke.sh is
# invoked directly with stale binaries.
#
# Heuristic: reference against the YOUNGER of the two binaries. The
# young binary's mtime represents the most recent successful build
# activity; the older binary, if older, did not need a rebuild during
# that build (its sources were already current). Therefore any tracked
# source NEWER than the young binary post-dates the last build and
# means the working tree is ahead of the binaries on disk.
#
# `ninja -q` is not available in this ninja version, and `ninja -n` is
# defeated by CMake's globbed-source regen step (it shows "Re-running
# CMake" on every invocation regardless of true staleness). Mtime is
# the most reliable signal we have.
#
# Override with RECMEET_SMOKE_SKIP_FRESHNESS=1 for the rare case where
# mtimes are noise (fresh git checkout on a system whose clock skewed).
if [ "${RECMEET_SMOKE_SKIP_FRESHNESS:-0}" != "1" ]; then
    if [ build/recmeet-daemon -nt build/recmeet-tray ]; then
        REF=build/recmeet-daemon
    else
        REF=build/recmeet-tray
    fi
    STALE=$(find src tests CMakeLists.txt cmake \
                 \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \
                    -o -name '*.cc' -o -name '*.cmake' \
                    -o -name 'CMakeLists.txt' \) \
                 -newer "$REF" -print 2>/dev/null | head -5)
    if [ -n "$STALE" ]; then
        echo "scripts/smoke.sh: STALE BUILD — source newer than $REF (the younger binary):" >&2
        echo "$STALE" | sed 's/^/  /' >&2
        echo "Run 'make build' (or 'make smoke' which now depends on build)." >&2
        echo "Override with RECMEET_SMOKE_SKIP_FRESHNESS=1 if intentional." >&2
        exit 1
    fi
fi

for tool in jq uuidgen ss curl; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "scripts/smoke.sh: required tool missing: $tool" >&2
        exit 1
    }
done

# Fixture sources — overridable via env. Default to canonical sample meetings.
LEGACY_SRC="${LEGACY_SRC:-$HOME/meetings/2026-02-21_13-49}"
V2_SRC="${V2_SRC:-$HOME/meetings/2026-02-21_13-52}"
if [ ! -d "$LEGACY_SRC" ] || [ ! -d "$V2_SRC" ]; then
    echo "scripts/smoke.sh: fixture source missing — override with LEGACY_SRC=... V2_SRC=..." >&2
    echo "  LEGACY_SRC=$LEGACY_SRC" >&2
    echo "  V2_SRC=$V2_SRC" >&2
    exit 1
fi

# V1 daemon snapshot — capture state at start so cleanup can restore it.
V1_WAS_ACTIVE=0
if systemctl --user is-active --quiet recmeet-daemon.service 2>/dev/null; then
    V1_WAS_ACTIVE=1
    echo "scripts/smoke.sh: stopping V1 recmeet-daemon.service (will restart at exit)"
    systemctl --user stop recmeet-daemon.service
fi

# ---------------------------------------------------------------------------
# Sandbox
# ---------------------------------------------------------------------------

SMOKE_ROOT="$(mktemp -d -t recmeet-smoke-XXXXXX)"
mkdir -p "$SMOKE_ROOT"/{meetings,speakers,logs,xdg-config/recmeet}

# Export ONCE so BOTH binary spawns inherit it (rev-2 C1).
export XDG_CONFIG_HOME="$SMOKE_ROOT/xdg-config"

# daemon.yaml — rev-2 C3 corrected schema. `speaker_id.database` is the
# YAML key the V2 daemon reads at src/config.cpp:340-343 — NOT
# `server.speaker_db`.
cat > "$SMOKE_ROOT/xdg-config/recmeet/daemon.yaml" <<EOF
# recmeet daemon configuration (E.6 smoke-gate sandbox)
server:
  meetings_root: "$SMOKE_ROOT/meetings"
speaker_id:
  database: "$SMOKE_ROOT/speakers"
EOF

# ---------------------------------------------------------------------------
# Fixtures — legacy V1 (no meeting_id) + V2 (stamped).
# ---------------------------------------------------------------------------

cp -a "$LEGACY_SRC" "$SMOKE_ROOT/meetings/legacy_v1_meeting"
# Sanity: legacy fixture must not already have a meeting_id.
if [ -f "$SMOKE_ROOT/meetings/legacy_v1_meeting/context.json" ] && \
   grep -q meeting_id "$SMOKE_ROOT/meetings/legacy_v1_meeting/context.json"; then
    echo "scripts/smoke.sh: legacy fixture already contains meeting_id — pick a different LEGACY_SRC" >&2
    rm -rf "$SMOKE_ROOT"
    exit 1
fi

cp -a "$V2_SRC" "$SMOKE_ROOT/meetings/v2_meeting"
V2_MID="$(uuidgen | tr 'A-Z' 'a-z')"
if [ -f "$SMOKE_ROOT/meetings/v2_meeting/context.json" ]; then
    jq --arg mid "$V2_MID" '. + {meeting_id: $mid}' \
        "$SMOKE_ROOT/meetings/v2_meeting/context.json" \
        > "$SMOKE_ROOT/meetings/v2_meeting/context.json.tmp" \
        && mv "$SMOKE_ROOT/meetings/v2_meeting/context.json.tmp" \
              "$SMOKE_ROOT/meetings/v2_meeting/context.json"
else
    printf '{"context":"","meeting_id":"%s"}\n' "$V2_MID" \
        > "$SMOKE_ROOT/meetings/v2_meeting/context.json"
fi

# ---------------------------------------------------------------------------
# Cleanup trap (rev-2 M2 + M3)
# ---------------------------------------------------------------------------

DAEMON_PID=""
TRAY_PID=""
cleanup() {
    local rc=$?
    # On failure (or unclean exit), tail the daemon log so the operator
    # sees the failure context BEFORE we nuke the sandbox (rev-2 M3).
    if [ "$rc" -ne 0 ] && [ -d "$SMOKE_ROOT/logs" ]; then
        echo
        echo "=== Daemon log tail (assertion failure context) ==="
        # shellcheck disable=SC2012
        if ls -1 "$SMOKE_ROOT/logs"/*.log >/dev/null 2>&1; then
            tail -50 "$SMOKE_ROOT/logs"/*.log
        else
            echo "(no log files)"
        fi
        echo "=== End log tail ==="
    fi

    # SIGTERM everything; the watchdog will SIGKILL if anything is wedged.
    if [ -n "$TRAY_PID" ]; then
        kill -TERM "$TRAY_PID" 2>/dev/null || true
    fi
    if [ -n "$DAEMON_PID" ]; then
        kill -TERM "$DAEMON_PID" 2>/dev/null || true
    fi

    # Bounded watchdog (rev-2 M2). Backgrounded; the foreground `wait`
    # below joins the children; if THIS function's `wait` doesn't return
    # within 5s, the watchdog SIGKILLs and frees us. The watchdog is
    # cleaned up on success path.
    (
        sleep 5
        kill -KILL "$TRAY_PID" "$DAEMON_PID" 2>/dev/null || true
    ) &
    WATCHDOG=$!
    if [ -n "$TRAY_PID" ];   then wait "$TRAY_PID"   2>/dev/null || true; fi
    if [ -n "$DAEMON_PID" ]; then wait "$DAEMON_PID" 2>/dev/null || true; fi
    kill "$WATCHDOG" 2>/dev/null || true

    rm -rf "$SMOKE_ROOT"

    if [ "$V1_WAS_ACTIVE" = "1" ]; then
        systemctl --user start recmeet-daemon.service 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Assertion harness (rev-2 O2)
# ---------------------------------------------------------------------------

PASS_COUNT=0
FAIL_COUNT=0
record() {
    local result="$1" label="$2" detail="${3:-}"
    if [ "$result" = "ok" ]; then
        echo "[PASS] $label"
        PASS_COUNT=$((PASS_COUNT+1))
    else
        echo "[FAIL] $label${detail:+ — $detail}"
        FAIL_COUNT=$((FAIL_COUNT+1))
    fi
}

assert_status() {
    local label="$1" url="$2" expected="$3"
    local actual
    actual=$(curl -sS "$url" -o /dev/null -w '%{http_code}')
    if [ "$actual" = "$expected" ]; then
        record ok "$label"
    else
        record fail "$label" "got $actual, expected $expected"
    fi
}

assert_jq() {
    local label="$1" url="$2" expr="$3"
    local body
    body=$(curl -sS "$url")
    if echo "$body" | jq -e "$expr" >/dev/null 2>&1; then
        record ok "$label"
    else
        record fail "$label" "body=$(echo "$body" | head -c 200)"
    fi
}

# ---------------------------------------------------------------------------
# Spawn daemon
# ---------------------------------------------------------------------------

build/recmeet-daemon \
    --socket "$SMOKE_ROOT/daemon.sock" \
    --log-dir "$SMOKE_ROOT/logs" >/dev/null 2>&1 &
DAEMON_PID=$!

# Poll for socket bind (up to 5s).
for _ in $(seq 1 50); do
    [ -S "$SMOKE_ROOT/daemon.sock" ] && break
    sleep 0.1
done
if [ ! -S "$SMOKE_ROOT/daemon.sock" ]; then
    echo "scripts/smoke.sh: daemon failed to bind $SMOKE_ROOT/daemon.sock within 5s" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Spawn tray (--headless + --listen-now — the CI smoke shape)
# ---------------------------------------------------------------------------

build/recmeet-tray \
    --daemon "$SMOKE_ROOT/daemon.sock" \
    --listen-now --headless >/dev/null 2>&1 &
TRAY_PID=$!

# Poll for listener bind via `ss -ltnp` (up to 5s). The tray's startup
# log line "embedded WebUI listening on http://127.0.0.1:<port>" is an
# alternative discovery path if `ss` proves flaky in the future, but
# `ss` is reliable on Linux and avoids parsing a freeform log stream.
PORT=""
for _ in $(seq 1 50); do
    PORT=$(ss -ltnp 2>/dev/null | awk '/recmeet-tray/ {print $4}' \
           | awk -F: '{print $NF}' | head -1)
    [ -n "$PORT" ] && break
    sleep 0.1
done
if [ -z "$PORT" ]; then
    echo "scripts/smoke.sh: tray failed to bind WebUI listener within 5s" >&2
    exit 3
fi
URL="http://127.0.0.1:$PORT"
echo "scripts/smoke.sh: daemon socket=$SMOKE_ROOT/daemon.sock  tray listener=$URL"

# ---------------------------------------------------------------------------
# Assertions
# ---------------------------------------------------------------------------

# Health endpoint — local, no IPC.
assert_status "GET /api/health is 200"     "$URL/api/health" 200
assert_jq     "GET /api/health body shape" "$URL/api/health" '.status == "ok"'

# Meetings list — 2 entries, 1 with meeting_id, 1 legacy. The wire
# returns `meetings` as an array (per the embedded JSON-array body the
# translator forwards raw from the daemon).
assert_jq "GET /api/meetings is an array" "$URL/api/meetings" \
    '. | type == "array"'
assert_jq "GET /api/meetings has 2 entries" "$URL/api/meetings" \
    'length == 2'
assert_jq "GET /api/meetings has 1 v2 meeting" "$URL/api/meetings" \
    '[.[] | select(.meeting_id != null and .meeting_id != "")] | length == 1'
assert_jq "GET /api/meetings has 1 legacy meeting" "$URL/api/meetings" \
    '[.[] | select(.meeting_id == null or .meeting_id == "")] | length == 1'

# Meeting detail (V2 fixture).
assert_status "GET /api/meetings/<v2>/speakers is 200" \
    "$URL/api/meetings/$V2_MID/speakers" 200
assert_jq "GET /api/meetings/<v2>/speakers is array shape" \
    "$URL/api/meetings/$V2_MID/speakers" '. | type == "array"'

# Note endpoint — rev-2 M1: accept set is 200|400 (the translator does
# NOT emit 404 for missing notes; src/tray_web.cpp:150-161 confirms).
NOTE_STATUS=$(curl -sS "$URL/api/meetings/$V2_MID/note" \
              -o /dev/null -w '%{http_code}')
case "$NOTE_STATUS" in
    200|400|404) record ok   "GET /api/meetings/<v2>/note returns 200|400|404 (HTTP $NOTE_STATUS)" ;;
    *)           record fail "GET /api/meetings/<v2>/note returns 200|400|404" "got $NOTE_STATUS" ;;
esac

# Speakers endpoint.
assert_status "GET /api/speakers is 200" "$URL/api/speakers" 200
assert_jq "GET /api/speakers is array shape" "$URL/api/speakers" \
    '. | type == "array"'

# Batch reidentify — only assert the happy path (rev-2 H4: drop the
# second-call rejection — the worker clears its in-progress flag in
# <1ms in a single-meeting sandbox and the assertion races against the
# epilogue. Coverage already deterministic in
# tests/test_speakers_meetings_ipc.cpp lines 767+).
BATCH_BODY=$(curl -sS -X POST "$URL/api/speakers/batch-reidentify")
if echo "$BATCH_BODY" | jq -e '.async == true and .ok == true' >/dev/null 2>&1; then
    record ok "POST /api/speakers/batch-reidentify async ack"
else
    record fail "POST /api/speakers/batch-reidentify async ack" "body=$BATCH_BODY"
fi

# Static assets — 200 + no-cache header. (The full Content-Type checks
# are covered by tests/test_tray_web.cpp; the smoke gate just confirms
# they're served at all from the embedded asset blob.)
assert_status "GET / is 200"            "$URL/"            200
assert_status "GET /app.js is 200"      "$URL/app.js"      200
assert_status "GET /style.css is 200"   "$URL/style.css"   200
assert_status "GET /favicon.svg is 200" "$URL/favicon.svg" 200

if curl -sI "$URL/app.js" | grep -qi '^cache-control:.*no-cache'; then
    record ok "GET /app.js has Cache-Control: no-cache"
else
    record fail "GET /app.js has Cache-Control: no-cache"
fi

# Daemon log — rev-2 C2: match uppercase [ERROR] / [WARN] (the log
# format from src/log.cpp:174 emits "[ERROR]", "[WARN]", "[INFO]";
# the prior lowercase grep was a silent gate bypass).
if ls -1 "$SMOKE_ROOT/logs"/*.log >/dev/null 2>&1; then
    if grep -qE '\[(ERROR|WARN)\]' "$SMOKE_ROOT/logs"/*.log; then
        record fail "daemon log free of ERROR/WARN" \
                    "$(grep -E '\[(ERROR|WARN)\]' "$SMOKE_ROOT/logs"/*.log | head -3 | tr '\n' ' | ')"
    else
        record ok "daemon log free of ERROR/WARN"
    fi
else
    record fail "daemon log free of ERROR/WARN" "no log files produced"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "============================================================"
echo "Smoke gate: $PASS_COUNT pass / $FAIL_COUNT fail"
echo "============================================================"

[ "$FAIL_COUNT" -eq 0 ]
