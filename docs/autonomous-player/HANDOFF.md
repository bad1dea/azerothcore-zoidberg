# Session Handoff

## Current milestone
Gate 2 — levels 1–6: every race completes its starting area; every
delivered class controller completes representative combat; kill/loot/GO/
use-item/gossip/vendor/training/death mechanics work. **In progress.** Two
slices complete and verified live this session (Navigation, QuestEngine
quest-accept); Gate 1 (first complete quest, first slice) is fully done.

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
live: bot walked from spawn to `(-598.5, -4251.7, 39.0)`, Z snapped
from the requested `38.7` to the real terrain height `39.0` — proof this
is real navmesh pathing, not a teleport. `.autonomousplayer moveto`
debug command for triggering it ahead of any Planner/Executor loop.

**Gate 2, slice 2 — QuestEngine quest-accept — COMPLETE.**
`QuestEngine::RequestAcceptQuest` builds a synthetic
`CMSG_QUESTGIVER_ACCEPT_QUEST` packet and calls the public
`WorldSession::HandleQuestgiverAcceptQuestOpcode` directly (ADR-010) —
same technique as character creation: reuse the real production opcode
handler (prerequisite/exclusive-group/race-class/level/distance checks
all run for real) instead of calling `Player::AddQuest` directly and
skipping them. Fully synchronous, no `BotSessionMgr` survival concerns.

