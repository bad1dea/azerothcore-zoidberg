# mod-autonomous-player — Zones, Quests & Routing (deep-dive report)

Generated from the live route JSONs (`tools/routes/*.json`) — this is exactly what the orchestrator executes, in order. Quest titles and creature levels are from `acore_world`. Segments are DEFERRED (not skipped) when a gate (min-level / prereq) isn't met, and re-tried on later passes; grind_to_level segments no-op when the bot is already at/above the target level.

The locally authoritative route-quality audit is committed under
`generated/coverage/`. Its first run found and repaired two comment/JSON
contradictions: q794 is a supported Durotar creature-kill quest, while Elwynn
q62 requires unsupported exploration credit and is removed pending that
behavior. The regenerated 14-variant manifest has zero contradictions.

## Fleet summary

| Route | Class | Char | Target | Map | Segments | Quests | Grinds |
|---|---|---|---|---|---|---|---|
| durotar_orc_warrior_1_12 | Orc Warrior | Grunttwelve | 12 | 1 | 52 | 29 | 7 |
| durotar_troll_hunter_1_12 | Troll Hunter | Trolltwelve | 12 | 1 | 52 | 29 | 7 |
| durotar_orc_warlock_1_12 | Orc Warlock | Locktwelve | 12 | 1 | 52 | 29 | 7 |
| mulgore_tauren_shaman_1_10 | Tauren Shaman | Taurtwelve | 10 | 1 | 26 | 11 | 5 |
| mulgore_tauren_druid_1_10 | Tauren Druid | Druidtwelve | 10 | 1 | 26 | 11 | 5 |
| mulgore_tauren_warrior_1_10 | Tauren Warrior | Tanktwelve | 10 | 1 | 26 | 11 | 5 |
| tirisfal_undead_rogue_1_10 | Undead Rogue | Roguetwelve | 10 | 0 | 21 | 9 | 4 |
| tirisfal_undead_priest_1_10 | Undead Priest | Priestwelve | 10 | 0 | 21 | 9 | 4 |
| eversong_belf_paladin_1_8 | Blood Elf Paladin | Paltwelve | 8 | 530 | 18 | 9 | 3 |
| eversong_belf_hunter_1_8 | Blood Elf Hunter | Hunttwelve | 8 | 530 | 18 | 9 | 3 |
| elwynn_human_warrior_1_8 | Human Warrior | Humantwelve | 8 | 0 | 31 | 22 | 3 |
| elwynn_human_mage_1_8 | Human Mage | Magetwelve | 8 | 0 | 31 | 22 | 3 |
| dunmorogh_dwarf_warrior_1_8 | Dwarf Warrior | Dwarftwelve | 8 | 0 | 21 | 13 | 2 |
| dunmorogh_gnome_mage_1_8 | Gnome Mage | Gnometwelve | 8 | 0 | 21 | 13 | 2 |


## Durotar + Barrens — Orc Warrior  (`durotar_orc_warrior_1_12`)

- **Character:** Grunttwelve  ·  **Map:** 1  ·  **Opportunistic spell:** 78  ·  **Heal spell:** —
- **Author notes:** Durotar 1->12 for an Orc Warrior. All quest/NPC/spawn data mined from acore_world 2026-07-02 (quest_template, creature_queststarter/ender, creature_loot_template, creature spawn clusters). GO-based quests (5441, 6394, 786, 825, 815, 808, 816, 834+its locked follow-up 835, 830/831) are unsupported and skipped; creature-based quest 794 is included. Multi-objective quests list several kill_entries an
- **Segments (52), in execution order:**

- **accept** q4641 "Your Place In The World" (QL1) from Kaltunk [10176] L20-20
- **turn-in** q4641 "Your Place In The World" (QL1) to Gornek [3143] L5-5
- **quest-grind** q788 "Cutting Teeth" (QL2)  (min L2)
    - kill/collect from: Mottled Boar [3098] L1-2
    - giver/turn-in: Gornek [3143] L5-5 / Gornek [3143] L5-5
- **quest-grind** q789 "Sting of the Scorpid" (QL3)  (min L3)
    - kill/collect from: Scorpid Worker [3124] L3-3
    - giver/turn-in: Gornek [3143] L5-5 / Gornek [3143] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Duokna [3158] L10-10
- **quest-grind** q792 "Vile Familiars" (QL4)  (min L4)
    - kill/collect from: Vile Familiar [3101] L3-4
    - giver/turn-in: Zureetha Fargaze [3145] L12-12 / Zureetha Fargaze [3145] L12-12
- **quest-grind** q794 "Burning Blade Medallion" (QL5)  (min L5)
    - kill/collect from: Yarrog Baneshadow [3183] L5-5
    - giver/turn-in: Zureetha Fargaze [3145] L12-12 / Zureetha Fargaze [3145] L12-12
- **quest-grind** q790 "Sarkoth" (QL5)  (min L5)
    - kill/collect from: Sarkoth [3281] L4-4
    - giver/turn-in: Hana'zua [3287] L2-2 / Hana'zua [3287] L2-2
- **accept** q804 "Sarkoth" (QL5) from Hana'zua [3287] L2-2
- **turn-in** q804 "Sarkoth" (QL5) to Gornek [3143] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Duokna [3158] L10-10
- **grind → L5** on Vile Familiar [3101] L3-4 (1 anchor(s))
- **accept** q805 "Report to Sen'jin Village" (QL5) from Zureetha Fargaze [3145] L12-12
- **walk** [walk-vot-to-ukor] 3 hop(s) → [-599.4, -4715.3]
- **accept** q2161 "A Peon's Burden" (QL5) from Ukor [6786] L4-4
- **walk** [walk-ukor-to-senjin] 2 hop(s) → [-825.6, -4920.8]
- **turn-in** q805 "Report to Sen'jin Village" (QL5) to Master Gadrin [3188] L12-12
- **accept** q823 "Report to Orgnil" (QL7) from Master Gadrin [3188] L12-12
- **quest-grind** q818 "A Solvent Spirit" (QL7)  (min L7, optional)
    - kill/collect from: Makrura Clacker [3103] L6-7, Makrura Shellhide [3104] L6-7, Pygmy Surf Crawler [3106] L5-6
    - giver/turn-in: Master Vornal [3304] L11-11 / Master Vornal [3304] L11-11
- **quest-grind** q817 "Practical Prey" (QL8)  (min L8, optional)
    - kill/collect from: Durotar Tiger [3121] L7-8
    - giver/turn-in: Vel'rin Fang [3194] L7-7 / Vel'rin Fang [3194] L7-7
- **grind → L6** on Vile Familiar [3101] L3-4 (3 anchor(s))
- **walk** [walk-senjin-to-razorhill] 5 hop(s) → [287.3, -4724.9]
- **turn-in** q823 "Report to Orgnil" (QL7) to Orgnil Soulscar [3142] L18-18
- **turn-in** q2161 "A Peon's Burden" (QL5) to Innkeeper Grosk [6928] L30-30
- **train** (learn available spells)
- **vendor/sell+repair** at Jark [3164] L14-14
- **grind → L7** on Clattering Scorpid [3125] L5-6 (4 anchor(s))
- **grind → L8** on Clattering Scorpid [3125] L5-6 (4 anchor(s))
- **quest-grind** q784 "Vanquish the Betrayers" (QL7)  (min L8)
    - kill/collect from: Kul Tiras Marine [3129] L6-7, Kul Tiras Sailor [3128] L5-6
    - giver/turn-in: Gar'Thok [3139] L10-10 / Gar'Thok [3139] L10-10
- **quest-grind** q791 "Carry Your Weight" (QL7)  (min L8)
    - kill/collect from: Kul Tiras Marine [3129] L6-7, Kul Tiras Sailor [3128] L5-6
    - giver/turn-in: Furl Scornbrow [3147] L6-6 / Furl Scornbrow [3147] L6-6
- **walk** [walk-razorhill-to-senjin] 4 hop(s) → [-825.6, -4920.8]
- **quest-grind** q818 "A Solvent Spirit" (QL7)  (min L8, optional)
    - kill/collect from: Makrura Clacker [3103] L6-7, Makrura Shellhide [3104] L6-7, Pygmy Surf Crawler [3106] L5-6
    - giver/turn-in: Master Vornal [3304] L11-11 / Master Vornal [3304] L11-11
- **quest-grind** q817 "Practical Prey" (QL8)  (min L8, optional)
    - kill/collect from: Durotar Tiger [3121] L7-8
    - giver/turn-in: Vel'rin Fang [3194] L7-7 / Vel'rin Fang [3194] L7-7
- **walk** [walk-senjin-to-razorhill-2] 4 hop(s) → [287.3, -4724.9]
- **train** (learn available spells)
- **vendor/sell+repair** at Jark [3164] L14-14
- **grind → L9** on Razormane Dustrunner [3113] L8-9 (3 anchor(s))
- **grind → L10** on Surf Crawler [3107] L7-8 (3 anchor(s))
- **quest-grind** q784 "Vanquish the Betrayers" (QL7)  (min L10)
    - kill/collect from: Kul Tiras Marine [3129] L6-7, Kul Tiras Sailor [3128] L5-6
    - giver/turn-in: Gar'Thok [3139] L10-10 / Gar'Thok [3139] L10-10
