# Honorbuddy QuestBehavior Import Plan

## Overview

Honorbuddy Quest Behaviors (QBs) are C# mini state machines that drive a WoW bot through
quest objectives. This document maps their semantics to IdleBot C++ implementations.

**We port semantics, not code.** The C# source is reference only — no code is copied.
IdleBot reimplements equivalent behavior using AzerothCore + mod-playerbots APIs.

Real repos surveyed:
- **HB-master**: `BosslandGmbH/Honorbuddy-Quest-Behaviors` — 52 behavior files
- **QB-master**: `Likon69/Quest-Behaviors` — 93 behavior files

---

## Status Key

| Symbol | Meaning |
|--------|---------|
| ✅ | Implemented |
| 🔶 | Partial — framework exists, gaps noted |
| ❌ | Not yet implemented |
| 📋 | Planned with priority A–E |

---

## Behavior Inventory

### 1. Core Framework

| Behavior File | Source | Category | WotLK Relevant | IdleBot Step Type | Status | Guide-Gen Work | Runtime Work | Priority | Notes |
|---|---|---|---|---|---|---|---|---|---|
| QuestBehaviorBase.cs | HB-master | Core | Yes | n/a | 🔶 | None | `IdleBotQuestBehavior.h` created; BehaviorStatus enum defined | — | Full lifecycle (Start/Tick/Stop) not yet wired into manager |
| LocalBlacklist.cs | HB-master | Core | Yes | n/a | ✅ | None | `IdleBotLocalBlacklist.h` created; per-GUID expiry blacklist | — | HB default: 7-min after loot, 90s if player within 25yd (NonCompeteDistance not yet implemented) |
| Types.cs | HB-master | Core | Yes | n/a | 🔶 | None | FailReason constants in `IdleBotQuestBehavior.h` | — | |

### 2. Interaction

| Behavior File | Source | Category | WotLK Relevant | IdleBot Step Type | Status | Guide-Gen Work | Runtime Work | Priority | Notes |
|---|---|---|---|---|---|---|---|---|---|
| InteractWith.cs | Both | Interaction | Yes | `interact_gameobject` / `talk_to_npc` / `gossip_interact` | 🔶 | gossip option metadata | Target select + move; gossip/quest-frame not fully driven | B | Key params: `InteractByGossipOptions` (1-based indices), `InteractByQuestFrameDisposition` (Accept/Complete/Continue/Ignore), `NumOfTimes`=1, `AttemptCountMax`=7, `InteractBlacklistTimeInSeconds`=180, `CollectionDistance`=100yd |
| BasicInteractWith.cs | QB-master | Interaction | Yes | `interact_gameobject` | 🔶 | None | Simple click + loot | — | Merged into InteractGameobject step handler |
| InteractWith2.cs | QB-master | Interaction | Yes | `interact_gameobject` | 🔶 | None | V2 variant; same as above | — | |

### 3. Collection

| Behavior File | Source | Category | WotLK Relevant | IdleBot Step Type | Status | Guide-Gen Work | Runtime Work | Priority | Notes |
|---|---|---|---|---|---|---|---|---|---|
| CollectThings.cs | HB-master | Collection | Yes | `collect_items` | ✅ | ✅ emit `collect_items` for GO sources | ✅ `HandleCollectItemsStep` | A | **Key params**: `CollectItemId`, `CollectItemCount`=1, `HuntingGroundRadius`=120yd, `CollectUntil`=RequiredCountReached/NoTargetsInArea/QuestComplete, `NonCompeteDistance`=25yd (90s blacklist if player nearby — NOT YET implemented), `PostInteractDelay`=1500ms (not implemented). Blacklist 7-min after loot (IdleBot uses 60-tick ≈ 1-min; TODO: extend). q753 Water Pitcher fixed. |
| BasicUseObject.cs | QB-master | Collection | Yes | `collect_items` | ✅ via CollectItems | None | Merged with collect_items | — | Simple GO click + item verify |
| UseGameObject.cs | QB-master | Collection | Yes | `collect_items` / `interact_gameobject` | ✅ via CollectItems | None | Merged | — | |

