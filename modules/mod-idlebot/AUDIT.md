# mod-idlebot — Audit & Deep-Review Brief

A consolidated, self-contained brief for an external AI (or human) doing a deep
review/audit of this module. Pairs with `NOTES.md` (chronological running log,
newest first) and `CLAUDE.md` (the hard rules). Last updated 2026-06-20, at
commit `5614a48`.

---

## 1. What this is

`mod-idlebot` is a **private-server-only** AzerothCore (WoW 3.3.5a / WotLK) C++
module that drives **real, visible playerbot characters** to level up
autonomously in a player-like way. It is NOT a quest-script engine and NOT a
retail/anti-cheat-touching tool. It supervises bots that are real loginable
characters on a dedicated account (via mod-playerbots `bot add`), not the random
bot pool.

The design goal (emphasized repeatedly by the owner): **organic, player-like
leveling** — the bot quests, fights, loots, repairs, sells, trains, and travels
the way a player would, using Zygor guides + the world DB as a *reference* for
"where to go," NOT a rigid per-quest script.

### Two decision modes (`idlebot_bots.decision_mode`)
- **organic** (the active direction): hands questing/travel/combat to
  mod-playerbots' autonomous "new rpg" AI (`+new rpg +grind +loot`) and just
  *supervises* — death handling, force-active, quest-first weighting, the IdleRPG
  event feed, anti-stall hub steering, and player-like maintenance (vendor/trainer
  trips, talents, spells). `TickBot` → `TickOrganic`.
- **strict** (legacy): idlebot drives explicit `GuideStep`s (move/accept/kill/
  turnin) with a per-tick combat state machine. Still works (builtin Tirisfal
  guide) but not the focus.

---

## 2. Critical constraints (read before judging the code)

1. **Static-module build, NO hot reload.** Modules compile into the worldserver
   image (`-DMODULES=static`). Editing source does NOT affect the running server;
   every change needs a full image rebuild + redeploy. So the code is written to
   be re-asserted/idempotent across restarts, and some state is intentionally
   session-local.
2. **Never block the worldserver thread.** No sleeps/long loops/synchronous waits
   in the tick path. Everything is a state machine advancing at most one small
   action per bot per tick (`TickMs`, default 1000ms). This is why "trips" are
   multi-tick state machines, not procedures.
3. **Do not hallucinate AzerothCore/mod-playerbots APIs.** The fork may differ
   from training data. Every external call should be grep-verified against the
   local tree. (Reviewers: flag any call that can't be found in
   `../../src/server/` or `../mod-playerbots/src/`.)
4. **Long-lived pointers are forbidden.** Never store a raw `Player*`/`Creature*`
   past the current tick — resolve from `ObjectGuid` each use (logout/despawn
   dangles it). The bridge stores `BotGuid` (a uint64 GUID) and resolves via
   `ObjectAccessor::FindPlayer` each call (`ResolveOnlinePlayer`).
5. **SQL/style:** AzerothCore conventions, `-Werror`, `fmt`-style logging
   (`LOG_INFO("module.idlebot", "...{}", x)`), 4-space indent, 80 cols.

---

## 3. File map (src/, ~5100 LOC)

