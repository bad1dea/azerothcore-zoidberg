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

**Week 1 — Gate 0: project foundation.**

Target: module builds and loads standalone (no Playerbots dependency),
Lifecycle/Perception/Telemetry skeletons exist and are unit-tested, and an
automated check proves no Playerbots linkage. This is being delivered as a
single first session (design + skeleton combined) because Gate 0's scope is
inherently small — see `HANDOFF.md` for exactly what's done vs. deferred.

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

## Backlog (post Gate 0, not yet scheduled)

- Gate 1 slice: Orc Warrior online + read-only perception snapshot (this is
  the immediate `NEXT TASK` — see `HANDOFF.md`).
- Gate 1 slice: local navigation/pathing to a fixed quest-giver coordinate.
- Gate 1 slice: quest accept via authoritative game API (not DB write).
- Gate 1 slice: minimal combat engine (single target, melee, no CC).
- Gate 1 slice: quest turn-in + reward selection.
- Gate 1 slice: restart-safe persistence of planner/guide state.
