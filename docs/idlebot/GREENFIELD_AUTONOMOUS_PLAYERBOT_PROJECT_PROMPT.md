# Greenfield Autonomous AzerothCore Player Project Prompt

Copy everything below this line into Claude, Codex, ChatGPT, or another coding-agent environment as the initial project prompt.

---

You are the lead engineer for a new, greenfield AzerothCore module that implements autonomous player characters capable of legitimately leveling from character creation to the configured endgame level (Classic level 60 or Wrath level 80).

The result must behave like a real player using normal game mechanics. It must start at level 1, accept the race's first quest, learn how to fight as its class, follow a validated leveling route, improve its character, travel through the world, and continue until the configured maximum level. It must work for every playable race and every class that has a supported combat controller.

This is a new project written from scratch. Do not copy, link, wrap, call into, depend on, or adapt `mod-playerbots`, PlayerbotAI, its strategies/actions, its databases, its travel system, or any Playerbots-derived code. Do not use the existing IdleBot bridge or treat the current IdleBot implementation as a base. The new module may use public AzerothCore server/game APIs and normal third-party libraries compatible with the project, but all bot decision-making, movement control, combat, quest execution, travel, inventory logic, persistence, and recovery must be newly designed and implemented. If examining an older implementation for lessons, extract requirements and failure cases only; do not transplant its code or architecture.

Working project name: `mod-autonomous-player` (rename only if the repository already reserves that name).

## Mission

Build an autonomous character runtime that can complete a clean, reproducible, cheat-free leveling soak from level 1 to 60/80 while acting through the same mechanics available to a normal player.

The bot must be able to:

- Log in or be brought online as a real persisted character.
- Begin at the correct racial starting location and accept the starting quest.
- Support race-, class-, faction-, level-, expansion-, and prerequisite-specific routes.
- Walk, run, swim, jump, navigate terrain, enter buildings and caves, and avoid hazards.
- Learn riding, own and use legitimate mounts, select an appropriate mount, and dismount when required.
- Discover and use flight paths only when known and affordable.
- Board, ride, and exit boats, zeppelins, elevators, and the Deeprun Tram.
- Use legitimate hearthstones, portals, class travel spells, and other player-accessible travel.
- Never replace legal travel with coordinate changes or GM teleportation.
- Fight effectively for each supported class and specialization from low level through endgame.
- Pull safely, control engagement distance, avoid dense packs, retreat, kite where appropriate, manage threat and adds, and recover between pulls.
- Use class resources, buffs, debuffs, interrupts, dispels, crowd control, healing, pets, forms, stances, ranged attacks, consumables, and cooldowns appropriately.
- Handle underwater combat, indoor restrictions, line of sight, path obstruction, evade behavior, immunities, fleeing mobs, elites, escorts, and scripted encounters.
- Loot corpses and game objects and wait for actual server-confirmed inventory or objective progress.
- Accept, track, complete, abandon when valid, and turn in quests.
- Complete kill, item-drop, game-object, use-item, cast-on-target, talk/gossip, exploration, area-trigger, escort, defend, event, emote, delivery, breadcrumb, and multi-stage objectives.
- Select quest rewards using class/spec usefulness and equip upgrades.
- Evaluate and auto-equip legitimately obtained items, including weapons, armor, shields, ranged items, bags, rings, trinkets, and class-specific equipment.
- Preserve needed quest items, reagents, consumables, ammunition, food, drink, and equipment sets.
- Visit vendors, sell junk, repair, restock, buy required quest/vendor items, and manage bag space.
- Visit class trainers and profession/riding trainers when necessary; buy useful skills without wasting money needed for travel or repairs.
- Allocate talents using configurable class/spec builds and adapt rotations when new abilities are learned.
- Eat, drink, bandage, use potions, regenerate, and wait safely between encounters.
- Die, release spirit, navigate as a ghost to the corpse, resurrect safely, recover durability/resources, or use a legitimate spirit healer when appropriate.
- Detect repeated deaths and change approach without cheating or silently skipping required content.
- Grind safely when a route requires a level threshold, but prefer quests when they are efficient and available.
- Persist all state so a server restart resumes safely without duplicating actions, losing guide position, or retaining stale blocked state.
- Produce sufficient structured telemetry to explain every important decision and failure.

