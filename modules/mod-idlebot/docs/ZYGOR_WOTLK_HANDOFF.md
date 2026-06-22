# idlebot — Zygor-WotLK leveling handoff (2026-06-21)

Status note for whoever picks this up next (Claude or Codex). Read this before touching
the guide pipeline or the bots. It records what was done, what changed, every edited file,
and the open gaps.

---

## TL;DR — where we are

The guide SOURCE is now the **WotLK 3.3.5a Zygor remaster**
(`github.com/ErebusAres/ZygorGuidesRemaster-3.3.5a_WOTLK`), cross-referenced and
coord-resolved against our own `acore_world` DB. This replaced the old "generate from DB by
zone+level" approach, which kept failing (handed bots quests above their MinLevel, missing
auto-granted starters, broken prereq order).

**Validated:** the WotLK remaster cross-refs ~100% vs our DB (2016/2015 quests exist). The
earlier *CATA* Zygor (from `~/Zygor.zip`) does NOT — Cata revamped the classic zones, so its
1-60 classic quests are ~0% present in WotLK (only its TBC Blood Elf + Outland/Northrend
blocks cross-ref). Use the **remaster**, not the Cata zip.

**Live now:** 4 bots, all fresh L1, on Zygor 1-80 mega-guides, questing in Zygor order:
- Idlebot (undead) → `horde-undead-zygor-1-80`  (Tirisfal / Deathknell)
- Idleshaman (tauren) → `horde-tauren-zygor-1-80` (Camp Narache)
- Idletest, Idlepaladin (dwarf) → `alliance-dwarf-zygor-1-80` (Coldridge)

Full 10-race guide library generated (5 Horde + 5 Alliance), ~1060-1180 quests / ~3160-3412
steps each. Only the 3 in use are loaded in the running server; the other 7 load on next
restart.

---

## The pipeline (how to (re)generate guides)

Everything runs against zoidberg's DB. Tools live in `tools/`.

1. **Source guide** (per faction): the remaster's
   `ZygorGuidesViewerRM/Guides/Leveling/ZygorGuidesHorde.lua` / `...Alliance.lua`.
   26 `RegisterGuide` blocks each: race starts (X 1-13), `Main Guide (13-20)`,
   `Levels (20-25..55-60)`, `Outland (60-70)`, `Northrend (70-80)`, chained via `next`.

