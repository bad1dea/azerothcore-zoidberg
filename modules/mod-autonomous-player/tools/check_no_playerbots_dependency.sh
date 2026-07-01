#!/usr/bin/env bash
#
# Fails (exit 1) if modules/mod-autonomous-player/src contains anything
# that looks like a Playerbots dependency: an #include pointing into
# mod-playerbots, use of Playerbots-prefixed symbols, or a copy-paste
# marker referencing mod-playerbots/Playerbots.
#
# This is a cheap source-origin check (ADR-006 in
# docs/autonomous-player/ARCHITECTURE.md). It is not a substitute for
# code review, and it does not (yet) check build-time link output --
# that check is added once there's a non-trivial build to inspect.

set -euo pipefail

MODULE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${MODULE_DIR}/src"

if [[ ! -d "${SRC_DIR}" ]]; then
    echo "check_no_playerbots_dependency: ${SRC_DIR} not found" >&2
    exit 1
fi

fail=0

# 1. #include pointing at mod-playerbots
if grep -rEn '#include.*mod-playerbots' "${SRC_DIR}"; then
    echo "FAIL: found #include referencing mod-playerbots" >&2
    fail=1
fi

# 2. Playerbots-prefixed symbols (PlayerbotAI, PlayerbotMgr,
#    sRandomPlayerbotMgr, sPlayerbotAIConfig, etc.)
if grep -rEn '\b(Playerbot[A-Za-z]*|sRandomPlayerbotMgr|sPlayerbot[A-Za-z]*)\b' "${SRC_DIR}"; then
    echo "FAIL: found Playerbots-prefixed symbol usage" >&2
    fail=1
fi

# 3. Literal copy-paste marker referencing the other module/project
if grep -rEln '(mod-playerbots|Playerbots)' "${SRC_DIR}"; then
    echo "FAIL: found literal reference to mod-playerbots/Playerbots in source" >&2
    fail=1
fi

if [[ "${fail}" -eq 0 ]]; then
    echo "OK: no Playerbots dependency found in ${SRC_DIR}"
fi

exit "${fail}"