## Non-negotiable player-like rules

Normal runtime must never:

- Teleport or directly set position to recover, travel, reach an objective, or bypass pathing.
- Force quest completion, objective counters, rewards, experience, money, reputation, skills, spells, talents, items, or levels through direct database or GM operations.
- Force resurrect outside normal player mechanics.
- Advance a guide step unless its completion predicate is confirmed from authoritative game state.
- Skip a required quest merely because combat, navigation, target selection, or objective resolution is broken.
- Attack invalid targets, use inaccessible spells, equip illegal items, buy unaffordable items, or use undiscovered travel nodes.
- Read hidden information that a normal client could not reasonably act on unless it is static route metadata prepared offline. Server APIs may be used to implement perception, but decisions must remain plausibly player-like.

Admin tools may create/reset test characters before a run. Such setup must be isolated from runtime code, explicitly marked as test-only, and excluded from valid soak evidence.

## Definition of done

The project is not complete when it compiles or when one bot reaches level 10. Completion requires:

1. A clean level-1 test roster covering every supported race/class combination.
2. At least one validated player-like 1-to-60 run on Classic content or 1-to-80 run on Wrath content.
3. Representative successful runs for every supported combat class, with no runtime cheats.
4. Required intercontinental and special transport paths exercised in integration tests.
5. Server restarts during travel, combat recovery, quest execution, and maintenance with correct resumption.
6. No manual database edits, manual step advances, manual quest grants, or teleport recoveries in the accepted run.
7. A machine-readable run manifest proving configuration, build revision, guide revision, character identity, cheat flags, failures, restarts, and final level.
8. Every quarantine/failure includes evidence sufficient to reproduce and fix the root cause.

Treat level 1–12 as the first vertical slice, not the final product.

## Required architecture

Design explicit interfaces and keep game-world mutation on the worldserver thread. Never retain unsafe raw world-object pointers across ticks. Avoid blocking work in the world update loop.

At minimum, separate these responsibilities:

### 1. Character lifecycle

- Character registration, online/offline control, ownership, login/logout, restart restoration, and safe teardown.
- Stable identity based on character GUID, not character name.
- Versioned persisted runtime state.

### 2. Perception and world model

- A tick-safe snapshot of character state, nearby units, game objects, corpses, hazards, combat state, quests, inventory, equipment, money, spells, cooldowns, travel knowledge, and current movement.
- Stable object handles or GUIDs that are resolved afresh before use.
- Clear distinction between observed state, route metadata, and inferred state.

### 3. Hierarchical decision system

Use a hierarchical state machine, behavior tree, utility system, GOAP system, or defensible combination. It must support priority interruption and resumable tasks.

Top-level priorities should include:

1. lifecycle validity
2. death/ghost recovery
3. immediate combat survival
4. combat execution
5. post-combat loot/recovery
6. urgent maintenance
7. current quest objective
8. travel
9. training/equipment/economy
10. route selection and grinding

Every selected action must expose a reason and expected completion signal.

### 4. Action executor

- Issue small, non-blocking actions.
- At most one conflicting movement/interaction/cast decision per bot per decision tick.
- Use acknowledgements and authoritative state changes rather than fixed sleeps.
- Support cancellation, timeout, retry budget, backoff, and idempotency.
- Distinguish action failure from objective failure.

### 5. Movement and navigation

- Build on AzerothCore pathfinding/navmesh and movement APIs, but implement new bot navigation policy.
- Plan long travel as route segments: local path, zone transition, transport, flight, hearth, portal, or class travel.
- Replan when paths fail, targets move, transports are missed, combat interrupts, or the bot dies.
- Implement local unstuck behavior using stop, repath, small reverse, strafe, jump, alternate approach points, and route replanning—never teleportation.
- Track movement progress geometrically rather than assuming a Move command succeeded.
- Model indoor/outdoor, water, fatigue, lava, fall risk, faction safety, mob danger, and level-relative danger.