- **vendor/sell+repair** at Jark [3164] L14-14
- **quest-grind** q837 "Encroachment" (QL10)  (min L11)
    - kill/collect from: Razormane Battleguard [3114] L9-10, Razormane Dustrunner [3113] L8-9, Razormane Quilboar [3111] L6-7, Razormane Scout [3112] L7-8
    - giver/turn-in: Gar'Thok [3139] L10-10 / Gar'Thok [3139] L10-10
- **walk** [walk-durotar-to-crossroads] 5 hop(s) → [-450.0, -2650.0]  (min L10)
- **quest-grind** q844 "Plainstrider Menace" (QL12)  (min L12)
    - kill/collect from: Fleeting Plainstrider [3246] L12-13
    - giver/turn-in: Sergra Darkthorn [3338] L60-60 ELITE / Sergra Darkthorn [3338] L60-60 ELITE
- **quest-grind** q871 "Disrupt the Attacks" (QL12)  (min L12)
    - kill/collect from: Razormane Hunter [3265] L11-12, Razormane Thornweaver [3268] L10-11, Razormane Water Seeker [3267] L10-11
    - giver/turn-in: Thork [3429] L42-42 / Thork [3429] L42-42
- **vendor/sell+repair** at Uthrok [3488] L16-16
- **grind → L12** on Greater Plainstrider [3244] L11-12 (4 anchor(s))
- **quest-grind** q806 "Dark Storms" (QL12)  (min L12)
    - kill/collect from: Fizzle Darkstorm [3203] L12-12
    - giver/turn-in: Orgnil Soulscar [3142] L18-18 / Orgnil Soulscar [3142] L18-18
- **accept** q828 "Margoz" (QL12) from Orgnil Soulscar [3142] L18-18  (needs q806)
- **turn-in** q828 "Margoz" (QL12) to Margoz [3208] L18-18  (needs q806)
- **quest-grind** q827 "Skull Rock" (QL12)  (min L12, needs q828)
    - kill/collect from: Burning Blade Apprentice [3198] L10-11, Burning Blade Fanatic [3197] L9-10
    - giver/turn-in: Margoz [3208] L18-18 / Margoz [3208] L18-18


## Durotar + Barrens — Troll Hunter  (`durotar_troll_hunter_1_12`)

- **Character:** Trolltwelve  ·  **Map:** 1  ·  **Opportunistic spell:** 0  ·  **Heal spell:** —
- **Author notes:** Troll Hunter clone of the Durotar 1->12 route (Trolls share the VoT start). Auto Shot (75) as opportunistic/ranged opener -- exercises the ADR-044 ranged archetype plus hunter auto-tame under full orchestration. Durotar 1->12 for an Orc Warrior. All quest/NPC/spawn data mined from acore_world 2026-07-02. GO-based quests (5441, 6394, 786, 825, 815, 808, 816, 834+its locked follow-up 835, 830/831) a
- **Segments (52), in execution order:**

- **accept** q4641 "Your Place In The World" (QL1) from Kaltunk [10176] L20-20
- **turn-in** q4641 "Your Place In The World" (QL1) to Gornek [3143] L5-5
- **quest-grind** q788 "Cutting Teeth" (QL2)  (min L2)
    - kill/collect from: Mottled Boar [3098] L1-2
    - giver/turn-in: Gornek [3143] L5-5 / Gornek [3143] L5-5
- **quest-grind** q789 "Sting of the Scorpid" (QL3)  (min L3)
    - kill/collect from: Scorpid Worker [3124] L3-3
    - giver/turn-in: Gornek [3143] L5-5 / Gornek [3143] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Duokna [3158] L10-10
- **quest-grind** q792 "Vile Familiars" (QL4)  (min L4)
    - kill/collect from: Vile Familiar [3101] L3-4
    - giver/turn-in: Zureetha Fargaze [3145] L12-12 / Zureetha Fargaze [3145] L12-12
- **quest-grind** q794 "Burning Blade Medallion" (QL5)  (min L5)
    - kill/collect from: Yarrog Baneshadow [3183] L5-5
    - giver/turn-in: Zureetha Fargaze [3145] L12-12 / Zureetha Fargaze [3145] L12-12
- **quest-grind** q790 "Sarkoth" (QL5)  (min L5)
    - kill/collect from: Sarkoth [3281] L4-4
    - giver/turn-in: Hana'zua [3287] L2-2 / Hana'zua [3287] L2-2
- **accept** q804 "Sarkoth" (QL5) from Hana'zua [3287] L2-2
- **turn-in** q804 "Sarkoth" (QL5) to Gornek [3143] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Duokna [3158] L10-10
- **grind → L5** on Vile Familiar [3101] L3-4 (1 anchor(s))
- **accept** q805 "Report to Sen'jin Village" (QL5) from Zureetha Fargaze [3145] L12-12
- **walk** [walk-vot-to-ukor] 3 hop(s) → [-599.4, -4715.3]
- **accept** q2161 "A Peon's Burden" (QL5) from Ukor [6786] L4-4
- **walk** [walk-ukor-to-senjin] 2 hop(s) → [-825.6, -4920.8]
- **turn-in** q805 "Report to Sen'jin Village" (QL5) to Master Gadrin [3188] L12-12
- **accept** q823 "Report to Orgnil" (QL7) from Master Gadrin [3188] L12-12
- **quest-grind** q818 "A Solvent Spirit" (QL7)  (min L7, optional)
    - kill/collect from: Makrura Clacker [3103] L6-7, Makrura Shellhide [3104] L6-7, Pygmy Surf Crawler [3106] L5-6
    - giver/turn-in: Master Vornal [3304] L11-11 / Master Vornal [3304] L11-11
- **quest-grind** q817 "Practical Prey" (QL8)  (min L8, optional)
    - kill/collect from: Durotar Tiger [3121] L7-8
    - giver/turn-in: Vel'rin Fang [3194] L7-7 / Vel'rin Fang [3194] L7-7
- **grind → L6** on Vile Familiar [3101] L3-4 (3 anchor(s))
- **walk** [walk-senjin-to-razorhill] 5 hop(s) → [287.3, -4724.9]
- **turn-in** q823 "Report to Orgnil" (QL7) to Orgnil Soulscar [3142] L18-18
- **turn-in** q2161 "A Peon's Burden" (QL5) to Innkeeper Grosk [6928] L30-30
- **train** (learn available spells)
- **vendor/sell+repair** at Jark [3164] L14-14
- **grind → L7** on Clattering Scorpid [3125] L5-6 (4 anchor(s))
- **grind → L8** on Clattering Scorpid [3125] L5-6 (4 anchor(s))
- **quest-grind** q784 "Vanquish the Betrayers" (QL7)  (min L8)
    - kill/collect from: Kul Tiras Marine [3129] L6-7, Kul Tiras Sailor [3128] L5-6
    - giver/turn-in: Gar'Thok [3139] L10-10 / Gar'Thok [3139] L10-10
- **quest-grind** q791 "Carry Your Weight" (QL7)  (min L8)
    - kill/collect from: Kul Tiras Marine [3129] L6-7, Kul Tiras Sailor [3128] L5-6
    - giver/turn-in: Furl Scornbrow [3147] L6-6 / Furl Scornbrow [3147] L6-6
- **walk** [walk-razorhill-to-senjin] 4 hop(s) → [-825.6, -4920.8]
- **quest-grind** q818 "A Solvent Spirit" (QL7)  (min L8, optional)
    - kill/collect from: Makrura Clacker [3103] L6-7, Makrura Shellhide [3104] L6-7, Pygmy Surf Crawler [3106] L5-6
    - giver/turn-in: Master Vornal [3304] L11-11 / Master Vornal [3304] L11-11
- **quest-grind** q817 "Practical Prey" (QL8)  (min L8, optional)
    - kill/collect from: Durotar Tiger [3121] L7-8
    - giver/turn-in: Vel'rin Fang [3194] L7-7 / Vel'rin Fang [3194] L7-7
- **walk** [walk-senjin-to-razorhill-2] 4 hop(s) → [287.3, -4724.9]
- **train** (learn available spells)
- **vendor/sell+repair** at Jark [3164] L14-14
- **grind → L9** on Razormane Dustrunner [3113] L8-9 (3 anchor(s))
- **grind → L10** on Surf Crawler [3107] L7-8 (3 anchor(s))
- **quest-grind** q784 "Vanquish the Betrayers" (QL7)  (min L10)
    - kill/collect from: Kul Tiras Marine [3129] L6-7, Kul Tiras Sailor [3128] L5-6
    - giver/turn-in: Gar'Thok [3139] L10-10 / Gar'Thok [3139] L10-10
- **vendor/sell+repair** at Jark [3164] L14-14
- **quest-grind** q837 "Encroachment" (QL10)  (min L11)
    - kill/collect from: Razormane Battleguard [3114] L9-10, Razormane Dustrunner [3113] L8-9, Razormane Quilboar [3111] L6-7, Razormane Scout [3112] L7-8
    - giver/turn-in: Gar'Thok [3139] L10-10 / Gar'Thok [3139] L10-10
