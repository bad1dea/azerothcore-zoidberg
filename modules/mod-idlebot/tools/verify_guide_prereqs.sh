#!/usr/bin/env bash
# verify_guide_prereqs.sh — pre-soak gate: check that quest prereq chains for the
# ACTIVE TEST ROSTER's guides match DB PrevQuestID fields.
#
# Only checks guides that the current idlebot_bots test roster is using.
# Ignores guides not assigned to any active bot.
#
# A guide step that accepts quest N implicitly assumes any PrevQuestID chains are
# satisfied. If the guide is missing prereq steps, the bot gets QUEST_ACCEPT_FAILED.
#
# Notes:
#   - Negative PrevQuestID means "exclusive" (player must NOT have done it) — skipped.
#   - We look for the prereq turn-in within the guide file itself, not across files.
#   - "Checked $N" = number of (guide, quest) pairs examined.
#
# Exit codes: 0 = pass (no gaps for active roster), 1 = gaps found
#
# Usage:
#   bash tools/verify_guide_prereqs.sh
#   bash tools/verify_guide_prereqs.sh --host 10.10.30.20
#   bash tools/verify_guide_prereqs.sh --guides /path/to/guides

set -euo pipefail

HOST="${IDLEBOT_HOST:-10.10.30.20}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
GUIDES_DIR="$MODULE_DIR/data/guides"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host)   HOST="$2"; shift 2 ;;
        --guides) GUIDES_DIR="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# ---- DB helpers ----
if docker info >/dev/null 2>&1; then
    DB_PW="$(docker inspect ac-database \
        --format '{{range .Config.Env}}{{println .}}{{end}}' 2>/dev/null \
        | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2 || true)"
    [ -z "$DB_PW" ] && DB_PW="password"
    qw() { docker exec ac-database mysql --user=root --password="$DB_PW" -N acore_world -e "$1" 2>/dev/null || true; }
    qc() { docker exec ac-database mysql --user=root --password="$DB_PW" -N acore_characters -e "$1" 2>/dev/null || true; }
else
    DB_PW="$(ssh "khuong@$HOST" \
        "docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}' 2>/dev/null \
         | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2" 2>/dev/null || true)"
    [ -z "$DB_PW" ] && DB_PW="password"
    qw() { ssh "khuong@$HOST" "docker exec ac-database mysql --user=root --password='$DB_PW' -N acore_world -e \"$1\"" 2>/dev/null || true; }
    qc() { ssh "khuong@$HOST" "docker exec ac-database mysql --user=root --password='$DB_PW' -N acore_characters -e \"$1\"" 2>/dev/null || true; }
fi

echo ""
echo "=== verify_guide_prereqs.sh ==="
echo "Guides dir: $GUIDES_DIR"
echo ""

if [ ! -d "$GUIDES_DIR" ]; then
    echo "ERROR: guides directory not found: $GUIDES_DIR" >&2
    exit 1
fi

# ---- Get active bot guides and their classes from DB ----
echo "Fetching active bot guide assignments from idlebot_bots..."
ACTIVE_GUIDES=$(qc "SELECT DISTINCT guide_id FROM idlebot_bots WHERE guide_id IS NOT NULL AND guide_id != '';" 2>/dev/null || true)

# Build map of guide_id → list of active bot class integers
# Character classes: 1=Warrior 2=Paladin 3=Hunter 4=Rogue 5=Priest 6=DK 7=Shaman 8=Mage 9=Warlock 11=Druid
ACTIVE_BOT_CLASSES=$(qc "SELECT b.guide_id, c.class FROM idlebot_bots b JOIN characters c ON c.name=b.bot_name WHERE b.guide_id IS NOT NULL;" 2>/dev/null || true)

if [ -z "$ACTIVE_GUIDES" ]; then
    echo "  WARNING: no active bots found in idlebot_bots — checking all guides"
    # Fall back to all guides if no bots registered
    GUIDE_FILES=$(find "$GUIDES_DIR" -name "*.yaml" | sort)
else
    echo "  Active guides:"
    GUIDE_FILES=""
    while IFS= read -r guide_id; do
        # Find the YAML file for this guide ID by searching for 'id: <guide_id>'
        match=$(grep -rl "^id: ${guide_id}$" "$GUIDES_DIR" 2>/dev/null | head -1 || true)
        if [ -n "$match" ]; then
            echo "    $guide_id → $match"
            GUIDE_FILES="$GUIDE_FILES $match"
        else
            echo "    $guide_id → NOT FOUND in $GUIDES_DIR"
        fi
    done <<< "$ACTIVE_GUIDES"
fi
echo ""

GAPS=0
CHECKED=0

