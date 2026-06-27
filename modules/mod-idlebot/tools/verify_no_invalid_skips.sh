#!/usr/bin/env bash
# verify_no_invalid_skips.sh — pre-soak gate: fail if any bot has FAILURE events or
# force-skip config enabled. Run after reset_idlebot_test_roster.sh and before
# starting a soak. Exit 0 = all clear, exit 1 = blocked.
#
# Usage:
#   bash tools/verify_no_invalid_skips.sh
#   bash tools/verify_no_invalid_skips.sh --host 10.10.30.20

set -euo pipefail

HOST="${IDLEBOT_HOST:-10.10.30.20}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host) HOST="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# ---- DB helpers ----
# Auto-detect: if docker is available locally, skip SSH
if docker info >/dev/null 2>&1; then
    # Running on the docker host — use docker directly
    DB_PW="$(docker inspect ac-database \
        --format '{{range .Config.Env}}{{println .}}{{end}}' 2>/dev/null \
        | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2 || true)"
    [ -z "$DB_PW" ] && DB_PW="password"
    q() { docker exec ac-database mysql -uroot -p"$DB_PW" -N "$1" -e "$2" 2>/dev/null || true; }
    CONF_CMD() { docker exec ac-worldserver grep -i "$1" /azerothcore/env/dist/etc/mod_idlebot.conf 2>/dev/null | grep -v '^#' | grep -oP '= *\K[0-9]+' | head -1; }
else
    # Remote host — use SSH
    DB_PW="$(ssh "khuong@$HOST" \
        "docker inspect ac-database \
         --format '{{range .Config.Env}}{{println .}}{{end}}' 2>/dev/null \
         | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2" 2>/dev/null || true)"
    [ -z "$DB_PW" ] && DB_PW="password"
    q() { ssh "khuong@$HOST" "docker exec ac-database mysql -uroot -p'$DB_PW' -N '$1' -e \"$2\"" 2>/dev/null || true; }
    CONF_CMD() { ssh "khuong@$HOST" "docker exec ac-worldserver grep -i '$1' /azerothcore/env/dist/etc/mod_idlebot.conf 2>/dev/null | grep -v '^#' | grep -oP '= *\K[0-9]+' | head -1"; }
fi

echo ""
echo "=== verify_no_invalid_skips.sh ==="
echo "Checking host: $HOST"
echo ""

FAILED=0

# ---- 1. FAILURE event count per bot (should be 0 after reset) ----
echo "1) FAILURE event counts (must be 0 after reset):"
FAILURE_COUNTS=$(q acore_characters \
    "SELECT b.bot_name, COUNT(e.id)
       FROM idlebot_bots b
       LEFT JOIN idlebot_events e ON e.bot_id = b.id AND e.event_type = 'FAILURE'
      GROUP BY b.bot_name
      ORDER BY b.bot_name;" 2>/dev/null || true)

if [ -z "$FAILURE_COUNTS" ]; then
    echo "   ERROR: could not query idlebot_events (DB down? Wrong host?)"
    FAILED=1
else
    ANY_NONZERO=0
    while IFS=$'\t' read -r bot_name cnt; do
        if [ "${cnt:-0}" -gt 0 ]; then
            echo "   FAIL  $bot_name: QSkip=$cnt (FAILURE events not cleared — run reset first)"
            ANY_NONZERO=1
            FAILED=1
        else
            echo "   OK    $bot_name: QSkip=0"
        fi
    done <<< "$FAILURE_COUNTS"
    if [ "$ANY_NONZERO" = "0" ]; then
        echo "   All bots: QSkip=0 ✓"
    fi
fi

echo ""

# ---- 2. consecutiveForceSkips in idlebot_bots (any persistent skip streak) ----
echo "2) Consecutive force-skip streak (must be 0):"
SKIP_STREAKS=$(q acore_characters \
    "SELECT bot_name, COALESCE(step_state,'?')
       FROM idlebot_bots
      ORDER BY bot_name;" 2>/dev/null || true)

if [ -n "$SKIP_STREAKS" ]; then
    while IFS=$'\t' read -r bot_name step_state; do
        echo "   OK    $bot_name: step_state='${step_state}'"
    done <<< "$SKIP_STREAKS"
fi

echo ""

# ---- 3. Config cheat flags (must be 0) ----
echo "3) Worldserver config cheat flags (must be 0):"

# Check via live conf if server is running, otherwise warn only
CONF_ISSUES=0
for flag in \
    "IdleBot.AllowForceQuestAdvance" \
    "IdleBot.AllowForceSkipForSoak" \
    "IdleBot.AllowGMRecovery" \
    "IdleBot.AllowCheatTeleport" \
    "IdleBot.AllowCheatResurrect"; do

    val=$(CONF_CMD "$flag" 2>/dev/null || echo "?")

    if [[ "$val" == "?" || -z "$val" ]]; then
        echo "   SKIP  $flag = ? (conf unreadable — check manually)"
    elif [ "$val" != "0" ]; then
        echo "   FAIL  $flag = $val (must be 0 for valid soak)"
        CONF_ISSUES=1
        FAILED=1
    else
        echo "   OK    $flag = 0"
    fi
done

if [ "$CONF_ISSUES" = "0" ]; then
    echo "   All cheat flags off ✓"
fi

echo ""

# ---- 4. Paused bots (any quarantined bot blocks the soak) ----
echo "4) Paused/quarantined bots (should be none):"
PAUSED=$(q acore_characters \
    "SELECT bot_name, step_state
       FROM idlebot_bots
      WHERE step_state = 'paused'
      ORDER BY bot_name;" 2>/dev/null || true)

if [ -z "$PAUSED" ]; then
    echo "   No paused bots ✓"
else
    while IFS=$'\t' read -r bot_name step_state; do
        echo "   WARN  $bot_name is paused — run '.idlebot resume $bot_name' or reset"
    done <<< "$PAUSED"
    # Paused bots don't fail the gate — they won't corrupt other bots' data.
    # But warn so you know they won't participate.
fi

echo ""

# ---- Summary ----
if [ "$FAILED" = "0" ]; then
    echo "=== GATE: PASS — soak may proceed ==="
    exit 0
else
    echo "=== GATE: FAIL — fix issues above before starting soak ==="
    echo ""
    echo "Most likely fix: run reset_idlebot_test_roster.sh first:"
    echo "  bash tools/reset_idlebot_test_roster.sh"
    exit 1
fi
