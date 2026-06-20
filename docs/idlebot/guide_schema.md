# Guide Schema

## Current Runtime Schema

The current compiled runtime model is defined in [modules/mod-idlebot/src/IdleBotGuide.h](/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/src/IdleBotGuide.h:11).

Today it supports:

- guide identity: `id`, `name`, `faction`, `race`, `klass`, `levelMin`, `levelMax`
- steps with:
  - `id`, `name`, `type`
  - optional `questId`, `npcId`, `gameobjectId`, `itemId`, `creatureIds`
  - one `Coordinates` block
  - `completionCondition`
  - `timeoutSeconds`, `retryCount`
  - lightweight adaptive metadata

That model is enough for the current builtin strict guides, but it is too narrow for full guide import and full leveling AI.

## Normalized Target Schema

For converted/generated guides, use a normalized schema that stays close to the current runtime model while adding missing structure needed for future loader work.

Machine-generated guide files may use a JSON-compatible YAML subset so they remain valid YAML without depending on an extra emitter library.

Top-level shape:

```yaml
id: alliance_dwarf_dun_morogh_1_5
name: Alliance Dwarf Dun Morogh 1-5
source:
  kind: zygor_reference
  title: "..."
  file: ZygorLevelingAllianceCATA.lua
faction: alliance
races: [dwarf]
classes: []
level_min: 1
level_max: 5
runtime:
  mode: strict_questing
  use_playerbot_combat: true
  use_playerbot_loot: true
  use_playerbot_quest_actions: true
steps:
  - id: accept_24469
    type: accept_quest
    quest_id: 24469
    npc_id: 37081
    restrictions:
      races: [dwarf]
      classes: []
    source_hint:
      zone: "Coldridge Valley"
      x: 33.6
      y: 53.0
```

## Proposed Top-Level Fields

- `id`
- `name`
- `source`
- `faction`
- `races`
- `classes`
- `level_min`
- `level_max`
- `zones`
- `runtime`
- `steps`

## Proposed Step Fields

- `id`
- `type`
- `name`
- `quest_id`
- `npc_id`
- `gameobject_id`
- `item_id`
- `creature_ids`
- `required_count`
- `objective_index`
- `restrictions`
- `position`
- `source_hint`
- `completion_condition`
- `notes`

## Position Rules

There are two coordinate classes and they must not be conflated:

- `position`: world/map coordinates already validated for AzerothCore runtime use
- `source_hint`: source-guide zone coordinates such as `goto Northshire 33.6,53.0`

The current Zygor source data mostly gives zone-percent coordinates, not reliable world-space coordinates for direct runtime movement. Converted guides should preserve them as hints, not treat them as final world coordinates.

## Step Type Mapping

Use these normalized step types for generated guides:

- `move_to`
- `accept_quest`
- `turn_in_quest`
- `kill_mobs`
- `collect_items`
- `interact_gameobject`
- `talk_to_npc`
- `use_item`
- `discover_flight_path`
- `set_hearthstone`
- `checkpoint`
- `conditional`
- `fallback`

This remains compatible with the current `GuideStep` model once the loader maps names to `StepType`.

## Completion Conditions

Keep the already-working completion format from strict runtime:

```yaml
completion_condition: "quest_objective_complete:24469/1"
```

Rules:

- quest id is required
- objective index is 1-based in the guide text
- loader should normalize it into the current runtime expectation

## Restrictions

Converted guides should normalize guide-level and step-level restrictions into:

```yaml
restrictions:
  races: [human]
  classes: [mage]
  level_min: 1
  level_max: 5
```

This is cleaner than carrying raw Zygor `only` strings through runtime.

## Unknown / Unmapped Source Commands

Do not silently drop source commands that are not yet represented. Instead:

- keep the generated guide scoped to recognized actions
- record unknown commands in `docs/idlebot/zygor_unknown_commands.md`
- preserve enough `source` metadata to revisit the guide later

## Loader Implication

The current loader stub should eventually:

1. parse the normalized generated schema
2. map normalized fields into the existing `Guide` / `GuideStep` runtime structs
3. preserve `source_hint` metadata for logs and future DB validation