### 6. Combat framework

Create a class-neutral combat engine plus class/spec controllers.

The common engine must provide:

- target validation and scoring
- objective target preference
- aggro and social-pack estimation
- safe approach and pull points
- ranged pull, melee engage, line-of-sight pull, and pull-back behavior
- add detection and response
- retreat/flee decision and safe escape direction
- resource, health, cooldown, range, facing, movement, cast, and immunity awareness
- post-combat recovery
- encounter history and death attribution

Each class controller must define:

- leveling specialization/build
- spell/ability availability by level
- opener, sustained rotation, execute, AoE threshold, defensive, interrupt, crowd control, healing, escape, and recovery policy
- weapon/range/resource requirements
- pet/form/stance/totem/seal/aura/poison/aspect management where applicable
- equipment priorities

Do not solve combat with a single hard-coded spell list. Use testable conditions and priorities. Support incremental class delivery, but the architecture must not privilege one class.

### 7. Quest and objective engine

Represent quests as authoritative state plus normalized objective types. A guide step is a plan hint, not proof of completion.

Before accepting a quest, validate:

- expansion/content availability
- faction, race, class, profession, and level restrictions
- prerequisite and exclusive-group state
- quest-log capacity
- giver availability and interaction requirements

For each objective, resolve the real acquisition mechanism:

- creature kill or creature loot
- game-object interaction or game-object loot
- vendor purchase
- item use at a specific location or on a specific target
- spell cast, aura, emote, gossip, event, escort, exploration, or area trigger
- delivery of an existing or crafted item

Never assume every required item is a mob drop. Offline validation must inspect creature loot, game-object loot, vendors, quest starter items, spells, scripts, conditions, and objective metadata. Flag scripted or unresolved objectives before deployment.

Completion must be based on live quest/objective status, inventory, or other authoritative state. Handle shared targets, partial progress, respawns, competing bots, unavailable spawns, phase/condition restrictions, and multi-objective ordering.

### 8. Guide and route system

Create a versioned, documented, machine-validated guide format. It must support:

- guide identity and schema version
- expansion, faction, race, class, level range, and prerequisites
- deterministic step IDs that survive insertion/removal
- quest accept/turn-in and all objective types
- travel segments and transport metadata
- coordinates, patrol areas, safe approach points, hotspots, and alternates
- required versus optional content
- level gates and grind fallbacks
- expected objective source and completion predicate
- timeout/retry policy
- known hazards and density limits
- next-guide transitions and conditional branches

Never persist only a numeric step index. Persist stable step ID, guide revision, active task, and enough checkpoint data to migrate safely when a guide changes.

Build offline tools that:

- extract relevant quest, spawn, loot, vendor, item, spell, condition, transport, and prerequisite data from the target AzerothCore database
- normalize imported human-authored guides without trusting them blindly
- validate every quest and objective against the actual database revision
- detect missing prerequisites, wrong NPCs, wrong objective indices, missing sources, vendor items mistaken for drops, impossible coordinates, hostile/high-level route areas, unsupported scripts, and invalid cross-map travel
- render validation reports and fail CI for blocking defects
- compare a deployed guide checksum with the runtime checksum

Generated guides must not overwrite curated fixes silently. Define source-of-truth and override rules explicitly.

### 9. Inventory, equipment, rewards, and economy

Implement a deterministic item evaluator using class, spec, level, armor proficiency, weapon skills, stats, DPS, speed, sockets, effects, durability, uniqueness, and slot constraints.

It must:

- score quest reward choices before turn-in
- compare upgrades against currently equipped items and relevant alternate sets
- equip legal upgrades
- avoid selling quest items, hearthstone, needed gear, reagents, consumables, ammunition, keys, or future hand-ins
- sell configured junk and true downgrades
- buy food, drink, ammunition, reagents, bags, skills, repairs, mounts, and route-required vendor items according to a budget
- reserve money for mandatory travel, repairs, and training
- recover from full bags before continuing loot objectives

Every item decision must be explainable in telemetry.

### 10. Training, talents, mounts, and character growth

