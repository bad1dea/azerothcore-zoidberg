# Current IdleBot Review

## AUDIT.md Summary

`modules/mod-idlebot/AUDIT.md` describes two active runtime paths:

- `organic`: the current primary direction. `mod-idlebot` supervises a visible playerbot while `mod-playerbots` owns most low-level questing, travel, combat, looting, and recovery behavior.
- `strict`: the older explicit guide-step executor built around in-memory `Guide` and `GuideStep` data.

The audit also calls out the main constraints that still apply:

- no blocking work on the worldserver thread
- no long-lived raw world object pointers across ticks
- keep playerbot coupling inside the bridge
- route JSON under `modules/mod-idlebot/data/routes/` is reference data today, not runtime-loaded content

The audit's main open gaps still match the code:

- `IdleBotGuideLoader` is still a stub
- validated route JSON is not connected to runtime
- TravelMgr-backed long-haul travel is not integrated into the bridge
- trainer/spec drift state is session-local
- organic hub steering still uses rough hub centroids

## Existing Runtime Structure

### Manager

`IdleBotManager` is still the runtime brain. The top-level tick path is in [modules/mod-idlebot/src/IdleBotManager.cpp](/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/src/IdleBotManager.cpp:167):

- `Initialize()` loads config, builds the bridge, registers builtin guides, and restores persisted bots.
- `Tick()` iterates active bots and calls `TickBot()`.
- `TickBot()` restores login/control, ensures strategies, runs death handling, emits telemetry deltas, then dispatches by decision mode.

The real split is here:

- `TickOrganic()` in [IdleBotManager.cpp](/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/src/IdleBotManager.cpp:911)
- strict guide execution in [IdleBotManager.cpp](/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/src/IdleBotManager.cpp:270)

### Organic Mode

`TickOrganic()` currently does real work and is not a stub. It:

- reasserts `+new rpg`, `+grind`, `+loot`, `force active`, and `quest first`
- opportunistically turns in completed quests with `TurnInQuest`
- auto-specs talents on level-up
- runs maintenance through `HandleVendorTrip()`
- steers stalled bots to race/level hubs
- logs periodic live status

Relevant functions:

- `TickOrganic()` at `IdleBotManager.cpp:911`
- `HandleVendorTrip()` at `IdleBotManager.cpp:1059`
- `PollDeltas()` at `IdleBotManager.cpp:1204`

This is the most mature runtime path today.

### Strict Guide Executor

The strict executor is active code, but only for builtin guides registered in memory.

At a high level the strict path does:

1. resolve current guide from `_guides`
2. guard maintenance with `MaintenanceGuard()`
3. mark step running and persist state
4. apply grind/loot strategy changes per step
5. do combat-aware interruption handling on non-kill steps
6. execute exactly one step action
7. emit an event and `AdvanceStep()` if complete

Relevant functions:

- step dispatch loop in `IdleBotManager.cpp:270-711`
- `CompletionConditionMet()` in `IdleBotManager.cpp:1264`
- `QuestObjectiveProgress()` in `IdleBotManager.cpp:1237`
- `HandleInteractGameObjectStep()` in `IdleBotManager.cpp:1387`
- `AdvanceStep()` in `IdleBotManager.cpp:1354`

The strict executor currently has real implementations for:

- `MoveTo`
- `AcceptQuest`
- `TurnInQuest`
- `KillMobs`
- `InteractGameobject`

Unhandled `StepType` values still fall through the default skip path at `IdleBotManager.cpp:684`.

## Existing Guide Data / Loading

### In-Memory Guide Model

The current guide model lives in [modules/mod-idlebot/src/IdleBotGuide.h](/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/src/IdleBotGuide.h:11).

Current runtime fields already exist for:

- top-level guide identity and level range
- per-step IDs, names, step type
- optional quest/npc/object/item/creature targets
- one coordinate block
- free-form `completionCondition`
- timeout/retry metadata
- light adaptive metadata

Current `StepType` values are:

- `MoveTo`
- `AcceptQuest`
- `TurnInQuest`
- `KillMobs`
- `LootItems`
- `InteractGameobject`
- `TalkToNpc`
- `TrainClassSkills`
- `Vendor`
- `Repair`
- `EquipUpgrade`
- `SetHearthstone`
- `UseHearthstone`
- `GrindUntilLevel`
- `DiscoverFlightPath`
- `Conditional`
- `Checkpoint`
- `Fallback`

### Loader Status

`IdleBotGuideLoader` is still only a declaration in [modules/mod-idlebot/src/IdleBotGuideLoader.h](/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/src/IdleBotGuideLoader.h:11). The class exposes the intended API:

- `LoadDirectory`
- `Get`
- `ListIds`
- `ValidateFile`

But the private comment at `IdleBotGuideLoader.h:28` is still `TODO(M3): implement with yaml-cpp`.

### Runtime Guide Sources Today

Current runtime guide sources are:

