# Test Matrix

Tracks what's verified, how, and as of which commit. Update whenever a gate
adds coverage.

## Gate 0 — project foundation

| Check | Method | Status |
|---|---|---|
| Module directory is auto-discovered by CMake | Manual inspection of `GetModuleSourceList` glob behavior against `modules/mod-autonomous-player/src` | Verified (design-time, no build run this session — see `HANDOFF.md`) |
| Module has no Playerbots source dependency | `modules/mod-autonomous-player/tools/check_no_playerbots_dependency.sh` | Automated, run every session |
| Module has no forbidden player-like-policy API usage | `modules/mod-autonomous-player/tools/check_no_forbidden_apis.sh` | Automated, run every session |
| Module compiles standalone (no compiler on this dev box; only real build path is the zoidberg host) | `docker build --target worldserver` on host zoidberg against this branch | See `HANDOFF.md` verification section for this session's result |
| `BotLifecycleMgr` registry add/remove/tick-stagger logic | Manual code inspection this session (see `ARCHITECTURE.md` ADR-001). No gtest coverage yet — this repo's Google Test suite (`src/test/`) is core-only; module-level tests aren't wired into the zoidberg build's `BUILD_TESTING` flag (currently off in `apps/docker/Dockerfile`). Revisit once a Gate makes the arithmetic non-trivial enough to be worth wiring up. | Deferred, documented gap |
| `PerceptionSnapshot` is a value type with no engine pointers | Code review: struct has no `Player*`/`Unit*`/`Creature*` members (see header) | Verified by inspection |

## Gate 1 — first complete quest (first slice: online bot + perception)

| Check | Method | Status |
|---|---|---|
| Bot account provisioning (`.autonomousplayer provision`) | Live on zoidberg: `ap_test1` account created via `AccountMgr::CreateAccount` | Verified |
| Bot character creation via real opcode handler | Live on zoidberg: `Grunttestbot` (Orc/Warrior/male) created via `HandleCharCreateOpcode`, confirmed in `acore_characters.characters` | Verified |
| Bot login (no teleport, correct starting position) | Live on zoidberg: `.autonomousplayer login ap_test1 Grunttestbot` → online at map 1, `(-618.5, -4251.7, 38.7)` (Valley of Trials) | Verified |
| `BotLifecycleMgr` registration on real login | Live: `PLAYERHOOK_ON_LOGIN` → "1 bot(s) now registered" | Verified |
| `PerceptionSnapshot` read path (level/map/position/health/alive) | Live: perception log line matches expected values exactly | Verified |
| Bot session survives across world ticks | Live: `BotSessionMgr`-tracked session ticked repeatedly via `MapSessionFilter`, no premature deletion | Verified |
| No Playerbots dependency / no forbidden APIs | `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh` | Automated, pass every commit |
| Restart-resume (bot automatically comes back online after a worldserver restart with no manual login) | Not yet implemented — `Persistence` component (ADR-004) is still a stub. Currently a bot only comes online via explicit `.autonomousplayer login`. | Deferred, tracked in `ROADMAP.md` backlog |
| `BotLifecycleMgr::IsRegistered` false after logout | Mechanism implemented (`QueueForRemoval` in `OnPlayerLogout`) but not yet live-exercised (no test triggered an actual bot logout this session) | Deferred, low risk — revisit before closing full Gate 1 |

## Gate 2 — levels 1–6 (in progress)

