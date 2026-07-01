#!/usr/bin/env bash
#
# Fails (exit 1) if modules/mod-autonomous-player/src uses an API the
# player-like policy forbids at runtime: direct teleport, direct health
# manipulation used as a revive shortcut, or raw DB writes into
# quest/item/money/XP/rep/skill/spell/talent/travel-node/level state
# instead of the real game API (ADR-005 in
# docs/autonomous-player/ARCHITECTURE.md).
#
# This is a cheap early-warning grep, not a semantic check -- it will
# have false negatives (a disallowed effect reached through an allowed-
# looking call) and is not a substitute for code review once real
# Combat/Travel/QuestEngine code exists (Gate 3+).

set -euo pipefail

MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${MODULE_DIR}/src"

if [[ ! -d "${SRC_DIR}" ]]; then
    echo "check_no_forbidden_apis: ${SRC_DIR} not found" >&2
    exit 1
fi

fail=0

deny_patterns=(
    '\bTeleportTo\s*\('
    '\bNearTeleportTo\s*\('
    '\.DirectExecute\s*\('
    '\bCharacterDatabase\.Execute\s*\('
    '\bWorldDatabase\.Execute\s*\('
    '\bSetHealth\s*\(\s*GetMaxHealth'
)

for pattern in "${deny_patterns[@]}"; do
    if grep -rEn "${pattern}" "${SRC_DIR}"; then
        echo "FAIL: forbidden API pattern matched: ${pattern}" >&2
        fail=1
    fi
done

if [[ "${fail}" -eq 0 ]]; then
    echo "OK: no forbidden API usage found in ${SRC_DIR}"
fi

exit "${fail}"