- builtin guides registered in `RegisterBuiltinGuides()` at `IdleBotManager.cpp:1941`
- hand-written YAML prototypes under `modules/mod-idlebot/data/guides/`

What is not runtime-wired:

- generated or validated route JSON under `modules/mod-idlebot/data/routes/`
- repo-root generated YAML that this task adds

## Existing Bridge Methods

The bridge contract is already broad. The public interface is in `modules/mod-idlebot/src/IdleBotPlayerbotBridge.h`, and the active implementation is `IdleBotInternalBridge` in `modules/mod-idlebot/src/IdleBotPlayerbotBridge.cpp:197`.

Methods already present include:

- lifecycle: `EnsureBotOnline`, `ReleaseBot`, `GetBotGuid`, `GetLiveStatus`
- movement/travel: `MoveTo`, `TeleportBot`, `FollowPlayer`
- combat: `AttackCreature`, `CastSpell`, `IsInCombat`, `GetCombatContext`
- quests: `InteractWithNpc`, `AcceptQuest`, `TurnInQuest`, `GetQuestStatus`, `GetCompletedQuests`, `GetQuestObjectiveProgress`
- search helpers: `FindNearestQuestCreature`, `FindNearestHostile`, `FindNearestServiceNpc`, `FindNearestGameObjectEntry`, `IsNearGameObject`
- gameobject/loot: `UseGameObject`, `LootNearby`
- maintenance: `VendorTrash`, `Repair`, `Train`, `LearnAvailableSpells`, `Maintenance`, `AutoSpecTalents`
- survival: `Recover`, `IsGhost`, `RequestReleaseSpirit`, `RequestReviveFromCorpse`, `RequestSpiritHealerRevive`, `DirectResurrect`, `ReviveOrCorpseRun`
- inventory/economy: `GetInventoryStatus`, `GetItemCount`, `GetMoney`, `GetXp`

Important implementation details confirmed in code:

- `MoveTo()` is still raw `MotionMaster::MovePoint` style movement, not TravelMgr-backed long-haul travel.
- `LootNearby()` has custom corpse scanning plus debug detail about loot flags, allowed items, and queued loot packets.
- `GetQuestObjectiveProgress()` is already implemented and is used by the strict executor for reusable quest objective completion checks.

## Existing mod-playerbots APIs Already Reused

`modules/mod-idlebot/docs/PLAYERBOTS_SOURCE_MAP.md` correctly reflects what the current bridge is built on.

Important verified seams:

- `PlayerbotAI::DoSpecificAction(...)` is the main action execution seam.
- strategy toggling uses `PlayerbotAI::ChangeStrategy(...)`
- already registered action names include `loot`, `repair`, `sell`, `trainer`, `maintenance`, `accept quest`, `talk to quest giver`, `release`, `revive from corpse`, and `spirit healer`

This means the current module is already correctly reusing playerbot behavior for:

- death handling
- looting
- trainer/vendoring/repair triggers
- quest accept/turn-in interaction

## What Is Stubbed Or Incomplete

The main incomplete pieces are:

- `IdleBotGuideLoader` is not implemented
- `IdleBotDecisionEngine` is still an explicit stub in [modules/mod-idlebot/src/IdleBotDecisionEngine.h](/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/src/IdleBotDecisionEngine.h:22)
- runtime still does not load the validated JSON route corpus
- many `StepType` values exist in the model but are not implemented in the strict executor
- no general runtime source abstraction exists between:
  - builtin strict guides
  - hand-authored YAML
  - validated route JSON
  - future generated YAML

## What Should Be Reused

Reuse these pieces as-is unless they are proven wrong:

- `IdleBotPlayerbotBridge` as the only playerbot-specific seam
- `TickOrganic()` supervision path
- `HandleDeath()` and playerbot recovery integration
- `HandleVendorTrip()` for player-like maintenance trips
- `GetQuestObjectiveProgress()` and `CompletionConditionMet()`
- `LootNearby()` debug instrumentation
- persisted bot state in `idlebot_bots`
- existing Zygor extraction tools under `modules/mod-idlebot/tools/`

## What Should Be Replaced Or Extended

Extend rather than replace:

- add a real guide source loader instead of keeping `IdleBotGuideLoader` stubbed
- add a route-to-guide normalization layer so validated route data can feed runtime
- expand strict executor support for currently modeled but unimplemented step types
- add a real decision/executor state layer without bypassing the bridge

Do not replace:

- organic supervision with a new fake movement/combat system
- playerbot death/loot/trainer/vendor behavior with direct hacks

## Safest Next Milestone

The safest next implementation milestone is:

1. finish the review and schema normalization
2. keep guide conversion external to runtime for now
3. implement a real loader/normalizer that can read generated guide data into the existing `Guide` / `GuideStep` model
4. wire that loader into runtime without removing builtin guides
5. only then expand strict executor state coverage

Reason:

- the bridge already has enough quest, loot, and maintenance primitives to support a broader guide runtime
- the biggest real gap is not another combat hack, it is the missing runtime ingestion path between converted data and the existing executor