### 4. Item Usage

| Behavior File | Source | Category | WotLK Relevant | IdleBot Step Type | Status | Guide-Gen Work | Runtime Work | Priority | Notes |
|---|---|---|---|---|---|---|---|---|---|
| UseItem.cs | Both | Item Usage | Yes | `use_item_at_location` | 🔶 | None | `UseItem` bridge call; location only | C | |
| UseItemOn.cs | QB-master | Item Usage | Yes | `use_item_on_npc` | 🔶 | detect SpecialFlags=32 | `HandleUseItemOnNpcStep` works | C | **Params**: `ItemId`, `MobId1..N`, `MobType`=Npc/GameObject, `HasAuraId`, `IsMissingAuraId`, `MobHpPercentLeft`, `IgnoreCombat`. Aura/HP conditions not yet implemented. |
| UseItemTargetLocation.cs | Both | Item Usage | Yes | `use_item_at_location` | 🔶 | coordinate output | UseItem at coords | C | Aura/HP-% conditions not implemented |
| CombatUseItemOn.cs | HB-master | Item Usage | Yes | `use_item_on_npc` + `kill_mobs` | ❌ | None | Combat item use with target logic | D | e.g. use item on mob during combat |
| CombatUseItemOnV2.cs | HB-master | Item Usage | Yes | `use_item_on_npc` | ❌ | None | V2 with HP threshold + aura checks | D | |

### 5. Kill / Combat Objectives

| Behavior File | Source | Category | WotLK Relevant | IdleBot Step Type | Status | Guide-Gen Work | Runtime Work | Priority | Notes |
|---|---|---|---|---|---|---|---|---|---|
| KillUntilComplete.cs | Both | Kill | Yes | `kill_mobs` | 🔶 | None | Kill + loot + partial respawn wait | D | **Params**: `<HuntingGrounds>` waypoint list, `<Blackspots>`, `<PursuitList>`, `WaitForNpcs`=true (wait indefinitely). No explicit timeout. IdleBot `hotspots[]` covers HuntingGrounds partially. No over-grind prevention yet. |

### 6. Escort / Follow

| Behavior File | Source | Category | WotLK Relevant | IdleBot Step Type | Status | Guide-Gen Work | Runtime Work | Priority | Notes |
|---|---|---|---|---|---|---|---|---|---|
| Escort.cs | Both | Escort | Yes | `escort_quest` | 🔶 | detect escort flag | FollowCreature + combat defend | E | Framework wired; not deeply tested |
| EscortGroup.cs | HB-master | Escort | Yes | `escort_quest` | ❌ | None | Multi-NPC escort | E | |
| NPCAssistance.cs | HB-master | Escort | Yes | n/a | ❌ | None | Help friendly NPC in combat | E | Rare in 1-80 |
| FollowNpcUntil.cs | QB-master | Escort | Yes | `escort_quest` | 🔶 | None | Merged with escort_quest | E | |
| TalkToAndListenToStory.cs | QB-master | Gossip | Yes | `gossip_interact` | 🔶 | None | Gossip sequence (multiple options) | B | |

### 7. Travel / Transport

| Behavior File | Source | Category | WotLK Relevant | IdleBot Step Type | Status | Guide-Gen Work | Runtime Work | Priority | Notes |
|---|---|---|---|---|---|---|---|---|---|
| TaxiRide.cs | Both | Travel | Yes | `taxi_ride` | 🔶 | None | `TaxiTo` bridge call exists | B | No multi-hop routing yet |
| UseTransport.cs | Both | Travel | Yes | `use_transport` | 🔶 | transport metadata | `BoardTransport` state machine (TravelToDock → WaitForTransport → Boarding → Riding → Disembarking) | E | States: wait at dock → board → ride → disembark → walk to dest. Completion: distance to GetOffLocation < 2yd. No timeout (assumes transport always comes). Night Elf dock loop needs this. |
| NoCombatMoveTo.cs | QB-master | Travel | Yes | `move_to` | 🔶 | None | move_to step is basic; no combat-avoidance routing | — | |
| RunLikeHell.cs | QB-master | Travel | Rarely | `move_to` | ❌ | None | Sprint + avoid combat | — | Low priority |

