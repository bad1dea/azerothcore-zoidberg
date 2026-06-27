#!/usr/bin/env bash
# reset_idlebot_test_roster.sh — reset all idlebot test bots to level 1 in their
# race-appropriate starting zones and rewind guide progress to step 0.
#
# This is a DESTRUCTIVE operation: it wipes level, XP, quests, spells, and
# inventory to the race/class baseline. The bot's account/identity/guide
# assignment is preserved so it re-levels from scratch with the correct guide.
#
# Run ON zoidberg (SSH or local). Requires docker + the running stack.
#
# Usage:
#   ./reset_idlebot_test_roster.sh            # reset all registered idlebots
#   ./reset_idlebot_test_roster.sh Idledruid  # reset one bot by name
#   ./reset_idlebot_test_roster.sh --dry-run  # preview SQL, no changes
#
# After running:
#   - worldserver restarts automatically
#   - each bot reappears at level 1 in its starting zone
#   - check dashboard or: docker logs -f ac-worldserver 2>&1 | grep 'IdleBot'

set -euo pipefail

DRY_RUN=0
SINGLE_BOT=""

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        *)         SINGLE_BOT="$arg" ;;
    esac
done

# ---- Docker / compose setup ----
PROJ="zoidberg-stack"
COMPOSE_FILE="$HOME/homelab/compose/zoidberg/compose.yml"
ENV_SHARED="$HOME/secrets/shared.env"
ENV_HOST="$HOME/secrets/zoidberg.env"

compose_exists=0
if [ -f "$COMPOSE_FILE" ] && [ -f "$ENV_SHARED" ] && [ -f "$ENV_HOST" ]; then
    compose_exists=1
fi

run_sql() {
    # run_sql <db> <sql>
    local db="$1"; local sql="$2"
    if [ "$DRY_RUN" = "1" ]; then
        echo "[DRY-RUN] SQL on $db:"
        echo "$sql"
        return
    fi
    docker exec -i ac-database mysql -uroot -p"$DB_PW" "$db" -e "$sql"
}

run_sql_file() {
    # run_sql_file <db> (reads from stdin heredoc)
    local db="$1"
    if [ "$DRY_RUN" = "1" ]; then
        echo "[DRY-RUN] SQL on $db: (heredoc — run without --dry-run to see)"
        cat
        return
    fi
    docker exec -i ac-database mysql -uroot -p"$DB_PW" "$db"
}

# ---- Bot roster: name → (race, class, map, x, y, z, guide_id) ----
# Race IDs: HUMAN=1 ORC=2 DWARF=3 NIGHTELF=4 UNDEAD=5 TAUREN=6 GNOME=7
#           TROLL=8 BLOODELF=10 DRAENEI=11
# Class IDs: WARRIOR=1 PALADIN=2 HUNTER=3 ROGUE=4 PRIEST=5 SHAMAN=7
#            MAGE=8 WARLOCK=9 DRUID=11
# Positions: from playercreateinfo in acore_world (verified 2026-06-25)
declare -A BOT_RACE BOT_CLASS BOT_MAP BOT_X BOT_Y BOT_Z BOT_GUIDE

define_bot() {
    local name="$1" race="$2" class="$3" map="$4" x="$5" y="$6" z="$7" guide="$8"
    BOT_RACE["$name"]="$race"
    BOT_CLASS["$name"]="$class"
    BOT_MAP["$name"]="$map"
    BOT_X["$name"]="$x"
    BOT_Y["$name"]="$y"
    BOT_Z["$name"]="$z"
    BOT_GUIDE["$name"]="$guide"
}

# Night Elf Druid: Shadowglen in Teldrassil (map 1)
define_bot "Idledruid"   4 11  1  10311.3   832.463  1326.41  "nightelf-shadowglen-1-6"
# Dwarf Hunter: Coldridge Valley in Dun Morogh (map 0)
define_bot "Idlehunter"  3  3  0  -6240.32   331.033   382.758 "dwarf-coldridge-1-6"
# Undead Mage: Deathknell in Tirisfal Glades (map 0)
define_bot "Idlemage"    5  8  0   1676.71  1678.31   121.67  "undead-deathknell-1-6"
# Dwarf Paladin: Coldridge Valley in Dun Morogh (map 0)
define_bot "Idlepaladin" 3  2  0  -6240.32   331.033   382.758 "dwarf-coldridge-1-6"
# Human Priest: Northshire Abbey in Elwynn Forest (map 0)
define_bot "Idlepriest"  1  5  0  -8949.95  -132.493    83.531 "human-northshire-1-6"
# Undead Rogue: Deathknell in Tirisfal Glades (map 0)
define_bot "Idlerogue"   5  4  0   1676.71  1678.31   121.67  "undead-deathknell-1-6"
# Tauren Shaman: Camp Narache in Mulgore (map 1)
define_bot "Idleshaman"  6  7  1  -2917.58  -257.98    52.997 "tauren-camp_narache-1-6"
# Gnome Warlock: Coldridge Valley in Dun Morogh (map 0, gnomes start near dwarves)
define_bot "Idlewarlock" 7  9  0  -6240.0    331.0     383.0  "gnome-gnomeregan-1-6"
# Orc Warrior: Valley of Trials in Durotar (map 1)
define_bot "Idlewarrior" 2  1  1   -618.518 -4251.67    38.718 "orc-valley_of_trials-1-6"

