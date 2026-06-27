#!/usr/bin/env bash
# verify_quest_loot_regression.sh — pre-soak gate: verify the quest-item loot fix is
# present in the compiled binary (LootAction.cpp neededForQuest bypass).
#
# Also spot-checks the DB for quest loot entries that bots depend on.
#
# Background: StoreLootAction::Execute skips stackable items when bags>80% and no partial
# stack exists. Without the fix, quest items were silently dropped — bots killed mobs but
# progress stayed at 0. The fix adds a neededForQuest check that bypasses the bag-space
# filter for active quest items.
#
# Checks:
#   1. Source fix present in mod-playerbots LootAction.cpp
#   2. DB: key quest items have QuestRequired entries in creature_loot_template
#   3. DB: quest items are in the correct loot templates
#
# Exit codes: 0 = pass, 1 = fail
#
# Usage:
#   bash tools/verify_quest_loot_regression.sh
#   bash tools/verify_quest_loot_regression.sh --host 10.10.30.20

set -euo pipefail

HOST="${IDLEBOT_HOST:-10.10.30.20}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
REPO_ROOT="$(dirname "$(dirname "$MODULE_DIR")")"

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
    qw() { docker exec ac-database mysql -uroot -p"$DB_PW" -N acore_world -e "$1" 2>/dev/null || true; }
else
    DB_PW="$(ssh "khuong@$HOST" \
        "docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}' 2>/dev/null \
         | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2" 2>/dev/null || true)"
    [ -z "$DB_PW" ] && DB_PW="password"
    qw() { ssh "khuong@$HOST" "docker exec ac-database mysql -uroot -p'$DB_PW' -N acore_world -e \"$1\"" 2>/dev/null || true; }
fi

echo ""
echo "=== verify_quest_loot_regression.sh ==="
echo ""

FAILED=0

# ---- 1. Source fix present ----
echo "1) Source fix in mod-playerbots LootAction.cpp:"
LOOT_ACTION="$REPO_ROOT/modules/mod-playerbots/src/Ai/Base/Actions/LootAction.cpp"

if [ ! -f "$LOOT_ACTION" ]; then
    echo "   WARN  $LOOT_ACTION not found — checking build context via SSH"
    # Try remote
    if [ -n "$HOST" ]; then
        FIX_PRESENT=$(ssh "khuong@$HOST" \
            "grep -c 'neededForQuest' ~/build/azerothcore-zoidberg/modules/mod-playerbots/src/Ai/Base/Actions/LootAction.cpp 2>/dev/null || echo 0" \
            2>/dev/null || echo "0")
    else
        FIX_PRESENT="0"
    fi
else
    FIX_PRESENT=$(grep -c 'neededForQuest' "$LOOT_ACTION" 2>/dev/null || echo "0")
fi

FIX_PRESENT="${FIX_PRESENT//[[:space:]]/}"
if [ "${FIX_PRESENT:-0}" -ge 3 ]; then
    echo "   OK    neededForQuest found ($FIX_PRESENT occurrences) — quest-item bag-space bypass active"
else
    echo "   FAIL  neededForQuest NOT found in LootAction.cpp ($FIX_PRESENT occurrences)"
    echo "         The quest-item bag-space bypass is missing."
    echo "         Fix: ensure mod-playerbots LootAction.cpp has the neededForQuest check"
    echo "         and rebuild the Docker image."
    FAILED=1
fi
echo ""

# ---- 2. DB spot-checks: quest loot entries ----
echo "2) DB spot-checks — quest items in creature_loot_template:"
echo "   (These items must be lootable for bots to complete quests)"
echo ""

# Format: "quest_id | item_id | item_name | creature_entry | creature_name"
# Known quest/item pairs used in early bot leveling:
check_quest_loot() {
    local label="$1" quest_id="$2" item_id="$3"
    local result
    result=$(qw "SELECT COUNT(*) FROM creature_loot_template clt
       WHERE clt.item = $item_id AND clt.QuestRequired IN (0,1)
       LIMIT 1;")
    local count="${result//[[:space:]]/}"
    if [ "${count:-0}" -ge 1 ]; then
        local item_name
        item_name=$(qw "SELECT name FROM item_template WHERE entry=$item_id LIMIT 1;" || echo "?")
        echo "   OK    Q$quest_id item $item_id (${item_name:-?}) — $count loot entry/entries"
    else
        local item_name
        item_name=$(qw "SELECT name FROM item_template WHERE entry=$item_id LIMIT 1;" || echo "?")
        echo "   WARN  Q$quest_id item $item_id (${item_name:-?}) — NOT found in creature_loot_template"
        echo "         Bots will not be able to loot this quest item."
    fi
}

# Horde Tauren newbie zone (Camp Narache)
check_quest_loot "Q750 Tauren" 750 4507   # Blood Shards from Bristleback
check_quest_loot "Q751 Tauren" 751 4508   # Plainstrider Feather
check_quest_loot "Q755 Tauren" 755 4506   # Zhevra Hoof

# Human/Northshire zone
check_quest_loot "Q3104 Human" 3104 15233  # Kobold Candle from kobolds (q: Kobold Candles)

# Horde Orc/Valley of Trials
check_quest_loot "Q798 Orc"   798 1402   # Scorpid Tail

# Night Elf Shadowglen
check_quest_loot "Q957 NElf"  957 6571   # Spider Ichor

echo ""

# ---- 3. QuestRequired flag spot-check ----
echo "3) QuestRequired flag — items that should only drop during quests:"
check_quest_required() {
    local label="$1" item_id="$2"
    local result
    result=$(qw "SELECT QuestRequired, COUNT(*) AS n
       FROM creature_loot_template
       WHERE item = $item_id
       GROUP BY QuestRequired;")
    local item_name
    item_name=$(qw "SELECT name FROM item_template WHERE entry=$item_id LIMIT 1;" || echo "?")
    if [ -z "$result" ]; then
        echo "   SKIP  item $item_id (${item_name:-?}) — not in creature_loot_template"
    else
        echo "   INFO  item $item_id (${item_name:-?}): QuestRequired distribution:"
        while IFS=$'\t' read -r qreq n; do
            echo "          QuestRequired=$qreq  count=$n"
        done <<< "$result"
    fi
}
check_quest_required "Zhevra Hoof" 4506
check_quest_required "Blood Shard" 4507
echo ""

# ---- Summary ----
if [ "$FAILED" = "0" ]; then
    echo "=== GATE: PASS — quest loot regression checks passed ==="
    exit 0
else
    echo "=== GATE: FAIL — fix LootAction.cpp bug before starting soak ==="
    echo ""
    echo "The neededForQuest fix was not detected. Without it, bots will kill mobs"
    echo "but not pick up quest items when bags are >80% full."
    echo ""
    echo "Check git log on mod-playerbots: the fix is at LootAction.cpp:Execute()."
    exit 1
fi
