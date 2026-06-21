# Strict-mode follow-ups (in-depth handoff)

Status as of 2026-06-21: strict mode is live and self-healing. Bots are geared on
login, run DB-generated guides (`tools/gen_dbguide.py` → `data/guides/...1_18.yaml`),
and skip un-acceptable or stuck quests (AcceptQuest skip + per-step `timeout_seconds`
watchdog in `IdleBotManager::TickBot`). Three follow-ups remain. This doc is written
so either Claude or Codex can pick any of them up cold.

Key code references (verified):
- Guide generator: `tools/gen_dbguide.py` (`resolve_coords()`, the kill-step block).
- Executor: `src/IdleBotManager.cpp` — `TickBot()` (step switch ~L506+), step-timeout
  watchdog (before `MaintenanceGuard`), `SkipQuestSteps()`, `AdvanceStep()`.
- Login: `src/IdleBotManager.cpp` `TickBot` ~L287-298 (`EnsureBotOnline` + `loginRetryTicks=10`);
  `src/IdleBotPlayerbotBridge.cpp` `EnsureBotOnline()` (L214), `GetLiveStatus()` (online
  logic; connected-but-not-in-world → `out.online=true` at L282).
- Login internals (submodule): `modules/mod-playerbots/src/Bot/PlayerbotMgr.cpp`
  `AddPlayerBot()` / `botLoading` / `HandlePlayerBotLoginCallback()`;
  `RandomPlayerbotMgr.cpp` `ProcessBot()` (non-random early-return ~L1347).
- DB schema notes: this fork's `creature` table uses `id1/id2/id3` (not `id`);
  quest objectives in `quest_template.RequiredNpcOrGo1..4`(+Count) and
  `RequiredItemId1..6`(+Count); item→dropper via `creature_loot_template (Item,Entry,Chance)`;
  givers/enders via `creature_queststarter` / `creature_questender (quest,id)`;
  `quest_template` has NO `Type` column here; zone = `QuestSortID`
  (Coldridge=132, DunMorogh=1, LochModan=38, Mulgore=215, CampNarache=221, Barrens=17,
  Tirisfal=85, Silverpine=130). DB access on zoidberg:
  `ssh khuong@10.10.30.20`, then pipe SQL via
  `printf '%s' "<SQL>" | docker exec -i ac-database mysql -uroot -p"$PW" -N` where
  `PW=$(docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}' | grep -m1 MYSQL_ROOT_PASSWORD= | cut -d= -f2)`.

---

## 1. Tighten generated kill-step centroids

### What's wrong
`gen_dbguide.py::resolve_coords()` computes a kill objective's waypoint as the
**average of every spawn of that creature entry on the giver's map**, with
`radius = max distance from that centroid` (capped at 200). For mobs that spawn
across a whole zone this is bad:
- wolves 704/705 → centroid `(-6404, 489)` radius ~178
- troggs 707/724 → centroid `(-6334, 483)` radius 372 → capped to 200

The bot `MoveTo`s the centroid, then roams within `radius` looking for the mob
(`FindNearestQuestCreature`). If the centroid lands in a low-density / wrong spot,
the bot wanders, the kill is slow, and eventually the **step-timeout watchdog skips
the whole quest** — the guide self-heals, but the bot loses that quest's XP and the
guide is less efficient than the hand-authored ones (which used tight, hand-picked
coords + radius ~60-140).

### Why it matters
Watchdog skips are a safety net, not a target. Every avoidable skip = lost quest XP
and a bot that looks like it's wandering. Tighter waypoints = more quests actually
completed = faster, more believable leveling.

### Fix approach (in `tools/gen_dbguide.py`)
Replace "global centroid" with "pick the spawn cluster the bot should actually go to":
1. Pull spawns as today (OR-match `id1/id2/id3`, group by map).
2. **Cluster** the spawns on the chosen map. Cheap approach: bucket by a coarse grid
   (e.g. 50-yd cells), find the densest cell, then take the centroid of spawns within
   ~80 yd of that densest cell. Set `radius` to that cluster's extent (clamp ~40-100).
3. **Anchor to the quest giver** when there's ambiguity: among candidate clusters,
   prefer the one nearest the giver's coords (most low-level kill objectives are near
   the giver), but DON'T hard-require it — some kill quests legitimately send you
   across the zone, so fall back to the densest cluster if none is near the giver.
