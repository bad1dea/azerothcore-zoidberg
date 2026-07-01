# Session Handoff

## Current milestone
Gate 1, first slice: bring one configured level-1 Orc Warrior online and
expose a read-only perception snapshot for it. **COMPLETE, verified live
on zoidberg.**

## Completed this session
- Implemented a bot account/character/session model (ARCHITECTURE.md
  ADR-008): a dedicated bot-owning account (`ap_`-prefixed), a
  `sock = nullptr, is_bot = true` `WorldSession`, character creation/login
  driven through real production code paths (not direct DB writes or a
  reimplemented login/creation flow).
- `Setup/BotProvisioning.{h,cpp}`, `Lifecycle/BotLogin.{h,cpp}`,
  `Lifecycle/BotSessionMgr.{h,cpp}` (new), `Setup/PendingCharacterCreations.{h,cpp}`
  (new), `Commands/cs_autonomousplayer.cpp` (`.autonomousplayer
  provision|login|status`).
- **Live-tested on zoidberg through many iterations and found + fixed
  four real bugs**, each only discoverable by actually running it:
  1. **Playerbots-bot false positive** — `WorldSession::IsBot()` isn't
     exclusive to this module; mod-playerbots sets it on its whole random-
     bot pool. Fixed with account-name-prefix ownership
     (`Setup::AccountPrefix`, `Setup::IsAutonomousPlayerAccount`).
     Commit `5d5840f`'s predecessor session; prefix itself later shortened
     to `"ap_"` (`AccountMgr::MAX_ACCOUNT_STR` is 17 chars).
  2. **Session doesn't survive past one world tick** — a
     `sock = nullptr` session registered via `sWorldSessionMgr->AddSession`
     gets deleted by `WorldSessionMgr::UpdateSessions` after exactly one
     `Update()` call (unconditional `if (!m_Socket) return false`),
     orphaning async DB work. Root-caused by reading (read-only, not
     depended on/copied) how mod-playerbots' own identically-constructed
     bot sessions avoid this: they never register with
     `WorldSessionMgr` at all, and drive their sessions from their own
     update loop instead. Fixed with new `Lifecycle::BotSessionMgr` (this
     module's own equivalent: ticks tracked sessions via
     `Update(diff, MapSessionFilter)`, which skips the null-socket
     eviction entirely while still draining `ProcessQueryCallbacks()`).
     **Commit `5d5840f`.**
  3. **Use-after-free in the creation-timeout path** — the first version
     of the fix above deleted a session on a polling timeout even though
     its async DB chain could still be in flight; confirmed live (a
     character appeared in the DB *after* the "timed out" log line).
     Fixed: never delete on timeout, only on confirmed completion; a
     stuck session is now a leak to investigate, not something to guess-
     and-delete. **Commit `3d49e7f`.**
  4. **Login rejected by `IsLegitCharacterForAccount`** —
     `HandlePlayerLoginOpcode` requires the character's GUID to already be
     in `_legitCharacters`, a set only populated by the character-list-
     *enumeration* flow a real client runs before login; a headless bot
     that never enumerates always fails this ("Account can't login with
     that character"). Fixed by bypassing `HandlePlayerLoginOpcode`
     entirely: resolve our own `LoginQueryHolder` and call the also-public
     `WorldSession::HandlePlayerLoginFromDB` directly — the same real
     login-finalization code, minus the client-only gate ahead of it
     (matches, independently, how mod-playerbots' bots do it).
     **Commit `8ada2d4`.**
  5. **Case-sensitive account-prefix comparison** — `AccountMgr::CreateAccount`
     uppercases every stored username, so `ap_test1` is stored as
     `AP_TEST1`; `IsAutonomousPlayerAccount`'s `starts_with("ap_")` against
     that stored uppercase name was always false. This made a login that
     had *actually succeeded* core-side (confirmed via `characters.online`
     being set to `1`, which only happens after `AddPlayerToMap` succeeds)
     look identical to a stuck/failed one from the outside, since our own
     `PLAYERHOOK_ON_LOGIN` ownership check silently rejected it. Fixed
     with a case-insensitive prefix comparison. **Commit `d83ea84`.**
- **Live verification, final state:** `.autonomousplayer login ap_test1
  Grunttestbot` → `bot 'Grunttestbot' ... logged in, 1 bot(s) now
  registered.` → perception log: `Grunttestbot lvl 1 map 1 pos (-618.5,
  -4251.7, 38.7) hp 70/70 alive=true combat=false` — correct Orc starting
  location (Valley of Trials, Durotar), no teleport call anywhere in the
  path. Also verified `characters.online` correctly resets to `0` on a
  worldserver restart (core's normal boot-time cleanup), so a stale
  "online" flag from an earlier attempt did not need manual handling.

## Files changed
- `docs/autonomous-player/ARCHITECTURE.md`: ADR-008, fully updated through
  all five bug findings and fixes above.
- `docs/autonomous-player/KNOWN_FAILURES.md`, `TEST_MATRIX.md`,
  `ROADMAP.md`: updated to reflect Gate 1 completion.
- `modules/mod-autonomous-player/src/Lifecycle/BotSessionMgr.{h,cpp}`
  (new), `Setup/PendingCharacterCreations.{h,cpp}` (new),
  `Setup/BotProvisioning.{h,cpp}`, `Lifecycle/BotLogin.{h,cpp}`,
  `Commands/cs_autonomousplayer.cpp`, `AutonomousPlayerModule.cpp`: all
  updated per the fixes above.

## Verification
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`,
  `codestyle-cpp.py`: pass on every commit this session.
- Compiled clean on zoidberg 9 times this session (one per fix
  iteration); final state (commit `d83ea84`) compiles clean and is
  currently deployed live on zoidberg.
- **Live behavior meets Gate 1's first-slice acceptance criteria**, see
  above. Restart-survival (full persistence, i.e. a bot automatically
  resuming a previous session after a worldserver restart with no manual
  `.autonomousplayer login`) is explicitly Persistence-component scope
  (ADR-004) and was correctly deferred, not part of this slice's
  acceptance bar — see NEXT TASK below for when that's needed.

## Current repository state
- Branch: `mod-autonomous-player`, commits this session: `5d5840f`,
  `fc631d0`, `3d49e7f`, `8ada2d4`, `d83ea84`, plus this handoff commit —
  all pushed to origin.
- zoidberg's live `ac-worldserver` is running this session's code
  (commit `d83ea84`), with one online-capable test bot provisioned:
  account `ap_test1` (id 204), character `Grunttestbot` (guid 2014, Orc
  Warrior, level 1).
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
None currently blocking. See `docs/autonomous-player/KNOWN_FAILURES.md`
for the full (now-fixed) history of this session's bugs, kept for future
reference since the debugging technique (verify everything live, don't
infer from silence — several of these bugs looked identical from the
outside despite having different root causes) is broadly reusable for
this project.

## Decisions made
- User gave explicit standing direction this session: "investigate how
  playerbots keeps its sessions alive - fix it and get to gate 3 on your
  own." Interpreting this as: keep working autonomously through this
  project's own bounded-increment/gate methodology (not a license to
  produce a huge unverified pile of code) — commit and live-verify each
  increment, only stop to report for a genuine blocker or a decision only
  the user can make. Gate 1 completion above was done this way (5
  separate bug-fix commits, each compiled and live-tested before moving
  on). Gate 2 work below continues the same pattern.
- Reused the existing test account/character (`ap_test1`/`Grunttestbot`)
  from the debugging process rather than provisioning a fresh one — it's
  a legitimately-created level-1 Orc Warrior, no different from one
  created fresh.

## NEXT TASK
Gate 2 first slice: prove the `Navigation` component works end-to-end —
move the online bot from its spawn point to a fixed nearby coordinate
using `MotionMaster` (never `TeleportTo`), and confirm the movement via
the `PerceptionSnapshot` position changing correctly over time. This is
the minimal proof needed before any quest-walk/combat-approach logic
(later Gate 2/3 slices) can be built on top of it.

Scope for that task:
1. Add a `Navigation` component (`Navigation/` directory) with a single
   function like `bool MoveTo(Player* bot, float x, float y, float z)`
   that uses `bot->GetMotionMaster()->MovePoint(...)` (or the equivalent
   current API in this codebase — check `MotionMaster.h` for the exact
   signature before assuming) rather than any teleport call.
2. Wire it into a minimal one-shot Planner/Executor action: on a debug
   command or a fixed schedule, tell the currently-online bot to walk a
   short distance (e.g. a few yards from its current position — something
   guaranteed reachable without navmesh/pathing edge cases, since real
   pathfinding is later Gate-2/3 scope, not this slice).
3. Verify live on zoidberg: trigger the move, watch the perception log
   over several ticks, confirm `PositionX/Y/Z` actually change smoothly
   toward the target (not an instant jump, which would indicate a
   teleport got used by mistake) and the bot ends up at/near the target
   coordinate.
4. Update `check_no_forbidden_apis.sh`'s deny-list coverage if needed (it
   already denies `TeleportTo`/`NearTeleportTo`).
5. Update docs (ARCHITECTURE.md for the Navigation ADR if a real decision
   is needed, HANDOFF.md, TEST_MATRIX.md), commit.

After that slice, Gate 2's remaining scope (tracked loosely, refine as
work proceeds): a first real quest accept via the public quest API for
the Orc/Troll starting quest chain in Valley of Trials, a minimal single-
target melee combat loop for the Warrior class controller, loot handling,
and repeating this for representative coverage of "every race completes
its starting area" / "every class controller completes representative
combat" per Gate 2's full acceptance bar. Each of those is its own bounded
slice — do not attempt them all in one commit.

## Next-session acceptance criteria
- The online bot's position visibly changes between two perception
  snapshots after a `MoveTo`-style call, ending at/near the intended
  coordinate, with no `TeleportTo`/`NearTeleportTo` call anywhere in the
  path (verified by `check_no_forbidden_apis.sh` passing and by manual
  code review of the new Navigation code).
- Verified live on zoidberg (not just compiled).
- `check_no_playerbots_dependency.sh` and `check_no_forbidden_apis.sh`
  still pass.
- Committed on `mod-autonomous-player`; `HANDOFF.md` updated.

## Recommended next-session prompt
Read the project files and complete the NEXT TASK in this handoff. Build,
test (live on zoidberg), commit, and update this handoff. Do not begin
later roadmap work in the same session unless this task, its tests,
documentation, and commit are all done and substantial budget remains.
