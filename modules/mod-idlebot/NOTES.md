# mod-idlebot — working notes / handoff

Continuation notes so either Claude or ChatGPT can pick up. Update this when you
change direction or land something significant. Newest context at the top.

## Current direction (2026-06-18)

Goal: **organic, player-like leveling**, NOT hand-authored per-quest guides. The
bot should look at what's around it (aggro/adds), quest naturally, and head to
level-appropriate hubs — using Zygor + the DB as a *reference* so it doesn't get
stuck, but not strictly bound to a script.

### Two modes (`idlebot_bots.decision_mode`)
- **strict** — idlebot drives explicit `GuideStep`s (move/accept/kill/turnin) with
  the per-tick combat state machine. The legacy path. Still works; the builtin
  Tirisfal guide (`horde-1-12-tirisfal-glades`) runs here.
- **organic** — idlebot hands questing/travel/combat to **mod-playerbots'
  autonomous AI** (`+new rpg +grind +loot`) and just supervises: death handling,
  the IdleRPG event feed, periodic status log, and force-active. `TickBot` calls
  `TickOrganic` and returns. This is the direction we're building on.

Flip a bot: `scripts/reset-bot.sh <Bot> --mode organic` (or `strict`).

### What playerbots already gives us (don't reinvent)
mod-playerbots has a full questing/travel system: `TravelMgr`,
`ChooseTravelTargetAction`, `AcceptAllQuestsStrategy`, and the **"new rpg"**
subsystem (`src/Ai/World/Rpg/`). "new rpg" auto-picks quests, travels to
givers/objectives (DB-derived), fights, turns in, and handles its own
leveling/teleport. Config on zoidberg: `EnableNewRpgStrategy=1`, `AutoDoQuests=1`.
The rpg status enum (`NewRpgStatus` in `PlayerbotAIConfig.h`): IDLE / GO_GRIND /
GO_CAMP / WANDER_RANDOM / WANDER_NPC / DO_QUEST / TRAVEL_FLIGHT / REST / OUTDOOR_PVP.

### The two submodule (mod-playerbots) patches we rely on
These are committed in the mod-playerbots submodule (NOT upstream):
1. `RandomPlayerbotMgr.cpp` — `AiPlayerbot.RandomizeManagedBots` gate so the random
   manager doesn't re-randomize/teleport idlebot-managed (non-pool) bots.
2. `PlayerbotAI.{h,cpp}` — `SetForceActive/IsForceActive` registry checked at the
   top of `AllowActive()`. **This is essential for organic mode**: without it a
   lone overworld bot with no real player nearby runs *minimal* AI (the
   `BotActiveAlone=10` throttle) and just stands/grinds. idlebot registers its
   bots via the bridge `SetForceActive` in `TickOrganic`.

### Organic tuning landed (2026-06-19)
- **Quest-first** — per-bot `PlayerbotAI::SetRpgQuestFirst(true)` (submodule)
  overrides the NewRpg status weights for idlebot bots only: DO_QUEST 100,
  WANDER_NPC 25, TRAVEL_FLIGHT 15, REST 5, everything else (grind/camp/wander-
  random/pvp) 0. So she always tries to quest; when no quest is available she
  wanders to NPCs / flies to the next hub; she never autonomously grinds. The
  500-bot ambient pool keeps its varied behaviour (global weights untouched).
  Read via `NewRpgBaseAction::RandomChangeStatus` -> `GetRpgStatusWeight`.
- **Zone direction** — `IdleBotZoneRoute.{h,cpp}` holds 33 DB-derived leveling
  hubs (first quest-giver coords of each validated Zygor route; faction+level).
  `TickOrganic` watches for "stalled" (no quests AND idle/rest) for ~60s and
  teleports her to `NextHubFor(faction, level)` if she's far from it, then a
  ~2min cooldown. Bridge: `TeleportBot`, `GetTeamId`. Regenerate the hub table
  with the scratch steps in tools/ (first-accept npc per route -> DB coords).