| File | LOC | Responsibility |
|---|---|---|
| `IdleBotManager.cpp` | 2257 | **The heart.** Tick loop, `TickBot`, `TickOrganic`, `HandleDeath`, `HandleVendorTrip` (repair/sell/train trips), hub steering, strict-mode guide executor, kill/loot/combat state machine, `PollDeltas`, event emission. |
| `IdleBotManager.h` | 239 | `BotRecord` (per-bot runtime state, persisted to `idlebot_bots`), `IdleBotManager` class, all config fields + defaults. |
| `IdleBotPlayerbotBridge.cpp` | 1227 | **The seam to mod-playerbots.** Concrete bridge: movement, combat, inventory, talents (`AutoSpecTalents`), spells (`LearnAvailableSpells`), NPC grid search (`FindNearestServiceNpc`), status/position reads, strategy toggles, force-active. All mod-playerbots/AC calls live here. |
| `IdleBotPlayerbotBridge.h` | 257 | Bridge interface (pure-virtual seam — lets the decision logic stay testable/stable). Types: `BotLiveStatus`, `InventoryStatus`, `BotPosition`, `QuestState`. |
| `IdleBotCommandScript.cpp/.h` | 282/17 | `.idlebot` chat commands (add/list/remove/status/guide/pause/resume). |
| `IdleBotZoneRoute.cpp/.h` | 114/37 | **Race-aware leveling hub table** + `NextHubFor(faction, race, level)`. Where a stalled bot is steered. |
| `IdleBotTrainers.cpp/.h` | 47/31 | **Capital class-trainer location table** + `ClassTrainerLoc(faction, class)`. Where a bot goes to train. |
| `IdleBotLog.cpp/.h` | 133/58 | IdleRPG event feed (per-bot log file + `idlebot_events` DB table). |
| `IdleBotChatLogScript.cpp/.h` | 107/10 | Hooks bot chat into the log feed. |
| `IdleBotModule.cpp` | 45 | WorldScript registration; `OnUpdate` → `IdleBotManager::OnWorldUpdate`. |
| `IdleBotGuide.h` | 104 | `Guide` / `GuideStep` types (strict mode). |
| `IdleBotGuideLoader.h` | 33 | **STUB** — Zygor route loading not wired (see open issues). |
| `IdleBotDecision*.h` | 36-51 | Decision-engine scaffolding (future; not the active path). |

### Non-source
- `NOTES.md` — chronological handoff log (START HERE for "why").
- `CLAUDE.md` — module rules.
- `docs/` — ARCHITECTURE / ROADMAP / COMMANDS / DECISION_ENGINE / GUIDE_FORMAT /
  ADAPTIVE_GUIDES / SOCIAL_BEHAVIOR / M4_M5_VERIFICATION.
- `tools/` — `zygor_parse.py`, `zygor_validate.py`, `derive_hubs.py` (offline DB
  derivation; output baked into the C++ tables, not loaded at runtime).
- `scripts/reset-bot.sh` — wipe a bot's controller state (+ optional char→L1).
- `conf/mod_idlebot.conf.dist` — config template.
- `patches/` — reference copies of the mod-playerbots patches.

### Persistence (DB: `acore_characters`)
- `idlebot_bots` — registry + progress + death counters + decision_mode.
- `idlebot_events` — IdleRPG event feed (cols: `bot_id, event_type, detail,
  created_at`).

---

## 4. Runtime model

```
WorldScript::OnUpdate(diff)  [IdleBotModule.cpp]
  └─ IdleBotManager::OnWorldUpdate(diff)   accumulates diff; fires Tick() every TickMs
       └─ Tick(): for each active bot → TickBot(rec)
            └─ organic mode → TickOrganic(rec):
                 1. if !online/!controlled → mark for re-attach, return
                 2. (re)assert force-active + quest-first + "+new rpg +grind +loot"
                    every ~15s (these live on the per-session PlayerbotAI and are
                    wiped on AI recreation — must self-heal)
                 3. auto-turn-in completed quests (new rpg often can't walk back)
                 4. on level-up: AutoSpecTalents (talents apply instantly)
                 5. HandleDeath (if dead/recovering, consume tick)
                 6. HandleVendorTrip (repair/sell/train town trips) — owns movement
                    while on a trip; sets "-new rpg" so questing pauses
                 7. anti-stall hub steer (if strayTicks>60 → NextHubFor → teleport)
                 8. PollDeltas + periodic status log
```

Key per-bot state (`BotRecord`, all session-local unless noted persisted):
`decisionMode`(persisted), `maintaining`+`maintTicks` (trip state),
`lastTrainedLevel`/`lastSpeccedLevel` (drift trackers), `strayTicks`/
`hubSteerCooldown` (anti-stall), `deathPhase`+counters, `organicStrategiesEnsured`,
`controlWaitArmed` (no-AI self-heal).

---

## 5. Key flows (audit these for correctness)

### Talents (verified working)
`TickOrganic` on level-up → bridge `AutoSpecTalents` →
`PlayerbotFactory::InitTalentsTree(true,true,true)` (increment + template + reset)
→ `Player::SaveToDB`. Logs `freeTalentPoints A→B`. Verified Idlebot L14 5→0.

