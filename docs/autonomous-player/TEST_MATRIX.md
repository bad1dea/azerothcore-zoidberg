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
| Gossip, vendor/repair, training mechanics | Not yet implemented | Deferred, `HANDOFF.md` `NEXT TASK` |
| Every race completing its starting area | Not yet attempted (only Orc/Durotar exercised so far) | Deferred |
| Second class controller (only Warrior exercised so far) | Not yet attempted | Deferred |
