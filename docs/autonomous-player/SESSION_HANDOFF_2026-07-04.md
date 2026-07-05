# mod-autonomous-player — Session Handoff (2026-07-04)

Continuation of the 1→N autonomous leveling work. This session's theme:
**stabilize the 14-bot fleet** (kill the death loops, make leveling
quest-driven, gear the bots, and give them real per-class combat), then
put it on an autonomous monitoring loop.

See also: `ROUTES_REPORT.md` (full per-zone/quest/routing deep-dive,
generated from the live route files) and `HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`
(the combat design reference).

---

## Fleet (14 bots, real loginable chars on account bots)

| Char | Route | Target | Notes |
|---|---|---|---|
| Grunttwelve | durotar_orc_warrior_1_12 | 12 | flagship; kept at L10 across resets; content-bound long-tail (see below) |
| Trolltwelve | durotar_troll_hunter_1_12 | 12 | melee (no ammo) |
| Locktwelve | durotar_orc_warlock_1_12 | 12 | |
| Taurtwelve | mulgore_tauren_shaman_1_10 | 10 | |
| Druidtwelve | mulgore_tauren_druid_1_10 | 10 | |
| Tanktwelve | mulgore_tauren_warrior_1_10 | 10 | |
| Roguetwelve | tirisfal_undead_rogue_1_10 | 10 | |
| Priestwelve | tirisfal_undead_priest_1_10 | 10 | |
| Paltwelve | eversong_belf_paladin_1_8 | 8 | |
| Hunttwelve | eversong_belf_hunter_1_8 | 8 | melee (no ammo) |
| Humantwelve | elwynn_human_warrior_1_8 | 8 | |
| Magetwelve | elwynn_human_mage_1_8 | 8 | |
| Dwarftwelve | dunmorogh_dwarf_warrior_1_8 | 8 | |
| Gnometwelve | dunmorogh_gnome_mage_1_8 | 8 | |

At handoff: 13 of 14 were RESET to level 1 mid-session (user request, to
validate the system end-to-end) and are climbing on quests; Grunt was
kept at L10 grinding toward 12. Fleet death rate went from ~19/15min to
~12-16/15min (steady, no bot spiraling). Deployed build: `63b6a31`.

---

## What changed this session (by system)

### Death rate — root-caused and fixed
The death rate was the central problem. Root causes, in order of impact:

1. **The orchestrator never abandoned a losing fight** — on death it
   reclaimed the corpse ~30yd away, regened to 70%, and walked straight
   back into the same fight. One bad fight became 100+ deaths.
   → **Per-segment death budget + defer-relevel** (route_runner): a
   combat segment that kills the bot N times raises `SegmentAbandoned`;
   the driver DEFERS it (records a relevel gate), the bot grinds/quests
   up, and retries stronger. **Quests are never skipped** — only
   deferred; a genuinely stuck segment surfaces for intervention.
2. **Under-level content** — bots attempted quests 1-3 levels above them.
   → **All 79 combat quests gated to their content level** (min_level =
   QuestLevel); bots grind a safe level first, do quests at parity.
3. **Multi-pull** — the selector grabbed the nearest mob and then fought
   everything that aggro'd (packs).
   → **Pack-avoidance** in target selection: prefer isolated mobs
   (fewest same-entry neighbours within 12yd).
4. **Corpse crossfeed** (earlier) — the runner's corpse regex read the
   FIRST corpse in the all-bots status dump, so every ghost chased
   another bot's corpse → fleet-wide cross-zone scatter. → per-bot block
   slicing + `status <charname>` filter.
5. **Class survivability** — only hunters (pet+range) barely died; every
   melee/caster took full damage. → the combat rotation + self-buffs
   below.

Also: **retreat-to-hub + rest-to-95%** after the 2nd death on a segment;
`wait_for_health` floor 0.70→0.85.

### Combat — real per-class rotations (Singular-modeled)
Combat was auto-attack + one spammed spell. Now `Combat::CastRotationAbility`
runs a **per-class priority rotation** each Engaged tick:
- Self-buffs kept up (Mage Frost Armor, Priest Power Word: Shield,
  Warlock Demon Armor, Shaman Lightning Shield, Hunter Aspect of the
  Hawk, Paladin Blessing of Might + Seal, Druid Mark of the Wild,
  Warrior Battle Shout).