- Detect useful available class spells and their prerequisites.
- Route to a suitable trainer using legal travel.
- Choose which ranks/skills to buy under budget pressure.
- Apply versioned talent templates with fallback choices.
- Recompute combat capabilities after learning or equipping changes.
- Learn riding and purchase/use a legal mount at configurable levels when affordable.
- Support class quests that unlock required capabilities when applicable.

### 11. Death and failure recovery

Implement a first-class death state machine:

- detect death
- release spirit through normal mechanics
- locate corpse/graveyard state
- navigate as ghost
- resurrect when legal and safe
- wait out resurrection sickness or use a spirit healer only when policy permits
- repair and recover resources
- reconsider the failed approach before retrying

Persist death history per objective and location. A death loop must trigger analysis and quarantine, not repeated identical attempts.

Use structured failure categories such as:

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

Failures must include bot GUID, guide/step revision, quest/objective, map/position, recent decisions, attempts, progress snapshots, nearby threats, target data, and relevant configuration.

### 12. Persistence and observability

Persist at least:

- character GUID and activation state
- guide ID/revision and stable step ID
- current high-level task and substate
- objective progress snapshot
- travel plan and segment
- retry/death counters and cooldowns
- maintenance intent
- talent/equipment policy revision
- blocked reason and evidence reference

Use database migrations owned by this module. Writes should be bounded, asynchronous where appropriate, and safe across crashes.

Provide:

- structured logs
- live bot status
- event history
- progress and death metrics
- decision traces sampled without flooding logs
- a CLI/admin surface for status, pause, resume, reload guide, validate, and start/stop soak
- no commands that fake normal runtime progress

## Engineering constraints

- Follow AzerothCore module conventions and the repository's supported C++ standard.
- Keep the module independently removable.
- Do not patch AzerothCore core unless a necessary general-purpose extension is proven and documented; prefer public hooks/APIs.
- Any core patch must be minimal, generic, tested, and separately reviewable.
- No blocking database, filesystem, network, or sleep operations in world ticks.
- No unsafe cross-thread access to game objects.
- Bound CPU work per tick and stagger bot updates.
- Design for multiple simultaneous bots without coordinating them through cheats.
- Use deterministic tests where possible and seeded randomness where behavior needs variation.
- Treat warnings, stale states, silent skips, and unimplemented guide actions as defects.

## Testing strategy

Build tests alongside implementation.

### Unit tests

- state-machine transitions and interruption
- target scoring and pack-risk evaluation
- class rotation decisions at representative levels
- quest prerequisite/objective normalization
- item/reward scoring
- budget and maintenance planning
- guide parsing, validation, revision migration, and branching
- transport route planning
- failure classification and retry budgets

### Integration scenarios

- each racial starting quest
- one kill, loot, game-object, vendor-item, use-item, escort, exploration, and scripted objective
- cave/dense camp approached from the perimeter without multi-pull death loops
- ranged and melee classes
- pet class lifecycle
- healer-capable class self-survival
- full bags during a loot quest
- broken gear and insufficient repair money
- trainer trip and new rotation activation
- mount acquisition/use
- flight discovery and flight use
- boat, zeppelin, tram, elevator, hearthstone, and class travel
- death in transit, death in a cave, corpse recovery, and spirit-healer fallback
- server restart during every major state family
- guide revision while a bot is mid-route

### Soak gates

Run staged gates:

1. level 1–6 for every race/class controller
2. level 1–12 for every starting-region route
3. level 1–20 including first major travel and class growth
4. level 1–40 including mounts and multi-zone routing
5. level 1–60
6. level 1–80 when Wrath is enabled

For each gate report elapsed play time, active time, deaths, deaths per level, quest failures, no-progress time, travel failures, manual interventions, restarts, equipment quality, money, and final state. A gate with runtime cheating or manual progression is invalid.

## Multi-agent execution plan

Use background agents in parallel only when workstreams have clear file ownership and contracts. Begin with a shared architecture decision record and interface definitions, then divide work approximately as follows:

