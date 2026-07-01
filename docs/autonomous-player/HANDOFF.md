# Session Handoff

## Current milestone
**Gate 2 — COMPLETE (2026-07-01).** Ten slices verified live across two
races/classes (Orc Warrior, Human Priest) — user confirmed this
representative sample satisfies Gate 2's "every race" bar (see
`ROADMAP.md`'s Week 3 entry).

**Gate 3 — levels 1–12, IN PROGRESS. An external review (2026-07-01) is
now the operative status check for this milestone — read it before
trusting this file's own framing.** The review rated the development
*process* 7/10 (careful, honest re-testing, small committed changes) but
current *capability* only 2-3/10: "an automatic scripted demo, not yet a
bot that can safely level." It found one genuine correctness bug (fixed
this session, see below) and several real, still-open gaps: `EncounterModel`
is diagnostic-only (nothing acts on it), target selection has no
hostility/tag/evade/LoS/other-player-fighting-it validation, several
guide operations can wait indefinitely (MoveTo stuck detection, `Engaged`
combat deadline/evade handling, quest accept/turn-in retry bound, "every
target blacklisted" dead-end, loot-success verification), there is no
real class-combat system (Warrior is autoattack+chase only), and testing
is manual with no automated regression suite. Treat these as the honest
state of the project, not a solved-and-moving-on list.

**What changed this session in direct response:**
1. **Fixed a real bug the review found:** `KillNearest`'s engagement
   confirmation checked `bot->IsInCombat()` (any fight, from anything)
   instead of `bot->GetVictim() == target` (the bot's own specific real
   attack target) — an unrelated add attacking the bot during approach
   would have falsely confirmed engagement with the untouched objective
   target, then waited forever for it to die. Fixed. The happy path was
   re-verified live; **the specific negative case the fix targets was
   not conclusively proven live** (couldn't force reliable overlapping
   combat with the weak test creatures available in this environment —
   see `KNOWN_FAILURES.md` #5 for the honest attempt record, don't skip
   this before touching the code again).
2. **Resolved the "isolated kills not incrementing quest counter"
   observation for real, not just reworded:** it was a premature read of
   an in-progress counter, not a defect — confirmed by running quest 788
   through a genuine `guidestartquest` completion: `status=6`
   (`QUEST_STATUS_REWARDED`), real XP granted. This is real evidence of
   one complete, correctly-credited multi-kill quest, addressing the
   review's point that the earlier "full quest loop" framing was
   overstated (it had only ever proven step *composition*, not
   completion, before this).
3. `Combat::RequestCastSpell` now returns the real `SpellCastResult`
   instead of a collapsed bool, needed to actually diagnose (not guess
   at) the still-open Warrior-ability-cast investigation
   (`KNOWN_FAILURES.md` #4).

**Everything else the review flagged (EncounterModel not influencing
behavior, unsafe target selection, unbounded waits elsewhere, no class
controllers, no automated tests) remains open.** See NEXT TASK — the
review's own priority order is adopted directly, not re-derived.

Full per-slice history is in `KNOWN_FAILURES.md` and `ARCHITECTURE.md`
(ADR-008 through ADR-026) — this file stays a live summary, not a
growing archive.

## What's proven, end to end, through real production code (not
## reimplemented or DB-shortcut)
**Orc Warrior** (`Grunttestbot`, Valley of Trials) and **Human Priest**
(`Priestestbot`, Northshire Abbey) both independently complete the full
Gate 1/2 arc: correct racial spawn (no teleport) → real navmesh movement
→ real quest accept/turn-in → real melee combat → real loot → real
death/recovery → real vendor buy/repair and gossip/trainer interaction.
See `ARCHITECTURE.md` ADR-008 through ADR-018.

**Gate 3 additions, calibrated:** `GuideRuntime` runs multi-step guides
automatically (`MoveTo`, `KillNearest`, `AcceptQuest`, `TurnInQuest`).
`KillNearest` uses an explicit `PullState` machine (ADR-023) with a
bounded approach timeout + blacklist (the blacklist path itself remains
unexercised live — see below). A minimal `EncounterModel` (ADR-024)
correctly reads real attacker state but nothing consumes it yet. One
real, complete, correctly-credited multi-kill quest has been run
end-to-end via `guidestartquest` (quest 788, real `REWARDED` status).
These are genuine, verified mechanisms — but per the external review,
they compose into a scripted demo of individual pieces, not yet a bot
capable of safely leveling unsupervised.

## Files changed (cumulative, this arc)
- `docs/autonomous-player/ARCHITECTURE.md`: ADR-008 through ADR-026.
- `docs/autonomous-player/HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`: new,
  user-provided, Gate 3's combat/pulling design baseline (ADR-022).
- `docs/autonomous-player/KNOWN_FAILURES.md`, `TEST_MATRIX.md`,
  `ROADMAP.md`: updated throughout.
- Components: `Lifecycle/BotSessionMgr`, `Setup/PendingCharacterCreations`,
  `Navigation/BotNavigation`, `QuestEngine/BotQuestEngine`, `Combat/BotCombat`
  (melee + `RequestCastSpell`, now returns real `SpellCastResult`),
  `Inventory/BotLoot`, `Recovery/BotRecovery`, `Economy/BotEconomy`,
  `Gossip/BotGossip`, `Growth/BotGrowth`, `GuideRuntime/BotGuideRuntime`,
  `EncounterModel/BotEncounterModel` (all `.h`/`.cpp` pairs).
- `Lifecycle/BotLifecycleMgr.{h,cpp}`: `BotSession` now carries a
  `GuideRuntime::BotGuideState`; `Update()` dispatches `GuideRuntime::Tick`
  on each per-bot second-tick.
- `Commands/cs_autonomousplayer.cpp` now has ~28 debug commands
  (`provision`, `login`, `status`, `moveto`, `acceptquest`, `queststatus`,
  `turnin`, `attack`, `creaturestatus`, `loot`, `releasespirit`,
  `reclaimcorpse`, `attackguid` [unreliable, see below], `multipull`,
  `buy`, `repair`, `gossiphello`, `gossiptrain`, `learnspell`,
  `castspell`, `spellbook`, `guidestart`, `guidestartcombat`,
  `guidestartquest`, `guidestatus`, `encountersnapshot`).

## Verification
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`,
  `codestyle-cpp.py`: pass on every commit.
- Compiled clean on zoidberg 36 times across this arc; currently deployed
  commit compiles clean.
- **No automated test suite exists** (external review point 7, accurate)
  — every verification claim in this project's docs is a manual live
  observation. This is a real gap, not addressed this session.

## Current repository state
- Branch: `mod-autonomous-player`. Most recent commit: `875958b`
  (engagement-confirmation fix), plus this handoff commit — all pushed
  to origin.
- zoidberg's live `ac-worldserver` is running the latest pushed commit.
- Test fixtures on zoidberg:
  - account `ap_test1` (id 204), character `Grunttestbot` (guid 2014, Orc
    Warrior, level 3+, from quest 788's real XP reward), in Mottled Boar
    territory on map 1 (Valley of Trials). Quest 788 fully rewarded.
  - account `ap_priest1` (id 205), character `Priestestbot` (guid 2015,
    Human Priest, level 1, 0 copper, quest 783 rewarded), near Marshal
    McBride (`-8902.6, -162.6, 81.9` on map 0, Northshire Abbey).
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
Full history (6 bugs in Gate 1, 1 in Gate 2, 5 in Gate 3 — see
`KNOWN_FAILURES.md` #1–5 for Gate 3, #5 is the review-found engagement
bug) plus non-bug findings is in `KNOWN_FAILURES.md`. Two items open,
neither blocking further work but both honest gaps: #3's bounded
blacklist path is unexercised live; #4's Warrior-ability cast rejection
still lacks a confirmed root cause (real `SpellCastResult` diagnostics
now exist to investigate it properly, not yet used to do so).

## Decisions made
- User's standing direction: "continue on your own until we get to gate
  5" / "continue on your own" (repeated). Interpreted as: keep working
  autonomously, bounded-increment discipline, only pausing for a genuine
  blocker or a decision only the user can make.
- User explicitly confirmed (2026-07-01, `AskUserQuestion`) Gate 2's
  race-coverage bar is a representative sample (2 races/classes).
- **When a fix doesn't hold up on re-test, don't declare success and
  don't blindly patch repeatedly without new evidence** — established
  earlier this session (`KillNearest`'s stuck-target bug, `KNOWN_FAILURES.md`
  #3) and reaffirmed by this session's response to external review:
  the engagement-confirmation fix is documented as "fixed by reasoning,
  happy path re-verified, negative case not conclusively proven live"
  rather than overclaimed as fully verified just because a fix was
  written and compiled.
- **User provided `HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`** — Gate 3's
  design baseline for combat/pulling work (ADR-022).
- **User (or a reviewer acting on the user's behalf) provided a direct
  external code review (2026-07-01)** rating process 7/10, capability
  2-3/10, with a specific priority list. **That priority list is now
  this project's NEXT TASK, directly, not reinterpreted.**

## NEXT TASK
Follow the external review's priority list directly:
1. ~~Correct target-specific engagement confirmation~~ — **done this
   session** (`GetVictim()==target`), happy path re-verified, negative
   case not yet proven live (see `KNOWN_FAILURES.md` #5).
2. **Make `EncounterModel` actually influence behavior**, not just
   report it. At minimum: when `HasUnplannedAdd()` is true during
   `KillNearest`'s `Approaching`/`Engaged` states, do *something* real
   with that signal — even a conservative first step (e.g. abort the
   current pull and re-select, or explicitly refuse to advance past
   `Engaged` while an unplanned add is present) is more honest progress
   than the current purely-diagnostic wiring. Design the smallest real
   behavior change, not the full "combat target may override objective
   target" model from the research document yet.
3. **Add bounded failure states to every guide operation that currently
   lacks one**, per the review's list: `MoveTo` has no stuck detection;
   `Engaged` has no combat deadline/evade handling; quest-giver approach
   (`AcceptQuest`/`TurnInQuest`) has no timeout; quest accept/turn-in
   retries indefinitely in `Acting`; `Selecting` waits forever once every
   candidate is blacklisted (no blacklist expiry/reset short of the whole
   step ending); loot is attempted once with no success verification.
   Each of these needs the same treatment `KillNearest`'s `Approaching`
   already got (ADR-023) — a real bound, not an infinite wait.
4. ~~Prove one complete multi-kill quest with real credit and turn-in~~
   — **done this session** (quest 788, real `REWARDED` status, real XP).
5. Only then: first genuine Warrior/Priest combat controllers (research
   document step 3) — already attempted once this session and left
   honestly incomplete (`KNOWN_FAILURES.md` #4); retry with the new
   `SpellCastResult` diagnostics once items 2-3 above are further along,
   not before.

**Calibration note for whoever picks this up:** the review's core
criticism was that documentation sometimes gave small mechanism proofs
more weight than they deserve. Before writing "Verified" in any doc,
check: did this observation actually rule out the failure mode it claims
to, or just show the happy path worked again? When uncertain, write the
honest, qualified version (as this session did for the engagement-fix
negative case) rather than the confident-sounding one.

## Next-session acceptance criteria
- At least one of review-priority items 2 or 3 is implemented, compiles
  clean, and is live-verified with real evidence — including, where
  applicable, evidence of the *failure* path actually being bounded
  (not just the happy path still working).
- `check_no_playerbots_dependency.sh` and `check_no_forbidden_apis.sh`
  still pass.
- Docs updated with calibrated claims (verified vs. attempted-but-
  inconclusive vs. deferred, clearly distinguished), committed.

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX,HONORBUDDY_SINGULAR_COMBAT_RESEARCH}.md in
full, especially this file's "Current milestone" section (external
review summary) and `KNOWN_FAILURES.md` #3-5, before touching
`Combat`/`GuideRuntime`/`EncounterModel` again. Continue Gate 3
autonomously per the user's standing instruction, following the external
review's priority list directly (see NEXT TASK): make `EncounterModel`
influence real behavior, then add bounded failure states to the
remaining unbounded guide operations. Design briefly, implement the
smallest testable increment, compile-check and live-verify on zoidberg
with real evidence (build-and-deploy is pre-approved), update docs with
calibrated (not overstated) claims, commit. Keep going without stopping
to check in, except for a genuine blocker or an ambiguous decision only
the user can make.
