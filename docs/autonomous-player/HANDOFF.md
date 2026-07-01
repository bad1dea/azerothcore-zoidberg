# Session Handoff

## Current milestone
Gate 0 — project foundation. Acceptance: module builds and loads without
Playerbots; Lifecycle/Perception/Telemetry skeletons exist and are tested;
an automated check proves no Playerbots linkage or copied source.

## Completed this session
- Created `docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,
  KNOWN_FAILURES,TEST_MATRIX,HANDOFF}.md`.
- Recorded ADRs 001–007 in `ARCHITECTURE.md`: lifecycle/scheduling,
  tick-safe perception, decision model, persistence model, player-like
  policy enforcement, Playerbots non-dependency enforcement, module
  placement. Deferred the character/account model decision explicitly to
  Gate 1 (documented under "Character/account model" in
  `ARCHITECTURE.md`).
- Scaffolded `modules/mod-autonomous-player/` as a plain (non-submodule)
  module directory, auto-discovered by the existing
  `GetModuleSourceList` CMake glob — no `.gitmodules` entry or top-level
  CMake change needed, no `CMakeLists.txt` needed inside the module either
  (confirmed by inspecting `modules/mod-npc-gambler`, which has none).
  - `Lifecycle/BotLifecycleMgr.{h,cpp}` — bot registry keyed by
    `ObjectGuid`, with a per-bot millisecond accumulator that produces a
    staggered `TickCount` (Gate 0 has no behavior to run per tick yet —
    this proves the stagger mechanism only).
  - `Perception/PerceptionSnapshot.h` — pointer-free value type.
  - `Perception/PerceptionBuilder.{h,cpp}` — builds a snapshot from a live
    `Player const*` synchronously.
  - `Telemetry/Telemetry.h` — shared log category constant
    (`module.autonomous_player`).
  - `AutonomousPlayerModule.cpp` — two `WorldScript`s: config load
    (`AutonomousPlayer.Enable`) and startup/update (logs on startup, ticks
    `BotLifecycleMgr` from `OnUpdate` when enabled).
  - `mod_autonomous_player_loader.cpp` — `Addmod_autonomous_playerScripts()`
    entry point, matching this repo's module-loader naming convention.
  - `conf/mod_autonomous_player.conf.dist`, `README.md`.
  - `tools/check_no_playerbots_dependency.sh` — greps module source for
    `#include`s into mod-playerbots, Playerbots-prefixed symbols, and
    literal `mod-playerbots`/`Playerbots` references. Passes (no hits).
  - `tools/check_no_forbidden_apis.sh` — greps for direct teleport,
    direct-DB-write, and health-as-revive shortcuts disallowed by the
    player-like policy. Passes (no hits).
- Added `!modules/mod-autonomous-player` to `.gitignore` (the repo's
  `/modules/*` blanket-ignore assumes every module is a submodule; this
  one isn't, so it needed the same explicit negation the `.gitignore`
  template already documents for exactly this case).
- Created git branch `mod-autonomous-player` off `origin/Playerbot` (see
  Decisions below for why history is identical to
  `idlebot-contested-go-deploy` at the branch point).
- Ran `python apps/codestyle/codestyle-cpp.py` — pre-existing failures
  only, all in files this session didn't touch; nothing flagged under
  `modules/mod-autonomous-player`.