# ---- Determine which bots to reset ----
if [ -n "$SINGLE_BOT" ]; then
    if [ -z "${BOT_RACE[$SINGLE_BOT]+x}" ]; then
        echo "ERROR: unknown bot '$SINGLE_BOT'. Known bots:"
        for name in "${!BOT_RACE[@]}"; do printf "  %s\n" "$name"; done
        exit 1
    fi
    BOTS=("$SINGLE_BOT")
else
    BOTS=("${!BOT_RACE[@]}")
fi

echo "=== IdleBot Test Roster Reset ==="
[ "$DRY_RUN" = "1" ] && echo "    *** DRY RUN — no changes will be made ***"
echo "Bots to reset: ${BOTS[*]}"
echo ""

# ---- Get DB password ----
DB_PW=""
if [ "$DRY_RUN" = "0" ]; then
    DB_PW="$(docker inspect ac-database \
        --format '{{range .Config.Env}}{{println .}}{{end}}' \
        | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2 || true)"
    if [ -z "$DB_PW" ]; then
        # fallback: try environment
        DB_PW="${MYSQL_ROOT_PASSWORD:-}"
    fi
    if [ -z "$DB_PW" ]; then
        echo "ERROR: could not read DB password from ac-database container."
        echo "Set MYSQL_ROOT_PASSWORD env var or run as a user with docker access."
        exit 1
    fi
fi

# ---- Stop worldserver so logout-save doesn't clobber the reset ----
if [ "$DRY_RUN" = "0" ] && [ "$compose_exists" = "1" ]; then
    echo "1) Stopping worldserver (saves and logs bots out cleanly)..."
    docker compose -p "$PROJ" \
        --env-file "$ENV_SHARED" --env-file "$ENV_HOST" \
        -f "$COMPOSE_FILE" stop -t 40 ac-worldserver 2>/dev/null || true
    sleep 2
elif [ "$DRY_RUN" = "0" ]; then
    echo "1) Stopping ac-worldserver container..."
    docker stop -t 40 ac-worldserver 2>/dev/null || true
    sleep 2
else
    echo "1) [DRY-RUN] Would stop worldserver."
fi

# ---- Reset each bot ----
echo ""
echo "2) Resetting bot DB state..."
for BOT in "${BOTS[@]}"; do
    race="${BOT_RACE[$BOT]}"
    cls="${BOT_CLASS[$BOT]}"
    map="${BOT_MAP[$BOT]}"
    sx="${BOT_X[$BOT]}"
    sy="${BOT_Y[$BOT]}"
    sz="${BOT_Z[$BOT]}"
    guide="${BOT_GUIDE[$BOT]}"

    echo "   Resetting $BOT (race=$race class=$cls) → $guide @ map=$map ($sx,$sy,$sz)"

    if [ "$DRY_RUN" = "0" ]; then
        docker exec -i ac-database mysql -uroot -p"$DB_PW" acore_characters <<SQL
-- Wipe all inventory so bags start empty (items carry over across DB resets otherwise)
DELETE ii FROM item_instance ii
  INNER JOIN character_inventory ci ON ci.item = ii.guid
  INNER JOIN characters c ON c.guid = ci.guid AND c.name = '$BOT';
DELETE ci FROM character_inventory ci
  INNER JOIN characters c ON c.guid = ci.guid AND c.name = '$BOT';

-- Wipe quest log (active + rewarded) for this bot
DELETE qs
  FROM character_queststatus qs
  JOIN characters c ON qs.guid = c.guid
 WHERE c.name = '$BOT';

DELETE qr
  FROM character_queststatus_rewarded qr
  JOIN characters c ON qr.guid = c.guid
 WHERE c.name = '$BOT';

