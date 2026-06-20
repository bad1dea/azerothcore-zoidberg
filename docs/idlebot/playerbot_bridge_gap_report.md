# Playerbot Bridge Gap Report

This report maps the desired IdleBot bridge surface to what already exists in `modules/mod-idlebot/src/IdleBotPlayerbotBridge.h` and `modules/mod-idlebot/src/IdleBotPlayerbotBridge.cpp`.

| Needed capability | Existing implementation | Location | Missing work | Recommended approach | Risk |
|---|---|---|---|---|---|
| Bot online/control lifecycle | Yes | `EnsureBotOnline`, `ReleaseBot`, `GetLiveStatus` | none for current scope | reuse | low |
| Alive/dead/ghost state | Partial | `IsDead`, `IsGhost`, `GetLiveStatus` | no explicit `IsAlive` helper | keep using current primitives or add thin wrapper only if it removes duplication | low |
| Combat state and context | Yes | `IsInCombat`, `GetCombatContext` | none | reuse | low |
| Short-range movement | Partial | `MoveTo` | still raw `MovePoint`, no TravelMgr routing | keep as short-range tool only; avoid using it as a world travel system | medium |
| Long-haul travel | No general player-like travel method | `TeleportBot` exists but is debug/dev behavior | no reusable `TravelTo` backed by TravelMgr/playerbot planning | add a higher-level travel method only if it can hand work to playerbots instead of bypassing them | high |
| Quest accept | Yes | `AcceptQuest` | none | reuse | low |
| Quest turn-in | Yes | `TurnInQuest` | can still fail when no ender is nearby | reuse, but keep manager-side town/hub steering | low |
| Quest state lookup | Yes | `GetQuestStatus`, `GetCompletedQuests` | no direct `HasQuest` helper | current enum lookup is enough | low |
| Objective progress lookup | Yes | `GetQuestObjectiveProgress` | none | reuse broadly for normalized guide completion | low |
| Kill objective execution | Partial | `FindNearestQuestCreature`, `FindNearestHostile`, `AttackCreature` | no single bridge method that encapsulates target selection and attack | keep target policy in `IdleBotManager` or a future executor, not in the bridge | medium |
| Loot nearby corpses | Yes | `LootNearby` | none; already has detailed debug output | reuse | low |
| Loot/use gameobjects | Partial | `FindNearestGameObjectEntry`, `IsNearGameObject`, `UseGameObject` | no generic quest-object abstraction beyond direct GO use | keep current seam and extend only if a repeated pattern appears | medium |
| NPC interaction/gossip | Partial | `InteractWithNpc`, `AcceptQuest`, `TurnInQuest` | no generalized multi-gossip helper | add only when a real quest flow proves it necessary | medium |
| Use item for guide objective | No explicit helper | none | item-use path missing | add `UseItem` only when guide/runtime actually needs it; prefer playerbot action if available | medium |
| Vendor trash | Yes | `VendorTrash` | proximity still required | reuse with manager-owned travel logic | low |
| Repair | Yes | `Repair` | proximity still required | reuse with manager-owned travel logic | low |
| Class training | Partial | `Train`, `LearnAvailableSpells`, `AutoSpecTalents` | no cohesive trainer trip method at bridge layer | keep trip logic in manager, keep trainer interaction in bridge | low |
| Eat/drink/recover | Partial | `Recover` triggers `food` and `drink` actions | no explicit `EatDrink` method | current `Recover` is enough | low |
| Equip upgrades | No explicit wrapper | playerbot actions exist per source map, bridge does not expose them | bridge lacks `EquipUpgrades`/`AutoGear` call | add a thin bridge method when equipment automation becomes part of the runtime milestone | medium |
| Stuck recovery | No explicit helper | none | bridge lacks unstuck path | add only after inspecting mod-playerbots unstuck actions/strategies in detail | medium |
| Corpse/death recovery | Yes | `RequestReleaseSpirit`, `RequestReviveFromCorpse`, `RequestSpiritHealerRevive`, `DirectResurrect`, `ReviveOrCorpseRun` | none for current scope | reuse | low |
| Inventory/free-slot and durability checks | Yes | `GetInventoryStatus` | none | reuse | low |
| Money/xp reads | Yes | `GetMoney`, `GetXp` | none | reuse | low |

## Immediate Conclusions

- The bridge is not the main blocker for guide-driven questing.
- The largest runtime gap is guide ingestion and executor state coverage, not missing low-level playerbot calls.
- New bridge work should stay narrow:
  - add `UseItem` when a real guide step requires it
  - add `AutoGear` or similar when equipment automation becomes a milestone
  - avoid putting step policy into the bridge

## Safest Bridge Extensions Next

1. `UseItem(bot, itemId)` for item-based quest objectives
2. `EquipUpgrades(bot)` or `AutoGear(bot)` if playerbot behavior is stable in this fork
3. explicit stuck/unstuck helper only after verifying mod-playerbots support and failure modes

Everything else already needed for the current guide normalization pass is present.
