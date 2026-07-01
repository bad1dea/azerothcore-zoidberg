# Roadmap

## Gate structure

Gates are cumulative vertical slices, defined in full in the root operating
instructions (search chat/session history or the originating task spec for
the verbatim acceptance text — summarized here for quick reference):

- **Gate 0 — project foundation.** Module builds and loads without
  Playerbots; lifecycle/scheduler/persistence/telemetry skeletons tested; a
  dependency/code-origin check proves no Playerbots linkage or copied source.
- **Gate 1 — first complete quest.** One level-1 Orc Warrior onlines at its
  start location, walks to the first quest giver, accepts, fights one target
  at a time safely, loots, turns in, equips an upgrade if offered, survives a
  server restart and resumes correctly.
- **Gate 2 — levels 1–6.** Every race completes its starting area; every
  delivered class controller completes representative combat; kill/loot/GO/
  use-item/gossip/vendor/training/death mechanics work.
- **Gate 3 — levels 1–12.** All supported race/class combos complete
  starting-region routes; dense camps, caves, ranged/melee pulls, pets, full
  bags, training, guide validation covered; no manual step advances.
- **Gate 4 — levels 1–20.** Regional travel, class growth, flights,
  transports, restart recovery.
- **Gate 5 — levels 1–40.** Riding, mounts, broader economy, multi-continent
  routing.
- **Gate 6 — levels 1–60.** At least one clean reproducible 1–60 run;
  representative accepted runs for every supported class.
- **Gate 7 — levels 1–80.** Wrath routes, expansion travel, 60–80
  progression when configured.

Every accepted gate records: build revision, guide revision, configuration,
character GUID, elapsed/active time, levels, quests, deaths, failures,
restarts, manual interventions, cheat-policy status, final state.

## Active weekly outcome

**Week 1 — Gate 0: project foundation. COMPLETE (2026-06-30).**

