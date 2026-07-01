# Session Handoff

## Current milestone
**Gate 2 — COMPLETE (2026-07-01).** Ten slices verified live across two
races/classes (Orc Warrior, Human Priest) — user confirmed this
representative sample satisfies Gate 2's "every race" bar (see
`ROADMAP.md`'s Week 3 entry).

**Gate 3 — levels 1–12, IN PROGRESS.** `GuideRuntime` (ADR-019/020/021)
is the project's first real automatic multi-step execution — every prior
capability in this arc required a human to trigger each individual step.
Three step types exist: `MoveTo` (verified live, clean), `KillNearest`
(verified live for a nearby/reachable target; **has an open,
not-yet-resolved bug for a target found near the edge of the search
radius** — see "Known failures" below, don't skip this), `AcceptQuest`/
`TurnInQuest` (interaction-range logic verified correct; full
accept→kill→turn-in chain not yet observed completing cleanly end-to-end
because of the `KillNearest` issue). Full per-slice history is in
`KNOWN_FAILURES.md` and `ARCHITECTURE.md` (ADR-008 through ADR-021) —
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

## An honest, unresolved bug — read this before touching KillNearest again
`.autonomousplayer guidestartquest` (chaining `AcceptQuest`→`KillNearest`→
`TurnInQuest`) repeatedly got the bot stuck during the `KillNearest` step
when the found target was near or past the edge of the 100-yard search
radius. **Two real fixes were applied and both are staying in the code**
(they're genuine improvements, verified correct by reasoning even though
neither fully solved the live symptom):
1. Wait for real arrival (`MeleeEngageToleranceYards`, 5 yards) before
   issuing `Combat::RequestAttack`, instead of firing it immediately from
   an arbitrary distance.
2. Use `MotionMaster::MoveChase` (continuous-follow) instead of a
   one-shot `Navigation::MoveTo`, since Mottled Boars have real wandering
   AI and a fixed-point walk order can't track a moving target.

**Both fixes were re-tested live and the bot still got stuck a third
time** — same symptom (frozen position, `combat=false`, no target
creature found within 100 yards afterward), different exact stall
position each time. Current best hypothesis, **not confirmed, not chased
further this session**: `Player::FindNearestCreature` is a straight-line
distance check with no navmesh-reachability awareness, so it can select a
target that looks close but requires a long/blocked real path (or one
whose live position has drifted well past the search snapshot) — even a
correct continuous-chase generator has nothing reachable to converge on
in that case. Full detail, the exact test sequence, and what to check
next time is in `KNOWN_FAILURES.md` #3 — **read it before attempting a
third fix**, don't re-derive from scratch.

## Files changed (cumulative, this arc)
- `docs/autonomous-player/ARCHITECTURE.md`: ADR-008 through ADR-021.
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
- Compiled clean on zoidberg 33 times across this arc; currently deployed
  commit compiles clean.
- Every capability above verified **live** on zoidberg. `KillNearest` in
  isolation and `MoveTo`-chains are cleanly verified; the full
  `guidestartquest` chain is not (see above).

## Current repository state
- Branch: `mod-autonomous-player`. Most recent commit: `3911009`
  (MoveChase fix attempt), plus this handoff commit — all pushed to
  origin.
- zoidberg's live `ac-worldserver` is running the latest pushed commit.
- Test fixtures on zoidberg:
  - account `ap_test1` (id 204), character `Grunttestbot` (guid 2014, Orc
    Warrior, level 2, 0 copper). **Currently has a stuck guide state**
    from the unresolved bug above (frozen near `-607, -4211` on map 1) —
    harmless (no crash, no resource leak observed), but be aware before
    reusing this fixture; the bot never actually logged out so
    `.autonomousplayer login` won't reset it (`TryLoginBot` correctly
    no-ops on an already-online character) — a genuine restart or a
    future `guidestop`/guide-clear command would be needed to reset it
    cleanly.
  - account `ap_priest1` (id 205), character `Priestestbot` (guid 2015,
    Human Priest, level 1, 0 copper, quest 783 rewarded), near Marshal
    McBride (`-8902.6, -162.6, 81.9` on map 0, Northshire Abbey).
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
Full history (6 bugs in Gate 1, 1 in Gate 2, 3 in Gate 3 — 2 fixed, 1
open — see `KNOWN_FAILURES.md` #1–3 for Gate 3) plus several documented
non-bug findings (quest interaction range, quest-gated loot,
no-graveyard-nearby ghost behavior, insufficient-funds rejections,
no-offensive-spell-at-level-1) is in `KNOWN_FAILURES.md`. **One item is
currently open and blocking full `guidestartquest` verification:**
`KillNearest` can get permanently stuck when its target is near/past the
search radius edge (`KNOWN_FAILURES.md` #3). One minor, non-blocking
anomaly: a one-time transient `provision` failure right after a fresh
redeploy, succeeded on identical retry — not root-caused.

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
  that exact situation with `KillNearest` and chose to document the
  investigation honestly (attempt-by-attempt, with the current best
  hypothesis) rather than either overclaiming a fix or endlessly
  patching without being confident of the mechanism. This is the
  standard to hold future sessions to as well.

## NEXT TASK
**First priority: root-cause and properly fix `KillNearest`'s stuck-on-
distant-target bug** (`KNOWN_FAILURES.md` #3) before adding more
GuideRuntime step types on top of a component with a known live gap.
Concrete next steps to try, in rough order of cost:
1. Cheap mitigation: reduce the guide commands' search radius (currently
   100 yards, hardcoded in `cs_autonomousplayer.cpp`'s `guidestartcombat`/
   `guidestartquest` handlers) to something smaller (e.g. 40-50 yards) and
   re-test — if targets found closer are reliably reachable, this proves
   the hypothesis without needing engine-level changes.
2. If still stuck: add real diagnostics before guessing again — e.g. a
   temporary debug command or log line that reports the target's actual
   live position vs. the bot's position every tick during the Approaching
   phase, so a genuine stall vs. slow-but-real progress can be told apart
   definitively (this session inferred "stuck" from position snapshots
   1-15 seconds apart, which is suggestive but not proof of a true
   deadlock vs. a very slow path).
3. If the target is confirmed reachable-but-slow: consider whether
   `MoveChase`'s default chase distance/behavior is appropriate, or
   whether Valley of Trials' terrain triggers unusually expensive
   pathing.
4. If the target is confirmed genuinely unreachable: `FindNearestCreature`
   needs either a reachability check or the guide step needs a stuck-
   timeout that gives up and retargets — a real, scoped follow-up slice.

**After that's resolved:** verify `guidestartquest`'s full
accept→kill→turn-in chain completes cleanly end-to-end (this has still
never been observed), then continue Gate 3 scope per `ROADMAP.md`: dense
camps/caves, ranged pulls, pets, full bags, broader guide validation,
more race/class combos.

## Next-session acceptance criteria
- `KillNearest`'s stuck-on-distant-target bug is either genuinely fixed
  and re-verified live (not just theorized), or the investigation has
  concretely advanced (e.g. real diagnostic evidence distinguishing
  "stuck" from "reachable but slow") — not just another unverified patch.
- If fixed: `.autonomousplayer guidestartquest` observed completing its
  full accept→kill→turn-in chain cleanly at least once, live.
- `check_no_playerbots_dependency.sh` and `check_no_forbidden_apis.sh`
  still pass.
- Docs updated honestly to match whatever was actually verified, committed.

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX}.md, especially `KNOWN_FAILURES.md` #3 (the
open `KillNearest` bug) before touching `GuideRuntime` again. Continue
Gate 3 autonomously per the user's standing instruction: root-cause and
properly verify-fix that bug first (see HANDOFF.md NEXT TASK for concrete
next steps), then resume adding Gate 3 scope. Compile-check and
live-verify every change on zoidberg (build-and-deploy is pre-approved),
update docs honestly to match what was actually observed, commit. Keep
going without stopping to check in, except for a genuine blocker or an
ambiguous decision only the user can make.
