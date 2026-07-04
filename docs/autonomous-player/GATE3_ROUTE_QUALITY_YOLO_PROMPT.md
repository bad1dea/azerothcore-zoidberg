# Gate 3 Route Quality — Autonomous Execution Prompt

Copy/paste the prompt below into the next Codex session. This is a development
server. The intent is unattended execution: do not stop for routine approvals,
status confirmations, build/deploy permission, or implementation choices that
can be resolved from code, data, tests, and the requirements.

---

## Prompt to run

Work autonomously until the Gate 3 route-quality addendum is implemented and
verified as far as this development environment permits.

Repository:

`/home/khuong/azerothcore-zoidberg`

Branch:

`mod-autonomous-player`

Start by reading these files in full and treat them as authoritative:

1. `AGENTS.md` instructions supplied for this repository/session.
2. `docs/autonomous-player/PROJECT.md`
3. `docs/autonomous-player/ROADMAP.md`, especially **Gate 3 route-quality
   addendum (required, 2026-07-04)**.
4. `docs/autonomous-player/TEST_MATRIX.md`, especially the new blocking Gate 3
   checks.
5. `docs/autonomous-player/SESSION_HANDOFF_2026-07-04.md`
6. `docs/autonomous-player/HANDOFF.md`
7. `docs/autonomous-player/ARCHITECTURE.md`
8. `docs/autonomous-player/KNOWN_FAILURES.md`
9. `docs/autonomous-player/HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`
10. `docs/autonomous-player/ROUTES_REPORT.md`

The referenced research repositories were reviewed at these revisions. Fetch
or refresh them under `/tmp` if needed; do not vendor or copy their source:

- `BosslandGmbH/Honorbuddy-Quest-Behaviors` @
  `4627b8d31d341ef3d896bf62045b34f90f4c06f5`
- `Likon69/CopilotBuddy` @
  `1522059b83a5a2765cec046cac35599098d82806`
- `Likon69/Questing-profiles` @
  `e33dac2b894360d69514b1e033c6b16298db9351`
- `Likon69/CopilotBuddyDocs` @
  `a66e13f668503742d48fc0597378f3ef077a6e68`
- `Likon69/Quest-Behaviors` @
  `35c407225f56fa71e3774b66fba3e6a527189766`
- `Likon69/Singular-wotlk` @
  `ccb8c978c99897d5a289a7334f8a6bdb9a24d41a`
- `Likon69/Navigation-C-` @
  `80cf1bf60148ccff92fd5bfa9eb4e7b354017981`

### Authorization and operating mode

This is an explicit unattended/yolo-mode authorization for normal development
work in this dev environment. Do not ask before:

- inspecting code, logs, processes, containers, databases, and live bot state;
- editing repository files;
- creating migrations or development data needed by this feature;
- building locally or on the approved zoidberg development host;
- running tests, linters, route generators, regression suites, and fleet runs;
- restarting/recreating the development worldserver or its Compose services;
- using SSH, Docker/Compose, SOAP, and read/write SQL scoped to this dev stack;
- resetting/reprovisioning the autonomous test fleet when needed for a clean
  acceptance run;
- making implementation decisions supported by the requirements and evidence;
- committing coherent checkpoints and pushing them to
  `origin/mod-autonomous-player`.

Do not wait for me between phases. Continue through inspect, design,
implementation, build, deploy, live verification, failure diagnosis, fixes,
regression, documentation, commit, and push. If a test fails, diagnose and fix
it, then rerun it. A failed first approach is not a reason to stop.

Preserve unrelated existing changes in the dirty worktree. Before editing,
inventory the current status and determine which modifications belong to this
task. Never reset, discard, overwrite, or commit unrelated user work. The four
Gate 3 documentation changes prepared on 2026-07-04 are in scope:

- `docs/autonomous-player/ROADMAP.md`
- `docs/autonomous-player/TEST_MATRIX.md`
- `docs/autonomous-player/HANDOFF.md`
- `docs/autonomous-player/SESSION_HANDOFF_2026-07-04.md`

Do not spend time on a security audit in this task. Do not print secrets in
logs or chat, but existing dev credentials/configuration may be used through
the established tooling. Security hardening is deferred.

### Required implementation program

Execute the work in dependency order. You may adjust internal design when the
code or live evidence demands it, but do not weaken the acceptance criteria.

#### Phase 0 — baseline and immediate death mitigation

1. Capture current git state, deployed revision, fleet state, route progress,
   per-bot levels/deaths, and the current regression-suite result.
2. Stop or quarantine runners that are actively feeding a death loop while
   code is being changed.
3. Change pure grind execution to one kill per external issue until readiness
   is enforced between every kill inside `GuideRuntime`.
