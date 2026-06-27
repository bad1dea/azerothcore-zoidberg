#!/bin/bash
# bot_status.sh — dump all idlebot bot status in a readable table
# Usage: ./bot_status.sh [ssh_host]

set -euo pipefail

HOST="${1:-10.10.30.20}"
PASS="36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"

is_local_host() {
  case "$1" in
    ""|localhost|127.0.0.1) return 0 ;;
  esac

  local short
  short="$(hostname -s 2>/dev/null || true)"
  local full
  full="$(hostname -f 2>/dev/null || true)"
  [ "$1" = "$short" ] || [ "$1" = "$full" ]
}

q() {
  local sql="$1"
  if is_local_host "$HOST"; then
    docker exec ac-database mysql -u root -p"${PASS}" -N -e "$sql" 2>/dev/null
  else
    ssh "khuong@${HOST}" "docker exec ac-database mysql -u root -p${PASS} -N -e \"$sql\"" 2>/dev/null
  fi
}

echo ""
echo "═══════════════════════════════════════════════════════════════════════════════"
echo "  IDLEBOT STATUS REPORT  $(date '+%Y-%m-%d %H:%M:%S')"
echo "═══════════════════════════════════════════════════════════════════════════════"
echo ""

# Main status table
printf "  %-12s %3s %-7s %-6s %-8s %4s %5s %5s %5s %8s %3s %16s %s\n" \
  "Name" "Lvl" "Class" "Race" "State" "Step" "Death" "QDone" "Fail" "Money" "Map" "Position" "Bags"
echo "  ──────────── ─── ─────── ────── ──────── ──── ───── ───── ───── ──────── ─── ──────────────── ───────"

CLASSES=(- Warrior Paladin Hunter Rogue Priest DK Shaman Mage Warlock - Druid)
RACES=(- Human Orc Dwarf NElf Undead Tauren Gnome Troll - BElf Draen)

q "
SELECT
  c.name, c.level, c.class, c.race,
  COALESCE(ls.state, b.step_state, 'unknown') as bot_state,
  COALESCE(ls.step_index, b.step_index) as step_index,
  b.death_count_total,
  c.map, ROUND(c.position_x), ROUND(c.position_y), c.money, c.online,
  (SELECT COUNT(*) FROM acore_characters.character_queststatus_rewarded r WHERE r.guid = c.guid) as qdone,
  (SELECT COUNT(*) FROM acore_characters.idlebot_events e WHERE e.bot_id = b.id AND e.event_type = 'FAILURE') as fail_count,
  (SELECT CONCAT(
    COUNT(CASE WHEN ci2.bag = 0 AND ci2.slot >= 23 THEN 1 END) + COUNT(CASE WHEN ci2.bag != 0 THEN 1 END),
    '/',
    CASE WHEN EXISTS(SELECT 1 FROM acore_characters.character_inventory ci3 WHERE ci3.guid = c.guid AND ci3.bag = 0 AND ci3.slot BETWEEN 19 AND 22) THEN '112' ELSE '16' END
  ) FROM acore_characters.character_inventory ci2 WHERE ci2.guid = c.guid) as baginfo,
  COALESCE(ls.blocked_reason, b.blocked_reason, ''),
  TIMESTAMPDIFF(SECOND, COALESCE(ls.updated_at, UTC_TIMESTAMP()), UTC_TIMESTAMP()) as live_age_s
FROM acore_characters.idlebot_bots b
JOIN acore_characters.characters c ON c.name = b.bot_name
LEFT JOIN acore_characters.idlebot_live_state ls ON ls.bot_name = b.bot_name
ORDER BY b.bot_name
" | while IFS=$'\t' read -r name level class race state step deaths map px py money online qdone fail_count baginfo blocked_reason live_age_s; do
  classname="${CLASSES[$class]:-C$class}"
  racename="${RACES[$race]:-R$race}"
  state_disp="${state:-unknown}"
  if [ -n "${blocked_reason:-}" ] && [ "$blocked_reason" != "NULL" ]; then
    state_disp="blocked"
  elif [ "${live_age_s:-0}" -gt 120 ]; then
    state_disp="stale"
  fi

  gold=$((money / 10000))
  silver=$(((money % 10000) / 100))
  copper=$((money % 100))
  if [ "$gold" -gt 0 ]; then
    mfmt="${gold}g${silver}s${copper}c"
  elif [ "$silver" -gt 0 ]; then
    mfmt="${silver}s${copper}c"
  else
    mfmt="${copper}c"
  fi

  [ "$online" = "1" ] && on="●" || on="○"

  # Parse bag used/total
  used="${baginfo%%/*}"
  total="${baginfo##*/}"
  free=$((total - used))

  printf "  %s%-11s %3s %-7s %-6s %-8s %4s %5s %5s %5s %8s %3s %7s,%-7s %s/%s\n" \
    "$on" "$name" "$level" "$classname" "$racename" "$state_disp" "$step" "$deaths" "$qdone" "${fail_count:-0}" "$mfmt" "$map" "$px" "$py" "$used" "$total"
done

echo ""
echo "  EQUIPPED GEAR"
echo "  ──────────────────────────────────────────────────────────────────────────────"

SLOTS=(Head Neck Shoulder Shirt Chest Waist Legs Feet Wrists Hands Ring1 Ring2 Trink1 Trink2 Back MH OH Ranged Tabard)

q "
SELECT c.name, ci.slot, it.name as iname, it.ItemLevel,
  CASE it.subclass WHEN 1 THEN 'Cloth' WHEN 2 THEN 'Leath' WHEN 3 THEN 'Mail' WHEN 4 THEN 'Plate' WHEN 6 THEN 'Shield' ELSE '' END as atype
FROM acore_characters.character_inventory ci
JOIN acore_characters.item_instance ii ON ii.guid = ci.item
JOIN acore_world.item_template it ON it.entry = ii.itemEntry
JOIN acore_characters.characters c ON c.guid = ci.guid
WHERE c.name IN (SELECT bot_name FROM acore_characters.idlebot_bots)
AND ci.bag = 0 AND ci.slot < 19 AND ci.slot != 3
ORDER BY c.name, ci.slot
" | {
  last=""
  while IFS=$'\t' read -r name slot iname ilvl atype; do
    if [ "$name" != "$last" ]; then
      [ -n "$last" ] && echo ""
      echo "  $name:"
      last="$name"
    fi
    slotname="${SLOTS[$slot]:-S$slot}"
    if [ -n "$atype" ]; then
      printf "    %-9s iL%-3s %-6s %s\n" "$slotname" "$ilvl" "[$atype]" "$iname"
    else
      printf "    %-9s iL%-3s        %s\n" "$slotname" "$ilvl" "$iname"
    fi
  done
  echo ""
}

echo "  RECENT EVENTS"
echo "  ──────────────────────────────────────────────────────────────────────────────"

q "
SELECT b.bot_name, e.event_type, LEFT(e.detail, 55), DATE_FORMAT(e.created_at, '%H:%i:%s')
FROM acore_characters.idlebot_events e
JOIN acore_characters.idlebot_bots b ON b.id = e.bot_id
ORDER BY e.id DESC LIMIT 15
" | while IFS=$'\t' read -r name etype detail ts; do
  printf "  %-12s %-8s %-55s %s\n" "$name" "$etype" "$detail" "$ts"
done

echo ""
echo "═══════════════════════════════════════════════════════════════════════════════"