4. Pass the giver coord into the resolver for kill entries so step 3 can use it.

### Gotchas
- Keep OR-matching `id1/id2/id3` in BOTH the spawn query and any centroid math, or you
  undercount pooled spawns.
- Some entries spawn on multiple maps; keep the existing "map with the most spawns" pick
  before clustering.
- Don't over-tighten: the executor needs SOME radius to find roaming mobs. ~60-100 is a
  good floor/ceiling.

### Verify
Regenerate the 3 guides, spot-check a few kill-step coords against known mob locations
(e.g. Coldridge troggs should be ~`(-6240, 560)`, not the zone centroid). Redeploy
(guides are runtime-loaded — just sync the YAML to
`/home/khuong/azerothcore-wotlk/modules/mod-idlebot/data/guides/...` and restart
`ac-worldserver`; NO rebuild needed). Watch for fewer
`exceeded ...s — skipping quest` lines and faster `step_index` advancement.

---

## 2. Zygor 55-80 converter (route JSON → guide YAML)

### Context
`tools/zygor_parse.py` already turned Zygor's Lua guides into 61 route JSONs in
`data/routes/{alliance,horde}/`. Classic 1-60 is mostly **dropped** (Zygor is
Cata-revamped — that's why classic guides come from the DB instead, see
`gen_dbguide.py`). But 55-80 coverage is **excellent** (cov=1.0): DK starter (55-58),
all Outland (60-70), all Northrend (70-80). Those routes carry the correct Zygor
quest ORDER but no coords (by design — "runtime resolves from DB by id").

We have the DB-resolution half already (in `gen_dbguide.py`). The converter is:
**route steps → resolve coords from DB → emit guide YAML**, reusing that resolution.

### Route step schema (output of zygor_parse.py)
Each route = `{id, title, faction, next, startlevel, endlevel, steps:[...]}`. Steps:
- `{action:"accept", quest, npc, only}` → `accept_quest` (npc→coords; if npc null use
  `creature_queststarter`).
- `{action:"turnin", quest, npc, only}` → `turn_in_quest` (npc→coords; if null use
  `creature_questender`).
- `{action:"kill", quest, obj, npc, count, name, only}` → `kill_mobs`
  `creature_ids=[npc]`, `completion_condition="quest_objective_complete:{quest}/{obj}"`,
  coords from npc spawn. **`obj` is already the 1-based objective index** — use it
  directly (the executor's `ParseQuestObjectiveCondition` expects 1-based).
- `{action:"collect", quest, obj, item, count, name, from_npc?, from_name?, only}` →
  `kill_mobs` on the droppers. Prefer `from_npc` if present (Zygor already named the
  mob); else resolve `item`→droppers via `creature_loot_template`. completion = the
  quest/obj.
- `{action:"use", quest, obj, go, name}` → `interact_gameobject` (go entry; coords from
  `gameobject` table — plain `id` column there).
- `{action:"fpath"|"hearth", ...}` → SKIP (the executor has no flight-path/hearth step;
  plain `MoveTo` between waypoints covers travel, just slowly). Optionally emit a
  `move_to` to the next step's coords as a travel hint.

### Implementation
- New tool `tools/zygor_route_to_guide.py`. Refactor the DB helpers out of
  `gen_dbguide.py` into a shared module (e.g. `tools/idlebot_db.py`: `q()`,
  `resolve_coords()`, starter/ender lookup, item→dropper) and import from both.
- Iterate a route's `steps` IN ORDER (Zygor order is better than level-sort — keep it).
- For chaining, either (a) follow `route["next"]` (a title string) to concatenate the
  whole 55→80 chain into ONE big guide per faction, or (b) emit one guide per route and
  rely on the "one big guide, re-run from step0 auto-skips done quests" pattern already
  in use. (a) is cleaner for 55-80 since it's linear.
- `faction` from the route; `race: any` (Outland/Northrend are faction-wide);
  `level_min/max` from `startlevel/endlevel`.
- Ignore `only` conditions (havequest/completedq) — the linear executor doesn't evaluate
  them and the ordering + AcceptQuest-skip + watchdog handle the edge cases.

### Gotchas
- Outland/Northrend have group/dungeon quests; Zygor's leveling routes already exclude
  most. If a quest can't be soloed it'll just time-out-skip (acceptable).
- `use` steps need the `gameobject` table; some "use" steps have `go:null` (item use, not
  a world object) — skip those.
- Can't live-test until a bot is ~55+. To test: pick a bot, `.levelup` it (or set
  `characters.level`) to 58/60, assign the generated Outland guide, restart, verify it
  accepts→kills→turns in. Or trust the logic since it reuses the proven gen_dbguide
  resolution path.