### Spell training (verified working)
Drift-triggered (`level - lastTrainedLevel >= 4`, level >= 10) capital trip in
`HandleVendorTrip`: teleport to `ClassTrainerLoc(faction,class)` → walk in →
bridge `LearnAvailableSpells` → `PlayerbotFactory::InitClassSpells()` +
`InitAvailableSpells()` (iterates class trainers, learns every level-eligible
spell via `CanTeachSpell`) → `SaveToDB`. Logs `spells A→B`. Verified Idlebot
23→27 in `character_spell`. The interactive "trainer" chat action was abandoned
(unreliable). Also trains opportunistically at a trainer in the current town.

### Vendor/repair trips (`HandleVendorTrip`)
Triggers on `needRepair` (durability < 35% or broken) OR `needSell` (≤2 free
slots) OR `needTrainTrip` (drift). Priority while on trip: (1) repair/sell at
nearest merchant via `FindNearestServiceNpc(REPAIR|VENDOR)`, hub-steer if none in
range; (2) train (local trainer, else capital). 400-tick timeout backstop patches
up and resumes so it never hangs. **Known historical bug (fixed):** `needTrain`
used to be a standalone trigger with `lastTrainedLevel=0`, hijacking every bot at
login into a perpetual trainer hunt — see commit `e584025`.

### Hub steering (`NextHubFor`, race-aware)
When stalled (`strayTicks>60` or bags full), steer to the best
`(faction, race, level)` hub. 1-19 = race-specific starting/second zones; 20+ =
race-neutral. Teleports cross-map. 120-tick cooldown after a steer.

### Death (`HandleDeath`)
Observes mod-playerbots' DeadStrategy (release→corpse-run→revive); counts deaths,
nudges on stall (`ghostStallTicks`), falls back to graveyard res. Organic bots
**never death-loop-PAUSE** (that caused an overnight freeze — commit `b4d97de`).

### Combat (strict mode only — `TickBot`)
Reactive defend/loot/recover-when-safe, add-clearing via `FindNearestHostile`,
roam on kill objectives. Organic mode uses mod-playerbots' native combat.

---

## 6. mod-playerbots dependency (3 non-upstream patches)

Located in `../mod-playerbots/` (a submodule). These are REQUIRED for organic
mode and are the integration risk surface:

1. **`2517b4bc`** — `PlayerbotAI.{h,cpp}` `SetForceActive`/`IsForceActive`
   registry checked at the top of `AllowActive()`, + `RandomPlayerbotMgr.cpp`
   `AiPlayerbot.RandomizeManagedBots` gate. **Essential:** without force-active a
   lone overworld bot with no real player nearby runs minimal AI
   (`BotActiveAlone=10` throttle) and just stands/grinds.
2. **`24c3ec71`** — `PlayerbotAI` `SetRpgQuestFirst` + `GetRpgStatusWeight`:
   per-bot NewRpg status weights for idlebot bots only (DO_QUEST 100, WANDER_NPC
   25, TRAVEL_FLIGHT 15, REST 5, grind/camp/pvp 0). Read in
   `NewRpgBaseAction::RandomChangeStatus`. Global 500-bot pool untouched.
3. **`ff54b303`** — `PlayerbotMgr.cpp` OnBotLogin: recreate AI when a bot is
   in-map but its AI was erased (fixed freshly-created chars getting no AI).

mod-playerbots "new rpg" subsystem lives in `../mod-playerbots/src/Ai/World/Rpg/`.
Server config on host: `EnableNewRpgStrategy=1`, `AutoDoQuests=1`,
`RandomBotAutologin=1`.

---

## 7. Data sources

- **World DB (`acore_world`)** — all coordinates in `IdleBotZoneRoute.cpp` and
  `IdleBotTrainers.cpp` are real DB spawn positions (innkeepers, flight masters,
  class trainers, `playercreateinfo`). Derived offline, baked into C++ tables.
- **Zygor guides** (`~/Zygor.zip`, retail Cata/MoP) — parsed by `tools/` into
  validated quest routes. Coverage: 58-80 is 100% valid (Outland/Northrend,
  TBC-era); Cata-revamped 1-58 is NOT valid on a WotLK server → vanilla 1-58
  leans on organic questing + the hub table, not Zygor routes.

---

## 8. Known open issues / risk areas (PRIORITY AUDIT TARGETS)

