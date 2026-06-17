# mod-playerbots source map (local checkout)

Produced before the M4/M5 build per handoff Section 3. All paths are relative to
`modules/mod-playerbots/`. Verified against the local zoidberg fork checkout, not
public docs. Layout differs from the handoff's assumed paths in some places
(actions live under `src/Ai/Base/Actions/`, contexts under `src/Ai/Base/`).

## How bots are driven from idlebot

`PlayerbotAI::DoSpecificAction(name, event, silent, qualifier)`
(`src/Bot/PlayerbotAI.cpp:1767`) iterates **all** engine states
(`BOT_STATE_COMBAT`, `BOT_STATE_NON_COMBAT`, `BOT_STATE_DEAD`) and runs the named
action in whichever engine knows it. Returns `true` on `ACTION_RESULT_OK`, `false`
otherwise (impossible/useless/failed/unknown). This is the single seam idlebot
uses — `IdleBotInternalBridge::DoBotAction(guid, name)` wraps it with
`silent=true`.

Strategy toggling: `PlayerbotAI::ChangeStrategy("+grind", BOT_STATE_NON_COMBAT)`
(`src/Bot/PlayerbotAI.h:405`), and `HasStrategy(name, state)` (`:413`).
`BotState` enum: `BOT_STATE_COMBAT / BOT_STATE_NON_COMBAT / BOT_STATE_DEAD`.

## Registered action names (what DoSpecificAction accepts)

Source: `src/Ai/Base/ActionContext.h`, `ChatActionContext.h`,
`WorldPacketActionContext.h`.

| Capability | Action name | Class | Notes |
|---|---|---|---|
| Combat (grind) | `attack anything` | AttackAnythingAction | picks nearest hostile, quest-need prioritised |
| Loot corpse/GO | `loot` | LootAction | needs `loot` strategy / `loot available` trigger |
| Loot add | `add loot`, `add gathering loot`, `open loot`, `move to loot` | LootAction family | |
| Release spirit | `release` | ReleaseSpiritAction | ChatActionContext |
| Auto release | `auto release` | AutoReleaseSpiritAction | WorldPacket; DeadStrategy uses on `often` |
| Find corpse | `find corpse` | — | DeadStrategy `dead` trigger |
| Revive at corpse | `revive from corpse` | ReviveFromCorpseAction | WorldPacket |
| Graveyard revive | `spirit healer` | SpiritHealerAction | ChatActionContext; teleports to closest grave + rez 0.5 |
| Accept resurrect | `accept resurrect` | AcceptResurrectAction | |
| Repair | `repair` | RepairAllAction | needs repair NPC in range |
| Sell trash | `sell` | SellAction | |
| Maintenance | `maintenance` | (MaintenanceStrategy) | learn/supplement/enchant/repair |
| Trainer | `trainer` | TrainerAction | needs trainer in range |
| Autogear | `autogear`, `autogear bis`, `equip upgrade` | | |
| Equip item | `equip`, `unequip` | EquipAction | |
| Accept quest | `accept quest` | (WorldPacket) accept_quest | |
| Turn in quest | `talk to quest giver` | (WorldPacket) turn_in_quest | also handles gossip |
| Gossip | `gossip hello` | GossipHelloAction | |
| Reward | `reward` | | |

## Death handling — ALREADY DONE by playerbots

`src/Ai/Base/Strategy/DeadStrategy.cpp` wires a full recovery state machine when
the bot is dead:
- `often` / `bg active` → `auto release` (release spirit)
- `dead` → `find corpse`
- `corpse near` → `revive from corpse`
- `resurrect request` → `accept resurrect`
- `falling far` / `location stuck` → `repop`
- `can self resurrect` → `self resurrect`

`SpiritHealerAction::Execute` (`ReviveFromCorpseAction.cpp:296`) moves to the
closest graveyard and `ResurrectPlayer(0.5f)` + `SpawnCorpseBones()` when in range
of a spirit healer; falls back to `TeleportTo(grave)` masterless. `isUseful()`
gates on `PLAYER_FLAGS_GHOST`.

**Consequence for idlebot:** do NOT reimplement recovery. idlebot's job is to
*observe* death (log + count + persist), let playerbots recover, actively *nudge*
the recovery actions if it stalls, and apply a configurable direct-resurrect
fallback + death-loop pause. See Priority 2.

## Gameobject interaction (Q3902)

No single "use game object" action — `"use game object"` is a TRIGGER that fans
out to `talk to quest giver` (quest GOs) and `add loot` (lootable GOs)
(`QuestStrategies.cpp:25`, `WorldPacketHandlerStrategy.cpp:28`). Quest-item GOs
like Scavenged Goods are looted.

Core direct path (verified): `GameObject::Use(Unit* user)`
(`src/server/game/Entities/GameObject/GameObject.h:222`),
`Player::GetGameObjectIfCanInteractWith(guid, type)` (`Player.h:1145`),
`Object::FindNearestGameObject(entry, range, onlySpawned)` (`Object.h:641`).

idlebot approach: find nearest GO by entry, move within interaction range, then
`GameObject::Use(player)` (private-server-direct, logged) and poll quest progress.

## Core AzerothCore APIs verified

- Ghost: `Player::HasPlayerFlag(PLAYER_FLAGS_GHOST)` (`Player.h:1123`,
  flag `0x10` `:463`).
- Resurrect: `ResurrectPlayer(float restore_percent, bool applySickness=false)`
  (`Player.h:2066`), `SpawnCorpseBones(bool)` (`:2058`),
  `RepopAtGraveyard()` (`:2068`), `BuildPlayerRepop()` (`:2067`).
- Inventory: `Player::GetFreeInventorySpace()` (`Player.h:1265`),
  `GetItemCount(item, bankAlso, skip)` (`:1258`).
- Repair: `Player::DurabilityRepairAll(bool cost, float discountMod, bool guildBank)`
  (`Player.h:2076`); `Item::IsBroken()` (`Item.h:257`), item durability via
  `GetUInt32Value(ITEM_FIELD_DURABILITY / ITEM_FIELD_MAXDURABILITY)`.
- World search: `Object::FindNearestCreature(entry, range, alive)` (`Object.h:640`),
  `FindNearestGameObject(entry, range, onlySpawned)` (`:641`).
- Bot detection: `WorldSession::IsBot()` (core), `GET_PLAYERBOT_AI(player)`
  (`Script/Playerbots.h`).
- Master-less login: `sRandomPlayerbotMgr.AddPlayerBot(guid, 0)` (existing bridge).

## Risks / TODO

- `repair` / `trainer` / `maintenance` need the relevant NPC in interaction range;
  idlebot must route the bot to a town hub first (town hubs are guide metadata,
  not yet populated — Priority 8, deferred).
- GO entry/coords for Q3902 must be read from `acore_world` DB (gameobject /
  gameobject_template). Marked TODO(verify) in the guide until confirmed.
- Raw `MovePoint` still used for short movement; long-haul TravelMgr integration
  deferred (handoff §2.9).
