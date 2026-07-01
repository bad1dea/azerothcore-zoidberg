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

## Gate 2 (in progress)

To be populated as Gate 2 slices land — see `HANDOFF.md` `NEXT TASK`.
