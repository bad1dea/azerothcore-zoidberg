# Session Handoff

## Current milestone
**Gate 2 — COMPLETE (2026-07-01).** Ten slices verified live across two
races/classes (Orc Warrior, Human Priest) — user confirmed this
representative sample satisfies Gate 2's "every race" bar (see
`ROADMAP.md`'s Week 3 entry).

**Gate 3 — levels 1–12, IN PROGRESS, substantially advanced but not
complete.** An external review (2026-07-01) rated process 7/10,
capability 2-3/10, and gave a 7-point priority list. **All 7 priorities
now have real, live-verified progress** (see `ROADMAP.md`'s Week 4 entry
for the full evidence trail and `ARCHITECTURE.md` ADR-026 through
ADR-034 for design detail) — the external review is closed out as an
operative blocker. Summary, calibrated:

- **Fixed, live-verified:** the engagement-confirmation bug the review
  found (`GetVictim()` not `IsInCombat()`); `EncounterModel` now gates a
  real decision (withholds `Engaged` confirmation on an unplanned add);
  every previously-unbounded guide wait is now bounded and one bound was
  directly observed firing; loot success is verified against real
  corpse state, not assumed; a real class-controller composition
  (`KillNearest` + `OpportunisticSpellId`) works for **two** class
  archetypes now — Warrior melee (spell 78) and Hunter ranged (spell 75,
  Auto Shot, ADR-034) — with no code changes needed between them,
  confirming the design is genuinely class-agnostic.
