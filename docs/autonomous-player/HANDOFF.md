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

**What changed this session in direct response (all 5 of the review's
numbered priorities now have real, live-verified progress — none are
fully "solved," see each item's calibrated status below):**
1. **Fixed a real bug the review found:** `KillNearest`'s engagement
   confirmation checked `bot->IsInCombat()` instead of
   `bot->GetVictim() == target`. Happy path re-verified live across many
   subsequent tests. **The specific negative case the fix targets was
   attempted 4 separate times and never conclusively proven live** —
   test creatures die faster than console-command latency allows genuine
   overlap to be observed; treated as a confirmed environment limitation,
   not worth further retries with the same approach (`KNOWN_FAILURES.md`
   #5).
2. **`EncounterModel` now gates a real decision, not just reports state**
   (ADR-027): `KillNearest`'s `Approaching` → `Engaged` transition
   withholds confirmation while `HasUnplannedAdd()` is true, even once
   the bot is genuinely attacking its own objective target. No
   regression across 4 live attempts; the gating logic's specific
   behavior under a genuine add was not directly observed (same overlap
   limitation as #1) but is correct by code review (synchronous check).
3. **Bounded failure states added to every previously-unbounded guide
   wait** (ADR-028): `MoveTo`'s arrival wait, quest-giver
   search/walk/retry waits, `KillNearest`'s `Selecting`/`Engaged` waits.
   **This one has real, concrete, positive evidence**, not just a
   no-regression check: a new `.autonomousplayer guidestartmoveto`
   command sent a bot toward a genuinely unreachable coordinate, and the
   timeout was directly observed firing (`operationTicks` progressing
   23→46 across polls, then `finished=true, failed=true` right at the
   bound), with the bot's state confirmed sane afterward. Also found and
   fixed an inaccurate comment along the way (real tick rate is
   ~2.3/real-second, not the assumed ~1/second).
4. **Resolved "isolated kills not incrementing quest counter" for real:**
   confirmed as a premature read of an in-progress counter, not a
   defect, by running quest 788 through a genuine `guidestartquest`
   completion: `status=6` (`QUEST_STATUS_REWARDED`), real XP granted.

5. **Priority 5's Warrior investigation resolved, then composed into a
   first real class-controller slice:** re-tried the spell-78 rejection
   with the new diagnostics — root cause confirmed as insufficient rage
   on the earlier attempts (not a defect), verified twice with real casts
   landing (`result=255`/`SPELL_CAST_OK`) and real kills. Composed this
   into `KillNearest` automatically (ADR-029, `GuideStep::OpportunisticSpellId`):
   the guide now tries the ability alongside melee during `Engaged`.
   **Honest result across 3 independent live runs: 2/3 completed
   cleanly, 1/3 genuinely timed out** (hit the `MaxOperationTicks` bound
   while `Engaged`, `failed=true`) — root cause of that one failure not
   investigated (not enough samples to distinguish "unlucky individual
   creature" from "a real interaction with the new cast call," see
   `KNOWN_FAILURES.md` #6). No crashes in any run; the one failure was
   also a real demonstration of ADR-028's bounded timeout working as
   designed rather than hanging forever.

**Still open, entirely unaddressed:** target selection safety
(hostility/tag/evade/LoS/other-player-fighting-it validation, review
point 3) and an automated test suite (review point 7).

Full per-slice history is in `KNOWN_FAILURES.md` and `ARCHITECTURE.md`
(ADR-008 through ADR-029) — this file stays a live summary, not a
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
Review priorities 1-4 all have real, live-verified progress (see above).
**Priority 5 is next: first genuine Warrior/Priest combat controller**
(research document step 3). A first attempt (Warrior spell 78, a
candidate for "Heroic Strike") was made earlier this session and left
honestly incomplete — rejected with no confirmed root cause among three
possibilities (insufficient rage, wrong spell ID, next-swing-queued
mechanic) — see `KNOWN_FAILURES.md` #4. **`Combat::RequestCastSpell` now
returns the real `SpellCastResult` instead of a bool** (this session's
own infrastructure work, not yet used) — retry that investigation with
real diagnostics before attempting anything else: read the actual
numeric result, check `Player::GetPower(POWER_RAGE)` before/after, and
only then decide whether a class controller slice is really blocked on
this specific ability or whether a different, simpler ability should be
tried first.

Two smaller loose ends, either is reasonable to close opportunistically:
- Loot success verification (`Inventory::LootCorpse`'s `bool` return is
  still not checked/acted on — explicitly deferred in ADR-028, cheap to
  close).
- `KillNearest`'s bounded-blacklist path (ADR-023) has still never been
  exercised by a genuine unreachable-target scenario live (distinct from
  the `MaxOperationTicks`/`guidestartmoveto` timeout just proven — this
  is specifically about the *combat* target-blacklist-and-retarget path).

**Not yet started, real scope:** target selection safety (hostility/tag/
evade/LoS/other-player-fighting-it validation — the review's point 3,
entirely unaddressed), any automated test suite (point 7).

**Calibration note for whoever picks this up:** the review's core
criticism was that documentation sometimes gave small mechanism proofs
more weight than they deserve. Before writing "Verified" in any doc,
check: did this observation actually rule out the failure mode it claims
to, or just show the happy path worked again? This session's bounded-
timeout work (ADR-028) is the model to follow — it has a genuine,
concrete, observed-firing timeout as evidence, not just "the code
compiles and the happy path still works."

## Next-session acceptance criteria
- The Warrior spell-cast investigation (`KNOWN_FAILURES.md` #4) reaches
  a real, diagnostics-backed conclusion (root cause identified, or a
  concrete "still inconclusive, here's what the SpellCastResult/rage
  data actually showed" — not another guess).
- If a class controller slice is implemented, it's live-verified with
  real evidence (a real spell cast lands and deals damage, verified via
  target HP delta, not just "no crash").
- `check_no_playerbots_dependency.sh` and `check_no_forbidden_apis.sh`
  still pass.
- Docs updated with calibrated claims, committed.

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX,HONORBUDDY_SINGULAR_COMBAT_RESEARCH}.md in
full, especially this file's "Current milestone" section (external
review summary) and `KNOWN_FAILURES.md` #4-5, before continuing. Review
priorities 1-4 are done with real live evidence; priority 5 (first
genuine class controller) is next — start by retrying the Warrior
spell-78 investigation with the real `SpellCastResult`/rage diagnostics
that now exist (see NEXT TASK), don't guess again. Design briefly,
implement the smallest testable increment, compile-check and live-verify
on zoidberg with real evidence (build-and-deploy is pre-approved),
update docs with calibrated (not overstated) claims, commit. Keep going
without stopping to check in, except for a genuine blocker or an
ambiguous decision only the user can make.
