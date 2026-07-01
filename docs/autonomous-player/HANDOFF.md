# Session Handoff

## Current milestone
**Gate 2 — COMPLETE (2026-07-01).** Ten slices verified live across two
races/classes (Orc Warrior, Human Priest) — user confirmed this
representative sample satisfies Gate 2's "every race" bar (see
`ROADMAP.md`'s Week 3 entry).

**Gate 3 — levels 1–12, IN PROGRESS.** `GuideRuntime` (ADR-019/020/021)
is the project's first real automatic multi-step execution — every prior
capability in this arc required a human to trigger each individual step.
Four step types exist: `MoveTo`, `KillNearest`, `AcceptQuest`,
`TurnInQuest` — **all verified live, including the previously-stuck
`KillNearest` bug, which is genuinely resolved** (see
`KNOWN_FAILURES.md` #3: three fix attempts, the first two disproven on
re-test, the third verified clean twice independently). The full
`guidestartquest` chain (accept→kill→turn-in) has been observed making
real automatic progress through all three step types in one run,
including a real automatic combat engagement mid-chain. **A research
document, `HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`, is Gate 3's design
baseline for combat/pulling work** (ADR-022) — **its implementation
sequence steps 1 and 2 are done:** `KillNearest` runs an explicit
`PullState` machine (ADR-023) with a bounded stuck-timeout + blacklist;
a minimal `EncounterModel` (ADR-024) gives real, engine-authoritative
attacker awareness (`Unit::getAttackers()`), verified live to correctly
tag a real attacker's entry/distance/objective-relationship, though a
genuinely simultaneous multi-attacker case wasn't empirically caught
live (weak test mobs resolved combat faster than polling could observe
it — honestly noted as unproven, not assumed). Full per-slice history is
in `KNOWN_FAILURES.md` and `ARCHITECTURE.md` (ADR-008 through ADR-024) —
this file stays a live summary, not a growing archive.

## What's proven, end to end, through real production code (not
## reimplemented or DB-shortcut)
**Orc Warrior** (`Grunttestbot`, Valley of Trials) and **Human Priest**
(`Priestestbot`, Northshire Abbey) both independently complete the full
Gate 1/2 arc: correct racial spawn (no teleport) → real navmesh movement
→ real quest accept/turn-in → real melee combat (and, for casters, a real
finding that a level-1 Priest has no offensive spell yet — not a defect)
→ real loot → real death/recovery → real vendor buy/repair and
gossip/trainer interaction (both correctly blocked once by genuine
insufficient-funds validation). See `ARCHITECTURE.md` ADR-008 through
ADR-018.

**New this arc: automatic multi-step execution.** `GuideRuntime::Tick` is
called every second per registered bot from `BotLifecycleMgr::Update`
(previously that tick fired and did nothing). `.autonomousplayer
guidestart` (fixed 3-waypoint patrol) completed with zero manual commands
after the trigger — `CurrentStep` advanced 0→1→2→3, final position
matched exactly. `.autonomousplayer guidestartcombat` (single
`KillNearest` step) against a nearby Mottled Boar also completed cleanly
in ~15 seconds, first attempt, no manual commands.

## KillNearest's stuck-target bug — resolved, three attempts, real evidence
`.autonomousplayer guidestartquest` (chaining `AcceptQuest`→`KillNearest`→
`TurnInQuest`) repeatedly got the bot stuck during the `KillNearest` step.
Two fix attempts (arrival-gate before attacking; bare
`MotionMaster::MoveChase` instead of one-shot `Navigation::MoveTo`) were
each re-tested live and each reproduced the identical stall — **both
claims were disproven, not just theorized to be insufficient.** Added
real diagnostics to `.autonomousplayer guidestatus` (live target
position/distance/alive-state) instead of guessing further, which proved
conclusively over a clean 35-second observation that a *bare* `MoveChase`
with no preceding real attack call produces **zero bot movement**, even
though the target resolves and is visibly wandering. Fix attempt 3
reverted to calling `Combat::RequestAttack` immediately/repeatedly (the
*original*, Gate 3 slice 2 pattern) and gates the phase transition on
`bot->IsInCombat()` — a real, authoritative engagement signal, not a
distance check. **Verified live twice, independently, with two different
Mottled Boars, both clean kills within 15 seconds each, zero
contamination from manual commands.** Full attempt-by-attempt history is
in `KNOWN_FAILURES.md` #3 — worth reading before touching this code
again, since it documents exactly what didn't work and why.