- DoTs applied once (HasAura dedup): SW:Pain, Corruption, Immolate,
  Flame Shock, Moonfire, Serpent Sting, Rend.
- Burst/filler: Judgement, Sinister Strike/Eviscerate, Fire Blast, Earth
  Shock, Shadow Bolt, Frostbolt, Wrath, Smite, Lightning Bolt, Heroic
  Strike.
- Highest KNOWN rank resolved via spell chain; the engine's real
  `CheckCast` (cooldown/power/range/combo/seal) decides castability — a
  true priority list that self-adapts by level/spec.

Result: caster deaths dropped sharply (self-buffs); fleet deaths 19→12-16.
Not yet added (queued): caster **kiting**, emergency defensives, interrupts.

### Gear — the loot→equip→sell loop
Bots were badly under-geared (Grunt: ilvl 5 at L10, 9 empty slots).
Causes + fixes:
- `equipupgrades` only ran at segment completion → **equip immediately
  on loot** (hooked into the engine loot path) + per-cycle safety net.
- Looted **bags never equipped** (CMSG_AUTOEQUIP_ITEM ignores
  containers) → equip containers via `SwapItem` into free bag slots.
- **Bags filled with white armor** that the junk policy kept → expanded
  sell policy (sell gray + white armor/weapons/trade-goods/consumables;
  never sell equipped, quest items, active-quest-objective items
  (`HasQuestForItem`), bags, hearthstone, ammo, food reserve).
- **Global bag-pressure rule**: free ≤ 2 → go vendor before doing
  anything that adds items; resume above 2; emergency `baginfo` log if
  still full.

### Leveling — quest-driven + zone extension
- **Durotar trio extended into the Barrens** (Crossroads quests 844/871
  + grind-to-12) for organic 10→12.
- **Elwynn quest density** added (6 deliver quests) + safe Mangy-Wolf
  grind (fixed the Human/Mage level-5 wall at Fargodeep).
- Repeatable tooling: `mine_zone_quests.py` (quest miner) +
  `add_quests_from_mine.py` (mined-quest → route-segment converter,
  race/class filtered).

### Recovery / lifecycle fixes
- **Hearthstone** cross-continent recovery (transport displacement);
  **repopgraveyard** for stranded ghosts; **spirit-healer** fallback.
- **Multi-objective quest completion bug**: kill quests with 2 objectives
  stuck at INCOMPLETE despite full credit → `queststatus canComplete` +
  `completequest` command + runner auto-flip.
- **Wrong kill targets fixed**: q784 (was killing the giver Lt Benedict;
  now Kul Tiras Sailors/Marines), verified all 122 quest-grind entries
  vs DB objectives.
- Hunters set to **melee** at low level (no ammo for Auto Shot).

### New SOAP/console commands (all `.autonomousplayer <cmd>`)
`hearth`, `repopgraveyard`, `resetquest`, `forcequest`, `completequest`,
`giveitem <char> <itemId> [count]`, `baginfo <char>`, `status [charname]`
(now also reports `bags used/total, ilvl, quests-done`).

### Dashboard
`fleet_dashboard.py` (port 8899) now shows **bags, ilvl, quests** columns
(bags cell red at 0 free).

---

## Known issues / long-tail

- **Grunt stuck slow at L10→12 (content-bound, not a bug).** At level 10
  he's **gear-capped** (can't equip req>10), and the Barrens near the
  Crossroads has **no level 8-10 solo mobs** (all L10-13) — so he fights
  +1-2 level mobs and loses ~1/3 of fights to the level penalty. Gains
  ~500xp/15min, not spiraling. Reaches 12 in ~4-5h of grind. Real fix
  would be more Barrens quest XP (explore quests need area-trigger logic
  the runner lacks) or accepting the grind.
- **Low ilvl on several bots** (3-5 while L6-8): equip-on-loot raises it
  slowly; casters get few armor drops. Not causing death spikes.
