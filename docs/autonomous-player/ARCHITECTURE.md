# Architecture

## Component boundaries

Fifteen components, each with an explicit interface, per the project's
governing spec: `Lifecycle`, `Perception`, `Planner`, `Executor`,
`Navigation`, `Travel`, `Combat`, `QuestEngine`, `GuideRuntime`, `Inventory`,
`Economy`, `Growth`, `Recovery`, `Persistence`, `Telemetry`.

Gate 0 implements only `Lifecycle` (skeleton), `Perception` (skeleton, read
path only), and `Telemetry` (skeleton). Everything else is a stub namespace
with no behavior, added incrementally per gate. Do not build ahead of the
active gate.

World-thread safety rules that apply to every component from day one:

- All world-object touches happen synchronously inside a `WorldScript`/
  `PlayerScript` hook callback on the world thread. No component may retain
  a raw `Player*`/`Creature*`/`Unit*` across ticks — store `ObjectGuid` and
  resolve via `ObjectAccessor::FindPlayer` / `Map::GetCreature` etc. at use
  time.
- No filesystem, DB, network, or sleep calls inline in a hook callback.
  Anything blocking goes through the existing async DB callback path
  (`_queryProcessor.AddCallback(...)`) or is deferred to a bounded, staggered
  per-bot budget inside `OnUpdate`.

## ADR-001: Lifecycle and scheduling

**Decision:** Bots are modeled as a registry of `BotSession` records keyed by
`ObjectGuid`, owned by a singleton `BotLifecycleMgr` (a `WorldScript`). The
registry is populated by `.playerbots bot add`-equivalent character
management (deferred to Gate 1 — see [[decisions-account-model]] below) and
driven forward by a single `OnUpdate(uint32 diff)` hook that iterates the
registry and calls each bot's `Tick(diff)` with a per-bot elapsed-time
accumulator, so bots are staggered rather than all ticking every world
frame.

**Why:** AzerothCore's `WorldScript::OnUpdate` is the only sanctioned
per-frame hook that runs on the world thread without extra threading
machinery. A single top-level dispatcher keeps the world-thread-safety
invariant enforceable in one place instead of scattered across components.

**Rejected alternative:** A dedicated bot thread pool. Rejected because it
reintroduces exactly the cross-thread pointer-lifetime hazard the
world-thread-safety rule exists to avoid, for no clear win at this scale
(tens to low hundreds of bots).

## ADR-002: Tick-safe perception

**Decision:** `Perception` never exposes live engine pointers to the
`Planner`. Each tick, for each bot, it builds a small value-type
`PerceptionSnapshot` struct (position, health/power, nearby units by
`ObjectGuid` + distance/class/hostility, quest log summary, inventory
summary) from the resolved `Player*`, then hands the snapshot to the
`Planner`. The snapshot's lifetime is scoped to that tick's call stack; nothing downstream stores it past the tick.

**Why:** This is the direct enforcement mechanism for the "no long-lived raw
pointers" rule and for "tick-safe" in the component list — by construction,
`Planner`/`Executor`/etc. physically cannot hold a stale pointer because they
never receive one.

## ADR-003: Decision model

**Decision:** `Planner` is a hierarchical priority list (highest-priority
task wins) re-evaluated every tick from the fresh `PerceptionSnapshot`, not a
persistent state machine object graph. Priority order (highest first, Gate 0
placeholder — will grow per gate): survive (flee/recover) > follow active
guide step > idle. Each priority level returns either "no opinion" (fall
through) or a `PlannedAction`, which `Executor` then attempts
non-blockingly with ack/timeout/cancel/retry.

**Why:** Re-deriving the decision from a fresh snapshot every tick, rather
than mutating long-lived planner state, makes resumability after a restart
(persist just the guide/step/retry counters, not an object graph) and
failure-classification (compare this tick's plan to last tick's outcome)
straightforward. This mirrors the `GUIDE_INVALID`/`OBJECTIVE_NO_PROGRESS`-
style structured-failure model the spec requires.

**Deferred:** The actual priority list beyond "survive > guide > idle" is
Gate 1+ scope.

## ADR-004: Persistence model

**Decision:** One `acore_characters` table,
`autonomous_player_state` (added via the standard `pending_db_characters`
SQL-update workflow — not yet created; first migration lands with Gate 1's
persistence slice), keyed by character GUID, storing: guide id + revision +
stable step id, planner priority-level snapshot, active action descriptor,
objective progress snapshot, travel-segment state, retry/death counters,
maintenance intent, policy revision, blocked reason + blocked-since
timestamp. Writes are transactional (single `SQLTransaction` per state
transition) so a bot can never resume with a stale `blocked` reason after
having moved on to a valid task.

