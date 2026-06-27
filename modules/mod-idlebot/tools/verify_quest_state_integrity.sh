#!/usr/bin/env bash
# verify_quest_state_integrity.sh — pre-soak gate: detect corrupt quest state.
#
# Fails loudly if any test bot has:
#   - the same quest in both character_queststatus AND character_queststatus_rewarded
#   - any active (non-zero status) quest rows left after a clean reset
#
# Usage:
#   bash modules/mod-idlebot/tools/verify_quest_state_integrity.sh [--bots "Name1,Name2,..."]
#
# Run on zoidberg (build host) or anywhere docker exec ac-database is available.
# Exits 0 = PASS, 1 = FAIL.

set -uo pipefail

BOTS="${1:-Idledruid,Idlehunter,Idlemage,Idlepaladin,Idlepriest,Idlerogue,Idleshaman,Idlewarlock,Idlewarrior}"

# Allow override from flag
if [[ "${1:-}" == "--bots" ]]; then
    BOTS="${2:-$BOTS}"
fi

echo "=== Quest State Integrity Check ==="
echo "Bots: $BOTS"
echo ""

# Get DB password from running container
DB_PW="$(docker inspect ac-database \
    --format '{{range .Config.Env}}{{println .}}{{end}}' 2>/dev/null \
    | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2 || true)"
if [[ -z "$DB_PW" ]]; then
    DB_PW="${MYSQL_ROOT_PASSWORD:-}"
fi
if [[ -z "$DB_PW" ]]; then
    echo "ERROR: cannot read DB password from ac-database container." >&2
    exit 1
fi

q() {
    docker exec ac-database mysql -uroot -p"$DB_PW" -N acore_characters -e "$1" 2>/dev/null || true
}

# Build SQL IN list from comma-separated bot names
BOT_SQL_LIST=$(echo "$BOTS" | tr ',' '\n' | sed "s/^/'/;s/$/'/" | tr '\n' ',' | sed 's/,$//')

FAILURES=0

# -----------------------------------------------------------------
# Check 1: quest in both active and rewarded tables simultaneously
# This should NEVER happen for non-repeatable quests.
# -----------------------------------------------------------------
echo "Check 1: quest in both queststatus AND queststatus_rewarded..."

DUAL=$(q "
SELECT c.name, qs.quest, qs.status, qs.mobcount1, qs.mobcount2
FROM characters c
JOIN character_queststatus qs ON qs.guid = c.guid
JOIN character_queststatus_rewarded qr ON qr.guid = c.guid AND qr.quest = qs.quest
WHERE c.name IN ($BOT_SQL_LIST)
ORDER BY c.name, qs.quest;
")

if [[ -n "$DUAL" ]]; then
    echo ""
    echo "QUEST STATE INTEGRITY FAILED — quest in both active and rewarded tables:"
    echo ""
    while IFS=$'\t' read -r bot quest status mob1 mob2; do
        echo "  FAIL: $bot has quest $quest in both character_queststatus (status=$status, mob1=$mob1) and character_queststatus_rewarded"
    done <<< "$DUAL"
    echo ""
    echo "Root cause: worldserver saved quest as rewarded but did not remove the InProgress row."
    echo "Fix: DELETE from character_queststatus for the affected bot+quest while worldserver is stopped."
    echo "     Then run reset_idlebot_test_roster.sh to ensure a clean state before soak."
    FAILURES=$((FAILURES + 1))
else
    echo "  PASS — no quests appear in both tables"
fi

# -----------------------------------------------------------------
# Check 2: any active quest rows exist at all (after a clean reset, should be 0)
# -----------------------------------------------------------------
echo ""
echo "Check 2: active quest rows (should be 0 after reset)..."

ACTIVE=$(q "
SELECT c.name, COUNT(*) AS active_quests
FROM characters c
JOIN character_queststatus qs ON qs.guid = c.guid
WHERE c.name IN ($BOT_SQL_LIST) AND qs.status > 0
GROUP BY c.name
HAVING active_quests > 0
ORDER BY c.name;
")

if [[ -n "$ACTIVE" ]]; then
    echo ""
    echo "QUEST STATE INTEGRITY FAILED — bots have active quests after reset:"
    while IFS=$'\t' read -r bot count; do
        echo "  FAIL: $bot has $count active quest(s) — reset may not have completed"
    done <<< "$ACTIVE"
    echo ""
    echo "Fix: run reset_idlebot_test_roster.sh and verify all quests are cleared."
    FAILURES=$((FAILURES + 1))
else
    echo "  PASS — no active quest rows"
fi

# -----------------------------------------------------------------
# Check 3: rewarded quest rows exist (after clean reset, should be 0)
# -----------------------------------------------------------------
echo ""
echo "Check 3: rewarded quest rows (should be 0 after reset)..."

REWARDED=$(q "
SELECT c.name, COUNT(*) AS rewarded_quests
FROM characters c
JOIN character_queststatus_rewarded qr ON qr.guid = c.guid
WHERE c.name IN ($BOT_SQL_LIST)
GROUP BY c.name
HAVING rewarded_quests > 0
ORDER BY c.name;
")

if [[ -n "$REWARDED" ]]; then
    echo ""
    echo "QUEST STATE INTEGRITY FAILED — bots have rewarded quest history after reset:"
    while IFS=$'\t' read -r bot count; do
        echo "  FAIL: $bot has $count rewarded quest(s) — character_queststatus_rewarded not cleared"
    done <<< "$REWARDED"
    echo ""
    echo "Fix: DELETE FROM character_queststatus_rewarded for these bots."
    echo "     reset_idlebot_test_roster.sh should do this automatically — check it ran fully."
    FAILURES=$((FAILURES + 1))
else
    echo "  PASS — no rewarded quest rows"
fi

# -----------------------------------------------------------------
# Summary
# -----------------------------------------------------------------
echo ""
echo "=== Quest State Integrity Summary ==="
if [[ "$FAILURES" -eq 0 ]]; then
    echo "GATE: PASS — all checks clean"
    exit 0
else
    echo "GATE: FAIL — $FAILURES check(s) failed"
    echo ""
    echo "Do NOT start soak until quest state is clean."
    echo "Corrective action:"
    echo "  1. Stop worldserver: docker stop ac-worldserver"
    echo "  2. Run reset:        bash modules/mod-idlebot/tools/reset_idlebot_test_roster.sh"
    echo "  3. Re-run this gate: bash modules/mod-idlebot/tools/verify_quest_state_integrity.sh"
    echo "  4. Start worldserver and then start soak"
    exit 1
fi
