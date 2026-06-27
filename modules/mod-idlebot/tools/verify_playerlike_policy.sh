#!/usr/bin/env bash
# verify_playerlike_policy.sh — pre-soak policy check.
#
# Verifies that no cheat/debug recovery flags are enabled in the worldserver config.
# Called automatically by run_idlebot_leveling_soak.sh before starting the soak.
# Run manually to check config health at any time.
#
# Exit codes:
#   0 = all clear; playerlike mode is enforced
#   1 = one or more cheat flags are enabled; soak must not start
#
# Usage:
#   bash tools/verify_playerlike_policy.sh
#   bash tools/verify_playerlike_policy.sh --conf /path/to/mod_idlebot.conf

set -euo pipefail

CONF_PATH=""
for arg in "$@"; do
    case "$arg" in
        --conf) shift; CONF_PATH="$1" ;;
        *) ;;
    esac
done

# Locate the active config file (search common locations).
if [ -z "$CONF_PATH" ]; then
    for candidate in \
        "./mod_idlebot.conf" \
        "/home/khuong/azeroth-server/etc/mod_idlebot.conf" \
        "/opt/azeroth-server/etc/mod_idlebot.conf" \
        "$HOME/azeroth-server/etc/mod_idlebot.conf"
    do
        if [ -f "$candidate" ]; then
            CONF_PATH="$candidate"
            break
        fi
    done
fi

DIST_PATH="$(dirname "$(dirname "$0")")/conf/mod_idlebot.conf.dist"

FAIL=0

echo "=== IdleBot Playerlike Policy Check ==="
echo ""

if [ -n "$CONF_PATH" ] && [ -f "$CONF_PATH" ]; then
    echo "  Config: $CONF_PATH"
    CONF="$CONF_PATH"
elif [ -f "$DIST_PATH" ]; then
    echo "  Config: $DIST_PATH (dist defaults — no local override found)"
    CONF="$DIST_PATH"
else
    echo "  WARNING: No config file found. Checking defaults only."
    CONF="/dev/null"
fi

echo ""

# Read a config value from the file, returning the last matching line's value.
# Falls back to the supplied default if not found.
get_conf() {
    local key="$1" default="$2"
    local val
    val=$(grep -E "^\s*${key}\s*=" "$CONF" 2>/dev/null | tail -1 | sed 's/.*=\s*//' | tr -d ' "' || true)
    echo "${val:-$default}"
}

check_flag() {
    local key="$1" wanted="$2" label="$3"
    local val
    val=$(get_conf "$key" "$wanted")
    if [ "$val" = "$wanted" ]; then
        printf "  %-42s = %s  OK\n" "$key" "$val"
    else
        printf "  %-42s = %s  *** FAIL — must be %s ***\n" "$key" "$val" "$wanted"
        FAIL=1
    fi
}

echo "  Checking policy flags (all must match expected value):"
echo ""
check_flag "IdleBot.PlayerlikeMode"         "1"  "Playerlike mode ON"
check_flag "IdleBot.AllowCheatTeleport"     "0"  "Cheat teleport OFF"
check_flag "IdleBot.AllowCheatResurrect"    "0"  "Cheat resurrect OFF"
check_flag "IdleBot.AllowForceQuestAdvance" "0"  "Force quest advance OFF"
check_flag "IdleBot.AllowForceSkipForSoak"  "0"  "Force skip for soak OFF"
check_flag "IdleBot.AllowGMRecovery"        "0"  "GM recovery OFF"
echo ""

# Also check the worldserver container's live config if possible.
if command -v docker &>/dev/null 2>&1; then
    CONTAINER_CONF=$(docker exec ac-worldserver cat /opt/azeroth-server/etc/mod_idlebot.conf 2>/dev/null || true)
    if [ -n "$CONTAINER_CONF" ]; then
        echo "  Also checking live container config (ac-worldserver):"
        for key in IdleBot.PlayerlikeMode IdleBot.AllowCheatTeleport IdleBot.AllowCheatResurrect \
                   IdleBot.AllowForceQuestAdvance IdleBot.AllowForceSkipForSoak IdleBot.AllowGMRecovery; do
            val=$(echo "$CONTAINER_CONF" | grep -E "^\s*${key}\s*=" | tail -1 | sed 's/.*=\s*//' | tr -d ' "' || true)
            echo "    $key = ${val:-<not set>}"
        done
        echo ""
    fi
fi

if [ "$FAIL" = "1" ]; then
    echo ""
    echo "  *** POLICY CHECK FAILED ***"
    echo ""
    echo "  One or more cheat/debug flags are enabled."
    echo "  Soak must not run with these settings — progression data would be invalid."
    echo ""
    echo "  To fix: set the failing flags to the required value in mod_idlebot.conf"
    echo "  then rebuild and redeploy the worldserver image."
    echo ""
    echo "  See docs/PLAYERLIKE_POLICY.md for the full policy."
    exit 1
else
    echo "  All policy flags OK — playerlike mode is enforced."
    echo "  Soak may proceed."
    echo ""
fi
