# IdleBot Feature Tracker

Extracted from CopilotBuddy/HonorBuddy analysis (2026-06-23). Prioritized by impact on bot quality.
Check off items as they're implemented. Reference: CopilotBuddy (Likon69/CopilotBuddy), Quest-Behaviors, Questing-profiles.

## CRITICAL — Should have from day 1

- [ ] **#7 Vendor/repair NPC coords per zone** — embed vendor Entry+coords in guide YAML instead of 200yd dynamic search
- [ ] **#70 Per-profile vendor/repair/food NPCs** — each zone section in guides has nearest vendor, repair, food vendor
- [ ] **#64 Quest item protection** — never vendor quest-required items (check quest log before selling)
- [ ] **#19 Safe spot revival** — on corpse run, scan 360° in 15° increments at 40yd for hostile-free revive spot
- [ ] **#30 Target move timeout + blacklist** — 45s to reach a target, then blacklist entry for 10min so bot doesn't retry unreachable mobs
- [ ] **#38 Blackspot system** — mark stuck positions as 5yd-radius/3yd-height avoid zones; navigator re-routes around them; added dynamically on stuck
- [ ] **#34 Mount if destination >75yd** — use mount for travel between objectives
- [ ] **#35 UseMount auto-select** — find fastest available mount and use it
- [ ] **#4 Sell white items** — SellWhite=True default (configurable); currently only selling gray

## HIGH — Big impact on bot quality

- [ ] **#5 Sell green items** — configurable SellGreen option; frees more bag space for leveling
- [ ] **#10 Buy food/drink from vendor** — FoodAmount/DrinkAmount settings; buy level-appropriate food/water
- [ ] **#22 Avoid spirit healer by default** — prefer corpse run; spirit healer only as absolute last resort (currently escalates too fast)
- [ ] **#31 Target level range filtering** — don't engage mobs too far above bot level (TargetMinLevel/TargetMaxLevel)
- [ ] **#36 UseFlightPaths** — take discovered flight paths for long-distance same-continent travel
- [ ] **#37 Learn flight paths** — auto-discover FPs when passing flight masters
- [ ] **#40 Swimming/water detection** — handle water movement, surface when swimming
- [ ] **#46 Multi-step gossip** — sequential gossip option selection through multiple dialog frames
- [ ] **#48 Escort hostile detection** — scan 10yd around escort NPC for hostiles, pull them before they kill the NPC
- [ ] **#50 Quest pickup via gossip** — handle quests available through GossipFrame (not just direct QuestFrame)
- [ ] **#55 Buy specific item from vendor** — BuyItemId behavior for quests requiring purchased items
- [ ] **#60 Weapon DPS scoring** — evaluate quest reward weapons by DPS × 3.0 weight
- [ ] **#62 Class stat weights** — per-class stat weights for reward evaluation (Warrior: STR 2.0, Hit 1.8; Mage: SP 2.2, INT 1.6; etc.)
- [ ] **#71 Per-profile class trainers** — 5 class trainers per zone location embedded in guide
- [ ] **#8 Food vendor coords per zone** — embedded in guide for buy-food runs
- [ ] **#14 Trainer coords per zone** — 5 class trainers per location in guides
- [ ] **#24 Death area avoidance** — blackspot the area where bot died repeatedly
- [ ] **#26 Blacklist tagged mobs** — 5-minute blacklist for mobs tagged by other players

## MEDIUM — Quality of life

