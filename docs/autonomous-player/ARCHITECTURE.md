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

## ADR-008: Bot account/character/session model <a name="decisions-account-model"></a>

**Decision:** A bot is a real character on a dedicated bot-owning account,
brought online through the exact same public, production opcode handlers a
game client uses -- not a reimplementation of login/creation logic, and not
a direct database write.

Three pieces, all under `Setup/` and `Lifecycle/BotLogin.*`:

1. **Session.** This fork's core (`src/server/game/Server/WorldSession.h`)
   already has a `WorldSession(..., std::shared_ptr<WorldSocket> sock, ...,
   bool is_bot = false)` constructor, and `WorldSession` is null-socket-safe
   throughout (`SendPacket` early-returns if `!m_Socket`, every socket touch
   in `Update()`/`KickPlayer()`/etc. is `if (m_Socket)`-guarded). This is
   core game-server API, not Playerbots module code, so using it is not a
   Playerbots dependency (see ADR-006) -- it's the same mechanism the core
   itself offers for any socketless session. `Setup::CreateBotSession`
   constructs one with `sock = nullptr`, `is_bot = true`, and registers it
   with `WorldSessionMgr::AddSession` exactly like a freshly-authenticated
   client session.
2. **Character creation.** `CharacterCreateInfo`'s fields are `protected`
   with `friend class WorldSession; friend class Player;` -- there is no
   public API to construct one and call `Player::Create` directly from
   outside `WorldSession`. So `Setup::SubmitCharacterCreate` builds a
   `CMSG_CHAR_CREATE`-shaped `WorldPacket` (name + 9 `uint8` fields, same
   wire format `WorldSession::HandleCharCreateOpcode` reads) and calls that
   *public* opcode handler directly. This guarantees every validation a
   real client's creation request goes through (name uniqueness, race/class
   DBC lookup, expansion gating, per-account/per-realm character limits,
   starting stats/spells/inventory) runs unmodified -- we are not
   re-deriving "what a level 1 character should have," we're asking the
   same code that already knows.
3. **Login.** Same technique: `Lifecycle::TryLoginBot` builds a
   `CMSG_PLAYER_LOGIN`-shaped packet (character GUID) and calls the public
   `WorldSession::HandlePlayerLoginOpcode`, which runs the real login
   pipeline (`HandlePlayerLoginFromDB` → `Player::LoadFromDB` →
   `ObjectAccessor::AddObject` → `Map::AddPlayerToMap`). No `TeleportTo`
   call is involved in the normal case (it only appears as a fallback if
   the character's saved position is inside an now-invalid instance, same
   as for a real player).

Both character creation and login are **asynchronous** (chained
`CharacterDatabase`/`LoginDatabase` queries via `_queryProcessor`). Neither
`Setup::SubmitCharacterCreate` nor `Lifecycle::TryLoginBot` blocks or
polls; completion is observed the same way the rest of the engine observes
it -- `WorldSessionMgr::UpdateSessions` (core, not us) pumps each
registered session's `Update()` every world tick, which drains
`_queryProcessor`. `BotLifecycleMgr` registration happens on the
`PLAYERHOOK_ON_LOGIN` hook once the world confirms login succeeded (see
`AutonomousPlayerModule.cpp`), not on request-submitted.

**Test-setup isolation (ADR-005):** `Setup::EnsureBotAccount` /
`CreateBotSession` / `SubmitCharacterCreate` are only ever called from the
`.autonomousplayer provision` admin/console command
(`Commands/cs_autonomousplayer.cpp`, `SEC_ADMINISTRATOR`) -- never from the
tick-driven bot runtime loop (`BotLifecycleMgr::Update`,
`AutonomousPlayerWorld::OnUpdate`). `.autonomousplayer login` is the
runtime-equivalent action (bring an *already-created* character online) and
is also currently only reachable via that same admin command; Gate 1's
later slices will move "log a configured bot in on world startup" into the
automatic runtime path once there's a Planner loop for it to feed.

**Why this design over alternatives:**
- *Rejected: direct DB row insertion for character creation.* Would need
  to hand-derive starting stats/spells/inventory/position per race/class
  and keep it in sync with every core/DBC change -- fragile, and exactly
  the kind of "skip legitimate content" shortcut the player-like policy
  forbids for runtime, so it shouldn't be normalized for setup either.
- *Rejected: drive a real embedded WoW client/socket loopback.* Far more
  infrastructure for the same result; the opcode-handler-reuse approach
  gets identical server-side validation without needing a client build at
  all.
- *Accepted risk:* this couples us to two `protected`/packet-shaped
  interfaces (`HandleCharCreateOpcode`, `HandlePlayerLoginOpcode`) that
  could change if this fork's core is patched. Low risk in practice --
  they're stable, long-standing WotLK 3.3.5a opcodes.

**Verification:** compile-checked and deployed to zoidberg's realm this
session (user confirmed it's a test server safe for this -- see
`HANDOFF.md`). Live testing immediately surfaced a real bug this design
missed: `WorldSession::IsBot()` is **not exclusive to this module**.
mod-playerbots sets the same flag on its own (large) random-bot pool, so an
`IsBot()`-only `PLAYERHOOK_ON_LOGIN` check silently registered hundreds of
Playerbots' bots into `BotLifecycleMgr` and spammed perception logs for all
of them. Fixed by adding account-name-prefix ownership
(`Setup::AccountPrefix = "ap_"`, `Setup::IsAutonomousPlayerAccount`, a small
synchronous `AccountMgr::GetName` lookup) as a second, required condition
alongside `IsBot()`. `EnsureBotAccount` now refuses to create an account
that doesn't start with the prefix, so the check is correct by
construction. This is the kind of thing only live testing catches -- worth
remembering for every future "is this thing mine" check in this module:
don't assume a core-level flag is exclusively ours just because we're the
reason it exists in this fork.