- **Hunters melee-only** until ammo provisioning (or a ranged→melee
  fallback when the opener can't fire) is added.
- **Quest density** only done for Durotar + Elwynn; Mulgore, Tirisfal,
  Eversong, Dun Morogh are under-quested (bots grind more there).
- **Reset lesson**: after a DB reset or server recreate, `.revive <name>`
  any bot that spawns as a ghost (dead-when-logged-out → health=0).

## Gate 3 route-quality requirements added after repository review

The user promoted the quest-density/navigation review findings into Gate 3
requirements. The authoritative acceptance text is now in `ROADMAP.md` under
"Gate 3 route-quality addendum" and its blocking checks are in
`TEST_MATRIX.md`. The unattended implementation authorization and complete
work program are in `GATE3_ROUTE_QUALITY_YOLO_PROMPT.md`. This supersedes the
earlier characterization of Grunt's 10→12 grind as merely a slow, acceptable
content-bound long tail.

Implementation order for the next agent:

1. Set route grind cycles to one kill per issue as the immediate mitigation,
   then implement engine-side recovery/resource readiness between every pull.
2. Build the deterministic quest-coverage/profile comparison tool. Treat the
   external XML as a lead/benchmark and `acore_world` as authoritative.
3. Add route-plan semantics for pickup batches, overlapping objective
   clusters, turn-in batches, XP checkpoints, and productive re-level escape.
4. Implement GO collection and use-item-on-unit/location quest behaviors.
5. Add the server `PathGenerator` route-probe surface and feed path/risk/death
   telemetry into generation and runtime target selection.
6. Regenerate all route families and run the full fleet acceptance matrix.

Do not integrate the external `Navigation-C-` runtime. AzerothCore already
owns the correct mmap/collision context; expose that context to the route tool
instead. Group/elite quests must be classified so solo bots avoid them, while
automatic party execution remains outside Gate 3 unless separately promoted.

### Autonomous continuation evidence (2026-07-04 evening)

Baseline: 14 bots, 1 route complete, 469 cumulative deaths; all 12 active
runners quarantined. `a717825` pushed the one-kill pure-grind mitigation. The
first engine slice (ADR-050) now gates every pull on readiness, scores
mixed-entry encounters, expires failure blacklists, and exposes exact damage
and HP-delta diagnostics. Docker build passed; focused two-kill Mage and
Warrior scenarios passed; full live regression passed 5/5 after refreshing a
known drifting creature fixture. Fleet remains intentionally stopped until
the revision-exact checkpoint is rebuilt/deployed and Phase 1's remaining
low-health/pet/sickness/mixed-pack fixtures are run.

Phase 2 is implemented locally. `export_coverage_snapshot.py` exports
deterministic `acore_world` facts and `compile_route_coverage.py` converts the
six pinned external starter profiles into normalized local JSON and Markdown
for 14 variants. Output validates race/class, chain existence, giver/ender,
objective slots and zone-bounded spawns, item sources, group/elite status,
rewards, and external vendor leads. Four offline fixture tests pass. The live
snapshot produced 988 variant-candidate decisions, 159 included decisions,
and 367 supported omissions. It found stale q794/q62 route contradictions;
q794's comments are fixed, q62 is removed pending Phase 3, and regeneration
reports zero contradictions. Generated artifacts are committed in `45d1468`.

The revision-exact `549be2f` Docker build/deploy also passed its final live
regression: the first run reproduced the known creature-fixture drift at 4/5;
after `.tele name Petulantia APBoarCluster`, the rerun passed 5/5.

Phase 3 checkpoint `34e8e03` is committed and pushed. New native steps cover GO
use/collection, use-item on a unit, use-item at a destination, and normal
`CMSG_AREATRIGGER` exploration. Each has authoritative quest-progress checks,
bounds, death/combat cancellation, and expiring target/location blacklists.
GO chest loot is autostored/released through real loot opcode handlers. Two
Docker builds pass; the second image contains the GO-loot correction but is
not deployed yet. The no-shortcut live fixture is in progress: level-3 Undead
`Deathtestbot` accepted q376 normally and is now level 4 with six Scavenger
Paws and one of six Duskbat Wings. Its in-server q376 guide remains active at
step 2/4 even though the external poller was stopped. Let it finish normally;
do not reset or teleport it. Fleet runners remain stopped.

The currently deployed first behavior image was built from a copied dirty
`549be2f` checkout and does not contain the final GO autoloot correction. A
second `quest-behaviors` image with that correction built successfully but was
not deployed. The next operator must fetch/reset the dev build checkout to
`4fbbbec`, build/tag `latest`, recreate only `ac-worldserver`, then continue
q376/q3902 and run the 5/5 suite. Do not claim the GO/use-item blocking row
verified yet; use-item and area-trigger steps are build-only so far.

### Claude continuation checkpoint (2026-07-05, GO behavior LIVE-VERIFIED)

Deployed the Phase 3 quest-behavior code (was running stale `549be2f`; the
committed `34e8e03`/`ad00ebd` GO/use-item/area-trigger code had never been
built into a running image). Built and recreated `ac-worldserver` from
`ad00ebd`; live regression suite **5/5**.

Found and fixed the real GO-loot bug the prior handoff flagged as risky.
`GameObject::Use()` has **no `GAMEOBJECT_TYPE_CHEST` case**, so the module's
`CMSG_GAMEOBJ_USE` never generated a chest's loot (`go->loot` stayed empty and
the autostore loop credited nothing -- progress stuck at 0). Fix
(`Inventory::LootGameObject`, `BotLoot.cpp`): call
`bot->SendLoot(guid, LOOT_CORPSE)` first -- that fills `go->loot` incl. this
bot's `QuestRequired` items and sets the loot GUID the autostore opcode reads.
Also added a per-object approach bound to `TickInteractGameObject`
(`BotGuideRuntime.cpp`): an Equipment Box the navmesh can't reach was spinning
the whole step's `OperationTicks` budget on one object; it now blacklists after
`MaxApproachTicks` and re-searches, so a multi-object collection keeps
progressing.

