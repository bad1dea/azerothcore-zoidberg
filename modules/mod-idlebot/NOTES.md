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

### Known issues / next steps (priority order)
1. **Zone direction** — "new rpg" picked a capital city (Undercity) for a level-1
   bot instead of a leveling hub. Need to feed it the *level-appropriate starting/
   quest hub* as a reference (use the validated Zygor routes' zone order, or a
   simple level→zone-hub table, and nudge/teleport when it strays). This is the
   "knows where to go for quest hubs" the user asked for.
2. **Wire Zygor routes into the runtime** — `IdleBotGuideLoader` is still a stub.
   The 61 validated route files (`data/routes/{horde,alliance}/*.json`, full 58-80
   + TBC starters) aren't loaded yet. Use them as the organic zone-reference, not
   as strict scripts.
3. **strict-mode combat polish** still lives in `TickBot` (reactive defend/loot/
   recover-when-safe, add-clearing via `FindNearestHostile`). Organic mode uses
   playerbots' native combat instead — if that combat is poor, improve it in the
   submodule rather than layering idlebot on top.

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