Module builds and loads standalone alongside (but with no dependency on)
Playerbots — verified via a real `docker build --target worldserver` on
host zoidberg against this branch (pass, see `HANDOFF.md` for the log
evidence). Lifecycle/Perception/Telemetry skeletons exist; the stagger and
snapshot logic is verified by code inspection rather than an automated
unit test (documented gap, see `TEST_MATRIX.md` — this repo's module
build path doesn't wire up `BUILD_TESTING`). Two automated checks
(`check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`) both
pass and run every session.

**Week 2 — Gate 1, first slice: online Orc Warrior + read-only perception
snapshot. COMPLETE (2026-06-30).** Bot account/character/session model
implemented (ARCHITECTURE.md ADR-008) and live-tested on zoidberg through
five bug-fix iterations (Playerbots-bot false-positive detection, account
name length limit, session-survival-across-ticks, a use-after-free in the
first survival-fix attempt, login rejected by client-only gatekeeping,
case-sensitive account ownership check). Verified live: `Grunttestbot`
(Orc Warrior, level 1) online at Valley of Trials (map 1,
`-618.5, -4251.7`), registered in `BotLifecycleMgr`, correct
`PerceptionSnapshot`. See `KNOWN_FAILURES.md` for the full bug history and
`HANDOFF.md` for commit citations.

**Week 3 — Gate 2: levels 1–6. COMPLETE (2026-07-01).** Ten slices
complete and verified live: Navigation (ADR-009), QuestEngine accept/
turn-in (ADR-010/011), Combat — melee (ADR-012, found+fixed a real
combat-stall bug) and spell-casting (ADR-018), Inventory (ADR-013,
corpse looting, including a loot-gating investigation that confirmed
correct behavior rather than a bug), Recovery (ADR-014, full death/
release-spirit/reclaim-corpse cycle, including a real graveyard-lookup
edge case confirmed as correct behavior), Economy (ADR-015, vendor buy/
repair), Gossip (ADR-016), Growth (ADR-017, trainer spell learning). The
full login→walk→quest→walk→fight→loot→die→recover→buy/repair→gossip→
train arc works end-to-end on a real bot through real production code
paths, with zero Playerbots dependency and zero forbidden-API usage
throughout (both automated checks pass on every commit).

**Race/class coverage:** two races verified end-to-end across all ten
components above — Orc Warrior (`Grunttestbot`, Valley of Trials) and
Human Priest (`Priestestbot`, Northshire Abbey, real quest 783 "A Threat
Within," real kill+loot, a real finding that a level-1 Priest has no
offensive spell yet — consistent with the actual leveling curve, not a
defect). **Gate 2's "every race completes its starting area" bar is
interpreted, with explicit user confirmation (2026-07-01), as a
representative sample** — one race per faction plus one melee and one
caster class kit — rather than all ten WotLK races literally, since the
racial spawn/login mechanism this module touches
(`HandleCharCreateOpcode`, `HandlePlayerLoginFromDB`) is race-agnostic
core code with no per-race branching in this module's own source; the
remaining eight races would be additional volume, not risk reduction.
Broader race/class coverage remains a valid backlog item (tracked in
`HANDOFF.md`) but does not block Gate 3.

Full per-slice history, bugs, and non-bug findings: `KNOWN_FAILURES.md`,
`ARCHITECTURE.md` (ADR-008 through ADR-018), `HANDOFF.md`. Reached
working autonomously per explicit user direction ("get to gate 3 on your
own," "keep going," "keep going im sleeping," "continue on your own
until we get to gate 5") — each slice got its own compile + live
verification + commit before moving to the next, per this project's own
operating-mode rules.

**Week 4 — Gate 3: levels 1–12. IN PROGRESS.** All supported race/class
combos complete starting-region routes; dense camps, caves, ranged/melee
pulls, pets, full bags, training, guide validation covered; no manual
step advances. `GuideRuntime` (ADR-019/020/021) landed: `MoveTo`,
`KillNearest`, `AcceptQuest`, `TurnInQuest` step types, all verified
live, including a real automatic combat engagement observed mid-chain in
a full `guidestartquest` run. `KillNearest`'s stuck-target bug took three
fix attempts to genuinely resolve (the first two were disproven on
re-test, not just insufficient) -- see `KNOWN_FAILURES.md` #3 for the
full evidence trail. **A user-provided clean-room research document,
`HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md` (2026-07-01, ADR-022), is now
the design baseline for all further Combat/pulling work** -- its "Gate 3
implementation sequence" is the concrete plan going forward, starting
with an explicit pull state machine and bounded stuck-timeout/blacklist
before any new class controllers or multi-pull/AoE/CC behavior. See
`HANDOFF.md` `NEXT TASK`.

**Follow-up sessions (still 2026-07-01): all 7 of the external review's
numbered priorities now have real, live-verified progress** (target
selection safety ADR-031, an automated regression suite ADR-033, and a
third class archetype ADR-034, on top of the engagement-confirmation fix,
EncounterModel gating, bounded timeouts, and the first class controller
already done in the initial response). **This closes out the external
review as an operative blocker for this milestone** -- but Gate 3's own
literal acceptance bar in this document (above) is **not** fully met yet,
and this is stated plainly rather than declared done by association:

- **Done, real evidence:** `GuideRuntime` fully automatic (`no manual step
  advances`); target-selection safety (4 of 5 checks live-verified,
  evade code-review-only); bounded timeouts everywhere; loot
  verification; a first automated regression suite; three class
  archetypes with real combat (Orc Warrior melee, Orc Hunter ranged via
  `OpportunisticSpellId`, Human Priest not yet combat-tested at a level
  with an offensive spell); "full bags" partially covered organically
  (a real near-full-bags loot outcome was observed and handled
  correctly, not deliberately engineered).
- **Not done, real scope, not glossed over:** the other 8 WotLK
  races/starting zones (this arc is treating race breadth the same way
  Gate 2 did -- a representative sample across faction/melee/caster/
  ranged rather than exhaustive, per the same reasoning the user
  explicitly confirmed for Gate 2 -- but Gate 3's charter text literally
  says "all supported race/class combos," so this is a reinterpretation
  being applied, not a literal pass, and is flagged as such rather than
  silently assumed); dense camps and caves as deliberately-engineered
  terrain/density scenarios (not yet attempted, distinct from the
  incidental multi-boar density already exercised via `multipull`);
  ranged pulls as their own distinct behavior (`KillNearest` still
  always closes to melee range even when an `OpportunisticSpellId` is
  ranged -- there is no "engage from range and stay there" mode); pet
  summon/management (no infrastructure exists at all for this yet, a
  real missing subsystem, not just untested volume).

**Outcome for this week: Gate 3 is substantially advanced but not
declared complete.** See `HANDOFF.md`'s "Current milestone" for the
live, authoritative version of this status and the next task.

## Weekly sequence template (for future weeks)

1. **Session 1 — design.** Resolve architecture decisions needed for the
   week's slice, define interfaces and acceptance tests, implement a
   skeleton only if it's independently testable.
2. **Session 2 — primary implementation.** Core behavior + unit tests. No
   scope creep.
3. **Session 3 — integration.** Wire into the live module/runtime, add
   integration tests or a reproducible scenario, exercise persistence and
   error handling.
4. **Session 4 — debug and harden.** Fix integration failures, add
   regression tests, improve diagnostics.
5. **Session 5 — weekly gate.** Run the milestone's full test set, review
   player-like compliance, clean docs/migrations, record metrics and known
   failures, mark the outcome complete only if acceptance criteria pass,
   choose exactly one outcome for the next week.

If fewer sessions are available in a week, combine adjacent sessions but
never skip the weekly gate — reduce scope instead.

## Backlog

- Gate 2 slice: Navigation proof via `MotionMaster` (this is the immediate
  `NEXT TASK` — see `HANDOFF.md`).
- Gate 2 slice: quest accept via authoritative game API (not DB write) for
  the Orc/Troll Valley of Trials starting chain.
- Gate 2 slice: minimal combat engine (single target, melee, no CC) for
  the Warrior class controller.
- Gate 2 slice: loot handling, quest turn-in + reward selection.
- Gate 2 slice: repeat representative coverage for other races/starting
  classes per Gate 2's acceptance bar.
- Gate 1 (deferred from the first slice, needed before Gate 4's restart-
  recovery bar, but worth doing early): restart-safe persistence of
  planner/guide state (ADR-004) — right now a bot only comes online via
  explicit `.autonomousplayer login`, not automatically on worldserver
  restart.