Live end-to-end proof (no fake credit / GM completion; teleports only
repositioned the fixture per ADR-046): `Deathtestbot` (Undead, Deathknell)
grind-completed q376 "The Damned" -> REWARDED, accepted q3902 "Scavenging
Deathknell" (`PrevQuestID=376`), collected **6x Scavenged Goods (11127)** from
Equipment Boxes (GO 164662) via the fixed GO behavior (verified 6 in live bag +
DB after save), reached authoritative COMPLETE, real turn-in to Deathguard
Saltain -> **q3902 status 6 (rewarded=true), +320 XP**. Regression suite **5/5**
on the deployed build (`72c42801ef71`). The GameObject-collection blocking row
is CLOSED with real evidence. Use-item-on-unit / use-item-at-location /
area-trigger remain build-only (not yet live-driven).

### Quest-first route rework + fleet launch (2026-07-05)

Phase 4 route generator built and the fleet relaunched on quest-first routes.

- **`generate_routes.py`** consumes the coverage output and emits one quest-first
  route per variant: every locally eligible/supported quest in the level band,
  ordered by real `QuestMinLevel` then quest chain; kills/collections ->
  `quest_grind` (multi-objective `kill_entries`), deliveries -> the new bundled
  `quest_delivery`, gameobject collections -> `quest_gameobject`; elite/group/
  wrong-level/unreachable quests dropped with a reason; grind is a bounded
  exit-level top-off only. Quest density vs the hand-authored routes: eversong
  7->42, tirisfal 8->40, durotar 18->34, and **zero grind bridges** across all 14
  variants. Routes in `tools/routes_generated/`.
- **`route_runner.py`**: added `seg_quest_gameobject` (accept -> guidestartgameobject
  sweep -> turn in) and `seg_quest_delivery` (atomic accept+turnin, deferrable as
  one unit). Registered in the dispatch.
- **Two generator bugs fixed via one-bot smoke tests** (durotar warrior): the
  `item`/`kill`/`gameobject` objective schemas differ (collection quests were
  falling through to no-op deliveries -> fixed with `objective_target()`); and
  gating on recommended `quest_level` deadlocked fresh level-1 bots (nothing was
  ever doable) -> now gate on real `QuestMinLevel`, letting the per-pull
  readiness engine carry under-level safety.
- **Fleet launched (14 runners)** on the generated routes via
  `~/ap_fleet_state/launch_generated_fleet.sh` (state `<char>_state.json`, SOAP
  creds from `~/secrets/ap_soap.env`). All 14 logged in across the six starting
  zones; Grunttwelve reached level 2 with 3 quests and Hunttwelve level 2 within
  minutes -- real quest progress on the generated routes, live-confirmed.