- **walk** [walk-durotar-to-crossroads] 5 hop(s) → [-450.0, -2650.0]  (min L10)
- **quest-grind** q844 "Plainstrider Menace" (QL12)  (min L12)
    - kill/collect from: Fleeting Plainstrider [3246] L12-13
    - giver/turn-in: Sergra Darkthorn [3338] L60-60 ELITE / Sergra Darkthorn [3338] L60-60 ELITE
- **quest-grind** q871 "Disrupt the Attacks" (QL12)  (min L12)
    - kill/collect from: Razormane Hunter [3265] L11-12, Razormane Thornweaver [3268] L10-11, Razormane Water Seeker [3267] L10-11
    - giver/turn-in: Thork [3429] L42-42 / Thork [3429] L42-42
- **vendor/sell+repair** at Uthrok [3488] L16-16
- **grind → L12** on Greater Plainstrider [3244] L11-12 (4 anchor(s))
- **quest-grind** q806 "Dark Storms" (QL12)  (min L12)
    - kill/collect from: Fizzle Darkstorm [3203] L12-12
    - giver/turn-in: Orgnil Soulscar [3142] L18-18 / Orgnil Soulscar [3142] L18-18
- **accept** q828 "Margoz" (QL12) from Orgnil Soulscar [3142] L18-18  (needs q806)
- **turn-in** q828 "Margoz" (QL12) to Margoz [3208] L18-18  (needs q806)
- **quest-grind** q827 "Skull Rock" (QL12)  (min L12, needs q828)
    - kill/collect from: Burning Blade Apprentice [3198] L10-11, Burning Blade Fanatic [3197] L9-10
    - giver/turn-in: Margoz [3208] L18-18 / Margoz [3208] L18-18


## Durotar + Barrens — Orc Warlock  (`durotar_orc_warlock_1_12`)

- **Character:** Locktwelve  ·  **Map:** 1  ·  **Opportunistic spell:** 686  ·  **Heal spell:** —
- **Author notes:** Orc Warlock clone -- Shadow Bolt cast-time opener + ambient demon maintenance (ADR-047) under orchestration. Durotar 1->12 for an Orc Warrior. All quest/NPC/spawn data mined from acore_world 2026-07-02. GO-based quests (5441, 6394, 786, 825, 815, 808, 816, 834+its locked follow-up 835, 830/831) are unsupported and skipped; creature-based quest 794 is included. Multi-objective quests list several k
- **Segments (52), in execution order:**

- **accept** q4641 "Your Place In The World" (QL1) from Kaltunk [10176] L20-20
- **turn-in** q4641 "Your Place In The World" (QL1) to Gornek [3143] L5-5
- **quest-grind** q788 "Cutting Teeth" (QL2)  (min L2)
    - kill/collect from: Mottled Boar [3098] L1-2
    - giver/turn-in: Gornek [3143] L5-5 / Gornek [3143] L5-5
- **quest-grind** q789 "Sting of the Scorpid" (QL3)  (min L3)
    - kill/collect from: Scorpid Worker [3124] L3-3
    - giver/turn-in: Gornek [3143] L5-5 / Gornek [3143] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Duokna [3158] L10-10
- **quest-grind** q792 "Vile Familiars" (QL4)  (min L4)
    - kill/collect from: Vile Familiar [3101] L3-4
    - giver/turn-in: Zureetha Fargaze [3145] L12-12 / Zureetha Fargaze [3145] L12-12
- **quest-grind** q794 "Burning Blade Medallion" (QL5)  (min L5)
    - kill/collect from: Yarrog Baneshadow [3183] L5-5
    - giver/turn-in: Zureetha Fargaze [3145] L12-12 / Zureetha Fargaze [3145] L12-12
- **quest-grind** q790 "Sarkoth" (QL5)  (min L5)
    - kill/collect from: Sarkoth [3281] L4-4
    - giver/turn-in: Hana'zua [3287] L2-2 / Hana'zua [3287] L2-2
- **accept** q804 "Sarkoth" (QL5) from Hana'zua [3287] L2-2
- **turn-in** q804 "Sarkoth" (QL5) to Gornek [3143] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Duokna [3158] L10-10
- **grind → L5** on Vile Familiar [3101] L3-4 (1 anchor(s))
- **accept** q805 "Report to Sen'jin Village" (QL5) from Zureetha Fargaze [3145] L12-12
- **walk** [walk-vot-to-ukor] 3 hop(s) → [-599.4, -4715.3]
- **accept** q2161 "A Peon's Burden" (QL5) from Ukor [6786] L4-4
- **walk** [walk-ukor-to-senjin] 2 hop(s) → [-825.6, -4920.8]
- **turn-in** q805 "Report to Sen'jin Village" (QL5) to Master Gadrin [3188] L12-12
- **accept** q823 "Report to Orgnil" (QL7) from Master Gadrin [3188] L12-12
- **quest-grind** q818 "A Solvent Spirit" (QL7)  (min L7, optional)
    - kill/collect from: Makrura Clacker [3103] L6-7, Makrura Shellhide [3104] L6-7, Pygmy Surf Crawler [3106] L5-6
    - giver/turn-in: Master Vornal [3304] L11-11 / Master Vornal [3304] L11-11
- **quest-grind** q817 "Practical Prey" (QL8)  (min L8, optional)
    - kill/collect from: Durotar Tiger [3121] L7-8
    - giver/turn-in: Vel'rin Fang [3194] L7-7 / Vel'rin Fang [3194] L7-7
- **grind → L6** on Vile Familiar [3101] L3-4 (3 anchor(s))
- **walk** [walk-senjin-to-razorhill] 5 hop(s) → [287.3, -4724.9]
- **turn-in** q823 "Report to Orgnil" (QL7) to Orgnil Soulscar [3142] L18-18
- **turn-in** q2161 "A Peon's Burden" (QL5) to Innkeeper Grosk [6928] L30-30
- **train** (learn available spells)
- **vendor/sell+repair** at Jark [3164] L14-14
- **grind → L7** on Clattering Scorpid [3125] L5-6 (4 anchor(s))
- **grind → L8** on Clattering Scorpid [3125] L5-6 (4 anchor(s))
- **quest-grind** q784 "Vanquish the Betrayers" (QL7)  (min L8)
    - kill/collect from: Kul Tiras Marine [3129] L6-7, Kul Tiras Sailor [3128] L5-6
    - giver/turn-in: Gar'Thok [3139] L10-10 / Gar'Thok [3139] L10-10
- **quest-grind** q791 "Carry Your Weight" (QL7)  (min L8)
    - kill/collect from: Kul Tiras Marine [3129] L6-7, Kul Tiras Sailor [3128] L5-6
    - giver/turn-in: Furl Scornbrow [3147] L6-6 / Furl Scornbrow [3147] L6-6
- **walk** [walk-razorhill-to-senjin] 4 hop(s) → [-825.6, -4920.8]
- **quest-grind** q818 "A Solvent Spirit" (QL7)  (min L8, optional)
    - kill/collect from: Makrura Clacker [3103] L6-7, Makrura Shellhide [3104] L6-7, Pygmy Surf Crawler [3106] L5-6
    - giver/turn-in: Master Vornal [3304] L11-11 / Master Vornal [3304] L11-11
- **quest-grind** q817 "Practical Prey" (QL8)  (min L8, optional)
    - kill/collect from: Durotar Tiger [3121] L7-8
    - giver/turn-in: Vel'rin Fang [3194] L7-7 / Vel'rin Fang [3194] L7-7
- **walk** [walk-senjin-to-razorhill-2] 4 hop(s) → [287.3, -4724.9]
- **train** (learn available spells)
- **vendor/sell+repair** at Jark [3164] L14-14
- **grind → L9** on Razormane Dustrunner [3113] L8-9 (3 anchor(s))
- **grind → L10** on Surf Crawler [3107] L7-8 (3 anchor(s))
- **quest-grind** q784 "Vanquish the Betrayers" (QL7)  (min L10)
    - kill/collect from: Kul Tiras Marine [3129] L6-7, Kul Tiras Sailor [3128] L5-6
    - giver/turn-in: Gar'Thok [3139] L10-10 / Gar'Thok [3139] L10-10
- **vendor/sell+repair** at Jark [3164] L14-14
- **quest-grind** q837 "Encroachment" (QL10)  (min L11)
    - kill/collect from: Razormane Battleguard [3114] L9-10, Razormane Dustrunner [3113] L8-9, Razormane Quilboar [3111] L6-7, Razormane Scout [3112] L7-8
    - giver/turn-in: Gar'Thok [3139] L10-10 / Gar'Thok [3139] L10-10
- **walk** [walk-durotar-to-crossroads] 5 hop(s) → [-450.0, -2650.0]  (min L10)
- **quest-grind** q844 "Plainstrider Menace" (QL12)  (min L12)
    - kill/collect from: Fleeting Plainstrider [3246] L12-13
    - giver/turn-in: Sergra Darkthorn [3338] L60-60 ELITE / Sergra Darkthorn [3338] L60-60 ELITE
- **quest-grind** q871 "Disrupt the Attacks" (QL12)  (min L12)
    - kill/collect from: Razormane Hunter [3265] L11-12, Razormane Thornweaver [3268] L10-11, Razormane Water Seeker [3267] L10-11
    - giver/turn-in: Thork [3429] L42-42 / Thork [3429] L42-42
