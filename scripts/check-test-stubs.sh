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
# File-level exemption policy
# ---------------------------
# Exemptions are baked into this script (the FILE_EXEMPTIONS array below),
# NOT carried in a separate .allowlist file. Each exemption MUST be
# preceded by a `# Reason:` comment that documents WHY conversion is not
# tractable (architectural infeasibility, Phase 2c follow-on, etc.).
#
# Exemptions are for tests where converting to production handlers is
# architecturally infeasible. Each requires a documented reason. The list
# should DECREASE, never grow. When a Phase 2c/3 lands that retires an
# exemption, drop the entry; CI re-runs the gate and confirms zero
# unexempted offenders remain.
#
# Exit codes:
#   0 — clean (zero unexempted offenders).
#   1 — one or more `tests/*.cpp` files register a production verb stub
#       outside the exemption list.
#   2 — internal error (missing daemon_handlers, malformed exemption,
#       derived zero production verbs, ...).
#
# Usage (run from repo root, CI runs this BEFORE `make build`):
#   ./scripts/check-test-stubs.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DAEMON_HANDLERS="${REPO_ROOT}/src/daemon_handlers.cpp"
TESTS_DIR="${REPO_ROOT}/tests"

# ---------------------------------------------------------------------------
# FILE_EXEMPTIONS — basenames of `tests/*.cpp` files whose production-verb
# stubs are SKIPPED by this gate. Every entry MUST be preceded by a
# `# Reason: ...` comment. The validate_exemptions step below enforces that
# at script startup; a missing-reason exemption is a hard error (exit 2).
#
# The list should DECREASE, never grow. New entries require an operator
# decision documented in the commit message that adds them.
# ---------------------------------------------------------------------------
FILE_EXEMPTIONS=(
    # Reason: WebUI translator test. DaemonSim's force_invalid_params /
    # force_internal_error knobs let the test inject error-path responses
    # production handlers can't simulate. Replacing the mock would couple
    # translator coverage to handler globals — wrong tool.
    "test_tray_web.cpp"

    # Reason: Phase 2c — Pre-streaming captions architecture. Handler API
    # mismatch: stub process.submit accepts captions_enabled at submit time;
    # production routes captions via session prefs / streaming session
    # params. Conversion blocks on either retargeting at streaming-session
    # path or Phase 3 daemon-spawning harness.
    "test_caption_ipc_integration.cpp"

    # Reason: Phase 2c — Legacy DaemonSim shared by ~30 TEST_CASEs driving
    # process.submit/process.cancel/job.context flows against a mocked
    # recording lifecycle. Conversion requires retiring or rewriting all
    # 30+ tests against the streaming / upload session managers. Out of
    # scope for Phase 2b; tracked as Phase 2c follow-on.
    "test_ipc_integration.cpp"
)

# Validate that every FILE_EXEMPTIONS entry has a `# Reason:` comment line
# immediately preceding it in this script. A missing reason means the
# exemption is undocumented — refuse to run.
validate_exemptions() {
    local self="${BASH_SOURCE[0]}"
    if [[ ! -f "${self}" ]]; then return 0; fi
    local i
    for (( i=0; i<${#FILE_EXEMPTIONS[@]}; ++i )); do
        local entry="${FILE_EXEMPTIONS[i]}"
        # Find the line in self that declares this entry inside the
        # FILE_EXEMPTIONS=( ... ) block, then check the preceding lines
        # for a `# Reason:` comment (allowing for multi-line reasons that
        # continue with `#` comment lines).
        local entry_lineno
        entry_lineno=$(grep -nE "^[[:space:]]*\"${entry}\"" "${self}" | head -1 | cut -d: -f1)
        if [[ -z "${entry_lineno}" ]]; then
            echo "check-test-stubs: error: exemption '${entry}' not declared "\
                 "literally in FILE_EXEMPTIONS — refactor must keep entries "\
                 "discoverable for grep" >&2
            exit 2
        fi
        # Walk backward over comment lines and require at least one
        # # Reason: line above the entry, before reaching the
        # FILE_EXEMPTIONS=( opener or a blank separator.
        local probe=$(( entry_lineno - 1 ))
        local found_reason=0
        while (( probe > 0 )); do
            local line
            line=$(sed -n "${probe}p" "${self}")
            if [[ "${line}" =~ ^[[:space:]]*\#[[:space:]]*Reason: ]]; then
                found_reason=1
                break
            fi
            # Allow continuation comment lines starting with `#` but not
            # opening `#` Reason yet — keep walking up.
            if [[ "${line}" =~ ^[[:space:]]*\# ]]; then
                probe=$(( probe - 1 ))
                continue
            fi
            # Anything else (blank line, FILE_EXEMPTIONS=( opener) breaks
            # the search — no Reason found above this entry.
            break
        done
        if (( found_reason == 0 )); then
            echo "check-test-stubs: error: exemption '${entry}' has no "\
                 "preceding '# Reason: ...' comment in this script" >&2
            exit 2
        fi
    done
}
validate_exemptions

# Build an associative lookup for fast file-exempt checks.
declare -A EXEMPT_MAP=()
for f in "${FILE_EXEMPTIONS[@]}"; do
    EXEMPT_MAP["${f}"]=1
done

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
exempted_hits=0
while IFS= read -r -d '' file; do
    basename="${file##*/}"
    is_exempt=0
    if [[ -n "${EXEMPT_MAP[${basename}]:-}" ]]; then
        is_exempt=1
    fi
    # `grep -nE` prints `lineno:matching-line`. Each hit is one offender
    # (or, if the file is exempt, one exempted hit we count separately
    # for the summary line).
    while IFS= read -r hit; do
        [[ -z "${hit}" ]] && continue
        lineno="${hit%%:*}"
        rest="${hit#*:}"
        # Extract the verb from the matching line.
        verb=$(printf '%s' "${rest}" | sed -nE 's/.*server(\.|->)on\("([^"]+)".*/\2/p')
        if (( is_exempt == 1 )); then
            exempted_hits=$((exempted_hits + 1))
        else
            printf '%s:%s  %s\n' "${file#${REPO_ROOT}/}" "${lineno}" "${verb}"
            offenders=$((offenders + 1))
        fi
    done < <(grep -nE "${PATTERN}" "${file}" 2>/dev/null || true)
done < <(find "${TESTS_DIR}" -maxdepth 1 -name '*.cpp' -print0)

if (( offenders > 0 )); then
    echo "" >&2
    echo "check-test-stubs: FAIL — ${offenders} production-verb stub(s) found in tests/" >&2
    echo "  Convert each test to use DaemonTestHarness + register_daemon_handlers()," >&2
    echo "  or change the verb name to a non-production placeholder (see test_ipc_server.cpp)." >&2
    echo "  File-level exemptions exist for ${#FILE_EXEMPTIONS[@]} file(s); see "\
         "FILE_EXEMPTIONS in this script (each entry has a documented '# Reason: ...')." >&2
    exit 1
fi

printf 'check-test-stubs: OK — zero unexempted production-verb stubs '\
'in tests/ (%d verbs checked; %d hit(s) skipped across %d exempt file(s))\n' \
    "${#VERBS[@]}" "${exempted_hits}" "${#FILE_EXEMPTIONS[@]}"
exit 0
