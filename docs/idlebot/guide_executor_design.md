# Guide Executor Design

## Current Actual Executor

There are already two real execution models in the repo:

- organic supervision in `IdleBotManager::TickOrganic()`
- strict step execution in `IdleBotManager::TickBot()` for registered guides

That means the correct next design is not a greenfield executor. It is an extension of the current manager-driven runtime.

## Runtime Roles

Keep the split this way:

- `IdleBotManager`: orchestration, persistence, throttling, high-level state
- `IdleBotGuideLoader`: parse and normalize guide sources into runtime guide objects
- `IdleBotDecisionEngine`: pure policy/scoring layer
- `IdleBotPlayerbotBridge`: low-level gameplay seam into `mod-playerbots`

## State Model

Target executor states:

- `INIT`
- `LOAD_GUIDE`
- `CHECK_SURVIVAL`
- `CHECK_MAINTENANCE`
- `TRAVEL_TO_STEP`
- `ACCEPT_QUESTS`
- `COMPLETE_OBJECTIVES`
- `TURN_IN_QUESTS`
- `GRIND`
- `TRAIN`
- `VENDOR_REPAIR`
- `EQUIP_UPGRADES`
- `HEARTH_OR_TRAVEL`
- `DEATH_RECOVERY`
- `STUCK_RECOVERY`
- `RETRY_STEP`
- `SKIP_STEP`
- `GUIDE_COMPLETE`
- `ERROR_PAUSED`

These should be modeled as manager/executor state, not as new direct player actions.

## Tick Contract

Per tick:

1. resolve bot and control state
2. recover death state first
3. apply maintenance guard
4. read current guide and step
5. check completion first
6. choose one next high-level action
7. send at most one meaningful command through the bridge
8. persist state changes
9. emit one debugable reasoned log line when the decision changes or progress occurs

The existing manager already follows most of this pattern. The main missing piece is explicit executor state instead of implicit branching.

## Decision Modes

The repo already defines broader decision modes in [modules/mod-idlebot/src/IdleBotDecisionEngine.h](/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/src/IdleBotDecisionEngine.h:10):

- `StrictGuide`
- `GuideAssisted`
- `AutonomousLeveling`
- `SandboxIdle`

Today the decision engine is still stubbed and the runtime string mode is effectively:

- `"organic"`
- strict fallback path

Recommended direction:

- keep the current string-config compatibility
- introduce an internal adapter that maps runtime mode to the typed `DecisionMode`
- keep the decision engine pure and side-effect free

## Immediate Executor Gaps

The current strict executor already supports:

- `move_to`
- `accept_quest`
- `turn_in_quest`
- `kill_mobs`
- `interact_gameobject`

The main missing step families for a full leveling loop are:

- `collect_items`
- `talk_to_npc`
- `use_item`
- `vendor`
- `repair`
- `train_class`
- `equip_upgrade`
- `set_hearthstone`
- `use_hearthstone`
- `grind_until_level`
- `stuck_recovery`

## Failure Handling Model

Failure handling should remain manager-owned.

Required checks:

- quest unavailable:
  - already complete
  - already in log
  - prerequisite missing
  - below level
  - faction/race/class mismatch
- objective not progressing:
  - verify quest active
  - verify objective incomplete
  - widen search
  - alternate area if known
  - skip only if optional
- death:
  - let playerbots recover first
  - count and persist
  - only fall back when stalled
- stuck:
  - stop chasing direct movement
  - repath or widen roam
  - escalate after repeated failures

## Safe Integration Plan

### Milestone 1

- finish review and schema docs
- convert one starter guide
- keep generated guides out of runtime

### Milestone 2

- implement `IdleBotGuideLoader`
- load generated guide files into `Guide` / `GuideStep`
- add validation and logging

### Milestone 3

- add executor support for `collect_items` and `use_item`
- keep `mod-playerbots` responsible for combat/loot/travel wherever possible

### Milestone 4

- add explicit decision-state handling for vendor/train/equip/grind/stuck recovery
- connect normalized route/guide data to organic supervision where it helps, without replacing organic playerbot behavior with waypoint scripting

## Safest Next Code Change After This Review

Implement the runtime loader path before adding more quest-specific execution logic.

Reason:

- the bridge already has enough primitives for many leveling actions
- the current bottleneck is guide ingestion and normalization
- adding more one-off executor branches before guide loading exists will increase maintenance cost without moving the data/runtime boundary forward