4. Add outgoing-damage and target-health-delta diagnostics sufficient to
   distinguish a hard fight from a combat-inert/stalled session.
5. Verify the immediate mitigation on at least one previously losing grind
   for a melee character and one mana character.

#### Phase 1 — engine-side per-pull readiness and encounter risk

1. Add an explicit between-pull recovery/readiness state. Before selecting the
   next target, evaluate health, mana/resource, pet, consumables, equipment and
   weapon durability, resurrection sickness, existing attackers, and safe rest
   conditions.
2. Do not passively rest in an unsafe camp. Move to a calculated safe edge/hub
   or defend against current attackers first.
3. Extend `EncounterModel` and target selection to evaluate all nearby
   attackable units, not only units sharing the requested creature entry.
4. Include objective relevance, level delta, rank/elite, mixed-entry social
   adds, path corridor, caster/ranged threats, current resources, pet state,
   recent death cells, and escape-path availability in an explainable risk
   score.
5. Add structured failure reasons and expiring target/location blacklists.
6. Add automated and live tests for low-health pull refusal, mixed-entry pack
   rejection, a safe single next to a pack, add override, combat stall, and
   recovery before the next chained kill.

#### Phase 2 — deterministic quest/profile coverage compiler

Create committed tooling that:

1. Parses the six relevant external starter profile families as research
   input: Durotar, Mulgore, Tirisfal, Eversong, Human/Elwynn, and
   Dwarf/Gnome/Dun Morogh.
2. Extracts quest ordering, level checkpoints, objectives, hotspots, vendors,
   trainers, and zone/profile transitions.
3. Validates every candidate against local `acore_world`: existence,
   race/class, min/quest level, prerequisite/exclusive chain, quest giver and
   ender, objective slots, creature/gameobject/item sources, actual spawn
   coordinates, map, and reward/XP metadata.
4. Never trusts external NPC IDs, coordinates, custom-server quest IDs, or
   ordering without local validation.
5. Emits a deterministic coverage report for each route family listing every
   locally eligible quest as included or omitted with a structured reason.
6. Detects stale contradictions such as a route comment saying a chain is
   skipped while the JSON still contains it.
7. Produces machine-readable normalized quest/route-plan input suitable for
   generation rather than directly executing the external XML.
8. Has fixtures/unit tests that run without the live server where practical.

Do not integrate or port `Navigation-C-`. It has unclear mixed provenance and
duplicates the navigation context already owned by AzerothCore.

#### Phase 3 — missing quest behaviors

Implement the clean-room AzerothCore-native minimum required to unlock the
largest validated quest-coverage gaps:

1. Gameobject discovery, approach, interaction, and collection.
2. Use item on unit.
3. Use item at location/gameobject when required by a validated starter quest.
4. Exploration/area-trigger completion if a high-value validated route needs
   it and normal player-like APIs support it.

Each behavior must have:

- authoritative progress/completion evidence;
- interaction/range/line-of-sight and inventory validation;
- timeout/cancel/retry handling;
- per-object/target/location blacklist with reason and expiry;
- death/combat interruption safety;
- idempotent restart/resume semantics;
- at least one real live quest regression test.

Do not fake quest credit, write quest status directly, teleport, or use GM
completion paths.

#### Phase 4 — route-plan model and generator

Replace static serial quest authoring with a generated plan that supports:

1. Pickup batches at a hub.
2. Overlapping quest-objective clusters.
3. Ordered objective sweeps based on actual travel/risk cost.
4. Turn-in batches.
5. Class/race conditionals and class quests.
6. Level/XP/training/vendor/repair/ammo/food checkpoints.
7. Zone transitions and route continuation.
8. Productive re-level alternatives after a death/difficulty gate.
9. Explicit classification of solo-safe, unsafe, elite/group, unsupported,
   and temporarily blocked objectives.

Forecast quest reward XP and expected objective kill XP. Filler grinding is a
bounded calculated deficit only. Target <=20% of active leveling time from
unstructured filler grinding unless the generated report proves no supported
quest alternative exists.

Do not require every eligible quest to be completed. Require every omission
to be visible and justified, and choose enough safe/efficient quest content to
meet the leveling checkpoints.

#### Phase 5 — server-authoritative navigation analysis

1. Add a small bounded path-probe interface around the existing AzerothCore
   `PathGenerator`/mmap/collision stack if the route compiler cannot otherwise
   query it.
2. Return path type, reachability, actual endpoint, endpoint error, path
   length, partial/no-path result, and useful vertical-layer diagnostics.
3. Validate generated route legs and objective hotspots through this probe.
4. Reject or repair unreachable, excessive-detour, wrong-Z/layer, cliff,
   cave-roof, transport, and cross-map assumptions before live execution.