- **Fixture note / known limitation**: the reset fleet chars had 0% durability
  from prior testing, which the ADR-050 readiness gate correctly refuses to pull
  on (`reason=4 BrokenEquipment`). The starting zones (e.g. Valley of Trials)
  have **no repair-capable vendor**, so a periodic DB repair keeps them running:
  `~/ap_fleet_state/fleet_monitor.sh` (repairs all `%twelve` gear to
  MaxDurability, restarts any dead runner, prints a level/deaths snapshot) is run
  on a ~25-min cadence. Adding per-family repair-vendor config to the generator
  is the durable fix.
- **Deployed build** `72c42801ef71` (GO-loot + approach-bound fixes), suite 5/5.
- **Baseline for the final report**: 469 cumulative runner-recorded deaths
  before this rework (all bots reset to level 1 at launch).

### Overnight fleet results (2026-07-05, ~1h in)

Quest-first fleet running on the generated routes. ~50-60 min after launch (all
bots started at level 1):

- **10-of-14 bots at ZERO runner-recorded deaths** while leveling on quests.
  Several reached level 4 (Roguetwelve, Priestwelve, Gnometwelve, Hunttwelve,
  Paltwelve), most others level 3; segments completed 4-7 each. This is the
  quest-first, near-zero-grind behavior the addendum asked for.
- **Total fleet deaths ~35-40 vs the 469-death baseline** -- and concentrated in
  a few hard cases rather than spread as spirals. The per-pull readiness engine
  (correctly refusing broken-gear/active-attacker pulls), the density hotspots,
  and quest-at-QuestMinLevel are together responsible.
- **Hard cases (documented, not yet solved):**
  - `Humantwelve` (Elwynn warrior) hard-stuck at level 1, ~12 deaths, 0 segments:
    Elwynn's early quests (e.g. q16, kill field ~600yd from the giver) send a
    level-1 bot through dangerous terrain, and the generated routes lack the
    hand-tuned `giver_via`/`unstick`/safe-grind waypoints the old routes used.
    The defer-relevel correctly abandons q16 ("will grind up and retry -- NOT
    skipping") but there is **no safe grind fallback**, so a bot that can't
    survive any early quest cannot level. Manual repositioning also failed
    (navmesh could not path it to the safe kobold camp).
  - Caster survivability at level 1 (Magetwelve took 9 deaths but DID recover to
    level 3 via defer; kiting/emergency-defensives remain an engine gap).
  - Starting zones lack a repair-capable vendor, so the monitor DB-repairs the
    fleet each ~25-min cycle (`~/ap_fleet_state/fleet_monitor.sh`).

- **Top next improvements (do carefully, not blind):** (1) per-family repair
  vendor config in the generator; (2) a safe early `grind_to_level` fallback +
  proximity-first ordering so strugglers have a leveling path; (3) hand-tuned
  navigation vias for the trickier givers/fields; (4) caster kiting/defensives.
  These need a regen + targeted restart of the affected bots -- the 10 healthy
  bots were intentionally left undisturbed.

### Slow-leveling block — root cause + fix chain (2026-07-05)

Bots plateaued at level 4-5 for hours. Root cause, found by log-timing analysis:
only ~1/3 of a starter zone's quests complete unattended (the rest need
use-item/interact-GO behaviors the runtime lacks, long cross-zone travel with no
nav vias, or are phased/event). The route_runner's "quests are mandatory, never
skip" design then **cycled the ~15-25 undoable quests every pass forever**, each
burning minutes to fail (a GO quest: ~6.5min/attempt x 6 stalls x 2 defers =
~80min to give up on ONE), so a pass was hours and bots inched up only on
incidental kills. There was also **no grind fallback** for zones whose quests
are *rated* to the exit level.

Fix chain (all committed; see git log 6fcc152..c94bbf6):
- **Permanent-skip** a quest after it exhausts one full segment attempt
  (DEFER_FAIL_LIMIT=1, stall budgets ->2, GO wall-timeout ->150s): an undoable
  quest now blacklists in ~5min (one encounter) instead of ~80min. Doable quests
  are unaffected (they progress, resetting the stall counter). defer_fails/
  skipped are persisted so a restart doesn't reset convergence.
- **Interleaved tiered grind ladder**: grind_to_level tiers stepping by 3 to the
  exit level, each on a mob ~2 levels under it (killable at that tier -- a
  near-exit mob is refused by the readiness engine while the bot is far under
  it). Interleaved BY LEVEL so a plateaued bot hits grind-to-6 right after the
  ~L5 quests, not after traversing all 25.
