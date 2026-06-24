# IdleBot Session Handoff — 2026-06-24

## Where we are

Private AzerothCore WotLK 3.3.5a server (`zoidberg`, 10.10.30.20). mod-idlebot drives 9 playerbot characters through leveling guides autonomously.

**Current state: 9 bots, ALL ZERO DEATHS, levels 2-6, actively questing on DB-backed guides.**

Best run ever. Priest hit L6 (previous runs every bot died at L6). GO loot fixed. Guide pipeline working.

## The 9 bots

| Name | Class | Race | Faction | GUID | Guide |
|------|-------|------|---------|------|-------|
| Idlebot | Mage | Undead | Horde | 2002 | undead-deathknell-1-6 |
| Idlerogue | Rogue | Undead | Horde | 2009 | undead-deathknell-1-6 |
| Idlewarrior | Warrior | Orc | Horde | 2008 | orc-valley_of_trials-1-6 |
| Idleshaman | Shaman | Tauren | Horde | 2007 | tauren-camp_narache-1-6 |
| Idlepaladin | Hunter | Dwarf | Alliance | 2006 | dwarf-coldridge-1-6 |
| Idletest | Paladin | Dwarf | Alliance | 2004 | dwarf-coldridge-1-6 |
| Idlewarlock | Warlock | Gnome | Alliance | 2011 | gnome-gnomeregan-1-6 |
| Idlepriest | Priest | Human | Alliance | 2010 | human-northshire-1-6 |
| Idledruid | Druid | Night Elf | Alliance | 2012 | nightelf-shadowglen-1-6 |

Note: names don't match classes (Idlepaladin is actually a Hunter, Idletest is the Paladin). User wants them renamed to Idle<classname> on next reset.

## Architecture

### Code (C++, static module)
- `modules/mod-idlebot/src/IdleBotManager.cpp` (~3500 LOC) — tick loop, step executor, combat, death, transport, vendor runs
- `modules/mod-idlebot/src/IdleBotPlayerbotBridge.cpp` (~2000 LOC) — bridge to mod-playerbots (movement, combat, loot, gear, spells)
- `modules/mod-idlebot/src/IdleBotGuide.h` — Guide/GuideStep structs
- `modules/mod-idlebot/src/IdleBotGuideLoader.cpp` — YAML guide parser (fkYAML)
- `modules/mod-idlebot/src/IdleBotCommandScript.cpp` — `.idlebot` chat commands

### Guide pipeline (Python, offline)
- `tools/guidegen/extract_db_quests.py` — batch extract quest/NPC/spawn data from DB to JSON
- `tools/guidegen/generate_guides.py` — generate YAML guides from DB + HB profile ordering
- `tools/guidegen/parse_honorbuddy_profiles.py` — parse HB Profile v3 XML as quest ordering hints
- `tools/guidegen/validate_generated_guides.py` — validate guides against DB
- `tools/guidegen/audit_stuck_bots.py` — diagnose stuck bots
- `data/generated/` — intermediate JSON (quests, NPCs, spawns, item sources, HB hints)
- `data/guides/generated/` — 20 zone guides (10 races × 1-6 + 6-12)

### Monitoring tools
- `tools/bot_status.sh` — full dashboard (level, deaths, quests, gear, events)
- `tools/death_report.sh <botname>` — per-bot quest/death/level timeline
- `tools/generate_hotspots.py` — DB spawn → patrol waypoints
- `tools/generate_vendor_coords.py` — nearest vendor per zone
- `tools/fix_bad_coords.py` — validate NPC coords against DB
- `tools/merge_class_quests.py` — merge class quests into zone guides

### Build/Deploy
- Dev box: `/home/khuong/azerothcore-zoidberg` (this machine, branch `idlebot-contested-go-deploy`)
- Build host: `ssh khuong@10.10.30.20`, checkout `~/build/azerothcore-zoidberg`
- Build: `DOCKER_BUILDKIT=1 docker build --target worldserver -t ghcr.io/bad1dea/ac-worldserver-zoidberg:latest -f apps/docker/Dockerfile .`
- Deploy: `docker compose -p zoidberg-stack --env-file $HOME/secrets/shared.env --env-file $HOME/secrets/zoidberg.env -f $HOME/homelab/compose/zoidberg/compose.yml up -d --no-deps --force-recreate ac-worldserver`
- Live config: `/home/khuong/acore/server/configs/modules/mod_idlebot.conf` (bind-mounted, read-only)
- AH bot config: same dir, `mod_ahbot.conf` — GUIDs 2003,2005 only (others conflict with bots)
- DB: container `ac-database`, root password `36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53`

## What works

