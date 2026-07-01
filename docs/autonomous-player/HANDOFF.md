# Session Handoff

## Current milestone
Gate 2 — levels 1–6: every race completes its starting area; every
delivered class controller completes representative combat; kill/loot/GO/
use-item/gossip/vendor/training/death mechanics work. **In progress.**
Three slices complete and verified live this session (Navigation,
QuestEngine accept, QuestEngine turn-in). Gate 1 (first complete quest,
first slice) is fully done.

## Completed this session (very long session — summary; full bug-by-bug
## history is in KNOWN_FAILURES.md, don't re-read this file for that)

**Gate 1, first slice — COMPLETE.** Bot account/character/session model
(ARCHITECTURE.md ADR-008): dedicated `ap_`-prefixed bot account, a
`sock = nullptr, is_bot = true` `WorldSession`, character creation/login
through real production code paths. Found and fixed six real bugs via live
testing on zoidberg (all in `KNOWN_FAILURES.md` with commit citations):
Playerbots-bot false-positive detection, account-name length limit,
session not surviving world ticks, a use-after-free in the first fix
attempt for that, login rejected by client-only gatekeeping
(`_legitCharacters`), case-sensitive account-ownership check. Final live
verification: `Grunttestbot` (Orc Warrior, level 1) online at Valley of
Trials (map 1, `-618.5, -4251.7`), correct perception snapshot, no
teleport calls anywhere.

**Gate 2, slice 1 — Navigation — COMPLETE.**
`Navigation::MoveTo(Player*, x, y, z)` wraps
`MotionMaster::MovePoint(..., generatePath=true)` (ADR-009). Verified
live: bot walked from spawn to `(-598.5, -4251.7, 39.0)`, Z snapped from
the requested `38.7` to the real terrain height `39.0` — proof this is
real navmesh pathing, not a teleport.

**Gate 2, slice 2 — QuestEngine accept — COMPLETE.**
`QuestEngine::RequestAcceptQuest` reuses the public
`WorldSession::HandleQuestgiverAcceptQuestOpcode` via a synthesized
`CMSG_QUESTGIVER_ACCEPT_QUEST` packet (ADR-010) — every real acceptance
rule (prerequisites, exclusive groups, race/class/level, distance) runs
for real instead of being re-derived. Target content (creature 10176
Kaltunk, quest 4641 "Your Place In The World") found by querying
zoidberg's live world DB, not recalled from memory. **Live-testing
gotcha, worth remembering:** `FindNearestCreature`'s 30-yard search radius
(for *locating* an NPC) is much larger than the actual interaction range
the accept handler enforces — first attempt at ~8.6 yards silently failed
(`QUEST_STATUS_NONE`, no error logged), succeeded at ~1-2 yards. Expected
game behavior (a real player has to walk up too), not a bug, but future
quest-walk logic needs to target real interaction range.

**Gate 2, slice 3 — QuestEngine turn-in — COMPLETE.**
`QuestEngine::RequestChooseReward` reuses the public
`WorldSession::HandleQuestgiverChooseRewardOpcode` (the function that
actually calls `Player::RewardQuest`) via a synthesized
`CMSG_QUESTGIVER_CHOOSE_REWARD` packet (ADR-011) — deliberately bypasses
`HandleQuestgiverCompleteQuest`, which only sends UI-display packets (a
no-op for a socketless bot, grants nothing). Turn-in NPC (creature 3143,
Gornek) found via `creature_questender` in the live world DB. **Verified
live end to end:** quest status went `QUEST_STATUS_COMPLETE` (1) →
`QUEST_STATUS_REWARDED` (6), `IsQuestRewarded` true, XP `0` → `40`,
confirmed both via the in-game command and directly in
`character_queststatus_rewarded`. The full quest lifecycle (accept →
complete → turn-in → reward) now works end-to-end through real production
code paths, on a real online bot, with real movement between each step.

`.autonomousplayer moveto`/`acceptquest`/`turnin`/`queststatus` debug
commands exist for triggering/verifying each of these ahead of any
Planner/Executor loop (that loop is Gate 2/3's remaining work — nothing
here runs automatically yet).

## Files changed
- `docs/autonomous-player/ARCHITECTURE.md`: ADR-008 (full Gate-1 bug
  history), ADR-009 (Navigation), ADR-010 (QuestEngine accept), ADR-011
  (QuestEngine turn-in).
- `docs/autonomous-player/KNOWN_FAILURES.md`, `TEST_MATRIX.md`,
  `ROADMAP.md`: updated throughout.
- New: `modules/mod-autonomous-player/src/Lifecycle/BotSessionMgr.{h,cpp}`,
  `Setup/PendingCharacterCreations.{h,cpp}`,
  `Navigation/BotNavigation.{h,cpp}`,
  `QuestEngine/BotQuestEngine.{h,cpp}`.
- Updated: `Setup/BotProvisioning.{h,cpp}`, `Lifecycle/BotLogin.{h,cpp}`,
  `Commands/cs_autonomousplayer.cpp` (now: `provision`, `login`, `status`,
  `moveto`, `acceptquest`, `queststatus`, `turnin`),
  `AutonomousPlayerModule.cpp`.