### Known issues / next steps (priority order)
1. **The 12-55 vanilla gap** — the hub table jumps Horde 12 (Ghostlands) -> 55
   (Silithus) and Alliance similarly, because the Cata Zygor guides don't cover
   WotLK 1-58. A stalled bot in that range has no hub to steer to (NextHubFor
   returns the highest <= level, e.g. Ghostlands at 12). NEXT: add WotLK-correct
   1-55 hubs (hand-pick canonical leveling-zone entry coords per faction, or
   derive from DB quest-giver density per zone/level), OR lean on organic
   in-zone questing for that band.
2. **Verify quest-first + zone-direction live** — was mid-deploy at handoff.
   Watch `[IdleBot][organic] ... doing=do-quest stray=N`; doing should sit on
   do-quest / wander-npc, stray should stay low, and she should level steadily.
   Tune the stall threshold (60 ticks) / hub-far distance (400yd) if she
   teleports too eagerly or sits too long.
3. **Wire Zygor routes into the runtime** — `IdleBotGuideLoader` still a stub.
   The 61 route files (`data/routes/`) aren't loaded; only the hub table uses
   their derived data. Loading the full ordered routes would let idlebot nudge
   her along a zone *sequence* (not just the entry hub) and detect zone
   exhaustion precisely.
4. **strict-mode combat polish** lives in `TickBot` (reactive defend/loot/
   recover-when-safe, add-clearing via `FindNearestHostile`). Organic mode uses
   playerbots' native combat — if it's poor, improve it in the submodule.

## Zygor pipeline (tools/)
- `zygor_parse.py <ZygorLeveling*.lua>` → ordered quest routes JSON (accept/turnin
  + npc + objective hints). Drops Zygor zone-% coords; runtime derives real coords
  from the DB by id.
- `zygor_validate.py routes.json --valid-quests <ids> --out data/routes` → filters
  to quests that exist on the 3.3.5a DB, emits per-guide route files.
- Coverage: **58-80 is 100% valid** (Outland + Northrend, TBC-era, unchanged in
  Cata) + TBC racial starters. **Cata-revamped 1-60 and Cata/MoP 80-90 are NOT on
  a WotLK server** → 1-58 vanilla must come from organic questing, not these guides.
- Source zip: `~/Zygor.zip` (retail Cata/MoP ZygorGuidesViewer). Extract guides
  from `ZygorGuidesViewer/Guides/Leveling/`.

## Deploy (see also memory: idlebot-deploy-runbook — corrected 2026-06-18)
Live worldserver = compose project **`azerothcore-wotlk`** at
`/home/khuong/azerothcore-wotlk`, image `acore/ac-wotlk-worldserver:masters`.
db/auth are under project `zoidberg-stack` (mixed) → always use `--no-deps`.
Modules are STATIC → C++ changes need a real image rebuild (the `./modules` bind
mount only updates runtime data: guide yaml / conf / sql).

```
# sync (tar; do NOT combine with a backgrounded build in one ssh — corrupts stream)
tar -cf - modules/mod-idlebot/src modules/mod-playerbots/src/Bot/<files> \
  | ssh khuong@10.10.30.20 'tar -xf - -C /home/khuong/azerothcore-wotlk'
# build (warm ccache ~20s idlebot-only, ~10min if PlayerbotAI.cpp changed)
ssh khuong@10.10.30.20 'cd /home/khuong/azerothcore-wotlk && docker compose build ac-worldserver'
# deploy
ssh khuong@10.10.30.20 'cd /home/khuong/azerothcore-wotlk && docker compose up -d --no-deps --force-recreate ac-worldserver'
```
Reset a bot: `scripts/reset-bot.sh <Bot> [--full] [--mode organic|strict] [--restart]`
(stops worldserver so edits stick). DB: `acore_characters.idlebot_bots`; test bot
is `Idlebot` (guid 2002).

## Logs
- Strict: `[IdleBot][dbg] ... mode=... reactive=...` per kill/travel tick.
- Organic: `[IdleBot][organic] <name> L<lvl> hp=% quests=N doing=<rpg-status> pos=(x,y) map=M` every ~5s.
- `docker logs --since 2m ac-worldserver | grep -i idlebot`.
