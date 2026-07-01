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

**Week 3 — Gate 2: levels 1–6. IN PROGRESS.** Three slices complete and
verified live (2026-07-01): Navigation (`MotionMaster`-based movement,
ADR-009), QuestEngine accept (ADR-010), QuestEngine turn-in (ADR-011) —
the full quest lifecycle (accept → complete → turn-in → reward) now works
end-to-end on a real bot via real production code paths. See
`HANDOFF.md` `NEXT TASK` for the next slice (minimal single-target melee
combat, Warrior class controller). Working autonomously through Gate 2's
full scope per explicit user direction ("get to gate 3 on your own") —
each slice still gets its own compile + live verification + commit before
moving to the next, per this project's own operating-mode rules. Session
paused after three slices (not a stopping point mandated by the work
itself, but a deliberate checkpoint after an unusually long session — see
"Decisions made" in `HANDOFF.md`).

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
