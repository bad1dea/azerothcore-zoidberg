# Session Handoff

## Current milestone
Gate 2 — levels 1–6: every race completes its starting area; every
delivered class controller completes representative combat; kill/loot/GO/
use-item/gossip/vendor/training/death mechanics work. **In progress.**
Six slices complete and verified live (Navigation, QuestEngine accept,
QuestEngine turn-in, Combat, Inventory, Recovery). Gate 1 is fully done.
Working toward Gate 5 per explicit user direction ("continue on your own
until we get to gate 5") — this is a long, ongoing multi-session arc; see
"Decisions made" for how that's being paced.

## Completed so far (summary — full bug-by-bug history is in
## KNOWN_FAILURES.md, don't re-read this file for that)

**Gate 1, first slice — COMPLETE.** Bot account/character/session model
(ADR-008). Six real bugs found+fixed via live testing. `Grunttestbot`
(Orc Warrior) online at Valley of Trials, no teleport calls anywhere.

**Gate 2, slice 1 — Navigation — COMPLETE.** `Navigation::MoveTo` wraps
`MotionMaster::MovePoint` (ADR-009). Real navmesh pathing, verified live.

**Gate 2, slices 2-3 — QuestEngine accept/turn-in — COMPLETE.**
`RequestAcceptQuest`/`RequestChooseReward` reuse real opcode handlers
(ADR-010/011). Full quest lifecycle verified live end-to-end (quest 4641,
XP 0→40).

**Gate 2, slice 4 — Combat — COMPLETE.** `RequestAttack` reuses
`HandleAttackSwingOpcode` (ADR-012). Found+fixed a real bug live:
`Unit::Attack()` doesn't chase a moving target — added
`MotionMaster::MoveChase` (commit `bf10901`).

**Gate 2, slice 5 — Inventory — COMPLETE.** `LootCorpse` reuses the real
loot opcode handlers (ADR-013), reads `Creature::loot` directly. Verified
live; investigated an apparent loot miss and confirmed it was correct
quest-gated behavior, not a bug.

**Gate 2, slice 6 — Recovery — COMPLETE.**
`RequestReleaseSpirit`/`RequestReclaimCorpse` reuse
`HandleRepopRequestOpcode`/`HandleReclaimCorpseOpcode` (ADR-014).
**Verified live, genuinely end-to-end:** triggering a real death took
real effort — the bot survived three escalating deliberate multi-pulls
(3, 5, 8 simultaneous Scorpid Workers, via a new debug `multipull`
command built on `WorldObject::GetCreatureListWithEntryInGrid`), even
leveling up to 2 mid-fight from the XP. Death only happened on a 4th
attempt (mixed pull: 2 Scorpid Workers + 1 Mottled Boar, hp
59→34→15→0). Release-spirit worked; **a genuinely interesting non-bug
finding**: the ghost stayed at the death location instead of moving to a
graveyard, because this death happened in open wilderness with no
registered graveyard nearby — confirmed via code review that
`Player::RepopAtGraveyard()` itself says "if no grave found, stay at the
current location," so this is a faithful reproduction of real core
behavior, not a defect. Waited the real ~30-40s reclaim delay, then
`RequestReclaimCorpse` succeeded: `alive=true`, `ghost=false`, full
health, corpse cleared.

**`PerceptionSnapshot` extended** with `IsGhost`/`HasCorpse`/`CorpseX/Y/Z`
(natural Gate-2 addition to the existing ADR-002 struct, needed for any
future Recovery-aware Planner logic).

**Full arc verified live, end-to-end, through real production code:** a
bot logs in at its correct spawn → walks → accepts a real quest → walks →
turns it in for real XP → walks → fights real creatures to death via real
combat (with a real bug found and fixed) → loots corpses via the real
loot system (with real quest-gating respected) → eventually dies for
real → releases spirit (with a real graveyard-lookup edge case correctly
handled) → walks back if needed → reclaims its corpse and resurrects.
Every step goes through actual AzerothCore production code, never a
reimplementation or a database/GM shortcut.

## Files changed (cumulative, this arc)
- `docs/autonomous-player/ARCHITECTURE.md`: ADR-008 through ADR-014.
- `docs/autonomous-player/KNOWN_FAILURES.md`, `TEST_MATRIX.md`,
  `ROADMAP.md`: updated throughout.
- New: `Lifecycle/BotSessionMgr.{h,cpp}`,
  `Setup/PendingCharacterCreations.{h,cpp}`, `Navigation/BotNavigation.{h,cpp}`,
  `QuestEngine/BotQuestEngine.{h,cpp}`, `Combat/BotCombat.{h,cpp}`,
  `Inventory/BotLoot.{h,cpp}`, `Recovery/BotRecovery.{h,cpp}`.
- Updated: `Setup/BotProvisioning.{h,cpp}`, `Lifecycle/BotLogin.{h,cpp}`,
  `Perception/PerceptionSnapshot.h`, `Perception/PerceptionBuilder.cpp`,
  `Commands/cs_autonomousplayer.cpp` (now: `provision`, `login`, `status`,
  `moveto`, `acceptquest`, `queststatus`, `turnin`, `attack`,
  `creaturestatus`, `loot`, `releasespirit`, `reclaimcorpse`,
  `attackguid` [unreliable, see below], `multipull`),
  `AutonomousPlayerModule.cpp`.

## Verification
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`,
  `codestyle-cpp.py`: pass on every commit.
- Compiled clean on zoidberg 21 times across this arc; currently deployed
  commit compiles clean.
- Every capability (online bot, movement, quest accept/turn-in, combat,
  loot, death/recovery) verified **live** on zoidberg, not just compiled.

## Current repository state
- Branch: `mod-autonomous-player`. Most recent commits:
  `19a0cfd` (Recovery), `dde8a6b` (attackguid, unreliable), `1d94930`
  (multipull, the reliable multi-target debug tool), plus this handoff
  commit — all pushed to origin.
- zoidberg's live `ac-worldserver` is running the latest pushed commit.
- Test fixture on zoidberg: account `ap_test1` (id 204), character
  `Grunttestbot` (guid 2014, Orc Warrior, **level 2** now, from combat
  XP), alive, full health, near `-456.7, -4175.7, 46.6` on map 1 (open
  wilderness north of Valley of Trials — no graveyard nearby, see
  Recovery finding above). Reusable for future sessions.
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
None currently blocking. Full bug history (6 in Gate 1, 1 in Gate 2 — the
MoveChase combat stall — all fixed) is in `KNOWN_FAILURES.md`. Documented
non-bug findings: quest-accept/turn-in need real interaction range; loot
respects quest-gating; release-spirit correctly leaves the ghost in place
when no graveyard is registered nearby. One known tooling wart (not a
module bug): `.autonomousplayer attackguid`'s low-GUID parameter doesn't
reliably match this fork's runtime GUID assignment when guessed from the
static `creature` spawn table — use `multipull` instead for targeting
specific/multiple creatures in tests.

## Decisions made
- User's standing direction has escalated across this arc: "investigate
  how playerbots keeps its sessions alive - fix it and get to gate 3 on
  your own" → "keep going" → "keep going im sleeping" → "continue on your
  own until we get to gate 5." This is now an explicit, very long-range
  instruction. Interpreted as: keep working autonomously through this
  project's own bounded-increment/gate methodology indefinitely — design
  briefly, implement the smallest testable increment, compile-check,
  live-verify on zoidberg, update docs, commit, before starting the next
  thing — never producing a large unverified pile of code, and only
  pausing to report back for a genuine blocker or a decision only the
  user can make. Every commit so far has been individually compiled and
  live-tested. This HANDOFF is being kept current continuously rather
  than treated as a one-time end-of-session artifact, since the arc
  itself is intentionally not stopping at a fixed point anymore.
- Triggering the test death required real, escalating effort (not a
  shortcut) — this is itself a meaningful confirmation that starting-zone
  content is appropriately safe for a legitimately-played level 1-2
  character, consistent with the player-like policy's spirit.
- Reused the existing test fixture (`ap_test1`/`Grunttestbot`) throughout
  rather than resetting it — it's now level 2 with real quest/combat/loot/
  death history, which is a more realistic ongoing test subject than a
  fresh character each time.

## NEXT TASK
Continuing toward Gate 2 completion (then Gate 3). Remaining named Gate 2
mechanics not yet touched: **gossip**, **vendor/repair**, **training**,
plus **broader race/class coverage** (only Orc Warrior exercised so far).
Pick the next one (no strong ordering constraint — vendor/repair and
gossip are natural next opcode-reuse slices given the established
pattern; broader race/class coverage is more about breadth than new
mechanics). Investigate the real opcode handlers first (gossip:
`CMSG_GOSSIP_HELLO`/`HandleGossipHelloOpcode`,
`CMSG_GOSSIP_SELECT_OPTION`/`HandleGossipSelectOptionOpcode` looked at
briefly this session, not yet used; vendor:
`CMSG_LIST_INVENTORY`/`CMSG_BUY_ITEM`/`CMSG_SELL_ITEM`/repair opcodes —
check `NPCHandler.cpp`/`TradeHandler.cpp` for exact names before
assuming), scope the first slice down hard, implement, compile-check,
live-verify on zoidberg, update docs (new ADR), commit — same pattern as
every slice so far.

## Next-session acceptance criteria
Depends on which slice is chosen — define specific, concrete, observable
criteria before starting (exact DB/log/perception-snapshot evidence, not
"should work"), following the style used for every slice so far.

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX}.md. Continue autonomously toward Gate 5 per
the user's standing instruction: pick the next Gate 2 slice from
HANDOFF.md's NEXT TASK, design briefly, implement the smallest testable
increment, compile-check and live-verify on zoidberg (build-and-deploy is
pre-approved), update docs, commit. Keep going slice by slice without
stopping to check in, except for a genuine blocker or an ambiguous
decision only the user can make.
