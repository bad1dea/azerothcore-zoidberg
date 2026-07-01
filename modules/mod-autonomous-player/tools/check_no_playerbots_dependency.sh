#!/usr/bin/env bash
#
# Fails (exit 1) if modules/mod-autonomous-player/src contains anything
# that looks like a Playerbots dependency: an #include pointing into
# mod-playerbots, use of Playerbots-prefixed symbols, or a copy-paste
# marker referencing mod-playerbots/Playerbots.
#
# `//` line comments are stripped before matching -- this module's
# ARCHITECTURE.md deliberately documents *why* it avoids coupling to
# Playerbots-owned state (e.g. ADR-008's WorldSession::IsBot() note), and
# those explanatory comments legitimately mention "Playerbots" without it
# being a dependency. A real dependency shows up in actual code (an
# #include, a symbol use), not prose.
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

strip_line_comments() {
    sed -E 's://.*$::'
}

fail=0

while IFS= read -r -d '' file; do
    stripped="$(strip_line_comments < "${file}")"

    if grep -Eqn '#include.*mod-playerbots' <<< "${stripped}"; then
        echo "FAIL: ${file}: #include referencing mod-playerbots" >&2
        fail=1
    fi

    if grep -Eqn '\b(Playerbot[A-Za-z]*|sRandomPlayerbotMgr|sPlayerbot[A-Za-z]*)\b' <<< "${stripped}"; then
        echo "FAIL: ${file}: Playerbots-prefixed symbol usage" >&2
        fail=1
    fi

    if grep -Eqn '(mod-playerbots|Playerbots)' <<< "${stripped}"; then
        echo "FAIL: ${file}: literal reference to mod-playerbots/Playerbots outside a comment" >&2
        fail=1
    fi
done < <(find "${SRC_DIR}" -type f \( -name '*.h' -o -name '*.cpp' \) -print0)

if [[ "${fail}" -eq 0 ]]; then
    echo "OK: no Playerbots dependency found in ${SRC_DIR}"
fi

exit "${fail}"
