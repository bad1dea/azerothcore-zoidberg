# Session Handoff

## Current milestone
Gate 2 — levels 1–6: every race completes its starting area; every
delivered class controller completes representative combat; kill/loot/GO/
use-item/gossip/vendor/training/death mechanics work. **Nine slices
complete and verified live** (Navigation, QuestEngine accept/turn-in,
Combat, Inventory, Recovery, Economy, Gossip, Growth). Gate 1 is fully
done. **Only remaining named Gate 2 item: broader race/class coverage**
(every slice above has only ever been exercised on one Orc Warrior).
Working toward Gate 5 per explicit user direction ("continue on your own
until we get to gate 5") — long, ongoing multi-session arc; see
"Decisions made" for how that's paced. Full per-slice history, bugs, and
non-bug findings are in `KNOWN_FAILURES.md` and `ARCHITECTURE.md`
(ADR-008 through ADR-017) — this file stays a live summary, not a growing
archive.

## What's proven, end to end, through real production code (not
## reimplemented or DB-shortcut)
A bot logs in at its correct racial spawn (no teleport) → walks via real
navmesh pathing → accepts a real quest → turns it in for real XP → fights
real creatures via real melee combat (a real "target moved out of range"
bug was found and fixed along the way, see `KNOWN_FAILURES.md` Gate 2 #1)
→ loots corpses via the real loot system (correctly honors quest-gated
drop rules) → can legitimately die (took real, escalating effort to
trigger — survived 3/5/8-creature deliberate pulls before finally dying
to a 4th) → releases spirit and reclaims its corpse (correctly reproduces
a real graveyard-lookup edge case) → can request to buy/repair at a real
vendor and browse/learn from a real trainer via real gossip (both
correctly blocked once by genuine insufficient-funds validation, not a
bypass — a positive "purchase/learn succeeds" test is still pending until
the bot legitimately earns some copper, which hasn't blocked verifying
the mechanisms themselves).

## Files changed (cumulative, this arc)
- `docs/autonomous-player/ARCHITECTURE.md`: ADR-008 through ADR-017.
- `docs/autonomous-player/KNOWN_FAILURES.md`, `TEST_MATRIX.md`,
  `ROADMAP.md`: updated throughout.
- New components: `Lifecycle/BotSessionMgr`, `Setup/PendingCharacterCreations`,
  `Navigation/BotNavigation`, `QuestEngine/BotQuestEngine`, `Combat/BotCombat`,
  `Inventory/BotLoot`, `Recovery/BotRecovery`, `Economy/BotEconomy`,
  `Gossip/BotGossip`, `Growth/BotGrowth` (all `.h`/`.cpp` pairs).
- `Commands/cs_autonomousplayer.cpp` now has ~20 debug commands (`provision`,
  `login`, `status`, `moveto`, `acceptquest`, `queststatus`, `turnin`,
  `attack`, `creaturestatus`, `loot`, `releasespirit`, `reclaimcorpse`,
  `attackguid` [unreliable, see below], `multipull`, `buy`, `repair`,
  `gossiphello`, `gossiptrain`, `learnspell`) — all debug-only triggers,
  not part of any automatic Planner/Executor loop (that doesn't exist yet).

## Verification
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`,
  `codestyle-cpp.py`: pass on every commit.
- Compiled clean on zoidberg 26 times across this arc (a few of those
  were fix-and-retry cycles for compile errors caught before ever
  reaching live testing — most-vexing-parse twice, once in Economy
  before learning the lesson and using brace-init from the start in
  Growth); currently deployed commit compiles clean.
- Every capability above verified **live** on zoidberg, not just compiled.

## Current repository state
- Branch: `mod-autonomous-player`. Most recent commits: `460dec5`
  (Gossip), `4fd2e09` (Growth), plus this handoff commit — all pushed to
  origin.
- zoidberg's live `ac-worldserver` is running the latest pushed commit.
- Test fixture: account `ap_test1` (id 204), character `Grunttestbot`
  (guid 2014, Orc Warrior, level 2, 0 copper), alive, full health, near
  Frang (`-639.3, -4230.2, 38.1` on map 1, Valley of Trials). Reusable.
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
None currently blocking. Full history (6 bugs in Gate 1, 1 in Gate 2 —
all fixed) plus several documented non-bug findings (quest interaction
range, quest-gated loot, no-graveyard-nearby ghost behavior, two
insufficient-funds rejections) is in `KNOWN_FAILURES.md`.

## Decisions made
- User's standing direction has escalated across this arc: "investigate
  how playerbots keeps its sessions alive - fix it and get to gate 3 on
  your own" → "keep going" → "keep going im sleeping" → "continue on your
  own until we get to gate 5." Interpreted as: keep working autonomously
  through this project's own bounded-increment/gate methodology
  indefinitely — design briefly, implement the smallest testable
  increment, compile-check, live-verify on zoidberg, update docs, commit,
  before starting the next thing — never a large unverified pile of code,
  only pausing for a genuine blocker or a decision only the user can
  make. This HANDOFF is kept current continuously now rather than treated
  as a one-time end-of-session artifact.
- Every opcode-reuse component follows the same pattern established in
  Gate 1: find the real public `WorldSession::Handle*Opcode`, feed it a
  synthesized packet (raw `WorldPacket` bytes, or this fork's structured
  `WorldPackets::*` classes where the opcode has been migrated to those —
  Economy and Growth are the first to hit the structured-packet style),
  and read any "what's available" state directly off the live server-side
  object (`Creature::loot`, `Creature::GetVendorItems()`,
  `GossipMenu::GetMenuItems()`, `Trainer::GetSpells()`) instead of parsing
  our own no-op outgoing packets. This has held up across 9 slices without
  needing a different approach.
- Reused the existing test fixture (`ap_test1`/`Grunttestbot`) throughout.
  It now has real quest/combat/loot/death/gossip/training history — a
  more realistic ongoing subject than resetting to a fresh character.

## NEXT TASK
Gate 2's last remaining item: **broader race/class coverage.** Provision
a second bot with a different race/class (a caster class — Mage or
Priest — is more valuable than another melee class, since it would
exercise real spell-casting, which the Combat component has never touched
— `Grunttestbot`'s Warrior only ever used melee autoattack) and run it
through a representative slice of the already-proven cycle: login at its
correct racial spawn, one real quest accept/turn-in, one real
kill+loot. Not every debug command needs re-exercising for the second
bot — the point is demonstrating breadth (a different race's starting
zone, a different class's starting kit/abilities), not redundant
re-verification of already-proven mechanics.

Design steps (same discipline as every slice so far):
1. Pick a race/class combo and look up its real starting zone/spawn
   position, first quest, and a nearby low-level hostile creature in the
   live world DB — don't guess from memory of WoW content.
2. If the chosen class can cast spells, casting itself may need a small
   `Combat` addition (`Unit::CastSpell` is already in the ADR-005
   allow-list, but this module hasn't called it yet) — investigate the
   real cast-request opcode (`CMSG_CAST_SPELL`/`HandleCastSpellOpcode`,
   check `SpellHandler.cpp` for the exact signature) before assuming it
   works the same way as melee attack.
3. Provision, compile-check, live-verify each step on zoidberg, update
   docs (new ADR only if spell-casting reveals a real design decision),
   commit.

Once this is done, **write a Gate 2 completion summary in `ROADMAP.md`**
(mark Gate 2 complete or precisely state what's still short of the bar)
before starting Gate 3 — per the project's own gate-based methodology,
don't skip straight into Gate 3 without that checkpoint.

## Next-session acceptance criteria
- A second bot (different race/class than Orc Warrior) is provisioned,
  logs in at its correct racial starting position (verified against the
  live world DB, not assumed), accepts and turns in one real quest, and
  kills+loots one real creature — all through the same real
  opcode-handler-reuse components already proven for the first bot.
- If the class can cast spells: at least one real spell cast verified
  live (target takes damage from a spell, not just melee).
- `check_no_playerbots_dependency.sh` and `check_no_forbidden_apis.sh`
  still pass.
- `ROADMAP.md` updated with a clear Gate 2 status (complete, or precisely
  what's short).
- Committed on `mod-autonomous-player`; `HANDOFF.md` updated.

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX}.md. Continue autonomously toward Gate 5 per
the user's standing instruction: complete HANDOFF.md's NEXT TASK (second
race/class coverage), then write a Gate 2 completion checkpoint in
ROADMAP.md before starting Gate 3. Design briefly, implement the smallest
testable increment, compile-check and live-verify on zoidberg
(build-and-deploy is pre-approved), update docs, commit. Keep going
without stopping to check in, except for a genuine blocker or an
ambiguous decision only the user can make.