- **DB-backed guide pipeline** — generates guides from acore_world with HB ordering hints
- **Zone guide chaining** — next_guide auto-loads when zone completes
- **Spell training on level-up** — LearnAvailableSpells every level
- **Smart gear/rewards** — class armor type scoring, stat weights
- **GO loot** — FillLoot + StoreNewItem for chest-type quest GOs
- **Flee during travel** — don't fight random aggro on accept/turn-in steps
- **Batch quest accept/turn-in** — pick up all quests at a hub before leaving
- **Cross-continent transport** — boat/zeppelin with waypoint chains + areatrigger
- **Navmesh walking** — 200yd chained segments, no teleports
- **Death recovery** — corpse run (10 attempts), safe spot scan, res sickness wait
- **Vendor runs** — guide step backtracking to nearest vendor/town hub
- **Stuck handler** — jump/strafe/reverse escalation (15-tick threshold)
- **Target blacklist** — 120-tick reach timeout, 120-tick blacklist (not for quest mobs at close range)
- **Attack fix** — meleeAttack=false from range so melee classes can initiate combat
- **79/79 CopilotBuddy features** implemented (see FEATURE_TRACKER.md)

## What still needs work

### Guide data (PRIORITY)
1. **Item source GO spawn verification** — item_sources.json found GO 337 (no spawns) instead of GO 2907 (has spawns). The extractor needs to cross-reference gameobject_loot_template with gameobject spawn table.
2. **12-80 guides not generated** — only 1-12 done. Need to extend generate_guides.py for higher level ranges.
3. **Transport profiles from HB** — extracted 0 entries. The XML parsing for transport waypoints needs work.
4. **Blackspots from HB** — extracted 0 entries. Need to check XML element names.
5. **Class quest interleaving** — class quests are merged into zone guides but timing may be off.
6. **48 item-use quests** — quests requiring UseItemAtLocation (like Call of Earth pt2 summon). Step type exists but guides need the steps generated.

### Runtime code
7. **GO loot for non-chest types** — current fix only handles GAMEOBJECT_TYPE_CHEST (type 3). Other GO types may need different handling.
8. **Timeout watchdog disabled** — step timeout skip is off (some quests take >5min). Need a smarter approach.
9. **InteractWith full behavior** — HonorBuddy's InteractWith.cs handles NPCs, GOs, items with loot/buy/gossip modes. We only have basic interact.
10. **Druid Teldrassil→Darkshore transport** — Night Elf boat route (Rut'theran→Auberdine) not configured.
11. **Bot names** — need renaming to Idle<classname> on next reset.

### Validation
12. **361 validation issues** in generated guides (226 far-spawn coords, 71 cross-zone prereqs, 58 unsafe, 6 NPC missing)
13. **788 quest items** with no creature/GO loot source (reference loot, scripted, skinning)

## Key files to read first
1. `modules/mod-idlebot/AUDIT.md` — module architecture overview
2. `modules/mod-idlebot/CLAUDE.md` — coding rules
3. `modules/mod-idlebot/docs/FEATURE_TRACKER.md` — 79-item feature checklist
4. `modules/mod-idlebot/docs/GUIDE_REBUILD_AUDIT.md` — guide pipeline docs

## Commands

```bash
# Monitor
./modules/mod-idlebot/tools/bot_status.sh
./modules/mod-idlebot/tools/death_report.sh Idleshaman

# Regenerate guides
cd modules/mod-idlebot
python3 tools/guidegen/extract_db_quests.py --out data/generated
python3 tools/guidegen/generate_guides.py --range 1-12 --out data/guides/generated
python3 tools/guidegen/validate_generated_guides.py --guides data/guides/generated

# Build + deploy
git add . && git commit -m "..." && git push origin idlebot-contested-go-deploy
ssh khuong@10.10.30.20 'cd ~/build/azerothcore-zoidberg && git fetch && git reset --hard FETCH_HEAD && docker build ... && docker compose ... up -d --no-deps --force-recreate ac-worldserver'

# Reset all bots
ssh khuong@10.10.30.20 'docker stop ac-worldserver'
# SQL: DELETE quests/inventory/spells, UPDATE level=1/positions, UPDATE guide_id/step_index=0
ssh khuong@10.10.30.20 'docker start ac-worldserver'
```

## Reference repos (use as structural hints, validate against our DB)
- https://github.com/Likon69/CopilotBuddy — bot engine (combat, maintenance, stuck handler)
- https://github.com/Likon69/Quest-Behaviors — quest behavior scripts (InteractWith, UseTransport, etc.)
- https://github.com/Likon69/Questing-profiles/tree/master/Profile%20v3 — WotLK quest profiles (best ordering)
- https://github.com/Likon69/Navigation-C- — navmesh (AC already has Recast/Detour built-in)