2. **Parse**: `tools/zygor_parse.py <ZygorGuides*.lua> > routes.json`. Already handles the
   leveling DSL (step/.talk/.accept/.turnin/.kill/.get/ding/only/|q/##id). Output = one route
   per block: `{title, next, steps:[{action,quest,npc,obj,count,name,only}...]}`.
   - Quirk: parsed `title` has ONE backslash + a stray leading `"`; `next` has TWO backslashes.
     Match blocks on the **last path segment** (e.g. "Main Guide (13-20)"). Also there is a
     duplicate empty "Main Guide (13-20)" block — keep the one with MORE steps.
   - Committed parses: `data/routes/zygor_wotlk_horde_leveling.json`,
     `data/routes/zygor_wotlk_alliance_leveling.json`.

3. **Chain → combined route**: follow `next` from a race start, concatenate all blocks' steps
   into one route (race 1-13 → Main → Levels → Outland → Northrend = 20 blocks, ~5100 steps).
   (Was done with an ad-hoc python loop in scratchpad — fold it into a tool if reused.)

4. **Convert + DB cross-ref**: `gen_dbguide.py --zygor-route <combined_route.json>`. This is
   the key: it takes the quest LIST + ORDER from Zygor and resolves EVERYTHING from the DB by
   quest id — accept (creature_queststarter), objectives (quest_template.RequiredNpcOrGo /
   RequiredItemId → droppers), turn-in (creature_questender), all coords via clustered
   centroids. **Zygor omits npc/item ids (uses names), so DB resolution by quest+objective is
   what makes it work.** Quests not in the DB are dropped (the WotLK validation).
   - `--map N` locks to one continent (skip quests whose giver isn't there). The 1-80
     mega-guides were generated WITHOUT `--map` on purpose, so cross-continent steps are
     present (we WANT the travel roadblocks to surface). Single-zone guides use `--map`.

Example (one race):
```
python3 tools/gen_dbguide.py --faction Horde --race undead --race-bit 16 \
  --zygor-route tools/route_undead_full.json \
  --id horde-undead-zygor-1-80 --name "Horde Undead Zygor 1-80" \
  --out data/guides/horde/undead/undead_zygor_1_80.yaml
```
Race bits: human 1, orc 2, dwarf 4, nightelf 8, undead 16, tauren 32, gnome 64, troll 128,
bloodelf 512, draenei 1024.

Guides are runtime-loaded → no rebuild; sync the YAML to
`/home/khuong/azerothcore-wotlk/modules/mod-idlebot/data/guides/` and `docker restart
ac-worldserver`.

---

## Files edited this session

### C++ (worldserver rebuild required) — `src/`
- **IdleBotManager.cpp / .h**
  - `GearBot()` + `.idlebot gear` plumbing — force factory gear+spec at the bot's CURRENT
    level (bypass the L<=5 starter gate); for test-prepping a manually-leveled bot.
  - Watchdog floor is now config `IdleBot.StepSkipSeconds` (default **2700 = 45 min**, was a
    hard 300s). Skips a stuck quest only after that much ACTIVE time.
  - **Frozen-combat breaker** (reactive `defend` + kill-step `fight`): detect a stalled
    rotation by HP STAGNATION (hp flat while engaged) — NOT myAttackers (unreliable, reads 0
    mid-fight). Re-assert the attack; on travel steps, stop deferring after a long stall.
  - **Anti-pin** (`reactivePinTicks`): incidental combat/loot must not defer a travel/accept
    step forever (was a 45-min `reactive=loot` freeze); after ~30 ticks push on toward the goal.
  - **Tactical RETREAT** (kill step): when hurt AND swarmed (hp<40% & aoeCount>=4), back off
    from the densest hostiles, fight on the way out, resume when clear.
  - **Death-loop now SKIPS the quest** (`skipQuestRequested` → SkipQuestSteps in TickBot)
    instead of dead-stopping (`blocked`); the bot revives and moves on.
  - New BotRecord fields: combatStallTicks, lastCombatHpPct, skipQuestRequested,
    reactivePinTicks, retreatTicks, _stepSkipSeconds.
- **IdleBotCommandScript.cpp** — `.idlebot gear <bot>` command (Console::Yes).

### Tools (python, no rebuild) — `tools/`
- **gen_dbguide.py** — biggest changes:
  - `cluster_centroid()`: resolve a mob to its DENSEST spawn cluster (50y grid → densest cell
    → spawns within 100y), tight radius 40-130 (was global avg, radius up to 200 → bots
    wandered into neighbouring camps).
  - Per-map coord resolution + **giver-map constraint**: objectives/turn-in resolve on the
    quest GIVER's map; cross-continent/instance objectives are dropped (MoveTo can't cross maps).
  - `--map` continent lock; `--zygor-route` mode (quest list+order from Zygor, details from DB);
    prereq topological sort (DB mode only) via quest_template_addon.PrevQuestID.
- **zygor_route_to_guide.py** — earlier converter (for the Outland/Loremaster route JSONs):
  same cluster fix, skip quest=None steps, accept/turn-in coherence. (Superseded by
  gen_dbguide --zygor-route for the leveling guides, kept for the route-JSON path.)
- **zygor_parse.py** — unchanged; used as-is to parse the leveling Lua.

### Data — `data/`
- `data/routes/zygor_wotlk_horde_leveling.json`, `zygor_wotlk_alliance_leveling.json` —
  parsed remaster routes (26 blocks each).
- `data/guides/{horde,alliance}/{race}/{race}_zygor_1_80.yaml` — 10 mega-guides.
- `data/guides/horde/undead/undead_zygor_1_13.yaml` — first single-block proof.
- Old DB guides (`*_1_18.yaml`, `*_1_60.yaml`, `alliance/outland/hellfire.yaml`) — superseded
  by the Zygor guides; safe to ignore/remove later.

### Submodules
- **mod-ah-bot-plus** (`src/AuctionHouseBot.cpp`, commit `d650f2e`): skip a seller GUID that
  has a live FindConnectedPlayer before AddObject. ⚠️ Committed in the submodule but
  **NOT pushable** (its only remote is NathanHandley upstream). Superproject pointer NOT bumped
  (would break `git submodule update`). Live in the binary. To make it permanent: fork to
  bad1dea, repoint .gitmodules.
- **mod-playerbots**: had `[IBDIAG]` instrumentation during debugging — all reverted, clean now,
  pointer still at 557a75b.

### Config (bind-mounted on zoidberg, NOT in git)
- `env/.../modules/mod_ahbot.conf`: **`AuctionHouseBot.GUIDs = 2003,2005`** — THE fix for the
  login-churn (see below). Must exclude idlebot bot guids (2002/2004/2006/2007).
- `mod_idlebot.conf`: `IdleBot.StepSkipSeconds` (default 2700).
- `playerbots.conf`: MinRandomBots 100 / MaxRandomBots 150 (reduced from 1000/1500).

---

## Major bug fixed this session: login churn (root cause)

The 3 fresh bots cycled login/logout ~1/min for the whole prior investigation. ROOT CAUSE was
NOT load/account/watchdog — it was a **GUID COLLISION with mod-ah-bot-plus**: its
`AuctionHouseBot.GUIDs` seller list (2003-2010) included the idlebot bot characters
(2004/2006/2007). Each AH cycle did `ObjectAccessor::AddObject`/`RemoveObject` on those guids,
evicting the live bot from the connected-player map (it went "ghost": online=1 but not
FindConnectedPlayer-able) → idlebot re-added it → endless churn. Idlebot (2002) was outside the
range, which is why it alone was stable. Fixed by config (`GUIDs = 2003,2005`) + the AH
safeguard. Diagnosed by instrumenting `ObjectAccessor::RemoveObject`.

---

## Missing / needs refinement (prioritized)

1. **Cross-continent travel — the #1 blocker.** `MoveTo` only paths within a map. The 1-80
   guides span maps 0 (EK), 1 (Kalimdor), 530 (Outland), 571 (Northrend). A bot will quest its
   starting continent, then STALL when the guide says cross an ocean (zeppelin/boat) or take a
   portal (Outland/Northrend). Needs: flight-path/boat/zeppelin/portal travel, OR segment the
   guides per continent and hand off. Expect the leveling run to stall here first.
2. **Combat viability at low level.** Retreat + death-skip + anti-pin help, but a fresh L1 vs
   over-level mobs still loses; over-pulling and the playerbots class rotation are the deeper
   layer. The user's framing: clear adds → kill → retreat; retreat is in, but the underlying
   class combat (mod-playerbots) is where wins/losses are decided.
3. **The 7 non-bot race guides aren't loaded** (generated after the last restart). Restart to
   load them; harmless until a bot uses one.
4. **~470 quests/guide dropped as "cross-continent/instance."** Some are legit quests whose
   objective creature's busiest map differs from the giver's map. Could refine objective
   resolution (prefer the giver's map even if the mob is busier elsewhere) to recover them.
5. **ding / grind markers not captured.** `zygor_parse.py` drops Zygor's `ding N` (be level N
   here). Level-gating currently relies on Zygor's ORDER being level-appropriate. If bots still
   out-level/under-level, capture `ding` → emit `grind_until_level` steps (executor supports it).
6. **Class `only` conditions ignored.** Zygor's per-class quest variants (`|only X Warrior`)
   are flattened in; a class may get quests it can't take → AcceptQuest-skip handles it but it's
   noisy. Could filter by the bot's class at generation or via StepAppliesToBot.
7. **No runtime guide chaining.** We concatenate blocks into one mega-guide per race instead.
   Fine, but a `next_guide` field + loader support would be cleaner and allow shared-tail reuse.
8. **AH safeguard / submodule pointers** unpushable (upstream remotes). Fork if we want them
   tracked.

---

## Build / deploy / DB cheatsheet (zoidberg)

- Host: `ssh khuong@10.10.30.20`. Live tree: `/home/khuong/azerothcore-wotlk` (compose project
  `azerothcore-wotlk`, image `acore/ac-wotlk-worldserver:masters`).
- C++ change: `tar -cf - <paths> | ssh ... 'tar -xf - -C /home/khuong/azerothcore-wotlk'`
  (SEPARATE from the build cmd — combining a stdin-tar with a backgrounded build corrupts the
  tar). Then `cd /home/khuong/azerothcore-wotlk && DOCKER_BUILDKIT=1 docker compose build
  ac-worldserver` (warm ccache ~min), then `docker compose up -d --no-deps --force-recreate
  ac-worldserver`.
- Guide/conf change: no rebuild; sync + `docker restart ac-worldserver`.
- DB: `PW=$(docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}' |
  grep -m1 MYSQL_ROOT_PASSWORD= | cut -d= -f2)`; `printf '%s' "<SQL>" | docker exec -i
  ac-database mysql -uroot -p"$PW" -N`.
- DB schema (this fork): quest_template has NO PrevQuestID (it's in `quest_template_addon`);
  `creature` uses id1/id2/id3; givers/enders in creature_queststarter/questender; item droppers
  in creature_loot_template (Item,Entry,Chance); QuestSortID = zone AreaID (positive).
- Bot state: `acore_characters.idlebot_bots` (guide_id, step_index, step_state). Bots are guids
  2002 (Idlebot/undead), 2004 (Idletest/dwarf), 2006 (Idlepaladin/dwarf), 2007
  (Idleshaman/tauren). To reset a bot to fresh L1: set characters level/xp/money/position +
  DELETE character_queststatus(+_rewarded)/character_inventory/item_instance/character_talent,
  then idlebot_bots step_index=0.
- Console-run an idlebot command headlessly (Tty container):
  `ssh -tt khuong@10.10.30.20 'timeout 6 docker attach ac-worldserver' <<< '.idlebot gear X'`.

Related: `STRICT_MODE_FOLLOWUPS.md` (older), and the auto-memory `idlebot-status`.