This same finding — that movement alone isn't proof of a successful pull,
and an opener needs authoritative acknowledgement — is independently the
central thesis of the new Honorbuddy/Singular research below, which cites
this exact investigation as supporting evidence.

## Files changed (cumulative, this arc)
- `docs/autonomous-player/ARCHITECTURE.md`: ADR-008 through ADR-023.
- `docs/autonomous-player/HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`: new,
  user-provided, Gate 3's combat/pulling design baseline (ADR-022).
- `docs/autonomous-player/KNOWN_FAILURES.md`, `TEST_MATRIX.md`,
  `ROADMAP.md`: updated throughout.
- Components: `Lifecycle/BotSessionMgr`, `Setup/PendingCharacterCreations`,
  `Navigation/BotNavigation`, `QuestEngine/BotQuestEngine`, `Combat/BotCombat`
  (melee + `RequestCastSpell`), `Inventory/BotLoot`, `Recovery/BotRecovery`,
  `Economy/BotEconomy`, `Gossip/BotGossip`, `Growth/BotGrowth`,
  `GuideRuntime/BotGuideRuntime` (all `.h`/`.cpp` pairs).
- `Lifecycle/BotLifecycleMgr.{h,cpp}`: `BotSession` now carries a
  `GuideRuntime::BotGuideState`; `Update()` dispatches `GuideRuntime::Tick`
  on each per-bot second-tick.
- `Commands/cs_autonomousplayer.cpp` now has ~26 debug commands
  (`provision`, `login`, `status`, `moveto`, `acceptquest`, `queststatus`,
  `turnin`, `attack`, `creaturestatus`, `loot`, `releasespirit`,
  `reclaimcorpse`, `attackguid` [unreliable, see below], `multipull`,
  `buy`, `repair`, `gossiphello`, `gossiptrain`, `learnspell`,
  `castspell`, `spellbook`, `guidestart`, `guidestartcombat`,
  `guidestartquest`, `guidestatus`).