A second live-testing catch, same command: the original prefix
(`"autonomous_player_"`, 19 chars) was longer than
`AccountMgr::MAX_ACCOUNT_STR` (17) all by itself, so every
`EnsureBotAccount` call failed with `AOR_NAME_TOO_LONG` before the prefix
even mattered. Shortened to `"ap_"`.

**A third, more fundamental live-testing catch invalidates this ADR's
session design as written, and is the reason Gate 1 is not done.**
`.autonomousplayer provision` on zoidberg produced no character at all,
silently -- no error, no DB row, no log line. Traced it to
`WorldSession::Update()` (`src/server/game/Server/WorldSession.cpp`,
around line 605): after `ProcessQueryCallbacks()`, there is an
**unconditional** `if (!m_Socket) { return false; }`, and
`WorldSessionMgr::UpdateSessions` deletes any session whose `Update()`
returns false (`_sessions.erase(itr); delete pSession;`). This check is
*not* gated on `_isBot` anywhere in this fork's current core. So a
`sock = nullptr` session survives for exactly one `WorldSessionMgr`
tick, then is destroyed -- which orphans `HandleCharCreateOpcode`'s
multi-hop chained DB queries before their results ever come back (the
name-uniqueness/char-count queries take at least one DB round trip, and
the session got promoted from `_addSessQueue` and torn down inside the
same or next tick, before any of that returns). The same problem would
happen to a logged-in bot's session: it cannot survive to a second
`WorldSessionMgr::UpdateSessions` tick, so `Lifecycle::TryLoginBot` would
fail the same way for the same reason once actually exercised end to end.

This means the "sock = nullptr, is_bot = true" session model this ADR
described is necessary but **not sufficient** -- something has to keep
the session's `m_Socket` non-null (or otherwise make it survive this
specific check) across ticks. Two candidate fixes for next session,
neither attempted yet:
1. A small, explicitly-documented core patch: exempt `_isBot` sessions
   from the `if (!m_Socket) return false;` eviction in
   `WorldSession::Update()`. This is a change to
   `src/server/game/Server/WorldSession.cpp` (core, not a module), which
   CLAUDE.md doesn't forbid -- it only forbids depending on
   *mod-playerbots' own source*. Needs care: verify nothing else in
   `WorldSessionMgr`/`Map` assumes "session has no socket" implies
   "session is going away."
2. Give the bot session a real (even if minimal/loopback) `WorldSocket`,
   which is the "drive a real socket" alternative this ADR originally
   rejected as too much infrastructure -- worth re-costing now that the
   null-socket path is proven not to work as-is in this fork's core.

**Resolved, same session, by reading mod-playerbots' source (read-only --
understanding a public core code path's behavior via how another module
uses it is not a dependency; no Playerbots code is included, called, or
adapted below).** `modules/mod-playerbots/src/Bot/PlayerbotMgr.cpp`
constructs its bot `WorldSession`s identically (`sock = nullptr`,
`is_bot = true`) -- but **never calls `sWorldSessionMgr->AddSession()`**.
Its sessions are never in `WorldSessionMgr::_sessions` at all, so they
never go through `WorldSessionMgr::UpdateSessions`'s per-session
`Update()`/deletion loop -- the exact code path with the unconditional
`if (!m_Socket) return false`. Instead, `PlayerbotHolder` keeps its own
bot map and drives each bot's packet queue directly from its own update
loop (`PlayerbotHolder::UpdateSessions` /
`PlayerbotHolder::HandleBotPackets`), entirely independent of
`WorldSessionMgr`.

**Fix implemented (new: `Lifecycle::BotSessionMgr`, this module's own
from-scratch equivalent of that technique):**
`Setup::CreateBotSession` no longer calls `sWorldSessionMgr->AddSession`.
Instead, every bot session this module creates is tracked in
`BotSessionMgr` (a small owned list), which every
`AutonomousPlayerWorld::OnUpdate` tick calls
`session->Update(diff, MapSessionFilter)` on. `MapSessionFilter`
(`ProcessUnsafe() == false`, a public core class meant for `Map::Update()`
callers) makes `WorldSession::Update()` skip the entire
`if (updater.ProcessUnsafe()) { ...; if (!m_Socket) return false; }` block
-- so `ProcessQueryCallbacks()` (called unconditionally, earlier in the
function) still drains the async DB chain every tick, without ever
tripping the null-socket eviction. `BotSessionMgr` owns the session for as
long as it's tracked; `UntrackAndDelete` cleans it up once done
(character-creation completion, detected by a new
`Setup::PendingCharacterCreations` poller) or `QueueForRemoval` defers
deletion to the *next* tick when triggered from a
`PlayerScript::OnPlayerLogout` hook (deleting the session synchronously
inside that hook would be a use-after-free -- the hook fires from within
the session's own logout call stack).

This makes the earlier "patch core" candidate fix unnecessary: no core
files are modified. `WorldSession`'s existing `is_bot`/null-socket support
plus the already-public `MapSessionFilter` was sufficient once sessions
are driven by our own loop instead of `WorldSessionMgr`'s.

The live deploy was rolled back to the pre-session image once the bug was
diagnosed (see above), then re-deployed once this fix was verified working
end to end on zoidberg (see `HANDOFF.md` for the actual verification
transcript). Test account `ap_test1` (account id 204) was created during
diagnosis and reused for the fix verification.