## Verification
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`,
  `codestyle-cpp.py`: pass on every commit this session (13 commits total).
- Compiled clean on zoidberg 13 times this session; final state (commit
  `2a883d7`, currently deployed) compiles clean.
- All capabilities (online bot, movement, quest accept, quest turn-in)
  verified **live** on zoidberg, not just compiled — see above and
  `TEST_MATRIX.md`.

## Current repository state
- Branch: `mod-autonomous-player`. This session's commits, oldest to
  newest: `5d5840f`, `fc631d0`, `3d49e7f`, `8ada2d4`, `d83ea84`, `a31afa6`,
  `2266a4a`, `871e98b`, `66ba78f`, `2a883d7`, plus this handoff commit —
  all pushed to origin.
- zoidberg's live `ac-worldserver` is running commit `2a883d7`.
- Test fixture on zoidberg: account `ap_test1` (id 204), character
  `Grunttestbot` (guid 2014, Orc Warrior, level 1, 40 XP), currently
  positioned near Gornek (north of Valley of Trials), quest 4641 fully
  completed and rewarded. Reusable for the next session — either continue
  its story (it's now "finished" the very first quest, so the next
  natural step for it would be combat/leveling) or provision a second bot
  for parallel testing.
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
None currently blocking. Full bug history (6 in Gate 1, all fixed) is in
`KNOWN_FAILURES.md`. One documented non-bug gotcha: quest-accept/turn-in
require real interaction range, not just "nearby" (see above).

## Decisions made
- User gave explicit standing direction: "investigate how playerbots
  keeps its sessions alive - fix it and get to gate 3 on your own," then
  "keep going" after a first checkpoint. Interpreted throughout as: work
  autonomously through this project's own bounded-increment/gate
  methodology (commit + live-verify every slice), not a license to
  produce a large unverified pile of code.
- Stopping this session after quest turn-in (3 Gate 2 slices total, plus
  full Gate 1) rather than starting Combat. This was an extremely long
  session (13 build/deploy cycles, six Gate-1 bugs found and fixed, four
  new components built and verified). Combat is Gate 2's largest
  remaining piece and explicitly needs its own ADR (the project spec's
  "Combat requirements" section is substantial) — starting it now, deep
  into an already-long session, risked exactly the sprawl-without-a-
  checkpoint failure mode the operating-mode rules exist to prevent.
  Finishing the quest lifecycle (accept→complete→turnin→reward, fully
  verified live) is a clean, complete, well-tested unit to stop on.

## NEXT TASK
Gate 2, slice 4: minimal single-target melee combat for the Warrior class
controller — the first slice of the `Combat` component.

Before writing any code:
1. Read the project's full "Combat requirements" section (root operating
   instructions / originating task spec) — the common engine's stated
   obligations (legal target validation, objective relevance, danger
   assessment, pack density, safe approach, adds/CC/interrupts, health/
   resource/cooldown/range/facing/LoS, post-combat loot, encounter
   history) are much broader than this first slice; don't try to build
   all of it at once.
2. Scope the first slice down hard, matching how every other slice this
   session was scoped: "bot melee-attacks one already-selected,
   already-adjacent, low-level hostile creature until it or the target
   dies, no pulling/kiting/CC/adds handling yet." That's still a real
   `Combat` component (not a toy), just the smallest legitimate vertical
   slice of it.
3. Investigate the real combat APIs before assuming: `Unit::Attack`/
   `Unit::AttackerStateUpdate`/auto-attack timers, `Unit::SetInCombatWith`,
   how melee auto-attack actually starts for a real player (likely
   `Player::Attack` or the client sends `CMSG_ATTACKSWING` — check if
   that's another opcode-handler-reuse opportunity, same pattern as
   quest accept/turn-in, before hand-rolling attack logic). Same
   discipline as every other slice: verify via code, not memory of how
   combat "usually" works.
4. Verify live on zoidberg: bot engages a real low-level creature near its
   current position, deals damage, creature or bot dies, confirm via
   perception snapshot (health changing, `combat=true` then `false`) and
   world DB/logs, not just "the command didn't error."
5. Update ARCHITECTURE.md (new ADR), KNOWN_FAILURES.md if bugs are found,
   TEST_MATRIX.md, HANDOFF.md, commit — same pattern as every slice this
   session.

Do not attempt loot, vendor, training, death/recovery, or additional
races/classes in the same session as this slice — those are separate
Gate 2 slices, tracked in `ROADMAP.md`'s backlog.

## Next-session acceptance criteria
- A real hostile creature's health visibly decreases across several
  perception snapshots after the bot engages it via a real attack API
  (not `SetHealth`/`Kill` shortcuts), ending in the creature's death (or
  the bot's, if it picked something too strong — either is fine as a
  first proof, but pick an appropriately weak target).
- `combat=true` appears in the perception snapshot during the fight and
  returns to `false` after.
- Verified live on zoidberg, not just compiled.
- `check_no_playerbots_dependency.sh` and `check_no_forbidden_apis.sh`
  still pass.
- Committed on `mod-autonomous-player`; `HANDOFF.md` updated.

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX}.md, then the project's "Combat requirements"
section. Complete Gate 2 slice 4 (minimal single-target melee combat) per
HANDOFF.md's NEXT TASK: design briefly, implement the smallest testable
increment, compile-check and live-verify on zoidberg (build-and-deploy is
pre-approved), update docs, commit. Do not attempt loot/vendor/training/
death or additional races/classes, and do not begin Gate 3, in the same
session unless this task, its live verification, documentation, and
commit are all done and substantial budget clearly remains.
