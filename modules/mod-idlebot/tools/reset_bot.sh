#!/usr/bin/env bash
# reset_bot.sh — reset an idlebot character to a fresh level-1 start for testing.
#
# Run ON zoidberg (needs docker + the running stack). Stops the worldserver first
# (so it SAVES + logs the bot out — otherwise the logout-save clobbers the reset),
# wipes the bot's quests + level/xp/money, repositions it to the guide start, and
# rewinds idlebot to step 1 (keeping the assigned guide so it re-runs).
#
# Usage:
#   ./reset_bot.sh [BotName] [startX] [startY] [startZ] [map]
# Defaults: Idlebot at Executor Sarvis, Deathknell (horde-1-12-tirisfal-glades start).
#
# Examples:
#   ./reset_bot.sh                 # reset Idlebot to Deathknell start
#   ./reset_bot.sh Idlebot 1843.32 1639.9 97.8 0
set -euo pipefail

BOT="${1:-Idlebot}"
SX="${2:-1843.32}"; SY="${3:-1639.9}"; SZ="${4:-97.8}"; SMAP="${5:-0}"

PROJ="zoidberg-stack"
COMPOSE_FILE="$HOME/homelab/compose/zoidberg/compose.yml"
ENV_SHARED="$HOME/secrets/shared.env"
ENV_HOST="$HOME/secrets/zoidberg.env"
C=(docker compose -p "$PROJ" --env-file "$ENV_SHARED" --env-file "$ENV_HOST" -f "$COMPOSE_FILE")

PW="$(docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}' \
      | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2)"
[ -n "$PW" ] || { echo "ERROR: could not read DB root password from ac-database"; exit 1; }

echo "==> Resetting bot '$BOT' to level 1 @ ($SX, $SY, $SZ) map $SMAP"

echo "1) stopping worldserver (saves + logs the bot out)"
"${C[@]}" stop -t 40 ac-worldserver

echo "2) resetting character + idlebot rows"
docker exec -i ac-database mysql -uroot -p"$PW" acore_characters <<SQL
DELETE qs FROM character_queststatus qs JOIN characters c ON qs.guid=c.guid WHERE c.name='$BOT';
DELETE qr FROM character_queststatus_rewarded qr JOIN characters c ON qr.guid=c.guid WHERE c.name='$BOT';
UPDATE characters SET level=1, xp=0, money=0,
       position_x=$SX, position_y=$SY, position_z=$SZ, map=$SMAP, orientation=0
 WHERE name='$BOT';
UPDATE idlebot_bots SET step_index=0, step_state='idle',
       death_count_total=0, death_count_current_step=0
 WHERE bot_name='$BOT';
SQL

echo "   verify:"
docker exec ac-database mysql -uroot -p"$PW" -N -e \
  "SELECT CONCAT('   level=',level,' xp=',xp,' money=',money,
                 ' quests=',(SELECT COUNT(*) FROM acore_characters.character_queststatus q WHERE q.guid=c.guid),
                 ' rewarded=',(SELECT COUNT(*) FROM acore_characters.character_queststatus_rewarded r WHERE r.guid=c.guid),
                 ' step=',(SELECT step_index FROM acore_characters.idlebot_bots WHERE bot_name='$BOT'))
     FROM acore_characters.characters c WHERE name='$BOT';"

echo "3) starting worldserver"
"${C[@]}" up -d ac-worldserver

echo "==> Done. '$BOT' will resume its assigned guide from step 1 after it logs back in."
echo "    Watch it:  docker logs -f ac-worldserver 2>&1 | grep --line-buffered 'IdleBot]\\[dbg\\]'"