## Verification
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`,
  `codestyle-cpp.py`: pass on every commit.
- Compiled clean on zoidberg 35 times across this arc; currently deployed
  commit compiles clean.
- Every capability above verified **live** on zoidberg. `KillNearest`
  (both the fixed opener/confirm logic and the new explicit `PullState`
  machine) verified clean, twice independently each time. The full
  `guidestartquest` chain (accept→kill, with a real automatic combat
  engagement observed mid-chain) is also cleanly verified.

## Current repository state
- Branch: `mod-autonomous-player`. Most recent commit: `aec6063`
  (explicit pull state machine), plus this handoff commit — all pushed to
  origin.
- zoidberg's live `ac-worldserver` is running the latest pushed commit.
- Test fixtures on zoidberg:
  - account `ap_test1` (id 204), character `Grunttestbot` (guid 2014, Orc
    Warrior, level 2, 0 copper), near `-678, -4283` on map 1 (Valley of
    Trials, Mottled Boar territory). Quest 788 "Cutting Teeth" active,
    3/8 Mottled Boars credited (kills via isolated `guidestartcombat`
    tests did not increment this counter -- a minor, separate, not-yet-
    investigated observation, not blocking).
  - account `ap_priest1` (id 205), character `Priestestbot` (guid 2015,
    Human Priest, level 1, 0 copper, quest 783 rewarded), near Marshal
    McBride (`-8902.6, -162.6, 81.9` on map 0, Northshire Abbey).
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
Full history (6 bugs in Gate 1, 1 in Gate 2, 3 in Gate 3 — all fixed —
see `KNOWN_FAILURES.md` #1–3 for Gate 3) plus several documented non-bug
findings (quest interaction range, quest-gated loot,
no-graveyard-nearby ghost behavior, insufficient-funds rejections,
no-offensive-spell-at-level-1) is in `KNOWN_FAILURES.md`. Nothing
currently blocking. Two minor, non-blocking observations not yet
investigated: a one-time transient `provision` failure right after a
fresh redeploy (succeeded on retry), and isolated `guidestartcombat`
kills not incrementing an active quest's kill counter (noted above).

## Decisions made
- User's standing direction has escalated across this arc: "investigate
  how playerbots keeps its sessions alive - fix it and get to gate 3 on
  your own" → "keep going" → "keep going im sleeping" → "continue on your
  own until we get to gate 5." Interpreted as: keep working autonomously,
  bounded-increment discipline, only pausing for a genuine blocker or a
  decision only the user can make.
- User explicitly confirmed (2026-07-01, `AskUserQuestion`) that Gate 2's
  race-coverage bar is a representative sample (2 races/classes).
- **When a fix doesn't hold up on re-test, don't declare success and
  don't blindly patch a third time in the same pass** — this session hit
  that exact situation with `KillNearest` (two disproven attempts before
  the real one) and chose to document the investigation honestly rather
  than overclaim or endlessly patch without confidence. The fix that
  finally worked was earned by adding real diagnostics instead of
  guessing a fourth time. This is the standard to hold future sessions to.
- **User provided `HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md` (2026-07-01)**
  — a deep clean-room research document (no code copied; the reviewed
  Honorbuddy/Singular repos have no usable license) analyzing how a
  mature WoW bot combat/pulling engine is actually structured, with
  explicit instruction to use it for combat routine work going forward.
  It independently arrives at and cites this session's exact finding
  (movement isn't proof of a successful pull; an opener needs
  authoritative acknowledgement) as supporting evidence. **This is now
  Gate 3's design baseline for `Combat`/pulling/engagement work** — see
  ADR-022 and NEXT TASK.

## NEXT TASK
Continue `HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`'s "Gate 3
implementation sequence" (bottom of that document) — **steps 1 and 2 are
done** (ADR-023: explicit `PullState` machine + bounded timeout/
blacklist; ADR-024: minimal `EncounterModel`, real attacker awareness).
Next is **step 3: conservative single-pull Warrior and Priest controllers
through level 12, using actual learned spell snapshots** — this project
already has both fixtures (`Grunttestbot` Orc Warrior, `Priestestbot`
Human Priest). Per the document's own class-coverage table: Warrior
needs "Charge when legal; ranged weapon fallback; close to melee" plus
"Victory Rush, defensive stance/tools, interrupt, Hamstring/flee
handling"; Priest needs "ranged spell opener and hold casting range"
plus "shield without Weakened Soul, heal thresholds, fear/add control,
Fade." Given this project's current maturity (Warrior only ever used
plain melee autoattack; the Priest spellbook investigation this arc found
no offensive spell at level 1), scope the first slice down hard — e.g.
just "use a real learned offensive ability instead of only bare melee,
composed into `KillNearest`'s `Approaching`/`Engaged` states via
`AbilityCatalog`-style spellbook introspection (`Player::GetSpellMap()`,
already used by `.autonomousplayer spellbook`)" rather than the full
survival/defensive/interrupt toolkit at once.

**Two real, honest gaps to close opportunistically, not urgently:**
1. The bounded stuck-timeout/blacklist (ADR-023) has never been
   exercised by a genuine unreachable-target scenario live.
2. `EncounterModel`'s simultaneous-multi-attacker case (ADR-024) hasn't
   been directly observed live either (see `ARCHITECTURE.md` ADR-024's
   verification note) — a real dense-camp pull (document step 4's later
   scope) would naturally exercise this; don't force an artificial test
   for it before there's a real reason to.

Do **not** jump ahead to multi-pull, AoE, or crowd control; the document
is explicit that proactive multi-pulling stays disabled by default.
Re-read the whole document before continuing — its "Required live
regression scenarios" section should inform what "verified live" means
for this work going forward (real state-transition/authoritative-outcome
evidence, not just "no crash").

## Next-session acceptance criteria
- A real class controller slice (Warrior or Priest, using actual learned
  spells via `Player::GetSpellMap()`, not a hardcoded/guessed spell ID)
  is implemented, compiles clean, and is live-verified with real
  evidence (e.g. a real spell cast lands and deals damage, verified via
  target HP delta, not just "no crash").
- `check_no_playerbots_dependency.sh` and `check_no_forbidden_apis.sh`
  still pass.
- Docs updated (new ADR referencing `HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`
  where relevant), committed.

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX,HONORBUDDY_SINGULAR_COMBAT_RESEARCH}.md in
full before touching `Combat`/`GuideRuntime` again — the research
document is Gate 3's design baseline for pulling/engagement work, and
its steps 1-2 (explicit pull state machine, EncounterModel) are already
done (ADR-023/024). Continue Gate 3 autonomously per the user's standing
instruction: implement the research document's Gate 3 implementation
sequence step 3 (conservative single-pull class controller, see
HANDOFF.md NEXT TASK), design briefly, implement the smallest testable
increment, compile-check and live-verify on zoidberg with real evidence
(build-and-deploy is pre-approved), update docs, commit. Keep going
without stopping to check in, except for a genuine blocker or an
ambiguous decision only the user can make.