| Check | Method | Status |
|---|---|---|
| `Navigation::MoveTo` uses real pathing, not teleport | Live on zoidberg: bot walked `(-618.5,-4251.7,38.7)` → `(-598.5,-4251.7,39.0)`; Z snapped to real terrain height (39.0), proving navmesh-based movement, not a position copy | Verified |
| `QuestEngine::RequestAcceptQuest` accepts a real quest via real opcode handler | Live on zoidberg: quest 4641 from creature 10176 (Kaltunk), `GetQuestStatus` went `0` → `1` (`QUEST_STATUS_COMPLETE`) after moving into real interaction range | Verified |
| Quest-accept respects real interaction range (not just "nearby") | Live: first attempt at ~8.6 yards silently failed (`QUEST_STATUS_NONE`); succeeded at ~1-2 yards | Verified (documented as expected behavior, not a bug — see `HANDOFF.md`) |
| No Playerbots dependency / no forbidden APIs | `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh` | Automated, pass every commit |
| `QuestEngine::RequestChooseReward` turns in a real quest via real opcode handler | Live on zoidberg: quest 4641 to creature 3143 (Gornek), status `1` (`QUEST_STATUS_COMPLETE`) → `6` (`QUEST_STATUS_REWARDED`), `IsQuestRewarded` true, XP `0` → `40`; confirmed both in-game and in `character_queststatus_rewarded` | Verified |
| Turn-in bypasses `HandleQuestgiverCompleteQuest` (UI-only, no-op for socketless bot) in favor of `HandleQuestgiverChooseRewardOpcode` (real reward grant) | Code review — see ADR-011 | Verified by design |
| `Combat::RequestAttack` starts a real melee engagement via real opcode handler | Live on zoidberg: attacked a Mottled Boar (55/55 hp), bot's `IsValidAttackTarget`/`Attack` ran for real via `HandleAttackSwingOpcode` | Verified |
| Combat completes without stalling (chase fix) | Live: after adding `MotionMaster::MoveChase`, killed 2 Mottled Boars and 2 Scorpid Workers cleanly, bot took 0 damage each time, `combat` returned to `false` after each kill | Verified (bug found + fixed this session, see `KNOWN_FAILURES.md`) |
| `Inventory::LootCorpse` opens/checks/releases loot via real opcode handlers | Live on zoidberg: looted all 4 kills above with no errors; reads `Creature::loot.items`/`.gold` directly rather than parsing our own no-op loot-response packet | Verified |
| Loot respects quest-gating (`QuestRequired` loot-table rows) | Live: `Scorpid Worker Tail` (90% chance) correctly did not drop because the bot's related quest was already turned in — confirmed via `creature_loot_template`, not assumed | Verified (documented as correct behavior, not a bug) |
| No Playerbots dependency / no forbidden APIs | `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh` | Automated, pass every commit |
| `Recovery::RequestReleaseSpirit` releases spirit via real opcode handler | Live on zoidberg: genuine death triggered (multi-pull escalation, see below), `HasPlayerFlag(PLAYER_FLAGS_GHOST)` went false→true | Verified |
| `Recovery::RequestReclaimCorpse` resurrects via real opcode handler | Live: after the real ~30-40s delay, `alive` went false→true, `ghost` true→false, full health, corpse cleared | Verified |
| Release-spirit correctly leaves the ghost in place when no graveyard is registered nearby | Live: death in open wilderness far from any graveyard zone; confirmed via code review this matches `Player::RepopAtGraveyard()`'s own documented fallback | Verified (documented as correct behavior, not a bug) |
| Triggering a genuine death requires real, escalating effort | Live: bot survived 3, 5, and 8-creature deliberate multi-pulls of Scorpid Workers (leveling up to 2 mid-fight); died only on a 4th, mixed 3-creature pull | Verified — meaningful confirmation starting-zone content is safe for legitimate play |
| `PerceptionSnapshot.IsGhost`/`HasCorpse`/`CorpseX/Y/Z` | Live: read back correctly at every stage of the death/recovery cycle above | Verified |
| No Playerbots dependency / no forbidden APIs | `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh` | Automated, pass every commit |
| `Economy::BuyItem`/`RepairAll` submit real vendor requests via real opcode handlers | Live on zoidberg: both submitted cleanly against Huklah (creature 3160) with no crashes/errors | Verified |
| Vendor buy respects real insufficient-funds validation | Live: bot had 0 copper, item cost 63; no money spent, no item received — confirmed as `Player::BuyItemFromVendorSlot`'s real check, not a bypass | Verified (documented as correct behavior, not a bug) |
| `Gossip::RequestGossipHello`/`RequestGossipSelectOption` build/select real menus via real opcode handlers | Live on zoidberg: Frang's menu correctly showed `[0] optionType=5 "I require warrior training."`; selecting it submitted with no errors | Verified |
| `Growth::RequestTrainerList`/`RequestLearnSpell` open/learn via real opcode handlers | Live on zoidberg: found spell 6673 (Battle Shout) as eligible via real `Trainer::CanTeachSpell`, submitted cleanly, no crashes | Verified |
| Trainer spell-learning respects real insufficient-funds validation | Live: spell costs 10 copper, bot had 0 — no state change, confirmed as `Trainer::TeachSpell`'s real check, not a bypass | Verified (documented as correct behavior, not a bug) |
| Second race/class (Human Priest) logs in at correct racial spawn | Live on zoidberg: `Priestestbot` online at `(-8950.0,-132.5,83.5)` map 0 (Northshire Abbey), confirmed against the live world DB `playercreateinfo`, no teleport | Verified |
| Second race/class completes a real quest cycle | Live: quest 783 "A Threat Within" (Deputy Willem, creature 823) accepted → complete → turned in to Marshal McBride (creature 197): XP 50→90, `rewarded=true` | Verified |
| Second race/class completes real kill+loot | Live: Diseased Young Wolf (creature 299) killed via melee (`Combat::RequestAttack`), 0 damage taken, looted cleanly | Verified |
| `Combat::RequestCastSpell` calls the real `Unit::CastSpell` pipeline | Live: cast request against spell 585 correctly rejected (`accepted=false`, no HP change) — consistent with a level-1 Priest having no offensive spell yet, not a defect | Verified (mechanism proven; positive damage-cast test deferred, documented as correct behavior) |
| `.autonomousplayer spellbook` reads live in-memory spellbook, not stale DB state | Live: `character_spell` in the DB was empty for a freshly-created, never-saved bot; the new command correctly listed all 42 live spell IDs from `Player::GetSpellMap()` | Verified |
| Every race completing its starting area | 2 of 10 races exercised (Orc, Human) — user confirmed (2026-07-01) this representative sample satisfies Gate 2's bar | Verified (Gate 2 marked complete in `ROADMAP.md`) |
| Second class controller | Human Priest exercised (melee + attempted spell-cast); Warrior and Priest both proven | Verified |

