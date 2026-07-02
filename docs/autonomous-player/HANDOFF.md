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
ADR-037 for design detail) — the external review is closed out as an
operative blocker. **Pets (one of Gate 3's own remaining literal-bar
gaps) also now has a real first slice**, done same arc at the user's
explicit direction to continue past the review-closure point. Summary,
calibrated:

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
- **Four real, previously-unknown bugs found and fixed as a byproduct of
  this work**, all silent-failure-class (matching every Gate 1 bug's
  root shape — a client-feedback path gated on a null socket, or a
  bookkeeping/physical-action split): character creation silently
  stalling on an invalid name (`KNOWN_FAILURES.md` #9, fixed by
  ADR-032's pre-validation); a bounded-timeout guide leaving an
  already-issued `MotionMaster` order running after its own bookkeeping
  gives up, which walked a real bot to its death unattended
  (`KNOWN_FAILURES.md` #10, **fixed and re-verified live**, ADR-035 —
  `OperationTimedOut` now calls `bot->StopMoving()` on every bail-out);
  the `castspell` debug command re-triggering `SPELL_FAILED_MOVING` on
  every retry regardless of actual position, which initially made a
  real, working spell (Tame Beast) look broken (`KNOWN_FAILURES.md` #12,
  fixed, ADR-036); and `creaturestatus`'s pre-existing `FindNearestCreature`
  alive-param footgun (`KNOWN_FAILURES.md` #8, found, not yet fixed at
  the source, worked around locally in `targetsafety`).
- **Pets, first slice + `GuideRuntime` integration (ADR-037/038):** new
  `Pets` component (`RequestTameBeast`, `PetSnapshot`/`BuildSnapshot`,
  `RequestSetPetReactState`, `RequestAttackTarget`) plus debug commands.
  **Major design-pass finding: taming itself needed zero new module
  code** — it composes entirely from the already-proven
  `Combat::RequestCastSpell` primitive against the real engine's Tame
  Beast spell. `KillNearest` now keeps a live pet on the guide's own
  planned target throughout `Approaching`/`Engaged`
  (`EnsurePetAssists`) — deliberately `REACT_DEFENSIVE` + an explicit
  `CMSG_PET_ACTION` attack command, not `REACT_AGGRESSIVE`, so the pet
  never acquires its own unplanned adds (a design correction made before
  ever verifying the aggressive-only version live, once it was noticed
  that would undermine `EncounterModel`'s ADR-027 gating). **Fully
  live-verified, including the hardest part**: a real pet was tamed,
  persists across a full worldserver restart+relogin (real engine
  behavior), its react state auto-corrected from aggressive to
  defensive within one tick, and — critically — direct evidence
  (`Pet::GetVictim()` matching the guide's own objective target's exact
  guid during a real `Approaching`/`Engaged` fight) proved the pet
  genuinely assists on the *planned* target, not just exists nearby or
  roams for its own.
- **`CombatIntent`/`CombatExecutor` + pet recovery-phase policy, fully
  finished (ADR-039/040):** design correction at the user's explicit
  direction -- pet-revive-on-death was about to be wired in the same
  ad-hoc, isolated-spell-ID way (directly inside `KillNearest`'s pet
  helper, triggered merely by an absent/dead pet). Replaced before
  shipping with a shared decision layer: `Combat::CombatIntent`/
  `Combat::Execute` (one place mapping "what the bot wants" to the real
  primitive call -- melee engage, ability cast, pet-assist, revive, and
  call-pet all route through it), `Pets::PetState`/`ClassifyPetState`
  (the full 6-state model: `NoPet`/`ActiveAlive`/`ActiveDead`/
  `MissingAlive`/`MissingDead`/`Dismissed`, reading real
  `PetStable::GetUnslottedHunterPet()->Health` to tell a dismissed-while-
  alive pet from a dead-and-unslotted one), and `Recovery::PlanPetRecovery`
  (the Singular model's "Recover" stage). `GuideRuntime::Tick` checks
  this once, centrally, before the current step ever runs -- guide state
  is never touched while recovery is pending, so it resumes automatically
  with no explicit save/restore.

  **The investigation initially reached a wrong conclusion** (that this
  was an unfixable structural fork limitation) before the user caught it
  and pointed at the actual fix: `RequestRevivePet` had a real bug,
  bailing out whenever `bot->GetPet()` was null -- exactly the case
  Revive Pet's real effect (`Spell::EffectResurrectPet`) is designed to
  handle via `SummonPet(0, ...)` reloading from `PetStable`/DB. Fixed
  (always self-casts now) and **live-verified twice**: cast against
  `Grunthunter`'s real broken pet (`PetState::MissingDead`, confirmed via
  DB: `slot=100, curhealth=0`), result `SPELL_CAST_OK`, and the *same*
  pet (matching pet number) loaded back in alive both times. Full
  in-order story of the wrong turns and the real fix is in
  `KNOWN_FAILURES.md` #13 -- read it before touching pet recovery again,
  it's genuinely instructive about verifying the actual code path
  instead of the first plausible-looking rejection.

  **Also verified**: no regression (`live_regression_suite.py` still
  `5/5`); a guide runs cleanly with no pet at all.

  **Two follow-up regressions found and fixed by the regression suite
  right after this shipped** (full detail in `KNOWN_FAILURES.md` #13 /
  `ARCHITECTURE.md` ADR-040): the `Tick()`-level recovery check
  originally ran unconditionally, which could (1) preempt an active
  `KillNearest`/quest pursuit whenever `!bot->IsInCombat()` read true
  even momentarily (a real evade, or a brief gap before the first hit
  registers), starving `OperationTimedOut`'s own tick counter and
  stretching wall-clock time even though correctness wasn't broken --
  fixed by gating on `state.CurrentTargetGuid.IsEmpty()`; and (2)
  re-issue the same cast every eligible tick, interrupting itself before
  a real cast time could complete -- fixed by checking
  `bot->IsNonMeleeSpellCast(false)` first. Both confirmed via the
  regression suite going from intermittently exceeding its 40s timeout
  back to a clean `5/5`. **Use this pattern for any future
  `Tick()`-level recovery/policy check**: gate on "no active objective in
  flight," not just the policy's own safety condition, and be aware that
  `Combat::Execute`'s cast-based intents can self-interrupt without an
  in-flight-cast check.

  **Honest gaps remaining**: `RequestCallPet` (`MissingAlive` recovery,
  spell id 883) is implemented and gated correctly but **not yet
  live-verified** -- no test Hunter reached the level to learn it. And a
  revived pet's DB row isn't updated by an explicit save in that code
  path -- an abrupt (non-clean) worldserver restart shortly after a
  revive can lose it before the normal periodic autosave (900s) or a
  clean logout would have persisted it; not itself a bug, just worth
  knowing when testing across a restart. Some intermittent
  `guidestartcombat` wall-clock-timeout failures persisted even after
  both fixes above -- live-investigated and attributed to the
  pre-existing, already-documented `KNOWN_FAILURES.md` #6 (ADR-029
  Engaged-phase timeout), confirmed unrelated to pet recovery (the pet
  was `MissingDead` and recovery correctly did not fire during the stuck
  `Engaged` state) -- not a new issue, not chased further given a small
  sample and a heavily-reused test character.

**Gate 3's own literal acceptance bar (`ROADMAP.md`) is closer but still
NOT fully met — stated plainly, not glossed over:**
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
- "Pets": **a real first slice now exists** (tame/status/react-state,
  combat-assist proven) — but `GuideRuntime` itself has zero pet
  awareness (no auto-tame step, no auto-aggressive-on-tame, no
  pet-revive-on-death), and Warlock/DK pet summoning is untouched. Not
  "done," but no longer "nothing exists."
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
  `45b252f` (ADR-035, stop-movement-on-timeout fix), plus this handoff
  commit — all pushed to origin.
- zoidberg's live `ac-worldserver` is running the latest code.
- Test fixtures on zoidberg:
  - account `ap_test1` (id 204), character `Grunttestbot` (Orc Warrior,
    level 3+). **Currently dead and NOT trivially recoverable** -- ended
    up ~1500 yards from Valley of Trials this arc (an unrelated real
    hazard of this session's own cross-country `moveto` testing, not a
    module defect, see `KNOWN_FAILURES.md` #10's closing note) and its
    corpse is in what appears to be an environmentally hazardous spot
    near `(227.0, -3261.3, 65.7)` on map 1 -- `reclaimcorpse` resurrects
    it but it dies again immediately at the same position with zero
    movement in between (consistent with a real damage-over-time/instant-
    death hazard like lava or void at that exact spot, not a code bug --
    a real human player who died in lava would have the same problem).
    Not fixed/recovered this session -- deprioritized as a real but
    self-inflicted test-environment issue, not blocking any other work.
    Whoever needs this fixture next: try releasing spirit, then manually
    walking the *ghost* well away from that exact spot before
    `reclaimcorpse` (ghosts are typically immune to environmental
    damage), or just re-provision a fresh Orc Warrior if that fails.
  - account `ap_priest1` (id 205), character `Priestestbot` (Human
    Priest, level 1), Northshire Abbey, map 0 -- untouched this arc,
    presumed still fine.
  - account `ap_test2` (id 206), character `Grunttestii` (Orc Warrior,
    level 1) — the second Horde character ADR-032 unblocked; used for
    live ADR-031 tap/other-player-attacking verification. Alive and
    controllable as of the last check this arc; was manually walked
    toward Valley of Trials mid-session and may not have arrived --
    check `.autonomousplayer status` before assuming its position.
  - account `ap_test3` (id 207), character `Grunthunter` (Orc Hunter,
    level 2), alive at full health as of session end (pos roughly
    `629.8, -2205.1, 133.7`, map 1 -- far from Valley of Trials after
    heavy `guidestartmoveto`/`guidestartcombat` regression cycling this
    session; check `.autonomousplayer status` before assuming its
    position, it will very likely have drifted further). **Died twice
    more during this session's own regression testing**
    (`guidestartmoveto_unreachable_target_times_out` walks it toward a
    genuinely unreachable point, which crossed real hazardous terrain
    both times) -- both real, both matching the already-documented
    `KNOWN_FAILURES.md` #10 cross-country-travel risk, both recovered via
    the normal `releasespirit`/`reclaimcorpse` sequence (respect the real
    ~30s reclaim cooldown). Its pet is currently `PetState::MissingDead`
    (confirmed `RequestRevivePet` works on this exact state earlier the
    same session, ADR-040, but it wasn't re-revived at session end --
    genuinely left in this state, not a broken fixture, just an accurate
    snapshot). Verify with `.autonomousplayer petstatus Grunthunter`
    before assuming its state. This bot does NOT currently reproduce a
    Call Pet test case (`PetState::MissingAlive`) -- for that, a Hunter
    needs to level far enough to learn Call Pet, then have its pet
    dismissed while still alive (not killed) to construct that state.
    Given how heavily this specific character has been battered this
    session (repeated deaths, repeated pet death/revival cycles, level 2
    the entire time), whoever picks up further pet-recovery or
    Engaged-phase-timeout work should strongly consider provisioning a
    fresh, higher-level character instead of continuing to reuse this
    one -- diminishing signal-to-noise from an increasingly worn-down
    fixture was a real, felt cost by the end of this session.
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
13 Gate 3 entries in `KNOWN_FAILURES.md` (plus 6 in Gate 1, several
non-bug findings in Gate 2). Open, non-blocking: #3 (bounded-blacklist
path unexercised live), #6 (ADR-029 timeout — re-tested with a 13-trial
sample, not reproduced, downgraded to low-priority), #8
(`creaturestatus`'s `FindNearestCreature` alive-param footgun — a
one-line fix, not yet applied to that command itself, worked around
locally in `targetsafety`). **#10 (bounded-timeout/stale-movement safety
gap), #12 (`castspell`'s MOVING-state bug), and #13 (`RequestRevivePet`
bailing out on a missing pet instead of self-casting) are all FIXED and
live-verified (ADR-035, ADR-036, ADR-040)** — no longer open items. #13's
writeup is worth reading regardless: it records two wrong theories
(a stale `GetPetGUID()`, then a wrongly-concluded "structural fork
limitation") before landing on the real fix, and remains a real,
noted-but-open item on one point -- `RequestCallPet`
(`PetState::MissingAlive` recovery) is implemented but not yet
live-verified (see NEXT TASK #1). #11 is a non-bug (`COMBAT_TOO_HARD`
observed for real, working as designed).

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
Gate 3's external-review debt is paid off, all safety/tooling bugs found
this arc (`KNOWN_FAILURES.md` #10, #12) are fixed and re-verified, and
pets now has a real, `GuideRuntime`-integrated slice with a proper,
fully-verified decision layer (`Combat::CombatIntent`/`Combat::Execute`,
`Recovery::PlanPetRecovery`, ADR-037/038/039/040 -- pet revival for a
dead pet, whether loaded or not, is confirmed working live). In rough
priority order:

1. **Live-verify `RequestCallPet`** (`PetState::MissingAlive` recovery,
   spell id 883): implemented and gated correctly (`Player::HasSpell`)
   but never actually cast this session -- no test Hunter reached the
   level to learn Call Pet. Needs a Hunter leveled far enough to have it,
   then a controlled scenario where the pet gets dismissed-while-alive
   (not killed) to construct a real `MissingAlive` state to test against.
2. **Auto-tame-if-no-pet**: still not attempted -- a Hunter guide with no
   pet does not try to acquire one. Real, separate scope from revive.
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
6. **`KillNearest`'s bounded-blacklist path** (`KNOWN_FAILURES.md` #3) —
   still never exercised by a genuine unreachable-target scenario live.
7. **Warlock demon summoning** — a separate mechanic from Hunter taming,
   entirely untouched; only worth it once a Warlock test character is
   provisioned and levels enough to have a summon spell.

**Architecture note for whoever picks this up:** combat/pet decisions
now go through `Combat::CombatIntent`/`Combat::Execute` rather than
`GuideRuntime` calling low-level primitives directly -- extend that
pattern for new behaviors (a new `IntentKind` + a case in
`Combat::Execute`) rather than adding another isolated call site.
Recovery-style "should this even run right now" decisions (like pet
revival) belong in a `Recovery::Plan*` policy function returning
`std::optional<CombatIntent>`, checked centrally in `GuideRuntime::Tick`
before the current step dispatches, not inlined into a step's own tick
function.

**Calibration note for whoever picks this up:** this arc's own
`IsHostileTo`→`IsValidAttackTarget` catch and the `FindNearestCreature`
alive-param footgun are good examples of why testing a diagnostic
command against real live state (not just reading the code) keeps
finding real bugs even in code that "looks right." Keep doing that
before declaring anything "Verified." Also: **run
`tools/live_regression_suite.py` before starting any new work and after
every change** — it's cheap, real, and would have caught this arc's own
regressions automatically instead of requiring a human/agent to notice.

## Next-session acceptance criteria
- Real progress on pets or dense-camp/cave validation — whichever is
  picked, with real live evidence, not just code review.
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`, and
  `tools/live_regression_suite.py` all still pass.
- Docs updated with calibrated claims, committed.

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX,HONORBUDDY_SINGULAR_COMBAT_RESEARCH}.md in
full, especially this file's "NEXT TASK" section and `KNOWN_FAILURES.md`
#13 (its full in-order writeup of two wrong theories before the real
fix -- genuinely instructive about verifying the actual code path a
spell runs instead of the first plausible-looking rejection), before
continuing. Also check agent memory for
`autonomous-player-zoidberg-soap-access` before doing any live testing --
it has the exact SOAP mechanism and the container-recreate procedure
needed to actually deploy new code (`docker restart` alone does not pick
up a rebuilt image). Gate 3's external-review debt is fully paid off,
all safety/tooling bugs found this arc are fixed and re-verified, and
pets now has a real, `GuideRuntime`-integrated slice with a proper,
fully-verified decision layer (`Combat::CombatIntent`/`Combat::Execute`,
`Recovery::PlanPetRecovery`, ADR-037/038/039/040) -- **use that pattern
for any new combat/pet behavior, don't add another isolated primitive
call site directly in `GuideRuntime`**. Pet revival (dead pet, whether
currently loaded or not) is confirmed working live -- reproduced twice.
Top priority: live-verify `RequestCallPet` (`PetState::MissingAlive`
recovery) the same rigorous way -- needs a Hunter leveled far enough to
actually have Call Pet learned (none did this session), then a
controlled scenario where the pet is dismissed while still alive (not
killed) to construct a real `MissingAlive` state. After that:
auto-tame-if-no-pet, dense camps/caves, ranged-pulls-as-distinct-
behavior, full race breadth, Warlock demon summoning. **Run
`tools/live_regression_suite.py` before starting and after any change
that touches `GuideRuntime`/`Combat`/`Setup`/`Pets`/`Recovery`** to catch
regressions automatically -- this arc found real bugs this way multiple
times (the `IsHostileTo` regression, the suite's own
own-pet-vs-wild-creature test ambiguity). Design briefly, implement the
smallest testable increment, compile-check and live-verify on zoidberg
with real evidence (build-and-deploy is pre-approved), update docs with
calibrated (not overstated) claims, commit. Keep going without stopping
to check in, except for a genuine blocker or an ambiguous decision only
the user can make.