5. Keep world-thread and request-budget safety intact; do not perform blocking
   filesystem/network work in world hooks.

#### Phase 6 — telemetry feedback and adaptive planning

Persist or export per-pull and per-leg results keyed by map/area cell, class,
level band, target entry, and route segment:

- path result and distance;
- selected risk score and contributors;
- outgoing damage and target HP delta;
- incoming damage, attackers, and adds;
- time to engage and time to kill;
- health/mana before and after;
- equipment/weapon durability and effective gear state;
- death, recovery time, and resurrection method;
- timeout/blacklist/failure reason.

Feed demonstrated losing cells, unreachable approaches, and bad mob/level
combinations back into runtime selection and subsequent route generation.
Keep the decisions explainable in logs and generated reports.

#### Phase 7 — regenerate routes and run fleet acceptance

1. Generate and validate the six route families and their class-specific
   variants.
2. Extend the under-covered routes to the appropriate starting-region exit
   level: generally 11–13 where the validated quest plan supports it, rather
   than stopping at 8 solely because behavior coverage is missing.
3. Run clean characters through the supported fleet matrix from their real
   starting state to the route target.
4. Do not use manual step advancement, forced quest/XP repair, GM travel, or a
   runner restart to manufacture progress.
5. No segment may exhaust its death budget. No objective/hunting area may kill
   the same bot more than twice. Any violation must be diagnosed and fixed,
   then the affected acceptance run restarted cleanly.
6. Save final per-bot levels, completed quests, quest/grind XP share, active
   time, deaths, stuck/path failures, blacklists, manual interventions, build
   revision, route revision, and final state.
7. Run the complete automated regression suite before and after changes and
   after the final deploy.

Group/elite quests must be identified so solo bots do not attempt them.
Automatic party formation and shared-credit execution are not Gate 3 blockers
for this run; preserve a clean metadata/interface seam for that later work.

### Build, deploy, test, and source-control expectations

For every coherent risky slice:

1. Run focused local/static tests and formatting checks.
2. Run `check_no_playerbots_dependency.sh` and
   `check_no_forbidden_apis.sh`.
3. Build through the documented zoidberg Docker pipeline when C++ changes.
4. Recreate/restart only the required dev services.
5. Run the focused live scenario, then `live_regression_suite.py`.
6. Inspect real authoritative state, not only command return text.
7. Update `ARCHITECTURE.md`, `KNOWN_FAILURES.md`, `TEST_MATRIX.md`, route
   reports, and the session handoff with calibrated evidence.
8. Commit a coherent checkpoint with a specific message and push it to
   `origin/mod-autonomous-player`.

Continue automatically to the next phase after a successful checkpoint. Do
not leave uncommitted task changes at the end. Do not include unrelated dirty
files in commits.

### Genuine stop conditions

Only stop and ask me if one of these is true after exhausting safe in-scope
alternatives:

1. The target is discovered to be production rather than the documented dev
   stack.
2. A required secret is unavailable through the existing environment/tooling.
3. Progress requires destructive host-wide action, destroying unrelated Docker
   volumes/data, firewall/SSH changes, credential rotation, or another action
   outside the repository/dev-stack authorization above.
4. An irreducible product decision would materially change the requested Gate
   3 outcome and cannot be resolved from `ROADMAP.md`, tests, or live evidence.
5. The same external blocker has persisted across at least three consecutive
   attempts/continuations and no meaningful implementation or diagnostic work
   remains.

Do not stop because the work is long, a build/test failed, the fleet needs a
reset, the implementation requires multiple commits, an approach was
disproven, or the current route data is poor. Those are the work.

### Final unattended handoff

When the run genuinely ends, leave a self-contained report containing:

- commits and pushed revision;
- files/components changed;
- generated coverage and route artifacts;
- builds/deploys/tests run and exact outcomes;
- per-bot fleet acceptance results;
- death and quest-vs-grind improvements versus baseline;
- remaining failed acceptance criteria with evidence;
- any processes/services intentionally left running;
- the single best next action if Gate 3 is not yet complete.

Do not declare Gate 3 complete unless every blocking row introduced by the
2026-07-04 addendum in `TEST_MATRIX.md` is verified with real evidence.

---

## Short invocation

After this file exists, the next user prompt can simply be:

> Read `docs/autonomous-player/GATE3_ROUTE_QUALITY_YOLO_PROMPT.md` in full and
> execute the "Prompt to run" autonomously. You have the unattended dev/build/
> deploy/test/commit/push authorization written there. Do not stop for routine
> approvals; continue until the Gate 3 acceptance work is complete or a listed
> genuine stop condition is reached.
