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

6. **Closed the explicitly-deferred loot-verification gap from ADR-028**
   (ADR-030): `KillNearest`'s `Looting` phase now checks the corpse's
   real state after `Inventory::LootCorpse` runs (`LastLootAttempted`/
   `LastLootVerified`), rather than advancing blindly. **Verified live,
   3 independent runs, all clean** (`true, true` every time) — unlike
   ADR-029, this one closed cleanly with no mixed result.
7. **(New session, 2026-07-01) Review point 3, target selection safety,
   now has real code + partial live verification** (ADR-031):
   `IsSafeToEngage` gates both `KillNearest`'s candidate search and its
   `Approaching` re-check on attackability, evade state, loot tag,
   another player already fighting the target, and LoS. **Caught a real
   regression before it ever reached `KillNearest`**: the first version
   used `IsHostileTo`, which live diagnostics showed was `false` for a
   Mottled Boar — most questing wildlife is faction-neutral, not
   Hostile, so this would have made `KillNearest` reject its own
   most-tested target entirely. Fixed to `IsValidAttackTarget`,
   re-verified live (`attackable=true, safe=true`), and `guidestartcombat`
   re-confirmed to complete with zero regression under the new gate.
   **Honest gap**: evade/tap/other-player-attacking/LoS are correct by
   code review (proven engine APIs) but a dedicated two-character live
   scenario to exercise the tap/other-player case hit a reproducible,
   unrelated character-creation stall (`KNOWN_FAILURES.md` #9) — not
   demonstrated live this session.

**Still open:** an automated test suite (review point 7) — entirely
unaddressed. Target selection safety (review point 3) now has real code
and partial live verification (see item 7 above) but not full coverage
of every check. Also still open, smaller: `KillNearest`'s
bounded-blacklist path has never been exercised by a genuine
unreachable-target scenario live (distinct from the
`MaxOperationTicks`/`guidestartmoveto` timeout already proven), and
ADR-029's 1-in-3 timeout (`KNOWN_FAILURES.md` #6) has not been
investigated further or gathered more samples.

Full per-slice history is in `KNOWN_FAILURES.md` and `ARCHITECTURE.md`
(ADR-008 through ADR-031) — this file stays a live summary, not a
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
- `Commands/cs_autonomousplayer.cpp` now has ~29 debug commands
  (`provision`, `login`, `status`, `moveto`, `acceptquest`, `queststatus`,
  `turnin`, `attack`, `creaturestatus`, `targetsafety` (new, ADR-031),
  `loot`, `releasespirit`, `reclaimcorpse`, `attackguid` [unreliable, see
  below], `multipull`, `buy`, `repair`, `gossiphello`, `gossiptrain`,
  `learnspell`, `castspell`, `spellbook`, `guidestart`, `guidestartcombat`,
  `guidestartquest`, `guidestatus`, `encountersnapshot`).

## Verification
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`,
  `codestyle-cpp.py`: pass on every commit.
- Compiled clean on zoidberg 40 times across this arc (4 in this
  session, including one full redeploy cycle per fix iteration); currently
  deployed commit compiles clean.
- **No automated test suite exists** (external review point 7, accurate)
  — every verification claim in this project's docs is a manual live
  observation. This is a real gap, not addressed this session.
- **New this session:** live testing is now done via the worldserver's
  SOAP interface (port 7878, GM account `SOAPADMIN`) rather than any
  manual console interaction — see
  `[[autonomous-player-zoidberg-soap-access]]` in agent memory for the
  exact mechanism (not written into this repo's docs, since it's
  operator/environment detail, not project design).

## Current repository state
- Branch: `mod-autonomous-player`. Most recent commit this session:
  `ac16ef1` (target selection safety, ADR-031), plus follow-up fix
  commits and this handoff update — all pushed to origin.
- zoidberg's live `ac-worldserver` is running the latest code as of this
  session (container was fully recreated, not just restarted, to pick up
  the new image — see `[[autonomous-player-zoidberg-soap-access]]` for
  why `docker restart` alone would not have worked).
- Test fixtures on zoidberg:
  - account `ap_test1` (id 204), character `Grunttestbot` (guid 2014, Orc
    Warrior, level 3+, from quest 788's real XP reward), in Mottled Boar
    territory on map 1 (Valley of Trials). Quest 788 fully rewarded.
  - account `ap_priest1` (id 205), character `Priestestbot` (guid 2015,
    Human Priest, level 1, 0 copper, quest 783 rewarded), near Marshal
    McBride (`-8902.6, -162.6, 81.9` on map 0, Northshire Abbey).
  - account `ap_test2` (id 206) exists but its character
    (`Grunttestbot2`) never finished creating this session
    (`KNOWN_FAILURES.md` #9) — no usable second character yet. Whoever
    investigates that stall should reuse this account rather than
    creating a third.
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
Full history (6 bugs in Gate 1, 1 in Gate 2, 9 in Gate 3 — see
`KNOWN_FAILURES.md` #1–9 for Gate 3; #5 is the review-found engagement
bug, #7-9 are new this session) plus non-bug findings is in
`KNOWN_FAILURES.md`. Open items, none blocking further work: #3's bounded
blacklist path is unexercised live; #4's Warrior-ability cast rejection
still lacks a confirmed root cause (real `SpellCastResult` diagnostics
now exist to investigate it properly, not yet used to do so); #9's
second-test-character creation stall (new, blocks constructing a live
tap/other-player-attacking scenario for ADR-031 until resolved).

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
Review priorities 1-3 and 5-6 all have real, live-verified progress now
(priority 4/quest-counter was a non-bug, see above). **Priority 7,
automated test suite, is the last entirely-unaddressed review priority**
— every verification claim in this whole project remains a manual live
observation via SOAP commands, with no regression protection at all.
Given `BUILD_TESTING`/gtest isn't wired into the module build path
(`TEST_MATRIX.md`'s original note), the realistic smallest first slice is
probably NOT a full gtest harness, but worth designing deliberately
rather than guessing — e.g. a scripted sequence of the existing SOAP
debug commands (`guidestartcombat`, `targetsafety`, etc.) with
pass/fail assertions on their output, runnable on demand against
zoidberg, would already catch regressions like this session's
`IsHostileTo` bug automatically instead of requiring a human (or agent)
to manually notice `hostile=false` looked wrong.

Smaller, real, in-scope loose ends, any is reasonable to pick up next or
alongside the test-suite work:
- **ADR-031's own gaps**: evade/tap/other-player-attacking/LoS checks are
  code-review-only, not live-verified (see `TEST_MATRIX.md`). The
  blocker was a reproducible character-creation stall for a second test
  account (`ap_test2`/`Grunttestbot2`, `KNOWN_FAILURES.md` #9) —
  investigate that first (reuse account id 206, don't create a third),
  then retry the two-character tap scenario this session designed but
  couldn't execute.
- `KillNearest`'s bounded-blacklist path (ADR-023) has still never been
  exercised by a genuine unreachable-target scenario live (distinct from
  the `MaxOperationTicks`/`guidestartmoveto` timeout already proven).
- ADR-029's 1-in-3 `Engaged`-phase timeout (`KNOWN_FAILURES.md` #6) has
  not been investigated further or gathered more samples.
- `creaturestatus`'s pre-existing `FindNearestCreature(entry, range,
  false)` footgun (`KNOWN_FAILURES.md` #8) — `false` means "only dead,"
  not "either" — is a one-line fix, not yet applied to that command
  itself (only worked around locally in the new `targetsafety`).

**Calibration note for whoever picks this up:** the external review's
core criticism was that documentation sometimes gave small mechanism
proofs more weight than they deserve. Before writing "Verified" in any
doc, check: did this observation actually rule out the failure mode it
claims to, or just show the happy path worked again? This session's
`IsHostileTo`→`IsValidAttackTarget` catch (ADR-031) is a good example of
why: a check that "looked right" by code review alone would have shipped
a real regression if live diagnostics hadn't been built and run before
wiring it into `KillNearest`. Test the diagnostic before trusting it.

## Next-session acceptance criteria
- Either real progress on an automated test suite (even a minimal
  scripted-SOAP-assertions slice counts, if it would have caught a real
  past bug like the `IsHostileTo` one), or ADR-031's evade/tap/
  other-player/LoS checks reach real live verification (which likely
  requires first resolving `KNOWN_FAILURES.md` #9's character-creation
  stall).
- `check_no_playerbots_dependency.sh` and `check_no_forbidden_apis.sh`
  still pass.
- Docs updated with calibrated claims, committed.

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX,HONORBUDDY_SINGULAR_COMBAT_RESEARCH}.md in
full, especially this file's "NEXT TASK" section and `KNOWN_FAILURES.md`
#6-9, before continuing. Also check agent memory for
`autonomous-player-zoidberg-soap-access` before doing any live testing —
it has the exact SOAP mechanism (port 7878, `SOAPADMIN` GM account) and
the container-recreate procedure needed to actually deploy new code;
`docker restart` alone does not pick up a rebuilt image. Review
priorities 1-3, 5, 6 are done with real live evidence; priority 7
(automated test suite) is the last untouched one and is the most
valuable next step, though ADR-031's remaining unverified checks
(evade/tap/other-player/LoS) are also legitimate to pick up if
`KNOWN_FAILURES.md` #9's character-creation stall turns out to be a
quick fix. Design briefly, implement the smallest testable increment,
compile-check and live-verify on zoidberg with real evidence
(build-and-deploy is pre-approved), update docs with calibrated (not
overstated) claims, commit. Keep going without stopping to check in,
except for a genuine blocker or an ambiguous decision only the user can
make.