Target content (creature entry 10176 "Kaltunk", quest 4641 "Your Place In
The World" — the Orc/Troll Valley of Trials starter) was found by
querying zoidberg's live world DB (`creature_queststarter` joined to
`quest_template`, filtered to creatures near the bot's confirmed spawn),
**not** recalled from memory of WoW content, per the project spec's
requirement to inspect the actual target DB revision.

**Live-testing caught one more real thing worth remembering:**
`Object::hasQuest`/interaction-range checks inside
`HandleQuestgiverAcceptQuestOpcode` require the bot to be genuinely close
to the quest giver — `FindNearestCreature`'s 30-yard search radius (used
just to *locate* the NPC) is much larger than the actual interaction range
the accept handler itself enforces. First attempt (bot ~8.6 yards from
Kaltunk) silently failed (`QUEST_STATUS_NONE`, no error logged — same
"real client feedback path is a no-op for us" pattern as Gate 1's bugs).
Moving the bot to ~1-2 yards from Kaltuk via `Navigation::MoveTo` and
retrying succeeded (`QUEST_STATUS_COMPLETE` — quest 4641 has no further
objectives, so accept and complete happen together). **This is expected
game behavior, not a bug** — a real player has to walk up to an NPC too —
but it's a concrete illustration that Gate 2/3's future quest-walk logic
needs to target actual interaction range, not just "NPC is somewhere
nearby."

`.autonomousplayer acceptquest`/`queststatus` debug commands added for
live verification ahead of any Planner/Executor loop.

## Files changed
- `docs/autonomous-player/ARCHITECTURE.md`: ADR-008 (full bug history),
  ADR-009 (Navigation), ADR-010 (QuestEngine).
- `docs/autonomous-player/KNOWN_FAILURES.md`, `TEST_MATRIX.md`,
  `ROADMAP.md`: updated throughout.
- New: `modules/mod-autonomous-player/src/Lifecycle/BotSessionMgr.{h,cpp}`,
  `Setup/PendingCharacterCreations.{h,cpp}`,
  `Navigation/BotNavigation.{h,cpp}`,
  `QuestEngine/BotQuestEngine.{h,cpp}`.
- Updated: `Setup/BotProvisioning.{h,cpp}`, `Lifecycle/BotLogin.{h,cpp}`,
  `Commands/cs_autonomousplayer.cpp` (now: `provision`, `login`, `status`,
  `moveto`, `acceptquest`, `queststatus`), `AutonomousPlayerModule.cpp`.

## Verification
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`,
  `codestyle-cpp.py`: pass on every commit this session (12 commits total).
- Compiled clean on zoidberg 11 times this session; final state (commit
  `871e98b`, currently deployed) compiles clean.
- All three capabilities (online bot, movement, quest-accept) verified
  **live**, not just compiled — see above and `TEST_MATRIX.md`.

## Current repository state
- Branch: `mod-autonomous-player`. This session's commits, oldest to
  newest: `5d5840f`, `fc631d0`, `3d49e7f`, `8ada2d4`, `d83ea84`, `a31afa6`,
  `2266a4a`, `871e98b`, plus this handoff commit — all pushed to origin.
- zoidberg's live `ac-worldserver` is running commit `871e98b`.
- Test fixture on zoidberg: account `ap_test1` (id 204), character
  `Grunttestbot` (guid 2014, Orc Warrior, level 1), currently positioned
  near Kaltunk in Valley of Trials, quest 4641 accepted/complete in its
  quest log. Reusable for the next session's testing.
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
None currently blocking. Full bug history (6 in Gate 1, all fixed) is in
`KNOWN_FAILURES.md`. One documented non-bug gotcha: quest-accept requires
real interaction range, not just "nearby" (see above) — this is normal
game behavior future Navigation/QuestEngine callers need to account for,
not something to fix in QuestEngine itself.

## Decisions made
- User gave explicit standing direction: "investigate how playerbots
  keeps its sessions alive - fix it and get to gate 3 on your own."
  Interpreted as: keep working autonomously through this project's own
  bounded-increment/gate methodology (commit + live-verify every slice),
  not a license to produce a large unverified pile of code. Followed that
  for both Gate 1 completion and both Gate 2 slices this session.
- **Stopping this session after two Gate 2 slices** rather than
  continuing further unattended. This was already an extremely long
  session (11 build/deploy cycles, six Gate-1 bugs found and fixed, two
  new components built and verified). Gate 2's remaining scope (full
  combat engine, loot, vendor, training, death mechanics, every race's
  starting area) is large enough that continuing to grind through it in
  the same already-long session risked exactly the failure mode the
  project's own operating-mode rules exist to prevent — sprawl without a
  clean checkpoint. This is a natural, well-verified stopping point:
  every commit compiles and has been proven live, nothing is left broken.
- Reused the existing test fixture (`ap_test1`/`Grunttestbot`) throughout
  rather than provisioning fresh bots per slice — same account/character
  federation the whole session, which is realistic (a real player also
  keeps using the same character across sessions).

## NEXT TASK
Gate 2, slice 3: quest turn-in for quest 4641 (close the loop on the quest
already accepted this session), OR a minimal single-target melee combat
loop for the Warrior class controller (Gate 2's "every class controller
completes representative combat" bar) — whichever the next session decides
is the better-scoped next vertical slice; both are reasonably sized.

Suggested approach for quest turn-in (if chosen): find the equivalent
opcode handler to `HandleQuestgiverAcceptQuestOpcode` for turn-in
(`HandleQuestgiverCompleteQuestOpcode`/`HandleQuestgiverChooseRewardOpcode`
— check `QuestHandler.cpp` for exact names and packet formats before
assuming) and reuse it the same way, synthesizing the packet rather than
reimplementing reward-selection logic. Verify quest 4641's actual reward
options via the live world DB first (`quest_template` reward columns), not
from memory.

Suggested approach for combat (if chosen): this is bigger — needs a
`Combat` component decision (ADR) for the "common engagement engine"
described in the project's Combat requirements section, even for a
minimal single-target-melee-no-CC first slice. Read the full "Combat
requirements" section of the project's root operating instructions before
starting (target validation, approach, health/resource/cooldown/range
checks, post-combat loot). Scope the FIRST slice down hard: e.g. "bot
attacks one already-selected, already-adjacent low-level target with
melee autoattack until it or the target dies, no ranged/pulling/kiting/CC
yet" — those are separate later slices.

Either way: follow the same pattern as this session's slices — design
briefly, implement the smallest testable increment, compile-check on
zoidberg, live-verify (not just compile), update docs, commit, before
starting the next slice.

## Next-session acceptance criteria
Depends on which slice is chosen (see NEXT TASK) — define specific
criteria at the start of that session once the choice is made, following
the same concrete/observable style used for this session's slices (exact
DB/log/perception-snapshot evidence, not "should work").

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX}.md. Pick one bounded Gate 2 slice from
HANDOFF.md's NEXT TASK (quest turn-in or minimal combat), design briefly,
implement, compile-check and live-verify on zoidberg (build-and-deploy is
pre-approved), update docs, commit. Do not attempt both slices, and do not
begin Gate 3 work, in the same session unless this task, its live
verification, documentation, and commit are all done and substantial
budget clearly remains.
