#!/usr/bin/env bash
# verify_runtime_guides_installed.sh — pre-soak gate: confirm the current Docker image
# contains the guide fixes in the CORRECT runtime path.
#
# Runtime path: /azerothcore/modules/mod-idlebot/data/guides/
# (IdleBotManager.cpp resolves this as the first existing candidate from CWD=/azerothcore)
#
# Checks:
#   1. Container is running
#   2. Tauren guide q753 steps present in the canonical starter guide
#   3. Human/dwarf q747 presence sanity check
#   4. No duplicate guide IDs between primary and generated trees
#   5. No stale env/dist/data/guides/ content confuses diagnosis
#
# Exit codes: 0 = pass, 1 = fail
#
# Usage:
#   bash tools/verify_runtime_guides_installed.sh
#   bash tools/verify_runtime_guides_installed.sh --host 10.10.30.20

set -euo pipefail

HOST="${IDLEBOT_HOST:-10.10.30.20}"
CONTAINER="ac-worldserver"
RUNTIME_PATH="/azerothcore/modules/mod-idlebot/data/guides"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host) HOST="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# ---- Helpers ----
if docker info >/dev/null 2>&1; then
    dexec() { docker exec "$CONTAINER" bash -c "$1" 2>/dev/null || true; }
    container_up() { docker inspect -f '{{.State.Running}}' "$CONTAINER" 2>/dev/null | grep -q true; }
else
    dexec() { ssh "khuong@$HOST" "docker exec $CONTAINER bash -c '$1'" 2>/dev/null || true; }
    container_up() { ssh "khuong@$HOST" "docker inspect -f '{{.State.Running}}' $CONTAINER 2>/dev/null" 2>/dev/null | grep -q true; }
fi

echo ""
echo "=== verify_runtime_guides_installed.sh ==="
echo "Container:    $CONTAINER"
echo "Runtime path: $RUNTIME_PATH"
echo ""

FAILED=0

# ---- 1. Container running ----
echo "1) Container running:"
if container_up; then
    echo "   OK    $CONTAINER is up"
else
    echo "   FAIL  $CONTAINER is not running — start it before verifying guides"
    FAILED=1
fi
echo ""

# ---- 2. Runtime path exists ----
echo "2) Runtime guide path exists in container:"
PATH_EXISTS=$(dexec "test -d \"$RUNTIME_PATH\" && echo yes || echo no")
if [ "${PATH_EXISTS:-no}" = "yes" ]; then
    GUIDE_COUNT=$(dexec "find \"$RUNTIME_PATH\" -name '*.yaml' | wc -l" || echo "?")
    echo "   OK    $RUNTIME_PATH exists ($GUIDE_COUNT yaml files)"
else
    echo "   FAIL  $RUNTIME_PATH does not exist in container"
    echo "         This means the Docker COPY step did not run. Rebuild the image."
    FAILED=1
fi
echo ""

# ---- 3. Tauren q753 fix present ----
echo "3) Tauren guide: q753 steps present (3 required: accept/collect/turnin):"
Q753_COUNT=$(dexec "grep -c 'quest_id: 753' \"$RUNTIME_PATH/horde/tauren/00_camp_narache-1-6.yaml\" 2>/dev/null || true")
Q753_COUNT="${Q753_COUNT//[[:space:]]/}"
if [ "${Q753_COUNT:-0}" -ge 3 ]; then
    echo "   OK    $Q753_COUNT occurrences of 'quest_id: 753' found in 00_camp_narache-1-6.yaml"
else
    echo "   FAIL  Found $Q753_COUNT occurrences of 'quest_id: 753' (need >=3)"
    echo "         The tauren guide is missing the q753 prereq steps."
    echo "         Fix: ensure 00_camp_narache-1-6.yaml has q753_accept/q753_water/q753_turnin"
    echo "         then rebuild the Docker image."
    FAILED=1
fi
echo ""

