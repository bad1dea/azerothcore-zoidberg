# mod-idlebot — branch handoff

Branch: `idlebot-contested-go-deploy`. Repo: `/home/khuong/azerothcore-zoidberg`.
This file travels with the branch; it's the build/deploy/debug source of truth plus
the few things to keep an eye on. (Live ops detail also in the personal-memory notes:
`idlebot-deploy-source-of-truth`, `idlebot-core-crash-dependency`, `idlebot-deploy-runbook`,
`idlebot-status`.)

## Goal (achieved & validated)

mod-idlebot runs on **STOCK core + STOCK upstream mod-playerbots**, with exactly ONE
owned core change: a GUID-based visibility container that fixes a real fork
use-after-free. No other edits outside the module. `git submodule update` works
normally (mod-playerbots is unmodified upstream `085e127e`).

## Commit history (this effort, newest first)

- `b4c9b17` fix(idlebot): stop quest-objective false-advance + recover stuck turn-ins
  - (A) observed-loot fallback no longer fakes kill completion when the bridge can read
    real credit (contested/AoE kills shared with another bot were faking 6/6 at 3/6);
  - (B) `RewindToQuestObjectives` — a turn-in on an InProgress quest rewinds to its
    objective steps instead of relocating to the ender forever;
  - loop guard: a quest whose represented objectives all complete yet stays InProgress
    has an objective with no guide step (e.g. q375 item 2320, no dropper) — structurally
    undoable; skip after 2 bounded rewinds (emits a QUEST event, not FAILURE).
- `72f23e2` build against STOCK upstream mod-playerbots (no fork); randomize-suppression
  + "online-but-no-AI" recovery moved INTO the module bridge.
- `6517bcf` drop force-active / quest-first playerbots bridge hooks (no-ops now).
- `7b6faea` CORE FIX (B): `ObjectVisibilityContainer` stores GUIDs
  (`unordered_set<ObjectGuid>`), readers resolve via `ObjectAccessor` — dangling pointer
  impossible. The one intentional core edit; legit bug fix (upstream has no such cache).
- `86a8fee` no-skip-below-level-20: quests never abandoned at low level; relocate instead.
  (Exception: structurally-undoable quests are still skipped — see `b4c9b17` loop guard.)

Earlier objective-type coverage (still in history): gameobject-objective quests
(`be39404`, q6395 class), cast-on-creature quests (`01671f2`), survivability combat +
navmesh grounding (`54337ed`). Guide pipeline: `tools/chain_route.py` +
`tools/regen_guides.sh` regenerate all 10 Zygor 1-80 mega-guides from the committed
parses + DB.

## Access / ops

- `ssh khuong@10.10.30.20` (host "zoidberg").
- DB pw: `docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}' | grep MYSQL_ROOT_PASSWORD | cut -d= -f2`
- 4 bots: Idlebot(undead,guid 2002) Idletest(dwarf,2004) Idlepaladin(dwarf,2006) Idleshaman(tauren,2007).
- Bot tables in `acore_characters`: `idlebot_bots` (key is `bot_name`; `character_guid`
  is NULL), `idlebot_events` (FAILURE/RECOVERY/QUEST/…). Levels live in `characters`.

## Build / deploy (THE live pipeline — verified)

Build checkout on zoidberg: `~/build/azerothcore-zoidberg`. Live container runs image
`ghcr.io/bad1dea/ac-worldserver-zoidberg:latest` (== `:stockpb`) under compose project
`zoidberg-stack`.

Sync local → build tree (do sync and build as SEPARATE ssh calls):

    tar -czf - <files> | ssh khuong@10.10.30.20 'tar -xzf - -C ~/build/azerothcore-zoidberg'

Build (GOTCHA: containerd image store — MUST pass `--provenance=false --sbom=false` or the
image becomes an unrunnable manifest list):

    cd ~/build/azerothcore-zoidberg
    DOCKER_BUILDKIT=1 docker build --provenance=false --sbom=false --target worldserver \
      -t ghcr.io/bad1dea/ac-worldserver-zoidberg:latest -f apps/docker/Dockerfile .

(Warm ccache. A change touching `Object.h` cascades a ~15min recompile; module-only
changes are fast.)

Deploy:

    docker rm -f ac-worldserver
    docker compose -p zoidberg-stack --env-file ~/secrets/shared.env \
      --env-file ~/secrets/zoidberg.env -f ~/homelab/compose/zoidberg/compose.yml \
      up -d --no-deps ac-worldserver

Rollback: `:stockpb` stays pinned to the last-good sha; `docker tag` it to `:latest` and
recreate (also tagged `noskip-good-20260622`).

## Crash detection

The worldserver crash handler exits 0, so `RestartCount`/`ExitCode` LIE. Detect crashes
via core dumps:

    docker exec ac-worldserver ls -la /azerothcore/core*

gdb a core:

    docker run --rm -u 0 -v /tmp/core.1:/core.1:ro --entrypoint bash <stock-image> \
      -c 'apt-get update -qq && apt-get install -y -qq gdb && gdb -batch -ex bt \
          /azerothcore/env/dist/bin/worldserver /core.1'