## Gate 3 — levels 1–12 (in progress)

| Check | Method | Status |
|---|---|---|
| `GuideRuntime::Tick` advances a bot through multiple steps automatically, no manual command between them | Live on zoidberg: issued `.autonomousplayer guidestart` once, then only polled `guidestatus`/`status` — `CurrentStep` advanced 0→1→2→3 (`finished=true`) on its own, final position exactly matched the last of 3 waypoints | Verified |
| `BotLifecycleMgr::Update`'s per-bot dispatch actually calls `GuideRuntime::Tick` (first time this call path has ever mattered) | Live: confirmed via the same test above — position changes prove `Navigation::MoveTo` was actually invoked automatically, not just bookkeeping | Verified |
| No Playerbots dependency / no forbidden APIs | `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh` | Automated, pass every commit |
| Combat-capable guide step (`StepType::KillNearest`: walk+kill+loot, fully automatic) against a nearby (~70yd), reachable target | Live on zoidberg: `guidestartcombat` against Mottled Boar (creature 3098) — finished within 15s, boar confirmed dead (`hp 0/55`) via `creaturestatus`, bot took 0 damage, zero manual commands after the single trigger | Verified |
| `KillNearest` against a target found near/past the search radius edge, after 3 fix attempts | Live on zoidberg: attempts 1-2 (arrival-gate, then bare `MoveChase`) each reproduced the identical stall on re-test — both disproven, not just insufficient. Attempt 3 (revert to real `Combat::RequestAttack` + `IsInCombat()` confirmation) verified clean **twice independently**, two different Mottled Boars, both dead within 15s, zero damage taken, zero contamination | **Resolved** — see `KNOWN_FAILURES.md` #3 for the full attempt-by-attempt evidence trail |
| `AcceptQuest`/`TurnInQuest` guide steps: interaction-range gating, already-active-quest fast path | Live: `AcceptQuest` step correctly advanced instantly when the target quest was already active/incomplete in the bot's log (`GetQuestStatus != NONE`) | Verified |
| Full automatic quest loop (AcceptQuest→KillNearest→TurnInQuest, one guide, zero manual steps) | Live: `guidestartquest` for quest 788 advanced 0→1→2 (accept, then a real automatic combat engagement — `combat=true`, took real damage, killed and looted the boar — then into turn-in), stalled correctly (not falsely) at step 2 since the quest genuinely needs 8 kills and only partial progress was made in this run | Verified — real automatic progress through all 3 step types in one guide, including a real mid-chain combat engagement |
| Explicit `PullState` machine (`Selecting`/`Approaching`/`Engaged`/`Looting`) for `KillNearest`, per `HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md` (ADR-022/023) | Live on zoidberg, twice independently: two Mottled Boars, 6s and ~11s completions, no regression from the prior flat-phase version; one run's diagnostics caught mid-flight showed real state progression (`pullState=2` `Engaged`, `approachTicks=5`) | Verified |
| Bounded stuck-timeout + blacklist for a target that never confirms engagement | Implemented (`MaxApproachTicks=20`, `FindNearestNonBlacklisted`) | Not yet exercised by a real unreachable-target scenario live — happy-path runs never needed it (`blacklisted=0` both times), so this specific code path remains unverified, noted honestly rather than assumed |
| `EncounterModel::BuildSnapshot` correctly reads real attacker state (`Unit::getAttackers()`) | Live on zoidberg: idle baseline correct (0 attackers); mid-fight against a real `KillNearest` pull showed `attackers=1`, correct `entry`, correct `isObjectiveTarget=true`, `distance≈0`; standalone `multipull` test showed a correctly-tagged real attacker (`hasUnplannedAdd=true` when no guide objective was set) | Verified (single-attacker case); multi-simultaneous-attacker case not empirically captured (combat resolved faster than polling), honestly noted as unproven rather than assumed |
| `KillNearest` engagement confirmation fixed (`GetVictim()==target`, not `IsInCombat()`) — real bug found via external review | Fixed by reasoning + code review; happy path re-verified correctly live (real target, correct guid, real melee range), 4 separate attempts across multiple pull sizes (2/4/6/8 boars) to force the negative case all failed to sustain overlap — test creatures die faster than console-command latency, a confirmed environment limitation, not further retried | Fixed, happy path re-verified; **the specific scenario the fix targets not proven by direct live observation after 4 honest attempts** — see `KNOWN_FAILURES.md` #5 |
| `EncounterModel` gates `KillNearest`'s engagement confirmation (first real behavior consumer, ADR-027) | Compiles clean, no regression across 4 live test attempts (happy path reconfirmed each time). The gated logic's behavior under a genuine add was not directly observed (same overlap-timing limitation as above) — correct by code review (synchronous check, no window for the review's original bug to reappear), not yet proven by observation | Implemented, no regression; specific gating behavior not directly observed live |
| Full multi-kill quest with real credit and real turn-in, end to end | Live on zoidberg: quest 788 "Cutting Teeth" (8 Mottled Boars) reached genuine `QUEST_STATUS_COMPLETE` after accumulated kills, then `guidestartquest` ran the full accept→kill→turn-in chain and confirmed `status=6` (`QUEST_STATUS_REWARDED`), real XP granted | Verified — a real, complete, automatic quest cycle, not just step composition |
| Dense camps/caves, ranged pulls, pets, full bags, broader guide validation, more race/class combos, class controllers (research doc step 3), bounded failure states for the remaining unbounded guide operations | Not yet attempted | Deferred, `HANDOFF.md` `NEXT TASK` |
