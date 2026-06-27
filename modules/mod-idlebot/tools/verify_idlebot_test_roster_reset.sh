#!/usr/bin/env bash
# verify_idlebot_test_roster_reset.sh — pre-soak gate: confirm all bots are in clean
# reset state before starting a new soak. Prevents re-runs that taint progression data.
#
# Checks (all must pass):
#   1. All bots at level 1
#   2. step_index = 0 for all bots
#   3. death_count_total = 0 for all bots
#   4. FAILURE events = 0 for all bots
#   5. character_queststatus_rewarded = 0 for all bots
#   6. character_inventory empty for all bots
#
# Exit codes: 0 = all clean, 1 = needs reset
#
# Usage:
#   bash tools/verify_idlebot_test_roster_reset.sh
#   bash tools/verify_idlebot_test_roster_reset.sh --host 10.10.30.20

set -euo pipefail

HOST="${IDLEBOT_HOST:-10.10.30.20}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host) HOST="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# ---- DB helpers ----
if docker info >/dev/null 2>&1; then
    DB_PW="$(docker inspect ac-database \
        --format '{{range .Config.Env}}{{println .}}{{end}}' 2>/dev/null \
        | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2 || true)"
    [ -z "$DB_PW" ] && DB_PW="password"
    q() { docker exec ac-database mysql -uroot -p"$DB_PW" -N "$1" -e "$2" 2>/dev/null || true; }
else
    DB_PW="$(ssh "khuong@$HOST" \
        "docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}' 2>/dev/null \
         | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2" 2>/dev/null || true)"
    [ -z "$DB_PW" ] && DB_PW="password"
    q() { ssh "khuong@$HOST" "docker exec ac-database mysql -uroot -p'$DB_PW' -N '$1' -e \"$2\"" 2>/dev/null || true; }
fi

echo ""
echo "=== verify_idlebot_test_roster_reset.sh ==="
echo ""

FAILED=0

# ---- Full roster state ----
echo "Checking all idlebot bots for clean reset state..."
echo ""

ROSTER=$(q acore_characters \
    "SELECT
         c.name,
         c.level,
         b.step_index,
         b.death_count_total,
         COALESCE(b.step_state, 'idle') AS step_state,
         (SELECT COUNT(*) FROM character_queststatus_rewarded r WHERE r.guid = c.guid) AS qdone,
         (SELECT COUNT(*) FROM idlebot_events e WHERE e.bot_id = b.id AND e.event_type = 'FAILURE') AS qfail,
         (SELECT COUNT(*) FROM character_inventory ci WHERE ci.guid = c.guid) AS inv_slots,
         (SELECT COUNT(*) FROM character_queststatus qs WHERE qs.guid = c.guid) AS qactive
       FROM idlebot_bots b
       JOIN characters c ON c.name = b.bot_name
      ORDER BY c.name;" 2>/dev/null || true)

if [ -z "$ROSTER" ]; then
    echo "  ERROR: could not query idlebot_bots — DB down or no bots registered?"
    FAILED=1
else
    printf "  %-14s %4s %5s %6s %8s %5s %5s %6s %7s\n" \
        "Bot" "Lvl" "Step" "Deaths" "State" "QDone" "QFail" "InvSlt" "QActive"
    echo "  ─────────────────────────────────────────────────────────────────────────"

    while IFS=$'\t' read -r name level step deaths state qdone qfail inv_slots qactive; do
        issues=()
        [ "${level:-0}" -ne 1 ]         && issues+=("level=${level} (want 1)")
        [ "${step:-0}" -ne 0 ]          && issues+=("step=${step} (want 0)")
        [ "${deaths:-0}" -ne 0 ]        && issues+=("deaths=${deaths} (want 0)")
        [ "${qdone:-0}" -ne 0 ]         && issues+=("QDone=${qdone} (want 0)")
        [ "${qfail:-0}" -ne 0 ]         && issues+=("QFail=${qfail} (want 0)")
        # Allow up to 30 inv slots — playerbots gives bots a starter kit on login.
        # Leftover soak items push this well above 30.
        [ "${inv_slots:-0}" -gt 30 ]    && issues+=("inv=${inv_slots} slots (>30 — likely soak residue, want ≤30)")
        [ "${qactive:-0}" -ne 0 ]       && issues+=("active_quests=${qactive} (want 0)")

        if [ "${#issues[@]}" -eq 0 ]; then
            printf "  %-14s %4s %5s %6s %8s %5s %5s %6s %7s  OK\n" \
                "$name" "${level:-?}" "${step:-?}" "${deaths:-0}" \
                "${state:-?}" "${qdone:-0}" "${qfail:-0}" "${inv_slots:-0}" "${qactive:-0}"
        else
            printf "  %-14s %4s %5s %6s %8s %5s %5s %6s %7s  FAIL\n" \
                "$name" "${level:-?}" "${step:-?}" "${deaths:-0}" \
                "${state:-?}" "${qdone:-0}" "${qfail:-0}" "${inv_slots:-0}" "${qactive:-0}"
            for issue in "${issues[@]}"; do
                printf "    *** %s\n" "$issue"
            done
            FAILED=1
        fi
    done <<< "$ROSTER"
fi

echo ""

# ---- Summary ----
if [ "$FAILED" = "0" ]; then
    echo "=== GATE: PASS — all bots are in clean reset state ==="
    exit 0
else
    echo "=== GATE: FAIL — bots need reset before soak ==="
    echo ""
    echo "Run the reset script:"
    echo "  bash tools/reset_idlebot_test_roster.sh"
    echo ""
    echo "Then restart the worldserver so the C++ state clears:"
    echo "  ssh khuong@10.10.30.20 'docker restart ac-worldserver'"
    echo ""
    echo "Then re-run this verifier."
    exit 1
fi