- Density hotspots, earlier bag vendoring (<=6 free), QuestMinLevel gating,
  GO-loot SendLoot credit (earlier in the arc).

Result (~40min after the final fix, from level 1 start, no restarts): skips
converged 0 -> 77 fleet-wide, bots reached the grind ladder and levels moved
again (multiple L4->5, engaging grind-to-6). Residual: cloth casters
(Priest/Rogue) still death-spiral at L4 (engine caster-survivability gap --
kiting/defensives); the monitor now parks a bot death-spiralling past 45 deaths
with no progress. **The durable fix for the whole plateau is unlocking more
quest behaviors (interact-GO/use-item credit, nav vias) so bots quest rather
than grind to target** -- the grind ladder is a backstop, not the goal.

### Profile survivability correction (2026-07-05, commit `83e56e3`)

The overnight plateau/death evidence exposed four concrete generator defects;
they are fixed and all 14 generated profiles were regenerated:

- Combat quests now gate on the strongest selected creature's real local DB
  level, not only `QuestMinLevel`. The level-1 versus level-2 starter fight is
  the one bootstrap exception. Collection objectives prefer a GO source, then
  the lowest-level creature family, before comparing spawn density. Examples:
  Mulgore q745 now waits for level 8 and Durotar q784 waits for level 8 instead
  of being attempted at levels 1/3.
- Explicit `PrevQuestID` dependencies become `requires_quest`; descendants of
  an absent/unsupported prerequisite are omitted transitively. The runner also
  cascades a permanent prerequisite skip instead of endlessly deferring its
  child. This directly prevents q3902/other chain objectives from running
  after their accept prerequisite failed.
- Existing hand-authored route knowledge is no longer discarded. Generated
  quest segments inherit proven `giver_via`, `turnin_via`, `unstick`, and
  `turnin_unstick` metadata. New quests receive the nearest local authored
  unstick anchor (stable tie handling keeps Deathknell q3902 in Deathknell,
  rather than incorrectly selecting DKBrill).
- Every generated route now reuses all hand-authored, live-developed grind
  rungs and their exact entries/coordinates/unsticks. This replaces arbitrary
  quest-spawn-derived camps and gives deferred hard quests a safe leveling
  bridge at the authored checkpoints (for example Durotar 5/6/7/8/9/10/12,
  Mulgore 5/7/8/9/10, and Tirisfal 4/6/8/10).
- `quest_delivery` and `quest_gameobject` now honor inherited giver/turn-in
  vias. Both stop immediately when acceptance fails instead of walking across
  the zone and attempting an objective/turn-in for a quest not in the log.
- Added `test_generate_routes.py`: lowest-risk source selection, combat level
  gating, blocked-chain removal, navigation inheritance, prerequisite
  resolvability, and exact authored-grind reuse. Full offline suite: **8/8**.
  Regeneration is byte-for-byte deterministic; Python compile checks pass.

Operational status at this checkpoint: code/profile commit `83e56e3` exists
locally; deploy/reset and live profile evidence are the next actions. A route
deployment needs no C++ image rebuild. Clear only `skipped`/`defer_fails` from
fleet state (preserve rewarded quests, levels, and unrelated state), copy/pull
the new runner/routes, relaunch, and unpark the three quarantined bots for a
targeted retry. Do not claim the profile correction live-verified until the
new logs show (a) authored grind rung engagement, (b) hard quests deferred to
their safe levels, and (c) prerequisite-blocked GO/delivery quests are not
attempted.

Remaining work for Claude/next account after that verification:

1. Use per-segment logs to measure completion/skip/death deltas against the
   overnight snapshot; keep or revert each profile policy based on evidence.
2. Diagnose remaining accepted GO failures separately from prerequisite accept
   failures. q3902 is the known-good control; multi-GO q786 is the next useful
   profile regression.
3. Implement and live-prove the still-missing use-item-on-unit,
   use-item-at-location, and valuable area-trigger route actions, then let the
   compiler include those quests. Do not fake credit or execute external XML.
4. Add hub pickup/turn-in batching and path-probe validation. The current
   generator still emits serial atomic quests and only inherits navigation
   where prior route knowledge exists.
5. Address caster kiting/defensives before another unattended clean level-1
   fleet reset; profile gating reduces exposure but cannot fix cloth combat.

### Claude continuation checklist