- **vendor/sell+repair** at Uthrok [3488] L16-16
- **grind → L12** on Greater Plainstrider [3244] L11-12 (4 anchor(s))
- **quest-grind** q806 "Dark Storms" (QL12)  (min L12)
    - kill/collect from: Fizzle Darkstorm [3203] L12-12
    - giver/turn-in: Orgnil Soulscar [3142] L18-18 / Orgnil Soulscar [3142] L18-18
- **accept** q828 "Margoz" (QL12) from Orgnil Soulscar [3142] L18-18  (needs q806)
- **turn-in** q828 "Margoz" (QL12) to Margoz [3208] L18-18  (needs q806)
- **quest-grind** q827 "Skull Rock" (QL12)  (min L12, needs q828)
    - kill/collect from: Burning Blade Apprentice [3198] L10-11, Burning Blade Fanatic [3197] L9-10
    - giver/turn-in: Margoz [3208] L18-18 / Margoz [3208] L18-18


## Mulgore — Tauren Shaman  (`mulgore_tauren_shaman_1_10`)

- **Character:** Taurtwelve  ·  **Map:** 1  ·  **Opportunistic spell:** 403  ·  **Heal spell:** 331
- **Author notes:** Mulgore 1->10 for a Tauren Shaman -- the second authored zone, built with every lesson the Durotar 1->12 run taught: all objective slots mined (quest_template RequiredNpcOrGo1-4 + item slots), real spawn rows (never bucket centroids), quests at green parity / grinds a level band below, safe unstick hubs (APCampNarache/MGBloodhoof/MGRaintotem, game_tele 100004/100015/100016), repair NPCs authored s
- **Segments (26), in execution order:**

- **accept** q752 "A Humble Task" (QL2) from Chief Hawkwind [2981] L36-36
- **turn-in** q752 "A Humble Task" (QL2) to Greatmother Hawkwind [2991] L9-9
- **quest-grind** q747 "The Hunt Begins" (QL2)  (min L2)
    - kill/collect from: Plainstrider [2955] L1-2
    - giver/turn-in: Grull Hawkwind [2980] L4-4 / Grull Hawkwind [2980] L4-4
- **train** (learn available spells)
- **vendor/sell+repair** at Bronk Steelrage [3075] L10-10
- **quest-grind** q750 "The Hunt Continues" (QL3)  (min L3)
    - kill/collect from: Mountain Cougar [2961] L3-3
    - giver/turn-in: Grull Hawkwind [2980] L4-4 / Grull Hawkwind [2980] L4-4
- **grind → L5** on Plainstrider [2955] L1-2 (3 anchor(s))
- **quest-grind** q780 "The Battleboars" (QL4)  (min L5)
    - kill/collect from: Battleboar [2966] L3-4, Bristleback Battleboar [2954] L4-5
    - giver/turn-in: Grull Hawkwind [2980] L4-4 / Grull Hawkwind [2980] L4-4
- **train** (learn available spells)
- **vendor/sell+repair** at Bronk Steelrage [3075] L10-10
- **walk** [walk-narache-to-bloodhoof] 4 hop(s) → [-2340.0, -400.0]
- **quest-grind** q748 "Poison Water" (QL5)  (min L5)
    - kill/collect from: Adult Plainstrider [2956] L6-7, Prairie Wolf [2958] L5-6
    - giver/turn-in: Mull Thunderhorn [2948] L25-25 / Mull Thunderhorn [2948] L25-25
- **train** (learn available spells)
- **vendor/sell+repair** at Moorat Longstride [3076] L12-12
- **grind → L7** on Prairie Wolf [2958] L5-6 (3 anchor(s))
- **quest-grind** q761 "Swoop Hunting" (QL6)  (min L8, optional)
    - kill/collect from: Swoop [2970] L7-9, Wiry Swoop [2969] L5-7
    - giver/turn-in: Harken Windtotem [2947] L21-21 / Harken Windtotem [2947] L21-21
- **quest-grind** q745 "Sharing the Land" (QL6)  (min L8)
    - kill/collect from: Palemane Poacher [2951] L7-8, Palemane Skinner [2950] L6-7, Palemane Tanner [2949] L5-6
    - giver/turn-in: Baine Bloodhoof [2993] L10-10 / Baine Bloodhoof [2993] L10-10
- **grind → L8** on Adult Plainstrider [2956] L6-7 (3 anchor(s))
- **quest-grind** q766 "Mazzranache" (QL8)  (min L8)
    - kill/collect from: Adult Plainstrider [2956] L6-7, Flatland Cougar [3035] L7-8, Prairie Wolf [2958] L5-6, Wiry Swoop [2969] L5-7
    - giver/turn-in: Maur Raincaller [3055] L10-10 / Maur Raincaller [3055] L10-10
- **quest-grind** q743 "Dangers of the Windfury" (QL8)  (min L9)
    - kill/collect from: Windfury Harpy [2962] L7-8
    - giver/turn-in: Ruul Eagletalon [2985] L9-9 / Ruul Eagletalon [2985] L9-9
- **train** (learn available spells)
- **vendor/sell+repair** at Moorat Longstride [3076] L12-12
- **grind → L9** on Prairie Stalker [2959] L7-8 (3 anchor(s))
- **quest-grind** q833 "A Sacred Burial" (QL10)  (min L10)
    - kill/collect from: Bristleback Interloper [3232] L9-10
    - giver/turn-in: Lorekeeper Raintotem [3233] L8-8 / Lorekeeper Raintotem [3233] L8-8
- **grind → L10** on Prairie Stalker [2959] L7-8 (3 anchor(s))
- **vendor/sell+repair** at Moorat Longstride [3076] L12-12


## Mulgore — Tauren Druid  (`mulgore_tauren_druid_1_10`)

