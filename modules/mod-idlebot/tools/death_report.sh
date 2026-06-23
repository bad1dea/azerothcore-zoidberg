#!/bin/bash
# death_report.sh — show quest-by-quest progression with death counts
# Usage: ./death_report.sh [bot_name] [ssh_host]

BOT="${1:-Idleshaman}"
HOST="${2:-10.10.30.20}"
PASS="36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"

echo ""
echo "═══════════════════════════════════════════════════════════════════"
echo "  DEATH REPORT: $BOT  $(date '+%Y-%m-%d %H:%M:%S')"
echo "═══════════════════════════════════════════════════════════════════"
echo ""

printf "  %-6s %-8s %-50s %s\n" "Time" "Type" "Detail" ""
echo "  ────── ──────── ────────────────────────────────────────────────── "

ssh "khuong@${HOST}" "docker exec ac-database mysql -u root -p${PASS} -N -e \"
SELECT DATE_FORMAT(e.created_at, '%H:%i'), e.event_type, LEFT(e.detail, 65)
FROM acore_characters.idlebot_events e
WHERE e.bot_id = (SELECT id FROM acore_characters.idlebot_bots WHERE bot_name = '${BOT}')
ORDER BY e.id;
\"" 2>/dev/null | while IFS=$'\t' read -r ts etype detail; do
  # Color deaths red, levels green, quests blue
  case "$etype" in
    DEATH)   printf "  %-6s \033[31m%-8s\033[0m %s\n" "$ts" "$etype" "$detail" ;;
    LEVEL)   printf "  %-6s \033[32m%-8s\033[0m %s\n" "$ts" "$etype" "$detail" ;;
    FAILURE) printf "  %-6s \033[33m%-8s\033[0m %s\n" "$ts" "$etype" "$detail" ;;
    QUEST)   printf "  %-6s \033[36m%-8s\033[0m %s\n" "$ts" "$etype" "$detail" ;;
    COMBAT)  printf "  %-6s \033[35m%-8s\033[0m %s\n" "$ts" "$etype" "$detail" ;;
    *)       printf "  %-6s %-8s %s\n" "$ts" "$etype" "$detail" ;;
  esac
done

echo ""

# Summary
ssh "khuong@${HOST}" "docker exec ac-database mysql -u root -p${PASS} -N -e \"
SELECT
  (SELECT c.level FROM acore_characters.characters c WHERE c.name = '${BOT}') as level,
  (SELECT b.step_index FROM acore_characters.idlebot_bots b WHERE b.bot_name = '${BOT}') as step,
  (SELECT COUNT(*) FROM acore_characters.idlebot_events e WHERE e.bot_id = (SELECT id FROM acore_characters.idlebot_bots WHERE bot_name = '${BOT}') AND e.event_type = 'DEATH') as deaths,
  (SELECT COUNT(*) FROM acore_characters.idlebot_events e WHERE e.bot_id = (SELECT id FROM acore_characters.idlebot_bots WHERE bot_name = '${BOT}') AND e.event_type = 'FAILURE') as skips,
  (SELECT COUNT(*) FROM acore_characters.character_queststatus_rewarded r WHERE r.guid = (SELECT guid FROM acore_characters.characters WHERE name = '${BOT}')) as quests_done;
\"" 2>/dev/null | while IFS=$'\t' read -r level step deaths skips qdone; do
  echo "  Summary: L${level} step ${step} | ${deaths} deaths | ${skips} skips | ${qdone} quests done"
done

echo ""
echo "═══════════════════════════════════════════════════════════════════"