- Agent A: lifecycle, scheduler, state machine, persistence, admin commands, and telemetry.
- Agent B: perception, movement, local navigation, long-haul travel, and transports.
- Agent C: combat engine and class-controller framework; add class controllers incrementally with tests.
- Agent D: quest/objective engine, guide schema/loader, DB extraction, and offline validator.
- Agent E: inventory, reward scoring, vendors, training, talents, mounts, and economy.
- Agent F: test harness, reset tooling, integration scenarios, dashboards/reports, and soak automation.

Agents must not independently invent overlapping abstractions. Interface changes require an ADR or lead-agent review. Merge vertical slices frequently; do not let subsystems remain unintegrated until the end.

## Required delivery sequence

### Phase 0: discovery and design

- Inspect the target AzerothCore revision and public APIs.
- Confirm expansion, level cap, maps, database schema, module hooks, build system, and test environment.
- Produce ADRs for lifecycle control, scheduling, decision model, movement, combat API, guide schema, persistence, and player-like policy.
- Produce an explicit non-dependency check proving no Playerbots linkage or copied code.

### Phase 1: walking vertical slice

- One level-1 character logs in, perceives state, walks through navmesh, accepts its first quest, completes it, selects a reward, turns it in, equips an upgrade, and persists state.
- Include death recovery and restart-resume in this slice.

### Phase 2: all starter mechanics

- Complete levels 1–6 for all races with at least one combat controller per class family.
- Add loot, game objects, use-items, gossip, vendors, training, bags, and objective validation.

### Phase 3: levels 1–12 across all race/class combinations

- Deliver real class controllers.
- Solve pack-aware pulling, caves, target selection, corpse runs, and guide validation.
- No manual step skipping in accepted runs.

### Phase 4: regional and intercontinental travel

- Add flights, transports, hearth, portals/class travel, route replanning, mounts, and restart-safe transport state.
- Reach level 20/40 gates.

### Phase 5: complete 1–60/80 route corpus

- Validate guides against the exact database revision.
- Run continuous multi-bot soaks.
- Fix root causes, not individual database state.
- Publish accepted run manifests and reproducible instructions.

## Lessons that must shape the new design

Prior experiments repeatedly failed for predictable reasons. Prevent these classes of defect rather than applying one-off skips:

- Walking directly to the center of a quest area causes body pulls of five to ten mobs. Approach from a safe perimeter and clear inward.
- Correct target IDs are insufficient when nearby high-level or friendly creatures share an area. Validate hostility, level risk, objective relevance, and pull context.
- A radius reduction does not solve dense camps or caves. Pull discipline and approach planning are required.
- Repeating the same corpse path or recovery location creates death loops. Record why the prior attempt failed and change the plan.
- Quest-required items may come from vendors, game objects, spells, scripts, or starter items—not only creature loot.
- Use-item quests require the actual objective location/target, not the quest giver's coordinates.
- Database existence does not prove an NPC or object is reachable, spawned, conditioned correctly, or available to the character.
- Numeric guide indices become invalid when steps are inserted or removed. Persist stable IDs plus revisions.
- Live state can remain blocked after an administrator moves the logical step. Blocking and resume state must be transactional and internally consistent.
- Bag, equipment, money, skill, travel, and quest state are coupled. The planner must reason about them together.
- Silent force-skips create the appearance of progress while invalidating the leveling run. Quarantine with evidence instead.

## Expected outputs from you

Do not stop at a design document. Work iteratively until the project reaches the requested gates.

For each milestone provide:

- implementation and migrations
- architecture/ADRs and guide schema documentation
- unit and integration tests
- build and deployment instructions
- test roster/reset tooling
- validator and generated report artifacts
- exact commands executed and results
- known failures with evidence and next corrective work

Start by inspecting the repository and environment, then create a concrete plan with owners, dependencies, milestones, and acceptance criteria. Make reasonable non-destructive assumptions. Ask only when a missing decision would materially change architecture or safety. Use normal development/build/test/deploy commands without repeatedly requesting confirmation.

The governing objective is simple: create an autonomous AzerothCore character that earns its progression and can reliably play from the first starting quest to level 60/80 without Playerbots and without runtime cheats.
