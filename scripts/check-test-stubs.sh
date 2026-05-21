#!/usr/bin/env bash
# Phase 2b — strict invariant gate: tests/*.cpp MUST NOT register stub
# handlers for production IPC verbs.
#
# Phase 2a extracted every production verb registration into
# `src/daemon_handlers.cpp`'s `register_daemon_handlers(server)`. Phase 2b
# wired the test harness to that function so tests exercise the real
# handler bodies, not local stubs cloned from them. From here on, any
# `server.on("<production-verb>", ...)` or `server->on("<production-verb>",
# ...)` in tests/ is a regression — either the test is shadowing the
# production handler (silent divergence risk) or the harness wasn't wired
# in (silent stub coverage risk). Both are bugs.
#
# This gate has NO allowlist. If a test file genuinely cannot drive the
# real handler (handler spawns subprocess, requires real device, etc.),
# move it to a phase-3 daemon-spawning harness or change the test's verb
# name to a non-production name (see `tests/test_ipc_server.cpp` which
# uses `test.ack` / `echo` / `ping` for IPC plumbing tests).
#
# Exit codes:
#   0 — clean (zero offenders).
#   1 — one or more `tests/*.cpp` files register a production verb stub.
#
# Usage (run from repo root, CI runs this BEFORE `make build`):
#   ./scripts/check-test-stubs.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DAEMON_HANDLERS="${REPO_ROOT}/src/daemon_handlers.cpp"
TESTS_DIR="${REPO_ROOT}/tests"

if [[ ! -f "${DAEMON_HANDLERS}" ]]; then
    echo "check-test-stubs: error: ${DAEMON_HANDLERS} not found" >&2
    exit 2
fi
if [[ ! -d "${TESTS_DIR}" ]]; then
    echo "check-test-stubs: error: ${TESTS_DIR} not found" >&2
    exit 2
fi

# Derive the authoritative production-verb list from daemon_handlers.cpp.
# Same regex as the operator's reference command in the Phase 2b brief.
mapfile -t VERBS < <(
    grep -E '^[[:space:]]*server\.on\("[a-z_.]+",' "${DAEMON_HANDLERS}" \
        | sed -E 's/.*server\.on\("([^"]+)".*/\1/' \
        | sort -u
)

if (( ${#VERBS[@]} == 0 )); then
    echo "check-test-stubs: error: derived zero production verbs from ${DAEMON_HANDLERS}" >&2
    exit 2
fi

# Build a single alternation regex that matches:
#   server.on("verb",  ...
#   server->on("verb", ...
# for every derived production verb. The verbs are escaped for literal
# regex use (dots are escaped). The alternation is wrapped in a
# non-capturing group so grep -E sees one pattern.
escape_for_regex() {
    # Escape regex metacharacters that might appear in a verb name.
    # Production verbs use only [a-z_.] but escape defensively.
    printf '%s' "$1" | sed -E 's/[][\\.*^$+?{}()|/]/\\&/g'
}

verb_alt=""
for v in "${VERBS[@]}"; do
    escaped=$(escape_for_regex "$v")
    if [[ -z "${verb_alt}" ]]; then
        verb_alt="${escaped}"
    else
        verb_alt="${verb_alt}|${escaped}"
    fi
done

# Match BOTH `server.on("verb"` and `server->on("verb"`. The pattern
# anchors on `server` followed by `.` or `->` so unrelated identifiers
# like `mock_server.on("foo")` are NOT matched (mock test scaffolding
# uses different object names).
PATTERN="server(\.|->)on\(\"(${verb_alt})\""

offenders=0
while IFS= read -r -d '' file; do
    # `grep -nE` prints `lineno:matching-line`. Each hit is one offender.
    while IFS= read -r hit; do
        [[ -z "${hit}" ]] && continue
        lineno="${hit%%:*}"
        rest="${hit#*:}"
        # Extract the verb from the matching line.
        verb=$(printf '%s' "${rest}" | sed -nE 's/.*server(\.|->)on\("([^"]+)".*/\2/p')
        printf '%s:%s  %s\n' "${file#${REPO_ROOT}/}" "${lineno}" "${verb}"
        offenders=$((offenders + 1))
    done < <(grep -nE "${PATTERN}" "${file}" 2>/dev/null || true)
done < <(find "${TESTS_DIR}" -maxdepth 1 -name '*.cpp' -print0)

if (( offenders > 0 )); then
    echo "" >&2
    echo "check-test-stubs: FAIL — ${offenders} production-verb stub(s) found in tests/" >&2
    echo "  Convert each test to use DaemonTestHarness + register_daemon_handlers()," >&2
    echo "  or change the verb name to a non-production placeholder (see test_ipc_server.cpp)." >&2
    echo "  No allowlist." >&2
    exit 1
fi

echo "check-test-stubs: OK — zero production-verb stubs in tests/ (${#VERBS[@]} verbs checked)"
exit 0
