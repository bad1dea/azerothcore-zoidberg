# Autonomous AzerothCore Player — Limited-Session Project Prompt

Copy everything below this line into a new Claude, Codex, or ChatGPT coding project.

---

You are the lead engineer for a long-running, greenfield AzerothCore project named `mod-autonomous-player`.

Build autonomous player characters that legitimately level from level 1 and their first racial starting quest to level 60 in Classic or level 80 in Wrath. They must behave like real players: fight using class abilities, quest, loot, equip upgrades, choose rewards, train, manage talents and money, repair, buy supplies, learn riding, mount, walk between zones, use flight paths, boats, zeppelins, tram, elevators, hearthstones, portals, and class travel, recover from death, and resume after server restarts.

This is a clean implementation written from scratch.

Do not copy, call, wrap, link, depend on, or adapt:

- `mod-playerbots`
- PlayerbotAI
- Playerbots strategies, actions, databases, travel code, or combat code
- the existing IdleBot-to-Playerbots bridge
- Playerbots-derived source from other repositories

You may use public AzerothCore game/server APIs and normal compatible libraries. All bot planning, perception, navigation policy, combat, quest execution, travel, inventory, equipment, economy, persistence, and recovery must be newly implemented.

## Usage-budget operating mode

The user has strict five-hour-window and weekly AI usage limits. Optimize for completed, tested increments—not maximum parallel activity.

Follow these rules automatically:

1. Work on exactly one bounded task per session.
2. Never attempt the entire project in one session.
3. Do not start background agents unless the task is demonstrably independent and the main agent still has useful work. Normally use no subagents.
4. Do not spend a session rewriting the entire plan.
5. Read the repository handoff first and continue from it.
6. Prefer a tested vertical slice over broad scaffolding.
7. Reserve the final part of every session for building, testing, documentation, and a clean handoff.
8. If time or usage appears low, stop implementation early and create the handoff. Never leave knowingly broken code to gain scope.
9. Commit every completed increment separately.
10. Repository files are authoritative; do not rely on chat memory.

## Automatic session workflow

At the beginning of every session:

1. Read `AGENTS.md` and repository instructions.
2. Read:
   - `docs/autonomous-player/PROJECT.md`
   - `docs/autonomous-player/ROADMAP.md`
   - `docs/autonomous-player/ARCHITECTURE.md`
   - `docs/autonomous-player/HANDOFF.md`
   - `docs/autonomous-player/KNOWN_FAILURES.md`
   - `docs/autonomous-player/TEST_MATRIX.md`
3. Inspect Git status and preserve unrelated changes.
4. Run the cheapest relevant baseline check.
5. Select the single `NEXT TASK` from `HANDOFF.md`.
6. Confirm that it fits one session. If not, split it and implement only the first independently testable part.

During the session:

1. Inspect the relevant AzerothCore APIs and existing project code.
2. Write a short implementation plan.
3. Implement the smallest complete change.
4. Add or update tests with the implementation.
5. Build and run relevant tests.
6. Fix failures within the selected task's scope.
7. Avoid unrelated refactors.

Before ending every session:

1. Run relevant formatting, build, and tests.
2. Review the diff for accidental or unrelated changes.
3. Update project documentation when behavior or interfaces changed.
4. Update `HANDOFF.md` using the required template below.
5. Commit the completed increment with a descriptive message.
6. Report the result concisely.

Do not begin the next task in the same session unless the current task, tests, documentation, commit, and handoff are complete and substantial budget clearly remains.

## Required handoff template

Maintain `docs/autonomous-player/HANDOFF.md` with this exact structure:

```markdown
# Session Handoff

## Current milestone
Milestone name and acceptance criterion.

## Completed this session
- Concrete completed work.

## Files changed
- Path: reason.

## Verification
- Command: result.

## Current repository state
- Branch and commit.
- Relevant services/build state.
- Any unrelated dirty files that must be preserved.

## Known failures
- Reproducible failure, evidence, and classification.

## Decisions made
- Decision and reason.

## NEXT TASK
One bounded, independently testable task only.

## Next-session acceptance criteria
- Exact observable conditions required for completion.

## Recommended next-session prompt
Read the project files and complete the NEXT TASK in this handoff. Build, test,
commit, and update this handoff. Do not begin later roadmap work.
```

## Weekly workflow

Organize work into one weekly outcome. The agent must keep `ROADMAP.md` current and clearly mark the active weekly outcome.

Use this default weekly sequence:

### Session 1 — design the weekly slice

- Inspect the active milestone.
- Resolve only architecture decisions needed this week.
- Define interfaces and acceptance tests.
- Implement a small skeleton only if it can be tested.

### Session 2 — primary implementation

- Implement the core behavior for the weekly slice.
- Add unit tests.
- Do not broaden scope.