### 8. Vendor / Training / Inventory

| Behavior File | Source | Category | WotLK Relevant | IdleBot Step Type | Status | Guide-Gen Work | Runtime Work | Priority | Notes |
|---|---|---|---|---|---|---|---|---|---|
| ForceSetVendor.cs | Both | Vendor | Yes | `vendor` | 🔶 | None | `VendorTrash` + `SellByQuality` | B | |
| ForceTrain.cs | QB-master | Training | Yes | `train_class_skills` | 🔶 | None | `Train` + `Maintenance` | B | |
| ForceTrainRiding.cs | QB-master | Training | Yes | `train_class_skills` | ❌ | None | Riding trainer detection | C | |
| SetHearthstone.cs | Both | Inventory | Yes | `set_hearthstone` | 🔶 | None | `SetHearthstone` action | B | |
| UseHearthstone.cs | QB-master | Inventory | Yes | `use_hearthstone` | 🔶 | None | `UseHearthstone` action | B | |

### 9. Wait / Misc / Control

| Behavior File | Source | Category | WotLK Relevant | IdleBot Step Type | Status | Guide-Gen Work | Runtime Work | Priority | Notes |
|---|---|---|---|---|---|---|---|---|---|
| WaitTimer.cs | QB-master | Control | Yes | n/a | ❌ | None | Explicit sleep with unstick suppression | — | Use `timeout_seconds` on steps instead |
| SafeQuestTurnin.cs | HB-misc | Quest | Yes | `turn_in_quest` | 🔶 | None | Verify quest complete before turn-in | A | Quest rewind logic (`RewindToQuestAcceptStep`) exists |
| TargetAndMoveToMob.cs | HB-misc | Combat | Yes | `kill_mobs` | 🔶 | None | Merged into kill step | — | |

### 10. Vehicle / Special