## Files changed
- `docs/autonomous-player/*.md`: new project documentation set (required
  by the project's operating instructions).
- `.gitignore`: added `!modules/mod-autonomous-player` negation so this
  non-submodule module's content is actually tracked.
- `modules/mod-autonomous-player/**`: Gate 0 module scaffold (listed
  above).

## Verification
- `python apps/codestyle/codestyle-cpp.py` (repo root): pre-existing
  failures only (all outside this module); no findings in
  `modules/mod-autonomous-player`.
- `modules/mod-autonomous-player/tools/check_no_playerbots_dependency.sh`:
  `OK: no Playerbots dependency found`.
- `modules/mod-autonomous-player/tools/check_no_forbidden_apis.sh`:
  `OK: no forbidden API usage found`.
- Compiler build check: this dev box has no local C++ toolchain (confirmed
  this session — no `cmake`/`g++` on PATH). The only real build path is
  SSH to host `zoidberg` + `docker build --target worldserver` (see prior
  idlebot-project sessions' runbook). User explicitly approved reusing
  that path for this project too, build-and-deploy, no need to ask again
  in future sessions.
  <!-- FILL IN AFTER RUNNING: outcome of the zoidberg build for this
       branch (pass/fail, any compile errors in mod-autonomous-player
       files, and whether it was deployed). -->

## Current repository state
- Branch: `mod-autonomous-player` (branched off `origin/Playerbot`,
  currently identical history to `Playerbot`/`idlebot-contested-go-deploy`
  at the branch point — see Decisions).
- Only `modules/mod-autonomous-player/**`, `docs/autonomous-player/**`,
  and `.gitignore` are staged for this session's commit.
- Unrelated dirty files present in the working tree that must be
  preserved (pre-existing, not from this session — belong to the
  idlebot/dashboard project on other branches): modified
  `modules/mod-ah-bot-plus`, `modules/mod-playerbots` (submodule pointer
  changes), `tools/dashboard/backend/routers/bots.py`; untracked
  `tools/dashboard/**` (frontend app, backend routers/db.py, etc.),
  `reports/`, and a stray file literally named
  `", m.get("Destination"), m.get("Type"))\nPY"` (looks like an accidental
  heredoc artifact from a prior session — left untouched, not this
  session's concern).

## Known failures
None yet — no runtime behavior exists to fail. See
`docs/autonomous-player/KNOWN_FAILURES.md`.

## Decisions made
- Branch name `mod-autonomous-player` (not `mod-idlebot`, which the user
  initially suggested but which was the freed-up name of the just-removed,
  unrelated idlebot module — confirmed with the user before creating it).
- Branched off `origin/Playerbot` rather than the current
  `idlebot-contested-go-deploy` branch, to keep this greenfield project's
  future commit history separate from idlebot's. Note: `origin/Playerbot`
  on this fork already contains all of the idlebot commits (they were
  pushed to it outside this session), so the two branches share identical
  history at the point of branching — separation is only guaranteed for
  commits made *from here forward*.
- No `CMakeLists.txt` inside the module: confirmed via
  `src/cmake/macros/ConfigureModules.cmake` /
  `src/cmake/macros/AutoCollect.cmake` that static modules are
  auto-globbed (source files and include directories, recursively) purely
  from the `modules/<name>/src` directory existing — matches the existing
  `mod-npc-gambler` module, which also has no `CMakeLists.txt`.
- No `SQLTransaction`/persistence schema created this session (ADR-004):
  there is no bot state yet to persist. `Persistence` stays an
  interface-only stub until Gate 1 needs it.
- No GTest-based unit tests added for `BotLifecycleMgr`'s stagger
  arithmetic: this repo's Google Test suite is core-only
  (`src/test/{common,server}`), and the zoidberg docker build path used
  for compile verification doesn't set `-DBUILD_TESTING=ON`. Verified the
  stagger logic by code inspection instead and recorded this as a
  documented gap in `TEST_MATRIX.md` rather than silently skipping it.
- Deferred the bot account/session model (how a `WorldSession` for a bot
  gets created) to Gate 1, per "resolve only architecture decisions needed
  this week."

## NEXT TASK
Gate 1, first slice: bring one configured level-1 Orc Warrior online and
expose a read-only perception snapshot for it.

Scope for that session only (do not also implement combat, guides, quest
accept, or travel — those are later Gate 1 slices, already listed in
`ROADMAP.md`'s backlog):
1. Decide and implement the account/session model deferred in this
   session's `ARCHITECTURE.md` (the "Character/account model" section) —
   likely a dedicated bot-owning account plus a real `Player` login path,
   consistent with the player-like policy (no direct DB-only character
   materialization that skips normal login).
2. Wire that bot into `BotLifecycleMgr` (`RegisterBot`/`UnregisterBot` on
   login/logout).
3. On each tick, call `PerceptionBuilder::BuildPerceptionSnapshot` for the
   bot and expose it read-only (a `.napi` GM command, a log line, or a
   simple accessor — pick the cheapest thing that lets a human verify the
   snapshot is correct without adding scope).
4. Verify manually via the zoidberg build+deploy path: the configured Orc
   Warrior actually appears online in the correct starting location, and
   the snapshot reads back correct level/position/health.

## Next-session acceptance criteria
- A configured level-1 Orc Warrior bot logs in at its correct racial
  starting location (Valley of Trials, Durotar) without any teleport call.
- `BotLifecycleMgr::IsRegistered` is true for that bot's GUID after login
  and false after logout.
- A `PerceptionSnapshot` built for that bot on a live tick reports the
  correct `CharacterGuid`, `Level` (1), `MapId`, position matching the
  spawn location, and `IsAlive == true`.
- `check_no_playerbots_dependency.sh` and `check_no_forbidden_apis.sh`
  still pass.
- Change is committed on the `mod-autonomous-player` branch;
  `docs/autonomous-player/HANDOFF.md` updated with this session's
  completed work and the next bounded task.

## Recommended next-session prompt
Read the project files and complete the NEXT TASK in this handoff. Build,
test, commit, and update this handoff. Do not begin later roadmap work.
