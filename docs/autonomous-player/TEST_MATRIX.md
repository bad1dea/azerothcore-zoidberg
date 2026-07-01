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

## Gate 1+ (not yet applicable)

To be populated when Gate 1 work begins: online-Orc-Warrior scenario,
perception-snapshot read path, restart-resume scenario.
