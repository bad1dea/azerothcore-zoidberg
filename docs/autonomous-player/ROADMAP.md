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

## Gate 3 route-quality addendum (required, 2026-07-04)

The user explicitly made the following requirements part of Gate 3 after a
comparison with the Honorbuddy/CopilotBuddy questing ecosystem. This addendum
is cumulative with the original Gate 3 bar and supersedes older statements
below that only race breadth and guide validation remained. Gate 3 is not
complete merely because isolated combat/quest primitives work: the supported
fleet must level through complete, efficient routes without grind-driven death
loops or manual route repair.

External sources are research inputs, not runtime dependencies:

- `Likon69/Questing-profiles` supplies a coverage/order/hotspot benchmark.
- `CopilotBuddy`, `CopilotBuddyDocs`, `Quest-Behaviors`,
  `Honorbuddy-Quest-Behaviors`, and `Singular-wotlk` supply observable
  behavior and profile-semantics references.
- Their quest IDs, coordinates, ordering, and assumptions must be validated
  against this server's `acore_world`; race/class gates, prerequisites,
  objective types, NPCs/gameobjects, loot sources, and real spawns are
  authoritative locally.
- No reviewed repository has a root license file at the reviewed revisions.
  Do not copy/port its implementation. Build the AzerothCore-native behavior
  clean-room. Do not integrate `Navigation-C-`: the server's own mmap,
  `PathGenerator`, and collision data are the authoritative navigation stack.

Gate 3 now additionally requires all of the following:

1. **Quest-density inventory and coverage.** For every supported starting
   route, generate a report of every locally eligible quest, whether it is in
   the route, and a structured reason for every omission (wrong race/class,
   unsupported objective behavior, unsafe/group content, invalid chain, or
   deliberate route-quality choice). The current external-profile comparison
   found 69/61/51/90/61/77 unique profile quest IDs for Durotar/Mulgore/
   Tirisfal/Eversong/Elwynn/Dun Morogh versus 20/10/8/7/16/9 in the current
   representative route files; these are leads to validate, not counts to
   copy blindly.
2. **Quest-first XP plan.** Routes must forecast quest rewards, expected kill
   XP, level checkpoints, training, and zone transitions. Unstructured
   `grind_to_level` is a bounded fallback, not the leveling backbone. In an
   accepted run, filler grinding must account for no more than 20% of active
   leveling time unless the generated coverage report proves the zone has no
   supported quest path for that deficit.
3. **Hub batching and objective overlap.** Route semantics must support
   picking up multiple compatible quests at a hub, ordering overlapping
   objectives together, then batching turn-ins. A route made entirely from
   serial `accept -> finish one quest -> turn in` transactions does not meet
   the efficiency requirement when locally valid quests can be co-routed.
4. **Adaptive re-leveling with a productive escape.** A death-budget or
   difficulty gate must select another quest, a safer hunting ground, gear/
   training maintenance, or a calculated XP bridge. It must never defer a
   step behind `level > current` when every remaining XP source is already
   complete or is the same losing grind. A no-progress pass is a test failure,
   not a successful bounded outcome.
5. **Recovery between every pull.** A chained guide may not bypass pull-safety
   checks. Before every new target, re-evaluate health, mana/resource, pet,
   consumables, equipment durability, resurrection sickness, attackers, and
   safe rest location. The current `kills_per_issue=6` flow, with a health
   check only before the six-kill command, is not acceptable. Until this is
   implemented engine-side, route runs use one kill per issue.
6. **Whole-encounter risk selection.** Isolation/risk scoring must count all
   nearby attackable units and likely social adds, not only creatures sharing
   the requested entry. Include level delta, elite/rank, current resources,
   path corridor, caster/ranged threats, pet state, recent deaths, and escape
   path. A target that is isolated only among same-entry creatures is not
   proven safe.
7. **Missing quest behaviors.** Implement the clean-room AzerothCore-native
   minimum needed to remove major quest-coverage holes, starting with game-
   object interaction/collection and use-item-on-unit/use-item-at-location.
   Every behavior needs authoritative completion evidence, timeout,
   blacklist/retry classification, and a live regression scenario.
8. **Navigation-aware route compilation.** Add a deterministic outside route
   compiler/analyzer if useful. It may read profile XML for comparison and
   query `acore_world`, but generated coordinates and paths are validated
   through the existing server navigation. Add a small server-side path probe
   if needed to expose reachability, actual path length/type, endpoint error,
   and vertical-layer failures. Generated route artifacts and the coverage/
   validation report must be reproducible from committed tooling.
9. **Telemetry-driven refinement.** Persist per target/area/class/level-band
   pull outcomes: path failure, adds, outgoing damage/target-health delta,
   time-to-kill, incoming damage, deaths, recovery time, and blacklist reason.
   The route compiler/runtime must be able to penalize or reject demonstrated
   death cells and losing mob/level combinations instead of retrying them
   indefinitely.
10. **Fleet acceptance run.** Every supported Gate 3 route reaches its target
    level with zero manual step advances, GM travel, forced quest/XP repair,
    or runner restart used to make progress. No segment may exhaust its death
    budget; no objective/hunting area may kill the same bot more than twice;
    route completion, quest/grind XP share, deaths, stuck events, and active
    time are captured in the final report.

Group-aware metadata may mark elite/group quests and compatible shared
objectives, but automatic party execution is not a Gate 3 blocker. The Gate 3
solo planner must at least avoid feeding group content to solo bots; party
formation, roles, and shared-credit execution remain a later gate unless the
user promotes them separately.

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

**Follow-up session (2026-07-02): three more literal-bar gaps closed
with live evidence** (see `HANDOFF.md`/`TEST_MATRIX.md` for detail):
pets' last unverified transition (`MissingAlive` -> `Alive`, via the
real Dismiss Pet spell -- ADR-043, closing `KNOWN_FAILURES.md` #17);
dense camps/caves (a real run through the Burning Blade cave, which
also fired the bounded-blacklist path live for the first time, closing
#3's remnant); and ranged pulls as a distinct behavior (ADR-044,
engage-from-range observed directly). Remaining literal-bar deltas:
race/class breadth (representative-sample reinterpretation applied but
not user-reconfirmed for Gate 3), deliberately-engineered full bags,
and the scope of "guide validation."

**Second follow-up session (2026-07-02, later the same day):
deliberately-engineered full bags is now closed too** — a controlled
A/B on the same bot and spot (bags verifiably 100% full via the real
`CanStoreNewItem` path refusing even one more item) completed the
fully-automatic kill+loot cycle with `finished=true, failed=false,
lastLootAttempted=true, lastLootVerified=false`, no hang: exactly
ADR-030's best-effort contract (see `TEST_MATRIX.md`). Remaining
literal-bar deltas are now only the two user judgment calls:
race/class breadth interpretation and the scope of "guide
validation." The same session also added `SelectionDiagnostics`
(ADR-045) and teleport-ack synthesis (ADR-046), and recorded three new
`KNOWN_FAILURES.md` entries (#22 fixed, #23 documented gap, #24 open).

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