-- Reset character to level 1 at starting position.
-- Keep account/name/race/class. Money=copper starter amount (varies by race; 0 is safe).
UPDATE characters
   SET level        = 1,
       xp           = 0,
       money        = 0,
       position_x   = $sx,
       position_y   = $sy,
       position_z   = $sz,
       map          = $map,
       orientation  = 0,
       zone         = 0
 WHERE name = '$BOT';

-- Clear talent points and specs (they're auto-applied by idlebot on level-up)
DELETE ct
  FROM character_talent ct
  JOIN characters c ON ct.guid = c.guid
 WHERE c.name = '$BOT';

-- Reset idlebot guide progress to step 0, keep guide assignment
UPDATE idlebot_bots
   SET guide_id                    = '$guide',
       active                      = 1,
       step_index                  = 0,
       step_state                  = 'idle',
       death_count_total           = 0,
       death_count_current_step    = 0,
       last_trained_level          = 0,
       last_specced_level          = 0,
       soak_run_id                 = NULL,
       bot_session_id              = NULL,
       reset_id                    = 0,
       blocked_reason              = NULL,
       blocked_since               = NULL,
       last_failure_code           = NULL,
       requires_user_action        = 0
 WHERE bot_name = '$BOT';

-- Clear blacklisted quests so we don't inherit old skip-lists
DELETE FROM idlebot_blocked_quests
  WHERE bot_id = (SELECT id FROM idlebot_bots WHERE bot_name = '$BOT' LIMIT 1);

-- Wipe failure counters (fresh run, clean slate)
DELETE FROM idlebot_failures
  WHERE bot_id = (SELECT id FROM idlebot_bots WHERE bot_name = '$BOT' LIMIT 1);

-- Clear all events (QSkip/QDone visible in bot_status.sh count these)
DELETE FROM idlebot_events
  WHERE bot_id = (SELECT id FROM idlebot_bots WHERE bot_name = '$BOT' LIMIT 1);

-- Clear live state snapshot (stale position/step from prior run)
DELETE FROM idlebot_live_state
  WHERE bot_name = '$BOT';

-- Clear decision history
DELETE FROM idlebot_decision_history
  WHERE bot_id = (SELECT id FROM idlebot_bots WHERE bot_name = '$BOT' LIMIT 1);
SQL
    else
        echo "   [DRY-RUN] Would run reset SQL for $BOT"
    fi
done

# ---- Verify results ----
echo ""
echo "3) Verifying DB state..."
if [ "$DRY_RUN" = "0" ]; then
    for BOT in "${BOTS[@]}"; do
        docker exec ac-database mysql -uroot -p"$DB_PW" -N acore_characters -e "
SELECT CONCAT(
    '   $BOT: level=', c.level,
    ' xp=', c.xp,
    ' map=', c.map,
    ' x=', ROUND(c.position_x,0),
    ' guide=', COALESCE(b.guide_id,'none'),
    ' step=', COALESCE(b.step_index,0),
    ' quests=', (SELECT COUNT(*) FROM character_queststatus q WHERE q.guid=c.guid),
    ' rewarded=', (SELECT COUNT(*) FROM character_queststatus_rewarded r WHERE r.guid=c.guid)
) AS status
FROM characters c
LEFT JOIN idlebot_bots b ON b.bot_name=c.name
WHERE c.name='$BOT';" 2>/dev/null || echo "   $BOT: NOT FOUND in characters table"
    done
fi

# ---- Restart worldserver ----
echo ""
echo "4) Starting worldserver..."
if [ "$DRY_RUN" = "0" ] && [ "$compose_exists" = "1" ]; then
    docker compose -p "$PROJ" \
        --env-file "$ENV_SHARED" --env-file "$ENV_HOST" \
        -f "$COMPOSE_FILE" up -d ac-worldserver 2>/dev/null || true
elif [ "$DRY_RUN" = "0" ]; then
    docker start ac-worldserver 2>/dev/null || true
else
    echo "[DRY-RUN] Would restart ac-worldserver."
fi

echo ""
echo "=== Reset complete! ==="
echo ""
echo "Each bot will log in at level 1 in its starting zone with guide from step 0."
echo ""
echo "Monitor progress:"
echo "  docker logs -f ac-worldserver 2>&1 | grep --line-buffered 'IdleBot'"
echo "  python3 tools/audit_idlebot_runtime_state.py"
echo ""
echo "Run soak test:"
echo "  bash tools/run_idlebot_leveling_soak.sh --duration 4h"