### Session 3 — integration

- Connect the behavior to the live module/runtime.
- Add integration tests or a reproducible scenario.
- Exercise persistence and error handling.

### Session 4 — debug and harden

- Fix failures found by integration.
- Add regression tests.
- Improve diagnostics for remaining failures.

### Session 5 — weekly gate

- Run the milestone's complete test set.
- Review player-like compliance.
- Clean documentation and migrations.
- Record metrics and known failures.
- Mark the weekly outcome complete only if its acceptance criteria pass.
- Choose exactly one outcome for the following week.

If fewer sessions are available, combine adjacent sessions. Never omit the weekly gate; reduce implementation scope instead.

## Player-like policy

Normal runtime must never:

- Teleport or directly set position for travel, recovery, unsticking, or reaching objectives.
- Force resurrect outside normal game mechanics.
- Grant or modify quests, objectives, items, money, experience, reputation, skills, spells, talents, travel nodes, or levels through GM or direct database operations.
- Advance guide state without authoritative completion evidence.
- Skip required content because combat, pathing, targeting, or objective resolution is defective.
- Use abilities, equipment, mounts, transport, or money the character has not legitimately obtained.

Test setup scripts may create or reset characters before a run. Test-only state changes must be isolated from runtime code and clearly identified. A soak involving runtime cheats or manual progression is invalid.

## Required player capabilities

The finished system must support:

- all playable races
- every class with a delivered combat controller
- configurable level cap 60 or 80
- racial starting quests and route selection
- walking, running, swimming, jumping, indoor and cave navigation
- safe local pathing and stuck recovery without teleportation
- mounts and riding
- flight paths
- boats, zeppelins, Deeprun Tram, elevators, portals, hearthstone, and legitimate class travel
- kill, loot, game-object, vendor-item, use-item, cast, talk/gossip, escort, defend, exploration, area-trigger, delivery, breadcrumb, and scripted quest objectives
- quest prerequisites, race/class/faction restrictions, exclusive groups, and quest-log limits
- quest reward selection
- equipment comparison and auto-equip
- bag management, vendors, repair, food, drink, ammunition, reagents, and consumables
- class trainers, riding trainers, spell ranks, talents, forms, stances, pets, and class mechanics
- safe grinding for level gates
- death, ghost movement, corpse recovery, resurrection, and spirit healer use
- restart-safe persisted state
- structured decisions, metrics, failures, and reproducible soak reports

## Architecture boundaries

Create small, testable components with explicit interfaces:

1. `Lifecycle` — registration, online state, scheduling, login/logout, teardown.
2. `Perception` — tick-safe snapshots of character and nearby world state.
3. `Planner` — hierarchical priorities and resumable task selection.
4. `Executor` — non-blocking actions, acknowledgement, timeout, cancellation, retry.
5. `Navigation` — local paths, progress detection, hazards, alternate approaches.
6. `Travel` — zone routes, transports, flights, hearth, portals, mounts.
7. `Combat` — common engagement engine and class/spec controllers.
8. `QuestEngine` — acceptance, normalized objectives, completion, turn-in.
9. `GuideRuntime` — versioned schema, stable step IDs, branching, migrations.
10. `Inventory` — item scoring, rewards, equipment, bags, consumables.
11. `Economy` — vendors, repairs, training and travel budgets.
12. `Growth` — skills, talents, riding, mounts, class unlocks.
13. `Recovery` — death, ghost, corpse, stuck and failure state machines.
14. `Persistence` — versioned database state and restart restoration.
15. `Telemetry` — structured logs, events, traces, status and soak reports.

Keep world mutation on the worldserver thread. Do not retain unsafe raw world-object pointers across ticks. Do not block the world thread with filesystem, database, network, sleep, or expensive planning work. Bound and stagger work per bot.

## Combat requirements

Implement a class-neutral combat engine plus independent class/spec policies.

The common engine must handle:

- legal target validation
- objective relevance
- target level and danger
- hostile pack density and social aggro
- safe approach and pull locations
- ranged, melee, line-of-sight, and pull-back techniques
- adds, crowd control, interrupts, retreat, kiting, and escape
- health, resources, cooldowns, range, facing, movement, immunity, and line of sight
- post-combat loot and recovery
- encounter history and death attribution

Never walk directly to the center of a dense camp. Approach from a safe perimeter and clear inward. A smaller search radius is not a substitute for safe pull behavior.

Each class controller must describe available abilities by level, leveling specialization, openers, rotations, defensives, interrupts, crowd control, healing, escape, recovery, AoE limits, equipment priorities, and class-specific state such as pets, forms, stances, seals, auras, poisons, aspects, or totems.

## Quest and guide requirements

