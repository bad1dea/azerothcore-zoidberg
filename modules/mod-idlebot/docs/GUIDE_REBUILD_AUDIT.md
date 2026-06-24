
## Fixed: Item objective source resolution (2026-06-24)

### What was broken
- Item-only quest objectives (RequiredItemId with no RequiredNpcOrGo) used the quest giver's position as the kill area coordinate
- `if has_objectives: continue` skipped item objectives when kill objectives already existed, so mixed quests only got kill steps
- Quest 747 "The Hunt Begins" exposed this: Plainstrider Feather drops from creature 2955 but the generated step pointed to quest giver (-2913,-258) not the mob spawn (-2984,-317)

### How item sources are now resolved
1. `item_sources.json` extracted from DB with lootid indirection:
   - `creature_template.lootid` → `creature_loot_template.Entry` → items
   - `gameobject_template.data1` → `gameobject_loot_template.Entry` → items
   - 2380 quest items resolved: 1758 from creatures, 797 from gameobjects, 175 from both
2. Generator looks up each RequiredItemId in item_sources.json
3. If creature source found → uses nearest creature spawn coords
4. If GO source found → uses nearest GO spawn coords
5. If same creature as a kill objective → merged (no duplicate step)
6. If same creature set as another item objective → merged
7. If no source found → marked `_unsafe` in YAML + flagged by validator

### What validation prevents this from happening again
- `OBJECTIVE_AT_QUESTGIVER`: fails if objective coord equals accept NPC coord while DB has source spawns elsewhere
- `OBJECTIVE_FAR_FROM_SPAWN`: warns if objective coord is >100yd from nearest source spawn
- `QUEST_NO_OBJECTIVE_STEP`: fails if quest has DB objectives but no generated step
- `UNSAFE_OBJECTIVE`: fails if step is marked unsafe (no known source)

### Remaining unsupported objective types
- 788 quest items with no loot source in creature_loot_template or gameobject_loot_template (may use reference_loot_template, skinning_loot_template, or quest-provided items)
- Items from pickpocketing, fishing, milling, prospecting are not resolved
- Some items may come from quest-specific scripted events not in loot tables