1. **20-52 neutral hubs are `derive_hubs.py` centroids**, not real towns — can be
   out in a field or cross-continent. Race-specific coverage stops at L19 by
   design. (`IdleBotZoneRoute.cpp`.) *Candidate: replace with real town anchors.*
2. **`IdleBotGuideLoader` is a stub** — the 61 validated Zygor route files in
   `data/routes/` are NOT loaded at runtime; only their derived hub coords are
   baked in. Loading full routes would enable zone-*sequence* nudging.
3. **Session-local training/spec trackers** — `lastTrainedLevel`/`lastSpeccedLevel`
   reset on restart, so an L10+ bot re-runs its capital train trip once per
   restart (idempotent, negligible, but redundant). Persist in `idlebot_bots` to
   fix.
4. **Raw `MoveTo` is straight-line** — vendor/trip travel uses `MoveTo`
   (line-of-sight pathing) + cross-map teleport. May fail over long/obstructed
   distances; teleport is the safety hatch.
5. **Horde Paladin trainer (Silvermoon) not pinned** in `IdleBotTrainers.cpp` —
   only common class without a Horde entry (besides DK, who start trained).
6. **Zygor talent builds unimplemented** — talents use the factory's per-class
   template, not Zygor's `ZTA:RegisterBuild` specs.
7. **Concurrency / lifetime** — verify no raw entity pointer outlives a tick, and
   that `FindNearestServiceNpc` grid search (`Cell::VisitObjects`) is safe on the
   map thread. (Historical bugs: `VisitGridObjects` doesn't exist; a recover-eat
   bug dropped HP mid-combat — both fixed, but re-verify the patterns.)
8. **Idempotency of strategy re-assertion** — `+new rpg/+grind/+loot` and
   force-active are re-applied every ~15s; confirm this can't fight the player's
   own strategy if they take manual control.

---

## 9. Build / deploy / verify (host: zoidberg, `khuong@10.10.30.20`)

Live worldserver = compose project `azerothcore-wotlk` at
`/home/khuong/azerothcore-wotlk`, image `acore/ac-wotlk-worldserver:masters`. DB
under project `zoidberg-stack` → always `--no-deps`.

```bash
# sync (tar; never combine with a backgrounded build in one ssh — corrupts stream)
tar -cf - modules/mod-idlebot/src | ssh khuong@10.10.30.20 'tar -xf - -C /home/khuong/azerothcore-wotlk'
# rebuild (warm ccache: idlebot-only ~20s; ~10min if PlayerbotAI.cpp changed)
ssh khuong@10.10.30.20 'cd /home/khuong/azerothcore-wotlk && docker compose build ac-worldserver'
# deploy
ssh khuong@10.10.30.20 'cd /home/khuong/azerothcore-wotlk && docker compose up -d --no-deps --force-recreate ac-worldserver'
```

Verify:
- Organic ticks: `docker logs --since 2m ac-worldserver | grep -iE "doing="`
  → want `[a=1 r=1]` (force-active on, new rpg on), low `stray`.
- Talents/spells: grep `AutoSpecTalents` / `LearnAvailableSpells` log lines; check
  `character_talent` / `character_spell` counts by guid.
- Events: `acore_characters.idlebot_events`.
- Codestyle: `python apps/codestyle/codestyle-cpp.py` (run from repo root).

---

## 10. Suggested deep-research questions for the auditor

- Is `HandleVendorTrip`'s state machine free of livelock/oscillation across all
  combinations of needRepair/needSell/needTrainTrip + reachable/unreachable NPCs?
- Does any path leave `maintaining=true` / `-new rpg` set without a guaranteed
  resume (a bot stuck not questing)? The 400-tick timeout is the only backstop —
  is it sufficient and correct for cross-map walks?
- Are all `_bridge->` calls grep-verifiable against the local AC/mod-playerbots
  source, with correct signatures? (No hallucinated APIs.)
- Race/faction correctness in `NextHubFor`: can any bot be steered to a
  hostile/wrong-faction or wrong-map zone?
- Thread-safety of the bridge reads/writes given they run on the world tick.
- Is the talent/spell `SaveToDB(false,false)` call appropriate (cost, transaction
  safety) on the tick path?
