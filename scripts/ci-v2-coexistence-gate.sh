#!/usr/bin/env bash
# scripts/ci-v2-coexistence-gate.sh — V2-coexistence-with-V1 grep guards.
#
# Regression-fails the build if any of the V1-coexistence cleanup
# regressed (legacy migration helpers, orphan ServerConfig/ClientConfig
# conversion helpers, hardcoded default-socket-path callsites in
# production code).
#
# Tied to `make v2-gate` and intentionally lightweight: pure grep, no
# build dependency, runs in < 1 s on the repo tree. Invoke from CI as a
# parallel step alongside `make build` + `make test`, or as a
# pre-commit hook. Exit codes:
#   0  All grep guards pass.
#   1  Any guard hit — output lists the violations.
#
# Allowlist conventions:
#   - `load_legacy_config_as_job_config` / `save_legacy_config_as_job_config`
#     remain in `src/config.cpp` (def) + `src/config.h` (decl) for the
#     test corpus that still exercises V1-format YAML round-trip.
#   - `migrate_legacy_config_if_present` must be absent everywhere
#     (function deleted, callers deleted, decl deleted).
#   - `to_server_config` / `to_client_config` must be absent everywhere
#     (orphan helpers deleted).
#   - `default_socket_path(` must only appear at its definition in
#     `src/ipc_protocol.cpp` and declaration in `src/ipc_protocol.h`;
#     production callsites should use `server_socket_path()` instead.

set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

FAIL_COUNT=0
PASS_COUNT=0

record() {
    local result="$1" label="$2" detail="${3:-}"
    if [ "$result" = "ok" ]; then
        echo "[PASS] $label"
        PASS_COUNT=$((PASS_COUNT+1))
    else
        echo "[FAIL] $label"
        if [ -n "$detail" ]; then
            printf '%s\n' "$detail" | sed 's/^/         /'
        fi
        FAIL_COUNT=$((FAIL_COUNT+1))
    fi
}

# Helper: count git-grep hits in src/, excluding allowlisted files via
# a regex on the colon-prefixed `path:line:` output.
grep_count_excluding() {
    local pattern="$1" exclude_regex="$2"
    git grep -n "$pattern" src/ 2>/dev/null \
        | grep -v -E "$exclude_regex" \
        | wc -l
}

# Gate 1 — migrate_legacy_config_if_present absent everywhere.
hits=$(git grep -n 'migrate_legacy_config_if_present' src/ tests/ 2>/dev/null | wc -l)
if [ "$hits" -eq 0 ]; then
    record ok "migrate_legacy_config_if_present absent from src/ + tests/"
else
    record fail "migrate_legacy_config_if_present must be absent" \
        "$(git grep -n 'migrate_legacy_config_if_present' src/ tests/)"
fi

# Gate 2 — load_legacy_config_as_job_config callsites (excluding the
# decl + def preserved for the test corpus).
hits=$(grep_count_excluding 'load_legacy_config_as_job_config' '^src/config\.(h|cpp):')
if [ "$hits" -eq 0 ]; then
    record ok "load_legacy_config_as_job_config has no production callers"
else
    record fail "load_legacy_config_as_job_config still has production callers" \
        "$(git grep -n 'load_legacy_config_as_job_config' src/ | grep -v -E '^src/config\.(h|cpp):')"
fi

# Gate 3 — save_legacy_config_as_job_config callsites (same allowlist).
hits=$(grep_count_excluding 'save_legacy_config_as_job_config' '^src/config\.(h|cpp):')
if [ "$hits" -eq 0 ]; then
    record ok "save_legacy_config_as_job_config has no production callers"
else
    record fail "save_legacy_config_as_job_config still has production callers" \
        "$(git grep -n 'save_legacy_config_as_job_config' src/ | grep -v -E '^src/config\.(h|cpp):')"
fi

# Gate 4 — to_server_config / to_client_config absent everywhere
# (orphan helper functions deleted in Phase 2D).
hits=$(git grep -nE 'to_server_config|to_client_config' src/ tests/ 2>/dev/null | wc -l)
if [ "$hits" -eq 0 ]; then
    record ok "to_server_config / to_client_config orphan helpers absent"
else
    record fail "to_server_config / to_client_config still present" \
        "$(git grep -nE 'to_server_config|to_client_config' src/ tests/)"
fi

# Gate 5 — default_socket_path() production callsites (excluding the
# decl + def at ipc_protocol.{h,cpp}). Production code should call
# `server_socket_path()` instead.
hits=$(grep_count_excluding 'default_socket_path(' '^src/ipc_protocol\.(h|cpp):')
if [ "$hits" -eq 0 ]; then
    record ok "default_socket_path() has no production callers (use server_socket_path())"
else
    record fail "default_socket_path() still has production callers" \
        "$(git grep -n 'default_socket_path(' src/ | grep -v -E '^src/ipc_protocol\.(h|cpp):')"
fi

# Gate 6 — Phase C retirement: captions_enabled must NOT be a field on
# SessionPreferences. (The struct may mention it in a retirement
# comment; we only fail on the field-declaration shape.)
hits=$(git grep -nE 'captions_enabled\s*;|bool\s+captions_enabled' src/ipc_server.h 2>/dev/null | wc -l)
if [ "$hits" -eq 0 ]; then
    record ok "Phase C retirement intact (no captions_enabled field on SessionPreferences)"
else
    record fail "captions_enabled field re-added to SessionPreferences" \
        "$(git grep -nE 'captions_enabled\s*;|bool\s+captions_enabled' src/ipc_server.h)"
fi

# Gate 7 — V2 binary-name discipline: the V1 binary names should not
# appear as install-name references in production code, the Makefile,
# the dist/ unit files, or CMakeLists.txt. (Comments in unrelated docs
# / scripts are out of scope.)
hits=$(git grep -nE 'recmeet-daemon|recmeet-tray' src/ Makefile CMakeLists.txt dist/ 2>/dev/null | wc -l)
if [ "$hits" -eq 0 ]; then
    record ok "V1 binary names (recmeet-daemon / recmeet-tray) absent from src/Makefile/CMake/dist"
else
    record fail "V1 binary names still present in production tree" \
        "$(git grep -nE 'recmeet-daemon|recmeet-tray' src/ Makefile CMakeLists.txt dist/)"
fi

echo
echo "============================================================"
echo "V2 coexistence gate: $PASS_COUNT pass / $FAIL_COUNT fail"
echo "============================================================"

[ "$FAIL_COUNT" -eq 0 ]