# ---- 4. Human/dwarf guide q747 collect step (regression check) ----
echo "4) Human guide: q747 collect step present (regression check):"
Q747_COUNT=$(dexec "grep -R -l 'quest_id: 747' \"$RUNTIME_PATH/alliance\" 2>/dev/null | wc -l" || echo "0")
Q747_COUNT="${Q747_COUNT//[[:space:]]/}"
if [ "${Q747_COUNT:-0}" -ge 1 ]; then
    echo "   OK    q747 found in $Q747_COUNT guide file(s)"
else
    echo "   WARN  q747 not found in $RUNTIME_PATH — human/dwarf guide may be missing"
    # Not a hard failure since this may not exist in all guide sets
fi
echo ""

# ---- 5. Duplicate guide IDs between horde/ and generated/ ----
echo "5) Duplicate guide IDs (primary vs generated/):"
PRIMARY_IDS=$(dexec "find \"$RUNTIME_PATH/alliance\" \"$RUNTIME_PATH/horde\" -type f -name '*.yaml' -print0 2>/dev/null | xargs -0 grep -h '^id:' 2>/dev/null | sort -u" || true)
GENERATED_IDS=$(dexec "find \"$RUNTIME_PATH/generated\" -type f -name '*.yaml' -print0 2>/dev/null | xargs -0 grep -h '^id:' 2>/dev/null | sort -u" || true)

if [ -z "$PRIMARY_IDS" ] || [ -z "$GENERATED_IDS" ]; then
    echo "   SKIP  One or both directories empty — cannot check for duplicates"
else
    DUPES=$(comm -12 <(printf '%s\n' "$PRIMARY_IDS") <(printf '%s\n' "$GENERATED_IDS") || true)
    if [ -z "$DUPES" ]; then
        echo "   OK    No duplicate IDs between primary guides and generated/"
    else
        echo "   WARN  Duplicate guide IDs (first-wins, walk order is unspecified):"
        echo "$DUPES" | while read -r line; do
            echo "         $line"
        done
        echo "         Fix: remove generated/ from the container COPY or deduplicate IDs."
        # Warning only — current content is correct, but fragile
    fi
fi
echo ""

# ---- 6. Stale env/dist decoy ----
echo "6) Stale env/dist/data/guides/ decoy:"
DECOY_EXISTS=$(dexec "test -d /azerothcore/env/dist/data/guides && echo yes || echo no" || echo "no")
if [ "${DECOY_EXISTS:-no}" = "yes" ]; then
    DECOY_COUNT=$(dexec "find /azerothcore/env/dist/data/guides -name '*.yaml' | wc -l" || echo "?")
    echo "   WARN  /azerothcore/env/dist/data/guides/ exists ($DECOY_COUNT files)"
    echo "         The server does NOT read from this path (see IdleBotManager.cpp:98-108)."
    echo "         It is a stale artifact from a prior cmake install. Not a blocker,"
    echo "         but delete it to avoid future misdiagnosis."
else
    echo "   OK    No stale env/dist/data/guides/ decoy found"
fi
echo ""

# ---- Summary ----
if [ "$FAILED" = "0" ]; then
    echo "=== GATE: PASS — runtime guides are installed correctly ==="
    exit 0
else
    echo "=== GATE: FAIL — fix guide deployment before starting soak ==="
    echo ""
    echo "Typical fix:"
    echo "  1. Ensure guide edits are in modules/mod-idlebot/data/guides/ locally"
    echo "  2. scp -r modules/mod-idlebot/data/guides/ khuong@10.10.30.20:~/build/azerothcore-zoidberg/modules/mod-idlebot/data/guides/"
    echo "  3. Rebuild Docker image (Komodo Build: ac-worldserver-zoidberg)"
    echo "  4. Redeploy: ssh khuong@10.10.30.20 'cd ~/build/azerothcore-zoidberg && docker compose pull && docker compose up -d'"
    echo "  5. Re-run this verifier"
    exit 1
fi