- **Target selection safety (ADR-031):** `IsSafeToEngage` gates
  `KillNearest` on attackability, evade, loot-tag, other-player-
  attacking, and LoS. **4 of 5 checks are live-verified with real
  evidence** (attackable, tag, other-player-attacking, LoS); evade
  remains code-review-only (test creatures die/reset faster than
  console-command latency allows catching it mid-state, same limitation
  as `KNOWN_FAILURES.md` #5). **Caught a real regression before it ever
  shipped**: the first version used `IsHostileTo`, which is `false` for
  most low-level questing wildlife (faction-neutral, not Hostile) — that
  would have made `KillNearest` reject its own most-tested target
  entirely. Fixed to `IsValidAttackTarget`.
- **Automated regression suite (ADR-033):** `tools/live_regression_suite.py`
  — 5 real assertions over the live SOAP interface, including a direct
  regression test for the `IsHostileTo` bug above. First automated
  regression protection this whole project has had; `5/5 passed` live.
- **Two real, previously-unknown bugs found and fixed as a byproduct of
  this work**, both silent-failure-class (matching every Gate 1 bug's
  root shape — a client-feedback path gated on a null socket):
  character creation silently stalling on an invalid name
  (`KNOWN_FAILURES.md` #9, fixed by ADR-032's pre-validation) and a
  bounded-timeout guide leaving an already-issued `MotionMaster` order
  running after its own bookkeeping gives up (`KNOWN_FAILURES.md` #10,
  **not yet fixed** — real character death, real recovery via the
  already-proven death cycle, root cause understood but no code change
  made this session).

**Gate 3's own literal acceptance bar (`ROADMAP.md`) is NOT fully
met — stated plainly, not glossed over:**
- "All supported race/class combos": only 2 races (Orc, Human) × 3
  classes (Warrior, Hunter, Priest — Priest not yet combat-tested at a
  level with an offensive spell) tested. Treating race breadth the same
  representative-sample way Gate 2 did, but Gate 3's charter text says
  "all," so this is a reinterpretation being applied, not a literal
  pass — flagged, not assumed.
- "Dense camps, caves": not deliberately engineered/tested (distinct
  from the incidental multi-target density already exercised via
  `multipull`).
- "Ranged pulls" as a distinct behavior: not modeled — `KillNearest`
  always closes to melee range even with a ranged `OpportunisticSpellId`.
- "Pets": no infrastructure exists at all — a real missing subsystem,
  not untested volume.
- "Full bags": partially covered — encountered organically (a real
  near-full-bags loot outcome was observed and handled correctly by the
  existing best-effort design), not deliberately engineered.

Full per-slice history: `KNOWN_FAILURES.md` (11 Gate 3 entries),
`ARCHITECTURE.md` (ADR-008 through ADR-034), `TEST_MATRIX.md`. This file
stays a live summary, not a growing archive — older per-session numbered
lists have been condensed here rather than kept verbatim.

## What's proven, end to end, through real production code (not
## reimplemented or DB-shortcut)
**Orc Warrior** (`Grunttestbot`) and **Human Priest** (`Priestestbot`)
complete the full Gate 1/2 arc: correct racial spawn → real navmesh
movement → real quest accept/turn-in → real melee combat → real loot →
real death/recovery → real vendor buy/repair and gossip/trainer
interaction. `GuideRuntime` runs multi-step guides (`MoveTo`,
`KillNearest`, `AcceptQuest`, `TurnInQuest`) fully automatically, no
manual step advances, with real target-selection safety and bounded
failure states throughout. **Orc Hunter** (`Grunthunter`) adds a second,
ranged class-controller composition. A real automated regression suite
now protects 5 of these behaviors from silent regression.

Per the external review's own framing, and still accurate: these are
genuine, verified mechanisms, not yet a bot proven capable of safely
leveling fully unsupervised across arbitrary content (see the literal
Gate 3 gaps above).

## Files changed (cumulative, this arc)
- `docs/autonomous-player/ARCHITECTURE.md`: ADR-008 through ADR-034.
- `docs/autonomous-player/HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`: new,
  user-provided, Gate 3's combat/pulling design baseline (ADR-022).
- `docs/autonomous-player/KNOWN_FAILURES.md`, `TEST_MATRIX.md`,
  `ROADMAP.md`: updated throughout.
- Components: `Lifecycle/BotSessionMgr`, `Setup/PendingCharacterCreations`,
  `Setup/BotProvisioning` (now with `ValidateCharacterName`, ADR-032),
  `Navigation/BotNavigation`, `QuestEngine/BotQuestEngine`, `Combat/BotCombat`
  (melee + `RequestCastSpell`, real `SpellCastResult`),
  `Inventory/BotLoot`, `Recovery/BotRecovery`, `Economy/BotEconomy`,
  `Gossip/BotGossip`, `Growth/BotGrowth`, `GuideRuntime/BotGuideRuntime`
  (now with `IsSafeToEngage`, ADR-031), `EncounterModel/BotEncounterModel`
  (all `.h`/`.cpp` pairs).
- `Lifecycle/BotLifecycleMgr.{h,cpp}`: dispatches `GuideRuntime::Tick`
  per-bot per-tick.
- `Commands/cs_autonomousplayer.cpp`: ~30 debug commands, including
  `targetsafety` (ADR-031).
- `tools/live_regression_suite.py`: new (ADR-033) — the project's first
  automated test.

## Verification
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`,
  `codestyle-cpp.py`: pass on every commit.
- Compiled clean on zoidberg 45+ times across this arc; currently
  deployed commit compiles clean.
- `tools/live_regression_suite.py`: `5/5 passed` against zoidberg as of
  the most recent commit — the project's first automated regression
  protection, on top of the still-manual verification everything else
  relies on.
- Live testing is done via the worldserver's SOAP interface (port 7878,
  GM account `SOAPADMIN`), not manual console interaction — see
  `[[autonomous-player-zoidberg-soap-access]]` in agent memory for the
  exact mechanism and the container-recreate procedure needed to deploy
  new code (`docker restart` alone does not pick up a rebuilt image).

## Current repository state
- Branch: `mod-autonomous-player`. Most recent commit this arc:
  `8390d28` (Hunter combat coverage, ADR-034), plus this handoff commit —
  all pushed to origin.
- zoidberg's live `ac-worldserver` is running the latest code.
- Test fixtures on zoidberg:
  - account `ap_test1` (id 204), character `Grunttestbot` (Orc Warrior,
    level 3+), Valley of Trials, map 1.
  - account `ap_priest1` (id 205), character `Priestestbot` (Human
    Priest, level 1), Northshire Abbey, map 0.
  - account `ap_test2` (id 206), character `Grunttestii` (Orc Warrior,
    level 1) — the second Horde character ADR-032 unblocked; used for
    live ADR-031 tap/other-player-attacking verification.
  - account `ap_test3` (id 207), character `Grunthunter` (Orc Hunter,
    level 1) — first ranged-class test character.
  - All four bots were alive and controllable as of the last check this
    arc. Two (`Grunttestbot`, `Grunttestii`) died during this arc's own
    stress-testing (`KNOWN_FAILURES.md` #10 — a stale movement order
    surviving a bounded-timeout bail-out) and were recovered via the
    already-proven `releasespirit`/`reclaimcorpse` cycle — note the real
    30-second server-side reclaim cooldown after releasing spirit if
    repeating this.
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
11 Gate 3 entries in `KNOWN_FAILURES.md` (plus 6 in Gate 1, several
non-bug findings in Gate 2). Open, non-blocking: #3 (bounded-blacklist
path unexercised live), #4 (Warrior spell-78 root cause — actually
resolved, see #4's own entry), #6 (ADR-029 timeout — re-tested with a
13-trial sample, not reproduced, downgraded to low-priority), #8
(`creaturestatus`'s `FindNearestCreature` alive-param footgun — a
one-line fix, not yet applied to that command itself), #10 (**real,
unfixed**: a bounded-timeout guide doesn't stop an already-issued
movement order — can walk a bot to its death unattended after its own
guide has already given up).

## Decisions made
- User's standing direction: "continue on your own until we get to gate
  5" / "continue on your own" / "continue on your own until gate 3 is
  completed and validated" (repeated, most recently 2026-07-01).
  Interpreted as: keep working autonomously, bounded-increment
  discipline, only pausing for a genuine blocker or a decision only the
  user can make.
- User explicitly confirmed (2026-07-01, `AskUserQuestion`) Gate 2's
  race-coverage bar is a representative sample (2 races/classes). This
  arc extends the same reasoning to Gate 3's race breadth (not user-
  reconfirmed for Gate 3 specifically — flagged as a reinterpretation,
  not a literal pass, in `ROADMAP.md`'s Week 4 entry).
- **When a fix doesn't hold up on re-test, don't declare success and
  don't blindly patch repeatedly without new evidence** — established
  with `KillNearest`'s stuck-target bug (`KNOWN_FAILURES.md` #3) and
  reaffirmed repeatedly since (the engagement-confirmation fix, the
  `IsHostileTo` catch, the ADR-029 re-test).
- **User provided `HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`** — Gate 3's
  design baseline for combat/pulling work (ADR-022).
- **An external code review (2026-07-01)** gave a 7-point priority list
  that became this project's operative NEXT TASK for a full arc; all 7
  now have real progress (see "Current milestone" above) — the review is
  closed out, but Gate 3's own literal bar is a separate, still-open
  thing (see above).

## NEXT TASK
Gate 3's external-review debt is paid off; what's left is Gate 3's own
literal acceptance bar. In rough priority order:

1. **`KNOWN_FAILURES.md` #10 (real, unfixed safety gap)**: a
   bounded-timeout guide (`OperationTimedOut`) stops `GuideRuntime`'s own
   polling but does not stop the character's already-issued
   `MotionMaster` movement order — this walked a real bot to its death
   unattended this arc. Fix candidates to evaluate: have
   `OperationTimedOut` (or its callers) issue a stop-movement /
   return-to-safe-position order on bail-out; or have `KillNearest`'s own
   failure paths do the same. This is a real safety issue for any
   future unsupervised-leveling milestone (Gate 4+), not just tidiness.
2. **Pets**: genuinely missing subsystem for Hunter (and later
   Warlock/Death Knight) viability — no summon/state-check
   infrastructure exists. Real, possibly substantial scope; worth a
   design pass before implementation (what does a pet actually need:
   summon-on-login, a `Combat`-layer awareness of pet HP/state, revive?).
3. **Dense camps / caves**: deliberately engineered terrain/density
   scenarios, distinct from `multipull`'s incidental density. Needs
   scouting real in-game locations that fit (a cave with multiple
   creatures, a camp with patrol/aggro-radius overlap) — likely doable
   with existing primitives, mostly a testing/validation task rather
   than new code.
4. **Ranged pulls as a distinct behavior**: `KillNearest`'s `Approaching`
   phase could stay at range when `OpportunisticSpellId` is a genuinely
   ranged ability rather than always closing to melee — a real design
   question (worth checking real spell range data via `SpellInfo`,
   not guessing).
5. **`creaturestatus`'s `FindNearestCreature` footgun** (`KNOWN_FAILURES.md`
   #8) — one-line fix, cheap to close opportunistically.

**Calibration note for whoever picks this up:** this arc's own
`IsHostileTo`→`IsValidAttackTarget` catch and the `FindNearestCreature`
alive-param footgun are good examples of why testing a diagnostic
command against real live state (not just reading the code) keeps
finding real bugs even in code that "looks right." Keep doing that
before declaring anything "Verified."

## Next-session acceptance criteria
- Real progress on `KNOWN_FAILURES.md` #10 (the safety gap) or pets or
  dense-camp/cave validation — whichever is picked, with real live
  evidence, not just code review.
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`, and
  `tools/live_regression_suite.py` all still pass.
- Docs updated with calibrated claims, committed.

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX,HONORBUDDY_SINGULAR_COMBAT_RESEARCH}.md in
full, especially this file's "NEXT TASK" section and `KNOWN_FAILURES.md`
#10, before continuing. Also check agent memory for
`autonomous-player-zoidberg-soap-access` before doing any live testing.
Gate 3's external-review debt is fully paid off (all 7 priorities have
real progress); what's left is Gate 3's own literal bar (pets, dense
camps/caves, full race breadth, ranged-pulls-as-distinct-behavior) plus
one real unfixed safety gap (#10). Start with #10 if unsure where to
begin -- it's a real bug with a concrete live reproduction, not new
feature scope. Run `tools/live_regression_suite.py` before and after any
change that touches `GuideRuntime`/`Combat` to catch regressions
automatically. Design briefly, implement the smallest testable
increment, compile-check and live-verify on zoidberg with real evidence
(build-and-deploy is pre-approved), update docs with calibrated (not
overstated) claims, commit. Keep going without stopping to check in,
except for a genuine blocker or an ambiguous decision only the user can
make.