---

## 3. Login churn under heavy random-bot load  — ✅ SOLVED 2026-06-21

ROOT CAUSE (none of the theories below): a **GUID collision with mod-ah-bot-plus**.
`AuctionHouseBot.GUIDs` (mod_ahbot.conf) listed the idlebot bot characters
(2004/2006/2007) as auction sellers. Each AH cycle the AH bot does
`ObjectAccessor::AddObject`/`RemoveObject` on those guids (AuctionHouseBot.cpp ~1866/1902),
and the RemoveObject **evicts the live idlebot bot from the connected-player map** →
`FindConnectedPlayer` null → idlebot re-adds → silent ~60s churn (online flag stays 1,
no logout). Idlebot (2002) was outside the seller range, so it alone stayed stable.
FIX: (1) `AuctionHouseBot.GUIDs = 2003,2005` (real AH sellers only); (2) AH bot now skips
any seller GUID with a live FindConnectedPlayer before AddObject. The diagnosis below is
kept for history but was wrong.

### [historical] Login churn under heavy random-bot load

### Symptom
With `RandomBotAutologin=1` and ~1383 random bots logging in alongside the 4 idlebot
bots, the idlebot bots intermittently drop offline and get re-queued
(`[IdleBot] queued login for '<name>'` every ~10 ticks). Worst right after a
`docker compose up --force-recreate` (mass re-login storm). Bots still make progress,
but not steadily — some sit at step 0 churning while others quest.

### What's already done (don't redo)
Submodule `557a75b` added: (a) `botLoading` stale-entry expiry (a leaked in-flight login
no longer blocks a bot forever) and (b) `RandomPlayerbotMgr::ProcessBot` early-returns
for non-random (idlebot-managed) bots so the random manager doesn't churn/log them out.
idlebot also re-asserts force-active + strategies every ~15s. These fixed the *common*
case; the residual is load-contention churn.

### SHARPENED diagnosis (2026-06-21 session — narrowed, not yet pinned)
Hard data gathered:
- It is **per-character, not load**. Reduced MinRandomBots 1000→100 / MaxRandomBots
  1500→150 (in playerbots.conf; backup *.idlebak). Online bots ~1383→123, and the
  acore_characters async query backlog 5000+ → ~421. **The 3 fresh bots still churn**
  (~6 "queued login"/5min each = full disconnect ~1/min). So load is NOT the cause.
- It is **not the watchdog, not death, not the random pool, not the account.** No
  death/recover logs. IsRandomBot=false (accounts aren't rndbot). playerbots_random_bots
  event rows are IDENTICAL for all 4 bots. Idlebot, Idlepaladin, Idleshaman are ALL on
  account 201 — yet only **Idlebot stays online (1 churn/5min, 92 dbg, levels)** while
  Idlepaladin+Idleshaman churn (0 dbg, step0 idle). Idletest (acct 202, alone) also churns.
- The bots DO connect + get controlled + geared (saw "EnsureStarterGear 'Idletest'…"
  which only runs online+controlled), then fully disconnect ~1/min. So it's a **periodic
  teardown of the master-less bot session**, AFTER successful login, for these specific
  (repeatedly-reset, low-level) characters — but NOT for the established Idlebot.
- The ONLY observed differentiator is established-vs-fresh/reset: Idlebot has been the
  continuously-stable bot all along; the other 3 have been reset/re-added dozens of times.
  Suspect a corrupted/half-loaded playerbots session or `playerBots` map state specific to
  them (cf. the old ff54b303 "stale playerBots entry, AI erased" class of bug — possibly a
  variant not fully covered).

### Most promising next attempts
1. **Trace one bot's full login→disconnect cycle with Debug on** (Appender.Console=1,5 in
   worldserver.conf) and find the teardown call site — grep around LogoutPlayerBot /
   session removal / `OnBotLogout` / UpdateSessions in RandomPlayerbotMgr + PlayerbotMgr,
   add a log at the teardown with the trigger. This is THE missing fact.
