#!/usr/bin/env bash
# regen_guides.sh — regenerate ALL 10 Zygor 1-80 mega-guides from the committed
# leveling parses + the live acore_world DB. Repeatable end-to-end pipeline:
#   parse (committed) -> chain_route.py -> gen_dbguide.py -> data/guides/*.yaml
#
# MUST run on the DB host (zoidberg): gen_dbguide.py shells out to
# `docker exec ac-database mysql`. No worldserver rebuild needed afterwards —
# guides are runtime-loaded; sync the YAML to the live tree + `docker restart
# ac-worldserver`.
#
# Usage (from the module root, on zoidberg):
#   bash tools/regen_guides.sh
set -euo pipefail

cd "$(dirname "$0")/.."          # module root
ROUTES=data/routes
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# race | faction | race-bit | parse-faction | start-block
RACES=(
  "undead|Horde|16|horde|Undead (1-13)"
  "tauren|Horde|32|horde|Tauren (1-13)"
  "orc|Horde|2|horde|Orc (1-13)"
  "troll|Horde|128|horde|Troll (1-13)"
  "bloodelf|Horde|512|horde|Blood Elf (1-13)"
  "human|Alliance|1|alliance|Human (1-13)"
  "dwarf|Alliance|4|alliance|Dwarf (1-13)"
  "nightelf|Alliance|8|alliance|Night Elf (1-13)"
  "gnome|Alliance|64|alliance|Gnome (1-13)"
  "draenei|Alliance|1024|alliance|Draenei (1-13)"
)

for row in "${RACES[@]}"; do
  IFS='|' read -r race faction bit pf start <<< "$row"
  combined="$TMP/route_${race}_full.json"
  out="data/guides/${faction,,}/${race}/${race}_zygor_1_80.yaml"
  mkdir -p "$(dirname "$out")"

  echo "=== ${race} (${faction}, bit ${bit}) ==="
  python3 tools/chain_route.py \
    --parse "$ROUTES/zygor_wotlk_${pf}_leveling.json" \
    --start "$start" --out "$combined"

  python3 tools/gen_dbguide.py \
    --faction "$faction" --race "$race" --race-bit "$bit" \
    --zygor-route "$combined" \
    --id "${faction,,}-${race}-zygor-1-80" \
    --name "${faction} ${race^} Zygor 1-80" \
    --out "$out"
done

echo "=== done: 10 guides regenerated under data/guides/ ==="