**Why:** Matches the "Persistence" component's stated obligations directly
and gives Gate 1's "survive a server restart and resume correctly"
acceptance criterion a concrete implementation target.

**Deferred:** Schema is not created in Gate 0 — no bot state exists yet to
persist. Gate 0's `Persistence` namespace is an empty stub with the intended
interface documented in a header comment, not a working implementation.

## ADR-005: Player-like policy enforcement

**Decision:** A single allow-list of AzerothCore APIs is used for all
character mutation: normal movement generators (`MotionMaster` /
pathfinding), the real spell-cast path (`Unit::CastSpell` and friends), the
real quest API (`Player::CanTakeQuest`/`AddQuest`/`CompleteQuest` etc. — not
`CharacterDatabase.Execute` writes to `character_queststatus`), and the real
vendor/trainer/loot APIs. Direct `TeleportTo`, `Player::SetHealth`-as-revive,
or any `*_DIRECT_DB_WRITE*` path into quest/item/money/XP/rep/skill/spell/
talent/travel-node/level state is disallowed in runtime code. Test/setup
scripts that create or reset characters live in a clearly separate
`tools/` or `tests/`-only path, never imported by runtime code.

**Why:** Directly required by the player-like policy in `PROJECT.md`. Making
it an allow-list (not a promise to avoid a deny-list) means a code reviewer
can check "is this call in the allow-list" instead of trying to enumerate
every disallowed shortcut.

**Enforcement (Gate 0):** `tools/check_no_forbidden_apis.sh` (see below)
greps the module's own source for a small deny-list of obviously-forbidden
calls (`TeleportTo`, `SetHealth(`, `CharacterDatabase.Execute`,
`.DirectExecute`) as a cheap early warning. This is not a substitute for
code review, but it catches accidental regressions cheaply. Full enforcement
is a Gate 3+ code-review checklist item once real Combat/Travel/QuestEngine
code exists.

## ADR-006: Playerbots non-dependency enforcement <a name="decisions-playerbots-non-dependency"></a>

**Decision:** Two independent checks, both automated:

1. **Build-time isolation:** `mod-autonomous-player`'s `CMakeLists.txt`
   (via the standard `modules/CMakeLists.txt` module machinery) does not
   add `modules/mod-playerbots` as an include directory or link
   dependency. It links only against `game`/`game-interface` like any other
   AzerothCore module.
2. **Source-origin check:** `tools/check_no_playerbots_dependency.sh` greps
   `modules/mod-autonomous-player/src/` for `#include` paths pointing into
   `mod-playerbots`, for `Playerbot`-prefixed symbol usage
   (`PlayerbotAI`, `PlayerbotMgr`, `sRandomPlayerbotMgr`, etc.), and for the
   literal string `mod-playerbots`/`Playerbots` in a way that would indicate
   copy-paste (excluding this doc set, where the exclusion itself is
   spelled out in the script). It also verifies, via
   `ldd`/`nm` on the built object files once a build exists (Gate 1+, once
   there's something non-trivial to link), that no Playerbots symbol is
   pulled in. Gate 0 only has the grep-based source check, since nothing is
   built yet.

**Why:** The spec requires the module to build and run standalone with
Playerbots absent, and to demonstrate this with an automated check rather
than a promise. Splitting into source-grep (cheap, every session) and
link-check (only meaningful post-build) matches the two ways a dependency
could sneak in.

## ADR-007: Module placement <a name="decisions-module-placement"></a>

**Decision:** `modules/mod-autonomous-player/` is a plain directory in this
repository (not a git submodule pointing at an external remote), matching
the auto-discovery mechanism in
`src/cmake/macros/ConfigureModules.cmake::GetModuleSourceList` (any
`modules/<name>/src` directory is picked up; no `.gitmodules` entry
required).

**Why:** Every other module under `modules/` in this repo is a submodule
pointing at an existing external GitHub repo. This module doesn't have one
(and creating one is a separate, out-of-scope decision the user hasn't
asked for). A plain in-repo directory is buildable today with zero
additional plumbing and can be extracted into its own repo + submodule
later without changing its internal layout, if that's ever wanted.

## Character/account model (deferred decision) <a name="decisions-account-model"></a>

Not decided in Gate 0. The spec's Gate 1 acceptance criterion ("one level-1
Orc Warrior comes online") requires deciding how a bot's `WorldSession` is
created (a dedicated login-less session vs. a real bound account+character
via normal login, akin to the existing idlebot project's "real account"
model — see that project's own memory notes, which are explicitly
off-limits as *code* but not as prior art for *how AzerothCore exposes this
capability*). This decision is explicitly deferred to the Gate 1 session
that implements it, per the "resolve only architecture decisions needed
this week" rule.
