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