for guide_file in $GUIDE_FILES; do
    guide_name="${guide_file#$GUIDES_DIR/}"

    # Collect unique quest IDs accepted in this guide (avoid duplicates)
    ACCEPT_IDS=$(grep -oP '(?<=quest_id: )\d+' "$guide_file" 2>/dev/null | sort -u || true)
    [ -z "$ACCEPT_IDS" ] && continue

    # Collect all quest IDs turned in this guide
    # Parse sequential blocks: when we see "type: turn_in_quest", the next quest_id is the turnin
    TURNIN_IDS=""
    IN_TURNIN=0
    while IFS= read -r line; do
        if echo "$line" | grep -qE 'type:\s*turn_in_quest'; then
            IN_TURNIN=1
        elif echo "$line" | grep -qE '^\s*type:'; then
            IN_TURNIN=0
        fi
        if [ "$IN_TURNIN" = "1" ] && echo "$line" | grep -qE '^\s*quest_id:'; then
            qid=$(echo "$line" | grep -oP '(?<=quest_id: )\d+')
            TURNIN_IDS="$TURNIN_IDS $qid "
        fi
    done < "$guide_file"

    while IFS= read -r qid; do
        [ -z "$qid" ] && continue
        CHECKED=$(( CHECKED + 1 ))

        PREV=$(qw "SELECT COALESCE(PrevQuestId, 0) FROM quest_template_addon WHERE id = $qid LIMIT 1;" || echo "")
        PREV="${PREV//[[:space:]]/}"
        [ -z "$PREV" ] && PREV="0"

        # Skip negative PrevQuestID — these are EXCLUSIVE prereqs (must NOT have done)
        if [[ "$PREV" =~ ^-[0-9]+$ ]]; then
            continue
        fi

        if [ "$PREV" != "0" ]; then
            # Check if the prereq is turned in within this guide
            if echo "$TURNIN_IDS" | grep -q " $PREV "; then
                : # prereq turnin is present in this guide — OK
            else
                PREV_NAME=$(qw "SELECT LogTitle FROM quest_template WHERE id = $PREV LIMIT 1;" || echo "?")
                QUEST_NAME=$(qw "SELECT LogTitle FROM quest_template WHERE id = $qid LIMIT 1;" || echo "?")

                # Check for class_mask restriction on this step — if restricted to a class
                # that no active bot uses, downgrade from FAIL to WARN.
                # Extract the class_mask that appears in the step block containing this quest_id.
                STEP_CLASS_MASK=$(awk "
                    /- id:/ { in_step=1; step_mask=\"\" }
                    in_step && /quest_id: $qid/ { found_q=1 }
                    in_step && /class_mask:/ { match(\$0, /[0-9]+/); step_mask=substr(\$0,RSTART,RLENGTH) }
                    found_q && step_mask { print step_mask; found_q=0; in_step=0 }
                    " "$guide_file" 2>/dev/null | head -1 || true)
                STEP_CLASS_MASK="${STEP_CLASS_MASK//[[:space:]]/}"

                IS_BLOCKED_FOR_ACTIVE=1
                if [ -n "$STEP_CLASS_MASK" ] && [ "$STEP_CLASS_MASK" != "0" ]; then
                    # Check if any active bot for this guide matches the class mask
                    GUIDE_ID=$(grep '^id:' "$guide_file" | head -1 | awk '{print $2}')
                    IS_BLOCKED_FOR_ACTIVE=0
                    while IFS=$'\t' read -r g_id b_class; do
                        if [ "$g_id" = "$GUIDE_ID" ]; then
                            class_bit=$(( 1 << (b_class - 1) ))
                            if (( STEP_CLASS_MASK & class_bit )); then
                                IS_BLOCKED_FOR_ACTIVE=1
                                break
                            fi
                        fi
                    done <<< "$ACTIVE_BOT_CLASSES"
                fi

                if [ "$IS_BLOCKED_FOR_ACTIVE" = "1" ]; then
                    echo "  FAIL  $guide_name"
                    echo "        Quest $qid (${QUEST_NAME:-?}) requires prereq q$PREV (${PREV_NAME:-?})"
                    echo "        Prereq q$PREV turn-in not found in this guide."
                    echo ""
                    GAPS=$(( GAPS + 1 ))
                else
                    echo "  WARN  $guide_name: q$qid prereq q$PREV missing, but class_mask=$STEP_CLASS_MASK (no active bot uses this class)"
                fi
            fi
        fi
    done <<< "$ACCEPT_IDS"
done

echo "Checked $CHECKED unique quest accept steps across active roster guides."
echo ""

if [ "$GAPS" = "0" ]; then
    echo "=== GATE: PASS — no missing prereq steps for active roster guides ==="
    exit 0
else
    echo "=== GATE: FAIL — $GAPS missing prereq chain(s) in active roster guides ==="
    echo ""
    echo "For each FAIL above, add the missing prereq steps to the guide:"
    echo "  accept_quest → <objective steps> → turn_in_quest"
    echo "for q<PREV>, placed BEFORE the quest that requires it."
    exit 1
fi