| Behavior File | Source | Category | WotLK Relevant | IdleBot Step Type | Status | Notes |
|---|---|---|---|---|---|---|
| DeathknightStart/* | QB-master | Vehicle | Yes (DK zone) | n/a | ❌ | DK starting zone events; low priority for 1-80 |
| ArgentTournament/* | QB-master | Vehicle | Yes | n/a | ❌ | Joust vehicle combat; out of scope |

---

## Priority Implementation Plan

### Priority A: CollectItems — DONE ✅

Implements Honorbuddy `CollectThings` / `UseGameObject` / `BasicUseObject` semantics.

**Required for:** quest 753 (Water Pitcher from GO 2907), any GO-sourced item collection.

**What was implemented:**
- `StepType::CollectItems` in `IdleBotGuide.h`
- `HandleCollectItemsStep()` in `IdleBotManager.cpp`
- Item-count bag check before and after interact
- Per-GUID local blacklist (`objectLocalBlacklist`) for GOs that fail to yield item
- Unstick suppression (`posStallTicks = 0`) while waiting for GO respawn
- Source entry iteration (tries all `source_gameobject_entries`, skips blacklisted GUIDs)
- QB_COLLECT structured log prefix
- Guide generator now emits `collect_items` (was `interact_gameobject`) for GO-sourced items

**HB gaps not yet ported:**
- `NonCompeteDistance` (90s blacklist if another player is within 25yd of target)
- `PostInteractDelay` (1500ms wait after interact before checking item count)
- HB's 7-minute loot blacklist (IdleBot uses 60 ticks ≈ 1 min; sufficient for WotLK respawn timers)

### Priority B: InteractWith (Gossip / Quest-Frame)

Full gossip option support and quest-frame disposition handling.

**Key HB parameters to implement:**
- `InteractByGossipOptions` — array of 1-based gossip menu option indices to select in sequence
- `InteractByQuestFrameDisposition` — Accept / Complete / Continue / Ignore / TerminateBehavior
- `InteractByBuyingItemId` + `BuyItemCount` — purchase specific item from vendor via gossip
- `InteractByUsingItemId` — use an inventory item on the target
- `InteractByLooting` — auto-loot after interact
- `NumOfTimes` (default 1) — how many times to interact
- `AttemptCountMax` (default 7) — GUID blacklisted after this many failed attempts
- `InteractBlacklistTimeInSeconds` (default 180s) — duration of per-GUID blacklist
- `CollectionDistance` (default 100yd) — search radius

**Required for:** NPC gossip chains, quest accept/complete frames, vendor interactions.

### Priority C: UseItemOn (Extended)

- Item use on ground location (already partial via `use_item_at_location`)
- Item use on dead NPC corpse  
- Item use with HP% condition (`MobHpPercentLeft`)
- Item use with aura-present / aura-missing conditions (`HasAuraId`, `IsMissingAuraId`)
- Item use during combat vs. out-of-combat

### Priority D: KillUntilComplete (Extended)

- Explicit respawn wait per-spawn-point (currently bots roam blindly)
- HuntingGrounds waypoint patrol (IdleBot `hotspots[]` is partial)
- Item-drop count tracking separate from quest objective counter
- Over-grind prevention (stop killing once objective meets, don't keep going)
- Blackspot avoidance integration

### Priority E: UseTransport / Escort

- **UseTransport**: Night Elf dock boat, Zeppelin, Deeprun Tram. State machine wired but not verified end-to-end.
- **Escort**: Combat defend during escort NPC movement. `FollowCreature` bridge call exists.

---

## Quest-Specific Regression Tests

### Quest 753: A Humble Task (Tauren, Mulgore)
- **Item**: 4755 (Water Pitcher)
- **Wrong GO**: 337 — has zero world spawns, must never appear in generated guide
- **Correct GO**: 2907 — spawned in Mulgore near quest giver
- **Expected step**: `type: collect_items`, `item_id: 4755`, `item_count: 1`, `source_gameobject_entries: [2907]`
- **Completion check**: bag count >= 1 (not just GO click)
- **Validator**: must pass COLLECT_GO_NO_SPAWNS check if 337 appears; must not fire SHOULD_BE_COLLECT_ITEMS

### Quest 747: Plainstrider Menace (Tauren)
- Item from creature drop
- **Expected step**: `type: kill_mobs` with `creature_ids`, `item_id` set
- Coords: near Plainstrider spawn, not quest giver coords

### Quest 218: Kobold Candles (Human, Northshire)
- Kill objective + item collection (independent)
- Both objectives must generate separate steps
- Neither objective step should be skipped because the other exists

### Goldshire Marshal Dughan (Human starting)
- Approach waypoint / blackspot support needed for pathing

### Warrior Valley of Trials issues
- Blackspot support for stuck positions

### Night Elf dock loop
- Requires `use_transport` behavior (Priority E)

---

## Implementation-Verified Parameter Map

### CollectThings → HandleCollectItemsStep

| HB Parameter | IdleBot Field | Implemented |
|---|---|---|
| `MobIdN` / `ObjectIdN` | `source_gameobject_entries[]` | ✅ |
| `CollectItemId` | `item_id` | ✅ |
| `CollectItemCount` | `item_count` | ✅ |
| `CollectUntil=RequiredCountReached` | bag count check | ✅ |
| `CollectUntil=QuestComplete` | `CompletionConditionMet()` | ✅ |
| `HuntingGroundRadius` | `coords.radius` | ✅ (via step coords) |
| Loot blacklist (7 min) | `objectLocalBlacklist[guid] = tick+60` | 🔶 60-tick only |
| `NonCompeteDistance` (90s) | Not implemented | ❌ |
| `PostInteractDelay` (1500ms) | Not implemented | ❌ |

### InteractWith → HandleInteractGameObjectStep (partial)

| HB Parameter | IdleBot Field | Implemented |
|---|---|---|
| `MobIdN` / `ObjectIdN` | `gameobject_id` / `npcId` | ✅ |
| `CollectionDistance` | `coords.radius` | ✅ |
| `NumOfTimes` | (always 1 currently) | 🔶 |
| `AttemptCountMax` | (no per-GUID attempt cap) | ❌ |
| `InteractBlacklistTimeInSeconds` | (no per-GUID timeout) | ❌ |
| `InteractByGossipOptions` | `gossipOption` (single index) | 🔶 single only |
| `InteractByQuestFrameDisposition` | quest accept/turn-in routing | 🔶 |
| `InteractByLooting` | `LootNearby()` always called | ✅ |
| `InteractByBuyingItemId` | Not implemented | ❌ |
| `InteractByUsingItemId` | `UseItemOnTarget()` (separate step) | 🔶 |

---

## Validation Rules Added (validate_generated_guides.py)

| Check | Issue Type | Trigger |
|---|---|---|
| `collect_items` missing `item_id` | `COLLECT_MISSING_ITEM_ID` | Required field |
| `collect_items` missing `item_count` | `COLLECT_MISSING_ITEM_COUNT` | Required field |
| `collect_items` with no source entries | `COLLECT_NO_SOURCES` | No GO or creature sources |
| `collect_items` GO entries have zero world spawns | `COLLECT_GO_NO_SPAWNS` | e.g. GO 337 for q753 |
| `interact_gameobject` used for GO-sourced item without `item_id` | `SHOULD_BE_COLLECT_ITEMS` | Legacy/wrong step type |

---

## Structured Log Prefixes

| Prefix | Behavior | Example |
|---|---|---|
| `QB_COLLECT` | CollectItems | `QB_COLLECT complete bot='Idleshaman' quest=753 item=4755 count=1/1 reason=item_count_reached` |
| `QB_COLLECT` | CollectItems waiting | `QB_COLLECT waiting bot='Idleshaman' quest=753 item=4755 count=0/1 sources=[2907] waitMs=5000` |
| `QB_COLLECT` | CollectItems blacklist | `QB_COLLECT retry bot='Idleshaman' quest=753 item=4755 have=0 need=1 blacklisting go_guid=12345` |
| `QB_OBJECT` | InteractGameobject | `QB_OBJECT waiting bot='Idleshaman' entry=2907 waitMs=5000 reason=no_go_found` |
| `QB_WAIT` | Unstick suppression | `QB_WAIT suppressed_unstick bot='Idleshaman' reason=waiting_for_go_respawn entries=[2907] quest=753` |
| `QB_INTERACT` | InteractWith (future) | `QB_INTERACT gossip selected option=0 npc=1234` |
| `QB_KILL` | KillUntilComplete (future) | `QB_KILL progress quest=747 kills=3/10` |

---

## Files Changed / Created (2026-06-24)

| File | Change |
|---|---|
| `src/IdleBotGuide.h` | Added `CollectItems` StepType; added `sourceGameobjectEntries`, `sourceCreatureEntries`, `itemCount` fields to `GuideStep` |
| `src/IdleBotGuideLoader.cpp` | Parse `collect_items` step type; parse `source_gameobject_entries`, `source_creature_entries`, `item_count` |
| `src/IdleBotManager.h` | Added `HandleCollectItemsStep` declaration; added `objectLocalBlacklist` to `BotRecord` |
| `src/IdleBotManager.cpp` | `HandleCollectItemsStep` implementation; routing in step switch; COLLECT category in event switch; `StepType::CollectItems` in quest-chain filter; improved QB_OBJECT log |
| `src/IdleBotQuestBehavior.h` | NEW: `BehaviorStatus` enum, `FailReason` constants |
| `src/IdleBotLocalBlacklist.h` | NEW: per-GUID tick-expiry blacklist class |
| `tools/guidegen/generate_guides.py` | GO item source → `collect_items` (was `interact_gameobject`); creature item source `kill_mobs` now includes `item_id`/`item_count` metadata |
| `tools/guidegen/validate_generated_guides.py` | `collect_items` recognized as objective step; new checks: COLLECT_MISSING_ITEM_ID, COLLECT_MISSING_ITEM_COUNT, COLLECT_NO_SOURCES, COLLECT_GO_NO_SPAWNS, SHOULD_BE_COLLECT_ITEMS |
| `docs/HONORBUDDY_QB_IMPORT_PLAN.md` | NEW: full behavior inventory, parameter maps, regression tests |

---

*Generated 2026-06-24. Update this file when new behaviors are implemented or parameters verified.*