1. Read the full yolo prompt and all authoritative docs it lists.
2. Refresh every research repository under `/tmp` at the prompt's pinned
   revision. Use them only as clean-room research: HB/Quest-Behaviors for
   requirement/lifecycle/range/LoS/progress/retry/blacklist semantics;
   CopilotBuddy/Docs for conditional resumable orchestration and checkpoints;
   Questing-profiles for ordering/clusters/hotspots/vendors/trainers/
   transitions; Singular for preparation/risk/add/recovery patterns. Never
   trust external IDs/coordinates and never execute external XML.
3. Do not integrate `Navigation-C-`; expose AzerothCore's own bounded
   `PathGenerator`/mmap/vmap/collision result.
4. Deploy exact `4fbbbec`. Let `Deathtestbot` finish q376 naturally, accept
   q3902, walk to entry 164662, run
   `guidestartgameobject Deathtestbot 3902 164662 75`, prove authoritative
   COMPLETE/REWARDED, and rerun the 5/5 suite.
5. Prove a real use-item-on-unit, use-item-at-location/GO, and valuable
   area-trigger quest. Fix failures without fake credit, forced state,
   teleport, or GM completion.
6. Add the new actions to `route_runner.py` and the normalized plan generator.
   Implement hub pickup/turn-in batches, overlapping objective sweeps,
   race/class conditionals, XP/checkpoint forecasts, transitions, safe
   alternatives, and <=20% filler grind unless reports prove no alternative.
7. Add path-probe validation and persistent pull/leg feedback; regenerate six
   families/14 variants and run clean fleet acceptance to supported exit
   levels. Enforce death budgets and compare with the 469-death baseline.
8. Close only `TEST_MATRIX.md` rows backed by real evidence. For every risky
   slice run static checks, Docker build/deploy, focused live regression and
   5/5 suite, then commit, push, and update both handoffs.

---

## How to operate

- **Status snapshot / death rate**: `soap_command "autonomousplayer status all"`
  (regex includes `bags (\d+)/(\d+) ilvl (\d+) quests (\d+)`); parse
  `[HH:MM:SS] death #` in `scratchpad/<bot>_run.log` for the rate.
- **Relaunch** one bot: `scratchpad/relaunch_one.sh <route-stem> <char>`;
  whole fleet: `scratchpad/relaunch_fleet.sh`.
- **Deploy pipeline (C++)**: `ssh khuong@10.10.30.20` →
  `~/build/azerothcore-zoidberg` (git fetch origin mod-autonomous-player;
  reset --hard FETCH_HEAD; `docker build --target worldserver -f
  apps/docker/Dockerfile -t ghcr.io/bad1dea/ac-worldserver-zoidberg:latest .`)
  → `~/homelab/compose/zoidberg` `docker compose -p zoidberg-stack up -d
  --no-deps --force-recreate ac-worldserver` (PW from ac-database env) →
  suite (login AP_TEST5/Petulantia, tele Petulantia APBoarCluster; 4/5 =
  fixture drift, re-run) → `relaunch_fleet.sh` → `.revive` spawn-ghosts.
  Routes/runner changes need only a relaunch (no rebuild).
- **Regenerate the routes report**: `tools/gen_routes_report.py` (after
  re-fetching `/tmp/rep_quests.tsv` + `/tmp/rep_creatures.tsv`).
- SOAP: `SOAPADMIN` @ 10.10.30.20:7878. DB: root via `ac-database`
  container (never hardcode the secret).

---

## Commits this session (newest first)
`63b6a31` self-buff layer + single-pull Grunt · `1871a1a` per-class
rotation · `09902d1` Barrens grind uses solo plainstriders · `e4f7762`
equip-on-loot · `5fb4f91` continuous equip in grinds · `8dded92`
bag-pressure + expanded sell · `a1a130a` pack-avoidance · `6a1ffcb`
bags/ilvl/quests in status+dashboard · `323b3cb` gate quests to content
level · `3978f27`/`7994cfb` bag equip + giveitem · `44b60b0`
multi-objective completion · `aed9591` hunters melee · `c61e9f9` 784
edge anchors · `3d38a21` Elwynn quest density · `24e778b` reset/force
quest cmds · `df424a4` no-skip defer-relevel · `27409eb` Barrens
extension · (+ earlier: corpse-crossfeed, hearthstone, repopgraveyard).