- [ ] **#9 Ammo vendor per zone** — hunter ammo vendor coords in guide
- [ ] **#11 Buy ammo** — hunters auto-buy ammo at vendors
- [ ] **#25 PullDistance configurable** — default 45yd, per-profile override
- [ ] **#32 Targeting distance** — 30 units while moving, 10 units while stationary
- [ ] **#42 POI precision distances** — Kill=15yd, Loot=4.5yd, Vendor=4yd, Quest=5yd interaction ranges
- [ ] **#47 Escort follow tuning** — 5yd re-follow trigger, 20yd MaxRange, 5min timeout
- [ ] **#56 NonCompeteDistance** — blacklist interact target for 90s if another player within 25yd
- [ ] **#57 WaitTime between interactions** — 3000ms default delay between NPC interactions
- [ ] **#63 Dual-slot comparison** — rings/trinkets: compare new item against weaker of the two equipped slots
- [ ] **#67 LootRadius configurable** — default 45yd, per-profile
- [ ] **#6 Mail items to alt** — MailGreen/MailBlue/MailPurple + MailRecipient setting

## LOW — Nice to have / not yet relevant

- [ ] **#16 Soulstone check** — check for soulstone before releasing spirit (7.5s wait)
- [ ] **#23 Instance death** — don't release in dungeons/BGs
- [ ] **#27 Blacklist dead targets** — 5-minute blacklist for dead mobs
- [ ] **#28 Pre-pull buffs** — check and apply class buffs before pulling
- [ ] **#33 Mount while pulling prevention** — don't engage while mounted unless configured
- [ ] **#41 FlyTo behavior** — flying mount navigation (post-60)
- [ ] **#44 Dismount before interaction** — auto-dismount before NPC interaction
- [ ] **#49 Quest frame retry** — 15 attempts before closing and retrying quest frame
- [ ] **#58 IgnoreCombat interaction** — option to interact with NPCs while in combat
- [ ] **#68 SkinMobs** — optional skinning after kills
- [ ] **#72 MailRecipient setting** — configurable mail target character
- [ ] **#73 UserSettings behavior** — per-profile-section overrides for pull distance, loot settings, training
- [ ] **#77 Jump-boarding transport** — repeated jump until IsOnTransport (more reliable boarding)
- [ ] **#78 StandLocation on transport** — move to specific spot while aboard

## ALREADY DONE ✅

- [x] **#1 MinFreeBagSlots** threshold (<=2 triggers vendor run)
- [x] **#2 MinDurability** threshold (40% triggers repair)
- [x] **#3 Sell gray items** (VendorTrash)
- [x] **#12 FindVendorsAutomatically** (200yd dynamic search)
- [x] **#13 TrainNewSkills** (AutoSpecTalents + InitClassSpells)
- [x] **#15 Road-safe vendor pathing** (guide step backtracking)
- [x] **#17 Release spirit** (RequestReleaseSpirit)
- [x] **#18 Corpse run** (ReviveOrCorpseRun)
- [x] **#20 Revival timeout** (ghostStallTicks)
- [x] **#21 Res sickness wait** (HasResSickness spell 15007)
- [x] **#29 Ranged vs melee positioning** (+ranged/+close)
- [x] **#39 Stuck handler escalation** (jump → strafe → reverse → walk to anchor)
- [x] **#43 Long-distance path chaining** (200yd MoveTo segments)
- [x] **#45 Gossip option selection** (GossipInteract step)
- [x] **#51 CollectItem from GameObjects** (InteractGameobject)
- [x] **#52 Auto-generate hotspots from DB** (generate_hotspots.py)
- [x] **#53 Profile chaining** (1-80 mega-guides)
- [x] **#54 UseItemOn behavior** (UseItemOnNpc step)
- [x] **#59 Armor type scoring** (ChooseBestReward)
- [x] **#61 Upgrade threshold** (playerbots 1.2x incremental)
- [x] **#65 LootMobs** (+loot strategy)
- [x] **#66 LootChests** (InteractGameobject, partial)
- [x] **#69 MinLevel/MaxLevel per profile** (levelMin/levelMax)
- [x] **#74 Profile chaining** (single 1-80 guide per race)
- [x] **#75 WaitAtLocation** for transport (dock coords)
- [x] **#76 Transport proximity detection** (IsTransportStopped)
- [x] **#79 EndLocation transport exit** (auto-disembark on map change)

---

**Score: 27/79 done (34%). Next session: start with CRITICAL items.**
