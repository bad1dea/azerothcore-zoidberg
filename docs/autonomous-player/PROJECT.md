# mod-autonomous-player — Project Charter

## Goal

Build autonomous player characters for AzerothCore (Classic level cap 60 or
Wrath level cap 80) that legitimately level from character creation and their
first racial starting quest to the configured level cap, playing like a real
human player: questing, fighting with class abilities, looting, equipping
upgrades, training, managing talents and money, repairing, buying supplies,
using every legitimate travel method, recovering from death, and resuming
correctly after server restarts.

This is a clean-room implementation. See [[decisions-playerbots-non-dependency]]
in `ARCHITECTURE.md` for the enforcement mechanism.

## Non-goals (for now)

- No PvP, raiding, dungeon grouping, or auction house automation.
- No GM commands, teleports, or database mutation of quest/item/level state at
  runtime (see the player-like policy below).
- No attempt to cover every quest in the game before Gate 6/7 — guides are
  built incrementally, gate by gate.

## Hard constraints (do not copy)

Must not copy, call, wrap, link, depend on, or adapt: `mod-playerbots`,
`PlayerbotAI`, Playerbots strategies/actions/databases/travel/combat code, the
existing IdleBot-to-Playerbots bridge, or Playerbots-derived source from other
repositories. Public AzerothCore game/server APIs and normal compatible
libraries are fair game. All bot planning, perception, navigation policy,
combat, quest execution, travel, inventory, equipment, economy, persistence,
and recovery are implemented from scratch in this module.

## Player-like policy (summary — authoritative version in root instructions)

Normal runtime must never teleport/set-position for travel or recovery, force
resurrect outside normal mechanics, grant/modify quests/items/money/XP/rep/
skills/spells/talents/travel-nodes/levels via GM or DB writes, advance guide
state without authoritative completion evidence, skip content because
combat/pathing/targeting is defective, or use abilities/equipment/mounts/
transport/money the character hasn't legitimately obtained. Test setup
scripts may create/reset characters before a run, but that is clearly
isolated from runtime code.

## Operating mode

This project is built in small, budget-bounded sessions — one task per
session, tested and committed before the session ends. See `HANDOFF.md` for
the current state and the next task. See `ROADMAP.md` for the gate structure
and the active weekly outcome. Do not attempt to plan or implement the whole
project in one sitting.

## Repository placement

`modules/mod-autonomous-player/` — a plain (non-submodule) module directory
under this repo's `modules/`, auto-discovered by
`src/cmake/macros/ConfigureModules.cmake::GetModuleSourceList` (any
`modules/<name>/src` directory is picked up automatically; no `.gitmodules`
entry or top-level CMake change needed). See
[[decisions-module-placement]] in `ARCHITECTURE.md`.

Work happens on the `mod-autonomous-player` git branch (branched off
`origin/Playerbot`), kept separate from the unrelated `idlebot-*` branch
history in this repo.