2. **Fresh-create the 3 churning characters** (delete + recreate as new chars, or on brand
   new dedicated accounts per the [[idlebot-real-account-model]] one-account-per-bot model)
   to clear whatever corrupted per-character state they're in, and see if new chars stay
   online like Idlebot did. Cheap-ish A/B test that would confirm "it's the character state."

### Original diagnosis path (still valid)
The churn = the bot becomes NOT `FindConnectedPlayer`-findable (fully logged out), so
`GetLiveStatus` returns `online=false` and `TickBot` (~L287-298) calls `EnsureBotOnline`
again. So **something logs the bot out under load** (it's not just slow attach — the
e530b7e change already makes connected-but-not-in-world count as online). Find what:
1. Re-add the IBDIAG-style `LOG_INFO("server","[IBDIAG]...")` probes (see git history /
   the pattern used this session) at: `AddPlayerBot` entry + each early-return + callback;
   and at any `LogoutPlayerBot` / session-removal path the bot could hit. Add a log when
   a managed bot's session is torn down, with the call site.
2. Watch one bot through a full churn cycle: login queued → callback → in-world → (what
   tears it down?) → offline → re-queue. The teardown call site is the bug.
3. Suspects: a global connection/player cap; the world-thread operation queue dropping
   ops under backlog (5000+ pending acore_characters queries were seen at shutdown); a
   random-mgr global "log out N bots" path not gated by the non-random check; or the
   session being recycled because `account.online`/duplicate-session handling trips under
   load.

### Candidate fixes (pick based on diagnosis)
- **Cheapest, highest-impact: reduce the random-bot population.** 1383 autologin bots is
  the source of the contention. If the user doesn't need that many, lower
  `AiPlayerbot.RandomBotAccountCount` / autologin count in `playerbots.conf`. The idlebot
  bots are what matter; the random crowd just starves them. (Ask the user — this is a
  product decision, not purely technical.)
- **idlebot login backoff:** don't re-queue while a login is genuinely in flight. Add a
  bridge method `IsLoginPending(guid)` that checks playerbots' `botLoading`, and have
  `TickBot` skip `EnsureBotOnline` while pending (instead of the blind `loginRetryTicks=10`
  retry which adds to the pile-up). Also consider exponential backoff on repeated failures.
- **Prevent the teardown** (best, needs the diagnosis): once you know what logs the
  managed bot out, gate it on the managed/force-active flag (same idea as the ProcessBot
  early-return, applied to whatever path teardown is found in).

### Files
`modules/mod-playerbots/src/Bot/RandomPlayerbotMgr.cpp` (logout/ProcessBots paths),
`PlayerbotMgr.cpp` (AddPlayerBot/botLoading/UpdateSessions),
`modules/mod-idlebot/src/IdleBotManager.cpp` (`TickBot` retry cadence ~L287-298),
`IdleBotPlayerbotBridge.cpp` (`EnsureBotOnline`, `GetLiveStatus`),
`env/.../playerbots.conf` (random bot count / autologin) on zoidberg.

### Verify
Measure `docker logs --since 2m ac-worldserver | grep -c 'queued login'` — once settled
it should trend to ~0, and all active bots stay `online=1` with steadily advancing
`step_index`.

---

## Build / deploy / verify cheatsheet (zoidberg)
- C++ change → rebuild: sync changed files to `/home/khuong/azerothcore-wotlk`
  (`tar -cf - <paths> | ssh khuong@10.10.30.20 'tar -xf - -C /home/khuong/azerothcore-wotlk'`),
  then `cd /home/khuong/azerothcore-wotlk && DOCKER_BUILDKIT=1 docker compose build ac-worldserver`,
  then `docker compose up -d --no-deps --force-recreate ac-worldserver`.
- Guide-only change → NO rebuild: guides are runtime-loaded from the bind-mounted
  `data/guides`. Sync the YAML + `docker restart ac-worldserver`.
- Logging: AC levels are 1=Fatal..4=Info,5=Debug,6=Trace. The Console appender caps at 4,
  so DEBUG is hidden — raise `Appender.Console=1,5,...` in worldserver.conf to see Debug.
  `[IdleBot][dbg]` lines are LOG_INFO (the "[dbg]" is just text).
- Bot state: `acore_characters.idlebot_bots` (decision_mode, guide_id, step_index, active).
- Caveat: submodule `mod-playerbots` commit `557a75b` is local-only (its only remote is
  upstream, not writable). A writable fork remote is needed to push it; until then the
  live deploy relies on the tar-synced source, not `git submodule update`.
