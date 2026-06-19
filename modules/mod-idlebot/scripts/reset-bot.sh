#!/usr/bin/env bash
#
# reset-bot.sh — reset an idlebot back toward "nothing".
#
# Tiers (combine as needed):
#   (default)      controller only: clear guide + step + state + death counters
#                  in idlebot_bots so the bot goes idle and stops questing/dying.
#   --quests       also wipe the WoW character's quest log + completed quests.
#   --level1       also reset the character to level 1, xp 0 (see caveat below).
#   --full         = --quests --level1.
#   --restart      recreate ac-worldserver afterwards so in-memory state reloads.
#
# Character edits (--quests/--level1) require the character to be OFFLINE, so
# those tiers stop ac-worldserver first and you should pass --restart to bring
# it back. Caveat: --level1 resets level/xp only; it does NOT unlearn spells,
# talents, or skills — the playerbots randomizer re-gears the bot by level on
# its next cycle, which is good enough for a fresh leveling run.
#
# Runs on the zoidberg host (needs docker + the ac-database / ac-worldserver
# containers). Usage:
#   ./reset-bot.sh Idlebot
#   ./reset-bot.sh Idlebot --full --restart
#
set -euo pipefail

BOT="${1:-}"
if [[ -z "$BOT" || "$BOT" == --* ]]; then
    echo "usage: $0 <BotName> [--quests] [--level1] [--full] [--restart]" >&2
    exit 1
fi
shift || true

DO_QUESTS=0
DO_LEVEL1=0
DO_RESTART=0
for arg in "$@"; do
    case "$arg" in
        --quests)  DO_QUESTS=1 ;;
        --level1)  DO_LEVEL1=1 ;;
        --full)    DO_QUESTS=1; DO_LEVEL1=1 ;;
        --restart) DO_RESTART=1 ;;
        *) echo "unknown flag: $arg" >&2; exit 1 ;;
    esac
done

DB_CONTAINER="${IDLEBOT_DB_CONTAINER:-ac-database}"
WORLD_CONTAINER="${IDLEBOT_WORLD_CONTAINER:-ac-worldserver}"
COMPOSE_DIR="${IDLEBOT_COMPOSE_DIR:-/home/khuong/azerothcore-wotlk}"

PW="$(docker inspect "$DB_CONTAINER" --format '{{range .Config.Env}}{{println .}}{{end}}' \
        | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2-)"
if [[ -z "$PW" ]]; then
    echo "could not read MYSQL_ROOT_PASSWORD from $DB_CONTAINER" >&2
    exit 1
fi

sql() { docker exec -i "$DB_CONTAINER" mysql -uroot -p"$PW" -N -e "$1" 2>/dev/null; }

# Resolve the character GUID for this bot name (case-insensitive).
GUID="$(sql "SELECT guid FROM acore_characters.characters WHERE name='${BOT}' LIMIT 1;")"
if [[ -z "$GUID" ]]; then
    echo "no character named '${BOT}' found" >&2
    exit 1
fi
echo "bot '${BOT}' -> character guid ${GUID}"

# The worldserver must be offline for ANY edit: a running server holds the
# bot's guide/char state in memory and would overwrite our DB writes on its
# next persist/save. So always stop it, edit, then bring it back.
echo "stopping ${WORLD_CONTAINER} (it would otherwise overwrite our edits)…"
docker stop "$WORLD_CONTAINER" >/dev/null
WORLD_STOPPED=1

# --- controller reset (always) ---
echo "resetting idlebot controller state…"
sql "UPDATE acore_characters.idlebot_bots
        SET guide_id=NULL, step_index=0, step_state='idle',
            death_count_total=0, death_count_current_step=0
      WHERE character_guid=${GUID} OR bot_name='${BOT}';"

if [[ "$DO_QUESTS" -eq 1 ]]; then
    echo "wiping quest log + completed quests…"
    for t in character_queststatus character_queststatus_rewarded \
             character_queststatus_daily character_queststatus_weekly \
             character_queststatus_monthly character_queststatus_seasonal; do
        sql "DELETE FROM acore_characters.${t} WHERE guid=${GUID};" || true
    done
fi

if [[ "$DO_LEVEL1" -eq 1 ]]; then
    echo "resetting level -> 1, xp -> 0…"
    sql "UPDATE acore_characters.characters SET level=1, xp=0 WHERE guid=${GUID};"
fi

if [[ "$DO_RESTART" -eq 1 ]]; then
    echo "recreating ${WORLD_CONTAINER}…"
    ( cd "$COMPOSE_DIR" && docker compose up -d --no-deps --force-recreate "$WORLD_CONTAINER" >/dev/null )
else
    docker start "$WORLD_CONTAINER" >/dev/null
    echo "started ${WORLD_CONTAINER}."
fi

echo "done."