## Clean-slate leveling test (2026-06-23, in progress)

All 4 bots were getting noisy/corrupted from repeated manual resets (over-leveled
Idlebot grinding gray content, Idleshaman inventory corruption + login churn). Per
owner direction, **wiped all 4 to fresh L1 / 0 quests** to watch a true full-picture
leveling run on Zygor and surface real struggle points.

Wipe (worldserver STOPPED, then start): per bot — `characters` level=1,xp=0,money=0 +
racial start position; `DELETE` from character_queststatus, character_queststatus_rewarded,
character_inventory, item_instance (clears item corruption), character_talent,
character_aura; `idlebot_bots` step_index=0/step_state=idle/death+train+spec counters=0.
Racial starts: undead (1676.71,1678.31,121.67) map0; dwarf (-6240.32,331.03,382.76) map0;
tauren (-2917.58,-257.98,53) map1.

Fresh boot verified clean: no inventory-corruption errors, all 4 online and questing
their first quests (Idlebot q3901, dwarves q179, tauren q747). Initial-spawn login churn
(~4 queued-login/bot) is the known tolerable kind. **Goal now: fix every struggle point
the bots hit (never skip — a real Zygor player gets through these), module-only or custom
routines.** Monitoring on ~20-min intervals.

Struggle points seen on the OLD (pre-wipe) state, to confirm/deny on the fresh run:
- collect quests where the bot fights the right dropper but itemcount stays 0 (q374:
  killed Scarlet Warrior 1535 which drops item 2875 @40%, progress stuck 0/10) — looks
  like a combat/kill-credit or loot reliability issue, NOT the loot allow-filter (that
  correctly permits quest items). Top suspect for the next fix if it recurs fresh.
- tauren racial chain talk/script quests (q755 "Rites of the Earthmother", no kill/collect
  objective) blocking follow-ups (q757) — needs a talk/gossip-complete routine.
- vendor-purchasable required items (q375 item 2320 "Coarse Thread", 21 such in 1-60) —
  needs a buy-item step/routine instead of the structurally-undoable skip.

## ⚠️ CRITICAL recurring trap: AH-bot GUID collision (login churn)

`mod-ah-bot-plus` uses real character GUIDs as auction sellers (`AuctionHouseBot.GUIDs`).
If that list INCLUDES idlebot's bot guids (2002/2004/2006/2007), every AH cycle does
`ObjectAccessor::AddObject/RemoveObject` on the live bot → evicts it from the connected-
player map → idlebot re-adds it → ~1×/min login churn → the bot can't make progress
(stuck at its current step, can't level). A bot whose guid is NOT in the list (e.g. 2002
when only 2004/2006/2007 collide) is stable, which is the tell.

The config is bind-mounted (`/home/khuong/acore/server/configs/modules/mod_ahbot.conf`,
NOT in git) so it REVERTS on config resets — this has bitten twice. **After any deploy/
config reset, verify:**

    docker exec ac-worldserver sh -c 'grep "^AuctionHouseBot.GUIDs" /azerothcore/env/dist/etc/modules/mod_ahbot.conf'
    # MUST NOT contain 2002,2004,2006,2007. Correct value: AuctionHouseBot.GUIDs = 2003,2005,2008,2009,2010

Fix when wrong: edit that file to exclude idlebot guids, clear the colliding listings
(`DELETE FROM acore_characters.auctionhouse WHERE itemowner IN (2002,2004,2006,2007);`),
then `docker restart ac-worldserver`. (2026-06-23: found GUIDs=2003..2010 incl. all bot
guids, ~53k listings owned by them; fixed → churn went to 0, stuck bots resumed leveling.)

## Watch / remaining

- **Dwarf/tauren leveling:** were hard-stuck for hours at a turn-in (FIXED in `b4c9b17`).
  As of deploy they rewound to objectives and are grinding (e.g. dwarves on q170 kills).
  Two dwarves share one starter camp, so q170 is slow (contested kills, 12 each) — confirm
  they clear it and pull away from L2. Idlebot (undead) is fine (L12+).
- **Login churn:** ~queued-login lines from self-healing logins (all bots end online). If a
  bot gets STUCK offline (repeated queued-login, never online), that's a leaked playerbots
  `botLoading` entry — recovery is a worldserver restart, or a ~10-line AddPlayerBot expiry
  patch (see `patches/README.md`).
- **Verify no unintended skips below 20:**
  `SELECT COUNT(*) FROM idlebot_events WHERE event_type='FAILURE' AND created_at>NOW()-INTERVAL 30 MINUTE;`
  (expect 0). Note: structurally-undoable quests (missing-objective, e.g. q375) are skipped
  intentionally and logged as a `QUEST` event, NOT a FAILURE.
- **gen_dbguide dropper gaps:** q375-style quests with an objective item that has no
  resolved dropper get no obj step → skipped at runtime. Could be reduced by improving
  dropper resolution in `tools/gen_dbguide.py` (some items are gathered/created, not dropped).
- **Cross-continent travel** remains the big unimplemented gap (MoveTo can't cross maps);
  expect the first hard wall there around L12–20.