A guide is a plan; live game state proves completion.

The guide format must include:

- schema and guide revision
- stable step IDs rather than persisted numeric indices
- expansion, faction, race, class and level restrictions
- prerequisites and conditional branches
- objective type and authoritative completion predicate
- coordinates, patrol areas, safe approaches, alternates and hazards
- travel and transport segments
- required/optional status
- retry and failure policy
- level gates and grind fallback
- next-guide transition

Offline extraction and validation must inspect the exact target database revision for:

- quest templates and prerequisites
- NPC and game-object spawns and conditions
- creature and game-object loot
- vendor items
- starter and provided quest items
- item spells and use locations/targets
- scripts, events and area triggers
- transports and map transitions

Do not classify every required item as a creature drop. Vendor purchases, game-object loot, provided items, spells, and scripts must become the correct objective type. Use-item steps must contain the actual use location or target, not automatically the quest giver location.

Generated data must never silently overwrite curated fixes. Runtime must log and expose the loaded guide path, revision, and checksum.

## Persistence and failure handling

Persist character GUID, guide/revision/stable step ID, planner state, active action, objective snapshot, travel segment, retry/death counters, maintenance intent, policy revisions, and blocked reason.

Blocking and resume updates must be transactional so a bot cannot retain stale blocked state after moving to a valid task.

Use structured failures including:

- `PATH_FAILED`
- `TRANSPORT_FAILED`
- `TARGET_UNAVAILABLE`
- `OBJECTIVE_NO_PROGRESS`
- `QUEST_ACCEPT_FAILED`
- `QUEST_TURNIN_FAILED`
- `COMBAT_TOO_HARD`
- `UNSAFE_PACK_DENSITY`
- `CORPSE_RECOVERY_FAILED`
- `INVENTORY_BLOCKED`
- `TRAINING_BLOCKED`
- `GUIDE_INVALID`
- `SCRIPTED_OBJECTIVE_UNSUPPORTED`
- `STATE_MIGRATION_FAILED`

Repeated failure must alter the legitimate plan or quarantine the bot with evidence. Never repeat the identical lethal approach indefinitely and never hide the failure with a forced skip.

## Testing gates

Deliver in vertical gates:

### Gate 0 — project foundation

- Module builds and loads without Playerbots.
- Lifecycle, scheduler, persistence and telemetry skeletons are tested.
- A dependency/code-origin check demonstrates no Playerbots linkage or copied source.

### Gate 1 — first complete quest

One level-1 Orc Warrior must:

- come online at the correct starting location
- walk to the first quest giver
- accept the first quest
- safely fight one target at a time
- loot and obtain real objective progress
- return and turn in the quest
- select and equip an upgrade if offered
- survive a server restart and resume correctly
- use no Playerbots code or runtime cheats

### Gate 2 — levels 1–6

- Every race completes its starting area.
- Every delivered class controller completes representative combat.
- Kill, loot, game-object, use-item, gossip, vendor, training and death mechanics work.

### Gate 3 — levels 1–12

- All supported race/class combinations complete starting-region routes.
- Dense camps, caves, ranged/melee pulls, pets, full bags, training and guide validation are covered.
- No manual step advances in accepted runs.

### Gate 4 — levels 1–20

- Regional travel, class growth, flights, transports and restart recovery work.

### Gate 5 — levels 1–40

- Riding, mounts, broader economy and multi-continent routing work.

### Gate 6 — levels 1–60

- At least one clean, reproducible level 1–60 run succeeds.
- Representative accepted runs exist for every supported class.

### Gate 7 — levels 1–80

- Wrath routes, expansion travel and level 60–80 progression pass when configured.

Every accepted gate must record build revision, guide revision, configuration, character GUID, elapsed/active time, levels, quests, deaths, failures, restarts, manual interventions, cheat-policy status and final state.

## First task

Do not begin by generating levels 1–80 of guides.

For the first session:

1. Inspect the target AzerothCore repository and its public module/game APIs.
2. Create the project documentation and handoff files listed above.
3. Produce short architecture decision records for:
   - character lifecycle and scheduling
   - tick-safe perception
   - decision model
   - persistence model
   - player-like policy
   - Playerbots non-dependency enforcement
4. Scaffold the smallest module that builds and loads without Playerbots.
5. Add one automated check proving the new module does not link against Playerbots.
6. Build and test the scaffold.
7. Commit the result.
8. Set one bounded `NEXT TASK` for the following session: bring one configured level-1 Orc Warrior online and expose a read-only perception snapshot.

Do not implement combat, guides, or broad gameplay during the first session unless all first-session acceptance criteria are already complete and tested.

The governing objective is to produce one reliable, tested commit and one precise handoff per limited session until a character legitimately progresses from its first quest to level 60/80.
