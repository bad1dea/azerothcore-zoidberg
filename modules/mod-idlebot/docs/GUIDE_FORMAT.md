# Guide format

Guides are YAML (JSON/SQL import possible later). See
`data/guides/alliance/human/northshire_1_6.yaml` for a worked example.

## Top level
`id`, `name`, `faction`, `race`, `class`, `level_min`, `level_max`, `steps[]`.

## Step types
move_to, accept_quest, turn_in_quest, kill_mobs, loot_items,
interact_gameobject, talk_to_npc, train_class_skills, vendor, repair,
equip_upgrade, set_hearthstone, use_hearthstone, grind_until_level,
discover_flight_path, conditional, checkpoint, fallback.

## Per-step fields
`id`, `name`, `type`, `level_min`/`level_max`, race/class/faction requirements,
`quest_id`, `npc_id`, `gameobject_id`, `item_id`, `creature_ids[]`, `map_id`,
`coordinates {x,y,z,radius,todo}`, `completion_condition`, `timeout_seconds`,
`retry_count`, `adaptive {...}`, `notes`.

## Coordinates
`todo: true` marks a placeholder coordinate. The loader logs a warning for these;
verify against the world DB before trusting them.

## IDs
Quest/NPC/creature/object IDs must match YOUR world DB. Questie is a hint source
only. The loader may cross-check and warn on mismatch.