- **Character:** Druidtwelve  ·  **Map:** 1  ·  **Opportunistic spell:** 5176  ·  **Heal spell:** 5185
- **Author notes:** Tauren Druid clone -- Wrath cast-time opener. Mulgore 1->10 for a Tauren Shaman -- the second authored zone, built with every lesson the Durotar 1->12 run taught: all objective slots mined (quest_template RequiredNpcOrGo1-4 + item slots), real spawn rows (never bucket centroids), quests at green parity / grinds a level band below, safe unstick hubs (APCampNarache/MGBloodhoof/MGRaintotem, game_tele
- **Segments (26), in execution order:**

- **accept** q752 "A Humble Task" (QL2) from Chief Hawkwind [2981] L36-36
- **turn-in** q752 "A Humble Task" (QL2) to Greatmother Hawkwind [2991] L9-9
- **quest-grind** q747 "The Hunt Begins" (QL2)  (min L2)
    - kill/collect from: Plainstrider [2955] L1-2
    - giver/turn-in: Grull Hawkwind [2980] L4-4 / Grull Hawkwind [2980] L4-4
- **train** (learn available spells)
- **vendor/sell+repair** at Bronk Steelrage [3075] L10-10
- **quest-grind** q750 "The Hunt Continues" (QL3)  (min L3)
    - kill/collect from: Mountain Cougar [2961] L3-3
    - giver/turn-in: Grull Hawkwind [2980] L4-4 / Grull Hawkwind [2980] L4-4
- **grind → L5** on Plainstrider [2955] L1-2 (3 anchor(s))
- **quest-grind** q780 "The Battleboars" (QL4)  (min L5)
    - kill/collect from: Battleboar [2966] L3-4, Bristleback Battleboar [2954] L4-5
    - giver/turn-in: Grull Hawkwind [2980] L4-4 / Grull Hawkwind [2980] L4-4
- **train** (learn available spells)
- **vendor/sell+repair** at Bronk Steelrage [3075] L10-10
- **walk** [walk-narache-to-bloodhoof] 4 hop(s) → [-2340.0, -400.0]
- **quest-grind** q748 "Poison Water" (QL5)  (min L5)
    - kill/collect from: Adult Plainstrider [2956] L6-7, Prairie Wolf [2958] L5-6
    - giver/turn-in: Mull Thunderhorn [2948] L25-25 / Mull Thunderhorn [2948] L25-25
- **train** (learn available spells)
- **vendor/sell+repair** at Moorat Longstride [3076] L12-12
- **grind → L7** on Prairie Wolf [2958] L5-6 (3 anchor(s))
- **quest-grind** q761 "Swoop Hunting" (QL6)  (min L8, optional)
    - kill/collect from: Swoop [2970] L7-9, Wiry Swoop [2969] L5-7
    - giver/turn-in: Harken Windtotem [2947] L21-21 / Harken Windtotem [2947] L21-21
- **quest-grind** q745 "Sharing the Land" (QL6)  (min L8)
    - kill/collect from: Palemane Poacher [2951] L7-8, Palemane Skinner [2950] L6-7, Palemane Tanner [2949] L5-6
    - giver/turn-in: Baine Bloodhoof [2993] L10-10 / Baine Bloodhoof [2993] L10-10
- **grind → L8** on Adult Plainstrider [2956] L6-7 (3 anchor(s))
- **quest-grind** q766 "Mazzranache" (QL8)  (min L8)
    - kill/collect from: Adult Plainstrider [2956] L6-7, Flatland Cougar [3035] L7-8, Prairie Wolf [2958] L5-6, Wiry Swoop [2969] L5-7
    - giver/turn-in: Maur Raincaller [3055] L10-10 / Maur Raincaller [3055] L10-10
- **quest-grind** q743 "Dangers of the Windfury" (QL8)  (min L9)
    - kill/collect from: Windfury Harpy [2962] L7-8
    - giver/turn-in: Ruul Eagletalon [2985] L9-9 / Ruul Eagletalon [2985] L9-9
- **train** (learn available spells)
- **vendor/sell+repair** at Moorat Longstride [3076] L12-12
- **grind → L9** on Prairie Stalker [2959] L7-8 (3 anchor(s))
- **quest-grind** q833 "A Sacred Burial" (QL10)  (min L10)
    - kill/collect from: Bristleback Interloper [3232] L9-10
    - giver/turn-in: Lorekeeper Raintotem [3233] L8-8 / Lorekeeper Raintotem [3233] L8-8
- **grind → L10** on Prairie Stalker [2959] L7-8 (3 anchor(s))
- **vendor/sell+repair** at Moorat Longstride [3076] L12-12


## Mulgore — Tauren Warrior  (`mulgore_tauren_warrior_1_10`)

- **Character:** Tanktwelve  ·  **Map:** 1  ·  **Opportunistic spell:** 78  ·  **Heal spell:** —
- **Author notes:** Tauren Warrior clone -- the Mulgore MELEE cell of the per-zone melee+ranged matrix. Mulgore 1->10 for a Tauren Shaman -- the second authored zone, built with every lesson the Durotar 1->12 run taught: all objective slots mined (quest_template RequiredNpcOrGo1-4 + item slots), real spawn rows (never bucket centroids), quests at green parity / grinds a level band below, safe unstick hubs (APCampNara
- **Segments (26), in execution order:**

- **accept** q752 "A Humble Task" (QL2) from Chief Hawkwind [2981] L36-36
- **turn-in** q752 "A Humble Task" (QL2) to Greatmother Hawkwind [2991] L9-9
- **quest-grind** q747 "The Hunt Begins" (QL2)  (min L2)
    - kill/collect from: Plainstrider [2955] L1-2
    - giver/turn-in: Grull Hawkwind [2980] L4-4 / Grull Hawkwind [2980] L4-4
- **train** (learn available spells)
- **vendor/sell+repair** at Bronk Steelrage [3075] L10-10
- **quest-grind** q750 "The Hunt Continues" (QL3)  (min L3)
    - kill/collect from: Mountain Cougar [2961] L3-3
    - giver/turn-in: Grull Hawkwind [2980] L4-4 / Grull Hawkwind [2980] L4-4
- **grind → L5** on Plainstrider [2955] L1-2 (3 anchor(s))
- **quest-grind** q780 "The Battleboars" (QL4)  (min L5)
    - kill/collect from: Battleboar [2966] L3-4, Bristleback Battleboar [2954] L4-5
    - giver/turn-in: Grull Hawkwind [2980] L4-4 / Grull Hawkwind [2980] L4-4
- **train** (learn available spells)
- **vendor/sell+repair** at Bronk Steelrage [3075] L10-10
- **walk** [walk-narache-to-bloodhoof] 4 hop(s) → [-2340.0, -400.0]
- **quest-grind** q748 "Poison Water" (QL5)  (min L5)
    - kill/collect from: Adult Plainstrider [2956] L6-7, Prairie Wolf [2958] L5-6
    - giver/turn-in: Mull Thunderhorn [2948] L25-25 / Mull Thunderhorn [2948] L25-25
- **train** (learn available spells)
- **vendor/sell+repair** at Moorat Longstride [3076] L12-12
- **grind → L7** on Prairie Wolf [2958] L5-6 (3 anchor(s))
- **quest-grind** q761 "Swoop Hunting" (QL6)  (min L8, optional)
    - kill/collect from: Swoop [2970] L7-9, Wiry Swoop [2969] L5-7
    - giver/turn-in: Harken Windtotem [2947] L21-21 / Harken Windtotem [2947] L21-21
- **quest-grind** q745 "Sharing the Land" (QL6)  (min L8)
    - kill/collect from: Palemane Poacher [2951] L7-8, Palemane Skinner [2950] L6-7, Palemane Tanner [2949] L5-6
    - giver/turn-in: Baine Bloodhoof [2993] L10-10 / Baine Bloodhoof [2993] L10-10
- **grind → L8** on Adult Plainstrider [2956] L6-7 (3 anchor(s))
- **quest-grind** q766 "Mazzranache" (QL8)  (min L8)
    - kill/collect from: Adult Plainstrider [2956] L6-7, Flatland Cougar [3035] L7-8, Prairie Wolf [2958] L5-6, Wiry Swoop [2969] L5-7
    - giver/turn-in: Maur Raincaller [3055] L10-10 / Maur Raincaller [3055] L10-10
- **quest-grind** q743 "Dangers of the Windfury" (QL8)  (min L9)
    - kill/collect from: Windfury Harpy [2962] L7-8
    - giver/turn-in: Ruul Eagletalon [2985] L9-9 / Ruul Eagletalon [2985] L9-9
- **train** (learn available spells)
- **vendor/sell+repair** at Moorat Longstride [3076] L12-12
- **grind → L9** on Prairie Stalker [2959] L7-8 (3 anchor(s))
- **quest-grind** q833 "A Sacred Burial" (QL10)  (min L10)
    - kill/collect from: Bristleback Interloper [3232] L9-10
    - giver/turn-in: Lorekeeper Raintotem [3233] L8-8 / Lorekeeper Raintotem [3233] L8-8
- **grind → L10** on Prairie Stalker [2959] L7-8 (3 anchor(s))
- **vendor/sell+repair** at Moorat Longstride [3076] L12-12


## Tirisfal Glades — Undead Rogue  (`tirisfal_undead_rogue_1_10`)

- **Character:** Roguetwelve  ·  **Map:** 0  ·  **Opportunistic spell:** 1752  ·  **Heal spell:** —
- **Author notes:** Tirisfal Glades 1->10 (Undead), third authored zone -- shared by the Rogue (melee cell) and Priest (ranged cell) of the per-zone matrix; clone per character. All Durotar/Mulgore lessons encoded. Zone notes from mining: the Deathknell->Brill breadcrumb chain (382/383) is locked behind a sparse named (Meven Korgal), so the transition is a plain road walk; 374/375 skipped (sparse Zealot/Greater Duskb
- **Segments (21), in execution order:**

- **accept** q363 "Rude Awakening" (QL1) from Undertaker Mordo [1568] L5-5
- **turn-in** q363 "Rude Awakening" (QL1) to Shadow Priest Sarvis [1569] L5-5
- **quest-grind** q364 "The Mindless Ones" (QL2)  (min L2)
    - kill/collect from: Mindless Zombie [1501] L1-1, Wretched Ghoul [1502] L1-2
    - giver/turn-in: Shadow Priest Sarvis [1569] L5-5 / Shadow Priest Sarvis [1569] L5-5
- **quest-grind** q3901 "Rattling the Rattlecages" (QL3)  (min L3, optional)
    - kill/collect from: Rattlecage Skeleton [1890] L2-3
    - giver/turn-in: Shadow Priest Sarvis [1569] L5-5 / Shadow Priest Sarvis [1569] L5-5
- **quest-grind** q376 "The Damned" (QL2)  (min L2)
    - kill/collect from: Duskbat [1512] L1-2, Mangy Duskbat [1513] L3-4, Ragged Scavenger [1509] L2-3
    - giver/turn-in: Novice Elreth [1661] L3-3 / Novice Elreth [1661] L3-3
- **train** (learn available spells)
- **vendor/sell+repair** at Joshua Kien [2115] L5-5
- **grind → L4** on Wretched Ghoul [1502] L1-2 (3 anchor(s))
- **quest-grind** q380 "Night Web's Hollow" (QL4)  (min L4)
    - kill/collect from: Night Web Spider [1505] L3-4, Young Night Web Spider [1504] L2-3
    - giver/turn-in: Executor Arren [1570] L5-5 / Executor Arren [1570] L5-5
- **quest-grind** q381 "The Scarlet Crusade" (QL4)  (min L4)
    - kill/collect from: Scarlet Convert [1506] L3-3, Scarlet Initiate [1507] L3-4
    - giver/turn-in: Executor Arren [1570] L5-5 / Executor Arren [1570] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Joshua Kien [2115] L5-5
- **grind → L6** on Ragged Scavenger [1509] L2-3 (3 anchor(s))
- **walk** [walk-deathknell-to-brill] 4 hop(s) → [2269.5, 244.9]
- **quest-grind** q404 "A Putrid Task" (QL6)  (min L7)
    - kill/collect from: Ravaged Corpse [1526] L6-7, Rotting Dead [1525] L5-6
    - giver/turn-in: Deathguard Dillinger [1496] L22-22 / Deathguard Dillinger [1496] L22-22
- **grind → L8** on Rotting Dead [1525] L5-6 (3 anchor(s))
- **quest-grind** q427 "At War With The Scarlet Crusade" (QL8)  (min L8)
    - kill/collect from: Scarlet Warrior [1535] L6-7
    - giver/turn-in: Executor Zygand [1515] L14-14 / Executor Zygand [1515] L14-14
- **train** (learn available spells)
- **vendor/sell+repair** at Abigail Shiel [2118] L9-9
- **grind → L10** on Ravaged Corpse [1526] L6-7 (3 anchor(s))
- **vendor/sell+repair** at Abigail Shiel [2118] L9-9


## Tirisfal Glades — Undead Priest  (`tirisfal_undead_priest_1_10`)

- **Character:** Priestwelve  ·  **Map:** 0  ·  **Opportunistic spell:** 585  ·  **Heal spell:** 2050
- **Author notes:** Tirisfal Glades 1->10 (Undead), third authored zone -- shared by the Rogue (melee cell) and Priest (ranged cell) of the per-zone matrix; clone per character. All Durotar/Mulgore lessons encoded. Zone notes from mining: the Deathknell->Brill breadcrumb chain (382/383) is locked behind a sparse named (Meven Korgal), so the transition is a plain road walk; 374/375 skipped (sparse Zealot/Greater Duskb
- **Segments (21), in execution order:**

- **accept** q363 "Rude Awakening" (QL1) from Undertaker Mordo [1568] L5-5
- **turn-in** q363 "Rude Awakening" (QL1) to Shadow Priest Sarvis [1569] L5-5
- **quest-grind** q364 "The Mindless Ones" (QL2)  (min L2)
    - kill/collect from: Mindless Zombie [1501] L1-1, Wretched Ghoul [1502] L1-2
    - giver/turn-in: Shadow Priest Sarvis [1569] L5-5 / Shadow Priest Sarvis [1569] L5-5
- **quest-grind** q3901 "Rattling the Rattlecages" (QL3)  (min L3, optional)
    - kill/collect from: Rattlecage Skeleton [1890] L2-3
    - giver/turn-in: Shadow Priest Sarvis [1569] L5-5 / Shadow Priest Sarvis [1569] L5-5
- **quest-grind** q376 "The Damned" (QL2)  (min L2)
    - kill/collect from: Duskbat [1512] L1-2, Mangy Duskbat [1513] L3-4, Ragged Scavenger [1509] L2-3
    - giver/turn-in: Novice Elreth [1661] L3-3 / Novice Elreth [1661] L3-3
- **train** (learn available spells)
- **vendor/sell+repair** at Joshua Kien [2115] L5-5
- **grind → L4** on Wretched Ghoul [1502] L1-2 (3 anchor(s))
- **quest-grind** q380 "Night Web's Hollow" (QL4)  (min L4)
    - kill/collect from: Night Web Spider [1505] L3-4, Young Night Web Spider [1504] L2-3
    - giver/turn-in: Executor Arren [1570] L5-5 / Executor Arren [1570] L5-5
- **quest-grind** q381 "The Scarlet Crusade" (QL4)  (min L4)
    - kill/collect from: Scarlet Convert [1506] L3-3, Scarlet Initiate [1507] L3-4
    - giver/turn-in: Executor Arren [1570] L5-5 / Executor Arren [1570] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Joshua Kien [2115] L5-5
- **grind → L6** on Ragged Scavenger [1509] L2-3 (3 anchor(s))
- **walk** [walk-deathknell-to-brill] 4 hop(s) → [2269.5, 244.9]
- **quest-grind** q404 "A Putrid Task" (QL6)  (min L7)
    - kill/collect from: Ravaged Corpse [1526] L6-7, Rotting Dead [1525] L5-6
    - giver/turn-in: Deathguard Dillinger [1496] L22-22 / Deathguard Dillinger [1496] L22-22
- **grind → L8** on Rotting Dead [1525] L5-6 (3 anchor(s))
- **quest-grind** q427 "At War With The Scarlet Crusade" (QL8)  (min L8)
    - kill/collect from: Scarlet Warrior [1535] L6-7
    - giver/turn-in: Executor Zygand [1515] L14-14 / Executor Zygand [1515] L14-14
- **train** (learn available spells)
- **vendor/sell+repair** at Abigail Shiel [2118] L9-9
- **grind → L10** on Ravaged Corpse [1526] L6-7 (3 anchor(s))
- **vendor/sell+repair** at Abigail Shiel [2118] L9-9


## Eversong Woods — Blood Elf Paladin  (`eversong_belf_paladin_1_8`)

- **Character:** Paltwelve  ·  **Map:** 530  ·  **Opportunistic spell:** 0  ·  **Heal spell:** 635
- **Author notes:** Eversong Woods 1->8 (Blood Elf, map 530 -- first expansion-map orchestrated route), shared by the Paladin (melee) and Hunter (ranged) matrix cells; clone per character. Sunstrider Isle chain 8325->8326->8334->8335 is all creature-based; deliver quests bridge to Falconwing Square. Scope capped at 8: Eversong's level 6-10 quests lean heavily on GO/item-use mechanics (sanctum crystals, documents), so
- **Segments (18), in execution order:**

- **quest-grind** q8325 "Reclaiming Sunstrider Isle" (QL1)  (min L1)
    - kill/collect from: Mana Wyrm [15274] L1-1
    - giver/turn-in: Magistrix Erona [15278] L5-5 / Magistrix Erona [15278] L5-5
- **quest-grind** q8326 "Unfortunate Measures" (QL3)  (min L3)
    - kill/collect from: Springpaw Cub [15366] L1-1, Springpaw Lynx [15372] L2-3
    - giver/turn-in: Magistrix Erona [15278] L5-5 / Magistrix Erona [15278] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Shara Sunwing [15287] L5-5
- **accept** q8327 "Report to Lanthan Perilon" (QL3) from Magistrix Erona [15278] L5-5
- **turn-in** q8327 "Report to Lanthan Perilon" (QL3) to Lanthan Perilon [15281] L5-5
- **quest-grind** q8334 "Aggression" (QL4)  (min L4)
    - kill/collect from: Feral Tender [15294] L3-3, Tender [15271] L2-3
    - giver/turn-in: Lanthan Perilon [15281] L5-5 / Lanthan Perilon [15281] L5-5
- **grind → L4** on Tender [15271] L2-3 (3 anchor(s))
- **quest-grind** q8335 "Felendren the Banished" (QL5)  (min L5)
    - kill/collect from: Arcane Wraith [15273] L3-4, Tainted Arcane Wraith [15298] L4-4
    - giver/turn-in: Lanthan Perilon [15281] L5-5 / Lanthan Perilon [15281] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Shara Sunwing [15287] L5-5
- **accept** q8347 "Aiding the Outrunners" (QL5) from Lanthan Perilon [15281] L5-5  (min L5)
- **turn-in** q8347 "Aiding the Outrunners" (QL5) to Outrunner Alarion [15301] L5-5  (min L5)
- **walk** [walk-isle-to-falconwing] 4 hop(s) → [9476.9, -6859.2]
- **grind → L6** on Springpaw Lynx [15372] L2-3 (2 anchor(s))
- **quest-grind** q8475 "The Dead Scar" (QL6)  (min L6)
    - kill/collect from: Plaguebone Pillager [15654] L5-6
    - giver/turn-in: Ranger Jaela [15416] L30-30 / Ranger Jaela [15416] L30-30
- **grind → L8** on Springpaw Lynx [15372] L2-3 (2 anchor(s))
- **vendor/sell+repair** at Sheri [16259] L10-10


## Eversong Woods — Blood Elf Hunter  (`eversong_belf_hunter_1_8`)

- **Character:** Hunttwelve  ·  **Map:** 530  ·  **Opportunistic spell:** 0  ·  **Heal spell:** —
- **Author notes:** Eversong Woods 1->8 (Blood Elf, map 530 -- first expansion-map orchestrated route), shared by the Paladin (melee) and Hunter (ranged) matrix cells; clone per character. Sunstrider Isle chain 8325->8326->8334->8335 is all creature-based; deliver quests bridge to Falconwing Square. Scope capped at 8: Eversong's level 6-10 quests lean heavily on GO/item-use mechanics (sanctum crystals, documents), so
- **Segments (18), in execution order:**

- **quest-grind** q8325 "Reclaiming Sunstrider Isle" (QL1)  (min L1)
    - kill/collect from: Mana Wyrm [15274] L1-1
    - giver/turn-in: Magistrix Erona [15278] L5-5 / Magistrix Erona [15278] L5-5
- **quest-grind** q8326 "Unfortunate Measures" (QL3)  (min L3)
    - kill/collect from: Springpaw Cub [15366] L1-1, Springpaw Lynx [15372] L2-3
    - giver/turn-in: Magistrix Erona [15278] L5-5 / Magistrix Erona [15278] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Shara Sunwing [15287] L5-5
- **accept** q8327 "Report to Lanthan Perilon" (QL3) from Magistrix Erona [15278] L5-5
- **turn-in** q8327 "Report to Lanthan Perilon" (QL3) to Lanthan Perilon [15281] L5-5
- **quest-grind** q8334 "Aggression" (QL4)  (min L4)
    - kill/collect from: Feral Tender [15294] L3-3, Tender [15271] L2-3
    - giver/turn-in: Lanthan Perilon [15281] L5-5 / Lanthan Perilon [15281] L5-5
- **grind → L4** on Tender [15271] L2-3 (3 anchor(s))
- **quest-grind** q8335 "Felendren the Banished" (QL5)  (min L5)
    - kill/collect from: Arcane Wraith [15273] L3-4, Tainted Arcane Wraith [15298] L4-4
    - giver/turn-in: Lanthan Perilon [15281] L5-5 / Lanthan Perilon [15281] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Shara Sunwing [15287] L5-5
- **accept** q8347 "Aiding the Outrunners" (QL5) from Lanthan Perilon [15281] L5-5  (min L5)
- **turn-in** q8347 "Aiding the Outrunners" (QL5) to Outrunner Alarion [15301] L5-5  (min L5)
- **walk** [walk-isle-to-falconwing] 4 hop(s) → [9476.9, -6859.2]
- **grind → L6** on Springpaw Lynx [15372] L2-3 (2 anchor(s))
- **quest-grind** q8475 "The Dead Scar" (QL6)  (min L6)
    - kill/collect from: Plaguebone Pillager [15654] L5-6
    - giver/turn-in: Ranger Jaela [15416] L30-30 / Ranger Jaela [15416] L30-30
- **grind → L8** on Springpaw Lynx [15372] L2-3 (2 anchor(s))
- **vendor/sell+repair** at Sheri [16259] L10-10


## Elwynn Forest — Human Warrior  (`elwynn_human_warrior_1_8`)

- **Character:** Humantwelve  ·  **Map:** 0  ·  **Opportunistic spell:** 78  ·  **Heal spell:** —
- **Author notes:** Elwynn Forest 1->8 (Human) -- first Alliance route, shared by the Warrior (melee) and Mage (ranged) matrix cells; clone per character. Northshire chain 783->7->15->21 + 18/6, deliver 54 to Goldshire, then Fargodeep kobold item quests (47/60 share droppers) and optional 52. Quest 11's gnoll chain is locked behind an explore quest (62, AreaTrigger) -- skipped. Hubs ELWNorthshire/ELWGoldshire (game_t
- **Segments (31), in execution order:**

- **accept** q783 "A Threat Within" (QL1) from Deputy Willem [823] L18-18
- **turn-in** q783 "A Threat Within" (QL1) to Marshal McBride [197] L20-20
- **quest-grind** q7 "Kobold Camp Cleanup" (QL2)  (min L2)
    - kill/collect from: Kobold Vermin [6] L1-2
    - giver/turn-in: Marshal McBride [197] L20-20 / Marshal McBride [197] L20-20
- **quest-grind** q15 "Investigate Echo Ridge" (QL3)  (min L3)
    - kill/collect from: Kobold Worker [257] L3-3
    - giver/turn-in: Marshal McBride [197] L20-20 / Marshal McBride [197] L20-20
- **train** (learn available spells)
- **vendor/sell+repair** at Brother Danil [152] L5-5
- **quest-grind** q21 "Skirmish at Echo Ridge" (QL5)  (min L5)
    - kill/collect from: Kobold Laborer [80] L3-3
    - giver/turn-in: Marshal McBride [197] L20-20 / Marshal McBride [197] L20-20
- **quest-grind** q18 "Brotherhood of Thieves" (QL4)  (min L4)
    - kill/collect from: Defias Thug [38] L3-4
    - giver/turn-in: Deputy Willem [823] L18-18 / Deputy Willem [823] L18-18
- **quest-grind** q6 "Bounty on Garrick Padfoot" (QL5)  (min L5, optional)
    - kill/collect from: Garrick Padfoot [103] L5-5
    - giver/turn-in: Deputy Willem [823] L18-18 / Deputy Willem [823] L18-18
- **grind → L5** on Kobold Worker [257] L3-3 (3 anchor(s))
- **accept** q54 "Report to Goldshire" (QL5) from Marshal McBride [197] L20-20  (min L5)
- **walk** [walk-northshire-to-goldshire] 4 hop(s) → [-9464.0, 62.0]
- **turn-in** q54 "Report to Goldshire" (QL5) to Marshal Dughan [240] L25-25
- **quest-grind** q47 "Gold Dust Exchange" (QL7)  (min L7)
    - kill/collect from: Kobold Miner [40] L6-7
    - giver/turn-in: Remy "Two Times" [241] L5-5 / Remy "Two Times" [241] L5-5
- **quest-grind** q60 "Kobold Candles" (QL7)  (min L7)
    - kill/collect from: Kobold Miner [40] L6-7
    - giver/turn-in: William Pestle [253] L6-6 / William Pestle [253] L6-6
- **train** (learn available spells)
- **vendor/sell+repair** at Brog Hamfist [151] L10-10
- **accept** q5261 "Eagan Peltskinner" (QL2) from Deputy Willem [823] L18-18  (min L2, optional)
- **turn-in** q5261 "Eagan Peltskinner" (QL2) to Eagan Peltskinner [196] L3-3  (optional)
- **accept** q3903 "Milly Osworth" (QL4) from Deputy Willem [823] L18-18  (min L4, optional)
- **turn-in** q3903 "Milly Osworth" (QL4) to Milly Osworth [9296] L2-2  (optional)
- **accept** q2158 "Rest and Relaxation" (QL5) from Falkhaan Isenstrider [6774] L10-10  (min L5, optional)
- **turn-in** q2158 "Rest and Relaxation" (QL5) to Innkeeper Farley [295] L30-30  (optional)
- **accept** q85 "Lost Necklace" (QL6) from "Auntie" Bernice Stonefield [246] L6-6  (min L6, optional)
- **turn-in** q85 "Lost Necklace" (QL6) to Billy Maclure [247] L1-1  (optional)
- **accept** q114 "The Escape" (QL7) from William Pestle [253] L6-6  (min L7, optional)
- **turn-in** q114 "The Escape" (QL7) to Maybell Maclure [251] L2-2  (optional)
- **grind → L7** on Mangy Wolf [525] L5-6 (3 anchor(s))
- **quest-grind** q52 "Protect the Frontier" (QL10)  (min L10, optional)
    - kill/collect from: Prowler [118] L9-10, Young Forest Bear [822] L8-9
    - giver/turn-in: Guard Thomas [261] L30-30 / Guard Thomas [261] L30-30
- **grind → L8** on Kobold Miner [40] L6-7 (3 anchor(s))
- **vendor/sell+repair** at Brog Hamfist [151] L10-10


## Elwynn Forest — Human Mage  (`elwynn_human_mage_1_8`)

- **Character:** Magetwelve  ·  **Map:** 0  ·  **Opportunistic spell:** 133  ·  **Heal spell:** —
- **Author notes:** Elwynn Forest 1->8 (Human) -- first Alliance route, shared by the Warrior (melee) and Mage (ranged) matrix cells; clone per character. Northshire chain 783->7->15->21 + 18/6, deliver 54 to Goldshire, then Fargodeep kobold item quests (47/60 share droppers) and optional 52. Quest 11's gnoll chain is locked behind an explore quest (62, AreaTrigger) -- skipped. Hubs ELWNorthshire/ELWGoldshire (game_t
- **Segments (31), in execution order:**

- **accept** q783 "A Threat Within" (QL1) from Deputy Willem [823] L18-18
- **turn-in** q783 "A Threat Within" (QL1) to Marshal McBride [197] L20-20
- **quest-grind** q7 "Kobold Camp Cleanup" (QL2)  (min L2)
    - kill/collect from: Kobold Vermin [6] L1-2
    - giver/turn-in: Marshal McBride [197] L20-20 / Marshal McBride [197] L20-20
- **quest-grind** q15 "Investigate Echo Ridge" (QL3)  (min L3)
    - kill/collect from: Kobold Worker [257] L3-3
    - giver/turn-in: Marshal McBride [197] L20-20 / Marshal McBride [197] L20-20
- **train** (learn available spells)
- **vendor/sell+repair** at Brother Danil [152] L5-5
- **quest-grind** q21 "Skirmish at Echo Ridge" (QL5)  (min L5)
    - kill/collect from: Kobold Laborer [80] L3-3
    - giver/turn-in: Marshal McBride [197] L20-20 / Marshal McBride [197] L20-20
- **quest-grind** q18 "Brotherhood of Thieves" (QL4)  (min L4)
    - kill/collect from: Defias Thug [38] L3-4
    - giver/turn-in: Deputy Willem [823] L18-18 / Deputy Willem [823] L18-18
- **quest-grind** q6 "Bounty on Garrick Padfoot" (QL5)  (min L5, optional)
    - kill/collect from: Garrick Padfoot [103] L5-5
    - giver/turn-in: Deputy Willem [823] L18-18 / Deputy Willem [823] L18-18
- **grind → L5** on Kobold Worker [257] L3-3 (3 anchor(s))
- **accept** q54 "Report to Goldshire" (QL5) from Marshal McBride [197] L20-20  (min L5)
- **walk** [walk-northshire-to-goldshire] 4 hop(s) → [-9464.0, 62.0]
- **turn-in** q54 "Report to Goldshire" (QL5) to Marshal Dughan [240] L25-25
- **quest-grind** q47 "Gold Dust Exchange" (QL7)  (min L7)
    - kill/collect from: Kobold Miner [40] L6-7
    - giver/turn-in: Remy "Two Times" [241] L5-5 / Remy "Two Times" [241] L5-5
- **quest-grind** q60 "Kobold Candles" (QL7)  (min L7)
    - kill/collect from: Kobold Miner [40] L6-7
    - giver/turn-in: William Pestle [253] L6-6 / William Pestle [253] L6-6
- **train** (learn available spells)
- **vendor/sell+repair** at Brog Hamfist [151] L10-10
- **accept** q5261 "Eagan Peltskinner" (QL2) from Deputy Willem [823] L18-18  (min L2, optional)
- **turn-in** q5261 "Eagan Peltskinner" (QL2) to Eagan Peltskinner [196] L3-3  (optional)
- **accept** q3903 "Milly Osworth" (QL4) from Deputy Willem [823] L18-18  (min L4, optional)
- **turn-in** q3903 "Milly Osworth" (QL4) to Milly Osworth [9296] L2-2  (optional)
- **accept** q2158 "Rest and Relaxation" (QL5) from Falkhaan Isenstrider [6774] L10-10  (min L5, optional)
- **turn-in** q2158 "Rest and Relaxation" (QL5) to Innkeeper Farley [295] L30-30  (optional)
- **accept** q85 "Lost Necklace" (QL6) from "Auntie" Bernice Stonefield [246] L6-6  (min L6, optional)
- **turn-in** q85 "Lost Necklace" (QL6) to Billy Maclure [247] L1-1  (optional)
- **accept** q114 "The Escape" (QL7) from William Pestle [253] L6-6  (min L7, optional)
- **turn-in** q114 "The Escape" (QL7) to Maybell Maclure [251] L2-2  (optional)
- **grind → L7** on Mangy Wolf [525] L5-6 (3 anchor(s))
- **quest-grind** q52 "Protect the Frontier" (QL10)  (min L10, optional)
    - kill/collect from: Prowler [118] L9-10, Young Forest Bear [822] L8-9
    - giver/turn-in: Guard Thomas [261] L30-30 / Guard Thomas [261] L30-30
- **grind → L8** on Kobold Miner [40] L6-7 (3 anchor(s))
- **vendor/sell+repair** at Brog Hamfist [151] L10-10


## Dun Morogh — Dwarf Warrior  (`dunmorogh_dwarf_warrior_1_8`)

- **Character:** Dwarftwelve  ·  **Map:** 0  ·  **Opportunistic spell:** 78  ·  **Heal spell:** —
- **Author notes:** Coldridge Valley / Dun Morogh 1->8, shared by the Dwarf Warrior (melee) and Gnome Mage (ranged) matrix cells (Dwarves and Gnomes share this start); clone per character. The Coldridge chains (179 wolves, 170 troggs, 183 boars, 182 troll cave, 233/234/282/420 delivery line to Kharanos) were the earlier arc's proven Dwarf+Gnome content. 218 (Grik'nir, cave named) optional. Hubs DMColdridge/DMKharanos
- **Segments (21), in execution order:**

- **quest-grind** q179 "Dwarven Outfitters" (QL1)  (min L1)
    - kill/collect from: Ragged Timber Wolf [704] L2-2, Ragged Young Wolf [705] L1-1
    - giver/turn-in: Sten Stoutarm [658] L5-5 / Sten Stoutarm [658] L5-5
- **quest-grind** q170 "A New Threat" (QL2)  (min L2)
    - kill/collect from: Burly Rockjaw Trogg [724] L2-2, Rockjaw Trogg [707] L1-2
    - giver/turn-in: Balir Frosthammer [713] L5-5 / Balir Frosthammer [713] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Adlin Pridedrift [829] L5-5
- **accept** q233 "Coldridge Valley Mail Delivery" (QL3) from Sten Stoutarm [658] L5-5
- **turn-in** q233 "Coldridge Valley Mail Delivery" (QL3) to Talin Keeneye [714] L5-5
- **quest-grind** q183 "The Boar Hunter" (QL3)  (min L3)
    - kill/collect from: Small Crag Boar [708] L3-3
    - giver/turn-in: Talin Keeneye [714] L5-5 / Talin Keeneye [714] L5-5
- **accept** q234 "Coldridge Valley Mail Delivery" (QL4) from Talin Keeneye [714] L5-5
- **turn-in** q234 "Coldridge Valley Mail Delivery" (QL4) to Grelin Whitebeard [786] L5-5
- **quest-grind** q182 "The Troll Cave" (QL4)  (min L4)
    - kill/collect from: Frostmane Troll Whelp [706] L3-4
    - giver/turn-in: Grelin Whitebeard [786] L5-5 / Grelin Whitebeard [786] L5-5
- **grind → L5** on Frostmane Troll Whelp [706] L3-4 (3 anchor(s))
- **quest-grind** q218 "The Stolen Journal" (QL5)  (min L5)
    - kill/collect from: Grik'nir the Cold [808] L5-5
    - giver/turn-in: Grelin Whitebeard [786] L5-5 / Grelin Whitebeard [786] L5-5
- **accept** q282 "Senir's Observations" (QL5) from Grelin Whitebeard [786] L5-5  (min L5)
- **turn-in** q282 "Senir's Observations" (QL5) to Mountaineer Thalos [1965] L15-15
- **accept** q420 "Senir's Observations" (QL5) from Mountaineer Thalos [1965] L15-15  (min L5)
- **walk** [walk-coldridge-to-kharanos] 4 hop(s) → [-5602.0, -510.0]
- **turn-in** q420 "Senir's Observations" (QL5) to Senir Whitebeard [1252] L8-12  (min L5)
- **train** (learn available spells)
- **vendor/sell+repair** at Boran Ironclink [1240] L9-10
- **grind → L8** on Frostmane Troll Whelp [706] L3-4 (3 anchor(s))
- **vendor/sell+repair** at Boran Ironclink [1240] L9-10


## Dun Morogh — Gnome Mage  (`dunmorogh_gnome_mage_1_8`)

- **Character:** Gnometwelve  ·  **Map:** 0  ·  **Opportunistic spell:** 133  ·  **Heal spell:** —
- **Author notes:** Coldridge Valley / Dun Morogh 1->8, shared by the Dwarf Warrior (melee) and Gnome Mage (ranged) matrix cells (Dwarves and Gnomes share this start); clone per character. The Coldridge chains (179 wolves, 170 troggs, 183 boars, 182 troll cave, 233/234/282/420 delivery line to Kharanos) were the earlier arc's proven Dwarf+Gnome content. 218 (Grik'nir, cave named) optional. Hubs DMColdridge/DMKharanos
- **Segments (21), in execution order:**

- **quest-grind** q179 "Dwarven Outfitters" (QL1)  (min L1)
    - kill/collect from: Ragged Timber Wolf [704] L2-2, Ragged Young Wolf [705] L1-1
    - giver/turn-in: Sten Stoutarm [658] L5-5 / Sten Stoutarm [658] L5-5
- **quest-grind** q170 "A New Threat" (QL2)  (min L2)
    - kill/collect from: Burly Rockjaw Trogg [724] L2-2, Rockjaw Trogg [707] L1-2
    - giver/turn-in: Balir Frosthammer [713] L5-5 / Balir Frosthammer [713] L5-5
- **train** (learn available spells)
- **vendor/sell+repair** at Adlin Pridedrift [829] L5-5
- **accept** q233 "Coldridge Valley Mail Delivery" (QL3) from Sten Stoutarm [658] L5-5
- **turn-in** q233 "Coldridge Valley Mail Delivery" (QL3) to Talin Keeneye [714] L5-5
- **quest-grind** q183 "The Boar Hunter" (QL3)  (min L3)
    - kill/collect from: Small Crag Boar [708] L3-3
    - giver/turn-in: Talin Keeneye [714] L5-5 / Talin Keeneye [714] L5-5
- **accept** q234 "Coldridge Valley Mail Delivery" (QL4) from Talin Keeneye [714] L5-5
- **turn-in** q234 "Coldridge Valley Mail Delivery" (QL4) to Grelin Whitebeard [786] L5-5
- **quest-grind** q182 "The Troll Cave" (QL4)  (min L4)
    - kill/collect from: Frostmane Troll Whelp [706] L3-4
    - giver/turn-in: Grelin Whitebeard [786] L5-5 / Grelin Whitebeard [786] L5-5
- **grind → L5** on Frostmane Troll Whelp [706] L3-4 (3 anchor(s))
- **quest-grind** q218 "The Stolen Journal" (QL5)  (min L5)
    - kill/collect from: Grik'nir the Cold [808] L5-5
    - giver/turn-in: Grelin Whitebeard [786] L5-5 / Grelin Whitebeard [786] L5-5
- **accept** q282 "Senir's Observations" (QL5) from Grelin Whitebeard [786] L5-5  (min L5)
- **turn-in** q282 "Senir's Observations" (QL5) to Mountaineer Thalos [1965] L15-15
- **accept** q420 "Senir's Observations" (QL5) from Mountaineer Thalos [1965] L15-15  (min L5)
- **walk** [walk-coldridge-to-kharanos] 4 hop(s) → [-5602.0, -510.0]
- **turn-in** q420 "Senir's Observations" (QL5) to Senir Whitebeard [1252] L8-12  (min L5)
- **train** (learn available spells)
- **vendor/sell+repair** at Boran Ironclink [1240] L9-10
- **grind → L8** on Frostmane Troll Whelp [706] L3-4 (3 anchor(s))
- **vendor/sell+repair** at Boran Ironclink [1240] L9-10
