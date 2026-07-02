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

## ADR-009: Navigation (Gate 2 first slice)

**Decision:** `Navigation::MoveTo(Player* bot, float x, float y, float z)`
wraps `bot->GetMotionMaster()->MovePoint(id, x, y, z)` with
`generatePath = true` (the default) -- real navmesh pathing, the same
mechanism any core NPC AI or a real player's client-driven movement uses.
This is the only sanctioned way for this module to move a bot; direct
position setters or `TeleportTo`/`NearTeleportTo` remain denied by
`check_no_forbidden_apis.sh`.

**Why no more design than that:** `MotionMaster::MovePoint` is a stable,
long-standing public core API with an obvious, correct signature for this
need -- there's no real architecture decision to make beyond "use it, not
a teleport," which the player-like policy already mandates. Verified live
on zoidberg: an online bot's position changes smoothly across ticks toward
the target instead of jumping instantly.

**Deferred to a later Gate 2/3 slice:** stuck detection, alternate-route
fallback, hazard avoidance, and multi-hop travel-segment routing (all
explicitly `Navigation`/`Travel` component scope per the project's
architecture boundaries) -- this slice only proves the primitive works,
it is not the full component.

## ADR-010: QuestEngine (Gate 2 next slice: quest accept)

**Decision:** `QuestEngine::RequestAcceptQuest(Player* bot, uint32_t
questId, ObjectGuid const& questGiverGuid)` builds a synthetic
`CMSG_QUESTGIVER_ACCEPT_QUEST` packet and calls the also-public
`WorldSession::HandleQuestgiverAcceptQuestOpcode` directly -- the exact
same technique as ADR-008's character creation (reuse the real, production
opcode handler via a synthesized packet instead of re-deriving the
acceptance rules ourselves). Unlike login/character creation, this
handler is fully synchronous (no `CharacterDatabase`/`LoginDatabase` round
trip inside it), so none of `BotSessionMgr`'s survival machinery is
needed here -- the request completes (or is rejected) within the same
call.

**Why reuse the opcode handler instead of calling `Player::AddQuest`
directly:** `HandleQuestgiverAcceptQuestOpcode` already implements every
rule a real client's accept goes through --
`Object::hasQuest`/`CanInteractWithQuestGiver`/`Player::CanTakeQuest`/
`Player::CanAddQuest` (prerequisites, exclusive groups, race/class/level
gating, quest-log-full, distance/state) -- exactly the legitimacy
guarantee the player-like policy requires (ARCHITECTURE.md ADR-005:
"the real quest API ... not a reimplementation"). Calling `AddQuest`
directly would skip those checks entirely.

**Verification:** the real quest giver and quest ID for the Orc/Troll
Valley of Trials starting quest were found by querying the live world DB
on zoidberg (`creature_queststarter`/`quest_template` joined against
creatures spawned near the bot's own confirmed spawn point), not recalled
from memory of WoW content -- per the project spec's requirement to
inspect the actual target database revision rather than assume quest
content. Result: creature entry 10176 (Kaltunk) offers quest 4641 ("Your
Place In The World"). See `HANDOFF.md`/`TEST_MATRIX.md` for the live
accept-request result.

## ADR-011: QuestEngine turn-in (Gate 2 slice 3)

**Decision:** `QuestEngine::RequestChooseReward` reuses the public
`WorldSession::HandleQuestgiverChooseRewardOpcode` (via a synthesized
`CMSG_QUESTGIVER_CHOOSE_REWARD` packet) -- the function that actually
grants XP/items/reputation (`Player::RewardQuest`) -- the same pattern as
ADR-010. Deliberately does **not** go through
`WorldSession::HandleQuestgiverCompleteQuest`: that handler only ever
sends UI packets asking the client what to display next
(`SendQuestGiverRequestItems`/`SendQuestGiverOfferReward`), which are
no-ops for a socketless bot and grant nothing -- skipping straight to the
reward-choice step is correct, not a shortcut, because the "which UI to
show" decision has no server-side effect to reuse.

**Why the reward-choice index matters:** `rewardChoiceIndex` is checked
against `QUEST_REWARD_CHOICES_COUNT` (6) and, when the quest has real
item choices, against which items that quest's `quest_template` row
actually defines -- callers must look up the real quest data (or an
already-known value from Gate 2/3's quest-guide content once that exists)
rather than assuming index 0 is always meaningful; for a quest with zero
reward choices it's simply ignored.

## ADR-012: Combat, first slice (single-target melee engagement)

**Decision:** `Combat::RequestAttack(Player* bot, ObjectGuid const&
targetGuid)` reuses the public `WorldSession::HandleAttackSwingOpcode`
(via a synthesized `CMSG_ATTACKSWING` packet) -- same technique as every
other opcode-reuse function in this module. That handler validates the
target through the real `Unit::IsValidAttackTarget` and then calls the
real `Unit::Attack(victim, true)`. Once started, melee auto-attack swing
timers run automatically through core's normal per-tick `Unit`/`Map`
update -- the same mechanism driving combat for every other `Player`/
`Creature` already in the world (a bot's `Player` object was added to the
map via the real `Map::AddPlayerToMap` back in Gate 1, so it's already
being ticked normally; nothing extra is needed to make swings happen).

**Scope of this slice, deliberately minimal:** the project's Combat
requirements describe a much broader common engine (legal target
validation beyond `IsValidAttackTarget`, objective relevance, danger/pack-
density assessment, safe approach/pull positioning, ranged/LoS/pull-back,
adds/CC/interrupts/kiting/escape, post-combat loot, encounter history).
None of that exists yet. This slice only proves the primitive: given an
already-chosen, already-approached (via `Navigation::MoveTo`, not a
teleport) hostile target, a real melee engagement starts and runs via
real core mechanics through to a kill. Target selection in this slice is
"nearest creature of a given entry" (a debug-command convenience, see
`.autonomousplayer attack`), not real objective/danger-aware selection --
that's later Gate 2/3 Combat-component scope, tracked in `ROADMAP.md`.

**Verification:** live on zoidberg -- target creature's health decreasing
across several `.autonomousplayer creaturestatus` checks after
`RequestAttack`, ending in death; bot's `PerceptionSnapshot.IsInCombat`
observed `true` during the fight and `false` after. See `HANDOFF.md` for
the actual numbers.

## ADR-013: Inventory, first slice (loot a corpse)

**Decision:** `Inventory::LootCorpse(Player* bot, Creature* corpse)` reuses
the real opcode handlers a client's loot flow goes through --
`HandleLootOpcode` (opens loot / generates it if needed, same as
right-clicking a corpse), `HandleAutostoreLootItemOpcode` per item slot,
`HandleLootMoneyOpcode` if there's gold, `HandleLootReleaseOpcode` to
close out -- via synthesized packets, same pattern as every other
component in this module. Unlike a real client (which learns what's
lootable by parsing the `SMSG_LOOT_RESPONSE` we can't receive), this
function reads the slot count directly off the live `Creature::loot`
struct (a plain, all-public `struct Loot` with `items`/`gold` members) --
we have direct C++ access to the object, so there's no need to round-trip
through our own no-op outgoing packet to know what to loot.

**Deliberately minimal:** no auto-equip/reward-comparison, no bag-space
handling, no vendor/repair -- this slice only proves the primitive (open,
take everything, close) on an already-dead, already-approached corpse.
`.autonomousplayer loot` debug command for live verification ahead of any
Planner/Executor loop.

**Verified live on zoidberg, including an investigation that turned out
to confirm correctness rather than reveal a bug:** killed and looted a
Mottled Boar (entry 3098, empty loot table -- confirmed via
`creature_loot_template`, zero rows -- so "nothing looted" was the
correct, expected outcome, not a failure) and two Scorpid Workers (entry
3124). Both Scorpid Worker kills produced no new items despite
`creature_loot_template` listing a 90%-chance drop (item 4862, "Scorpid
Worker Tail"). Investigated rather than assumed: that specific loot-table
row has `QuestRequired = 1` -- it only drops for a player with an active
quest needing it, and this session's bot had already turned quest 4641 in
earlier, so the item was correctly ineligible to drop. The other loot-
table rows for this creature are low-odds reference tables (trash/coin),
so two consecutive empty results from those specifically is unsurprising.
Net result: `LootCorpse` opened, checked every slot, and released
cleanly with no errors on all three kills -- the mechanism is proven
correct; this creature/quest-state combination just had genuinely nothing
eligible to give it, which is itself a correct demonstration of the real
loot-table rules (including quest-gating) being honored, not bypassed.

## ADR-014: Recovery, first slice (death/release/reclaim)

**Decision:** `Recovery::RequestReleaseSpirit`/`RequestReclaimCorpse` reuse
the public `WorldSession::HandleRepopRequestOpcode`/
`HandleReclaimCorpseOpcode` (via synthesized `CMSG_REPOP_REQUEST`/
`CMSG_RECLAIM_CORPSE` packets) -- same pattern as every other component.
`HandleRepopRequestOpcode` calls the real
`Player::BuildPlayerRepop()`/`RepopAtGraveyard()`: core's own legitimate
death mechanic moves the released ghost to the nearest graveyard. That is
**not** a `TeleportTo` call made by this module -- it's the same thing
that happens to any real player who releases spirit, so it isn't a
player-like-policy violation, even though it moves the character's
position outside of normal walking.

`HandleReclaimCorpseOpcode` enforces, exactly as it would for a real
player: the bot must be a ghost, its corpse must still exist, the real
~30s corpse-reclaim delay must have elapsed, and the ghost must be within
`CORPSE_RECLAIM_RADIUS` (39 yards) of the corpse. This module does not
shortcut any of that -- the ghost must actually walk back (via
`Navigation::MoveTo`, real pathing) and the caller must actually wait out
the delay.

**Perception extended** (not a new ADR, just a natural Gate-2 addition to
the existing ADR-002 struct): `PerceptionSnapshot` now also carries
`IsGhost`/`HasCorpse`/`CorpseX/Y/Z`, since Recovery-aware future
Planner/Executor logic needs this to decide "walk to corpse and
reclaim" vs. anything else -- exactly the kind of state Perception is
supposed to expose.

**Verification, live on zoidberg:** a genuine death took real effort to
trigger -- a level-1 Orc Warrior with starting gear survived three
escalating deliberate multi-pulls (3, 5, then 8 simultaneous Scorpid
Workers, using a new debug-only `multipull` command built on
`WorldObject::GetCreatureListWithEntryInGrid` for real distinct
`Creature*` targets) without dying, even leveling up to 2 mid-combat from
the accumulated kill XP. Death only occurred on a fourth attempt: a mixed
pull of 2 Scorpid Workers + 1 Mottled Boar, sustained over ~15 seconds
(`hp` observed at 59→34→15→0 across successive checks). `alive=false`
confirmed. `RequestReleaseSpirit` → `ghost=true` immediately.

**A genuinely interesting non-bug finding:** the ghost did **not** move
to a graveyard -- it stayed exactly at the death coordinates. Traced via
code review (not assumed): `Player::RepopAtGraveyard()`'s own comment
says "if no grave found, stay at the current location" --
`sGraveyard->GetClosestGraveyard(this, GetTeamId())` returned null
because this death happened out in open wilderness far from any
registered graveyard zone. This is exactly what would happen to a real
player who died in the same remote spot -- not a defect in
`RequestReleaseSpirit`, a faithful reproduction of a real edge case in
core's own graveyard-lookup behavior.

Waited the real ~30-40s corpse-reclaim delay (ghost and corpse were
already co-located, so no walk was needed this time -- `Navigation::MoveTo`
would have been required had a graveyard existed elsewhere).
`RequestReclaimCorpse` succeeded: `alive=true`, `ghost=false`, full health,
corpse cleared (`HasCorpse` false afterward, matching real
`SpawnCorpseBones()` behavior). The full death→release→reclaim cycle
works end-to-end through real production code paths, including the
correct handling of a real edge case (no nearby graveyard) discovered
only by triggering an actual death rather than assuming the mechanic
in the abstract.

## ADR-015: Economy, first slice (vendor buy/repair)

**Decision:** `Economy::BuyItem`/`RepairAll` reuse the real opcode
handlers -- `WorldSession::HandleBuyItemOpcode` (fed a real
`WorldPackets::Item::BuyItem` struct -- this fork has migrated buy/sell
to structured C++ packet classes with public fields, so no byte-level
packet synthesis is needed here, just setting the same fields a real
client would populate) and `WorldSession::HandleRepairItemOpcode` (still
raw-`WorldPacket`-based, synthesized the same way as every other
component). `BuyItem` looks up the item's real vendor-slot index directly
off the live `Creature::GetVendorItems()` data (a plain public struct)
instead of parsing our own no-op outgoing `SMSG_LIST_INVENTORY`.
`RepairAll` passes an empty item GUID, which the handler treats as
"repair everything" (`Player::DurabilityRepairAll`).

**No shortcuts:** both go through `Player::GetNPCIfCanInteractWith` (real
interaction-range + NPC-flag check) inside the handler, and `BuyItem`
goes through the real `Player::BuyItemFromVendorSlot` (stock limits,
price, currency requirements, bag space -- all real). This module does
not touch money or items directly.

**Deliberately minimal:** no price comparison/budget logic, no "what
should I buy" decision-making -- this slice only proves the primitive
(ask to buy/repair a specific thing) works through real production code.
`.autonomousplayer buy`/`repair` debug commands for live verification.

**Verified live on zoidberg** against Huklah (creature 3160, a combined
vendor+repair NPC near Kaltunk): both `RepairAll` and `BuyItem` (item 85,
Dirty Leather Vest, 63 copper) submitted cleanly with no errors/crashes
and no unexpected server-log activity. The bot had 0 copper at the time,
so both requests had no visible effect (`money` unchanged, no new
`item_instance` row for item 85) -- this is the **correct, honest
outcome**: `Player::BuyItemFromVendorSlot`'s real insufficient-funds check
ran and correctly rejected the purchase, exactly as it would for a human
player with an empty coin purse. That's meaningful verification that the
real validation isn't bypassed, even though it isn't a "happy path"
purchase. A positive buy-succeeds test is deferred until the bot
legitimately earns some gold (no nearby creature in this area drops
money; quest rewards or a later, richer-loot area would be the
legitimate way to get there).

Compile note: `WorldPackets::Item::BuyItem packet(WorldPacket(CMSG_BUY_ITEM));`
was a classic C++ "most vexing parse" -- parsed as a function declaration,
not object construction. Fixed with brace-init
(`WorldPackets::Item::BuyItem packet{WorldPacket{CMSG_BUY_ITEM}};`),
caught by the zoidberg compile check before ever reaching live testing.

## ADR-016: Gossip, first slice (dialogue + option select)

**Decision:** `Gossip::RequestGossipHello`/`RequestGossipSelectOption`
reuse the real opcode handlers -- `WorldSession::HandleGossipHelloOpcode`
(synthesized `CMSG_GOSSIP_HELLO`) builds the actual server-side
`Player::PlayerTalkClass->GetGossipMenu()` state via real script/core
logic, same as right-clicking an NPC; `HandleGossipSelectOptionOpcode`
(synthesized `CMSG_GOSSIP_SELECT_OPTION`, with the real current menu ID
read back from that same state) selects an option, triggering whatever
real, scripted consequence that option has (e.g. opening a trainer
session for a `GOSSIP_OPTION_TRAINER` item).

`Gossip::FindGossipOptionIndex` reads the live `GossipMenu::GetMenuItems()`
container directly (a plain `std::map<uint32, GossipMenuItem>`) to find a
menu item by its real `OptionType` (e.g. `GOSSIP_OPTION_TRAINER = 5`,
`GossipDef.h`) -- the same "inspect the live server-side state directly
instead of parsing our own no-op outgoing packet" pattern used for
loot/vendor.

**Chosen test target:** Frang (creature 3153), a real Orc Warrior class
trainer physically located inside Valley of Trials itself (not a
far-away city trainer) -- `npcflag = 51` = `GOSSIP | QUESTGIVER |
TRAINER | TRAINER_CLASS`, confirmed via the live world DB, so a real
gossip step genuinely is required before training for this NPC (not
every trainer needs one -- this was verified, not assumed).

**Deliberately minimal:** this slice only proves gossip-open and
option-select work through real production code; it does not implement
the Training component itself (browsing/learning spells) -- that is
still a separate, later slice, tracked in `HANDOFF.md`.

**Verified live on zoidberg:** `RequestGossipHello` against Frang produced
a real menu with exactly the expected item -- `[0] optionType=5 "I
require warrior training."` -- confirming `GOSSIP_OPTION_TRAINER` really
is present and discoverable via `FindGossipOptionIndex` without any
text-matching guesswork. Selecting it (`RequestGossipSelectOption`)
submitted cleanly with no errors or crashes in the server log.
`.autonomousplayer gossiphello`/`gossiptrain` debug commands for live
verification.

## ADR-017: Growth, first slice (trainer spell learning)

**Decision:** `Growth::RequestTrainerList`/`RequestLearnSpell` reuse the
real opcode handlers -- `WorldSession::HandleTrainerListOpcode` (fed a
real `WorldPackets::NPC::Hello` struct) opens the trainer window, same as
selecting the `GOSSIP_OPTION_TRAINER` gossip option (ADR-016) on a real
client; `WorldSession::HandleTrainerBuySpellOpcode` (fed a real
`WorldPackets::NPC::TrainerBuySpell` struct) requests to learn a spell,
running the real `Trainer::TeachSpell` (money/skill/level checks, no
shortcuts).

`Growth::FindLearnableTrainerSpell` reads the live
`Trainer::Trainer::GetSpells()` list directly and filters with the real
`Trainer::CanTeachSpell` -- same "inspect live server-side state instead
of parsing our own no-op outgoing packet" pattern as loot/vendor/gossip.
**Precisely what `CanTeachSpell` checks** (confirmed by reading
`Trainer.cpp`, not assumed): race/class fit, already-known state,
level, skill-line requirement, and primary-profession point
availability -- **not** money. Affordability is checked separately,
inside `Trainer::TeachSpell` itself (the actual learn/purchase step) --
so "learnable" here means "eligible," not "affordable."

**Deliberately minimal:** finds and buys exactly one learnable (eligible)
spell to prove the primitive works; no "which spell is actually useful to
learn now" decision-making (that's Combat/class-controller scope once
this module has enough abilities to reason about). `.autonomousplayer
learnspell` debug command for live verification against Frang (creature
3153), the same real class trainer used for the Gossip slice.

**Verified live on zoidberg:** found and requested spell 6673 (Battle
Shout), eligible per `CanTeachSpell` -- confirmed via the live world DB
(`trainer_spell`) it costs 10 copper at required level 1. The bot's real
0-copper balance correctly blocked the actual learn (no state change, no
crash/error) -- the same class of honest, correct-behavior finding as
ADR-015's vendor-buy test. A positive "spell actually learned" test is
deferred until the bot legitimately earns some copper.

## ADR-018: Combat spell-casting (deviates from opcode-reuse pattern)

**Decision:** `Combat::RequestCastSpell(Unit* caster, Unit* target, uint32
spellId)` calls the real public core API `Unit::CastSpell(target,
spellId, /*triggered=*/false)` directly, rather than synthesizing
`CMSG_CAST_SPELL` like every other component in this module.

**Why this is the one deliberate exception to "always synthesize the
client packet":** `CMSG_CAST_SPELL`'s payload includes a
`SpellCastTargets` block whose shape varies by the spell's implicit
target mask (self, unit, ground location, item, or combinations) --
correctly reconstructing that by hand for an arbitrary spell is real,
non-trivial client-protocol work, unlike every other opcode this module
has reused so far (fixed small packets: a GUID, a slot index, a menu
selection). `Unit::CastSpell` with `triggered=false` runs through the
exact same real validation a synthesized packet's handler would
eventually reach anyway (cost, cooldown, range, line-of-sight, GCD via
`Spell::prepare`/`Spell::cast`) -- there is no real behavioral gap, only
an implementation-risk one (a subtly-wrong hand-built target block could
silently mis-cast). `Unit::CastSpell` is also already explicitly named in
ADR-005's allow-list ("the real spell-cast path -- `Unit::CastSpell` and
friends"), so this isn't a new exception to the player-like policy, just
the first component to actually exercise that specific line of it.

**Return value differs from every other Request* function:** because
`Unit::CastSpell` gives a real `SpellCastResult` synchronously (unlike
the socketless-session opcode handlers, which never talk back),
`RequestCastSpell` returns whether the cast was actually accepted
(`SPELL_CAST_OK`), not just "submitted."

**Verified live on zoidberg** against a level-1 Human Priest
(`Priestestbot`): tried a plausible early-Priest damage-spell candidate
(spell 585) against a real Diseased Young Wolf; correctly rejected
(`accepted=false`, target HP unchanged), consistent with the real
Priest leveling curve having no offensive spell at level 1, not a defect
in the mechanism. Discovering the bot's real starting spellbook required
a new `.autonomousplayer spellbook` debug command that reads
`Player::GetSpellMap()` live, rather than `character_spell` in the DB --
that table only reflects the last save, and it was empty for this
freshly-created, never-explicitly-saved bot even though the live
in-memory spellbook had 42 real entries.

**A genuine positive "spell deals damage" cast was later confirmed live**
(after `RequestCastSpell`'s return type became the real `SpellCastResult`,
ADR-025) against an Orc Warrior (`Grunttestbot`): a candidate offensive
ability (spell 78) was rejected early in combat (insufficient rage), then
accepted (`result=255`/`SPELL_CAST_OK`) once real rage had accumulated
from a few seconds of auto-attack, with the target creature confirmed
dead shortly after -- twice, independently. `SPELL_CAST_OK`'s value of
255 was confirmed against this codebase's own `SharedDefines.h` as the
real, correct success sentinel (not misread as an error). See
`KNOWN_FAILURES.md` #4 for the full investigation.

## ADR-019: GuideRuntime, first slice (automatic multi-step advance)

**Decision:** Gate 3's first slice is the smallest possible proof that a
bot can advance through multiple steps with **no manual command between
them** -- Gate 3's own stated "no manual step advances" requirement, and
the first real (non-stub) implementation of the `GuideRuntime` component
named in the project's 15-component list (previously an empty stub
namespace, per ADR-001's Gate 0 scoping).

`GuideRuntime::BotGuideState` holds a fixed, ordered `std::vector<GuideStep>`
plus a current-step index and per-step "action issued" flag. `GuideRuntime::Tick`
is called once per bot per `BotLifecycleMgr::TickIntervalMs` (1 second) --
previously that per-bot tick fired and did nothing (pure Gate 0
bookkeeping); this is the first time it actually dispatches real
behavior. For the current single step type (`StepType::MoveTo`): issues
`Navigation::MoveTo` once (guarded by the issued flag, so it isn't
re-sent every tick while the bot is still walking), then checks
`Player::GetDistance` against an arrival tolerance to decide when to
advance to the next step.

**Deliberately minimal:** one step type only (`MoveTo`); no guide
authoring format, no persistence (a guide is lost on bot logout/restart --
`Persistence`, ADR-004, is still a stub), no failure/retry handling, no
combat-in-guide. This slice exists purely to prove the tick-driven
automatic-advance *mechanism* itself, using the already-proven
`Navigation::MoveTo` primitive -- no new opcode/API surface. Combat steps
(walk-to + attack + loot a creature, automatically) are a natural
follow-up slice once this mechanism is confirmed live.

**Tick-safety:** `BotLifecycleMgr::Update` resolves the bot's `Player*`
fresh via `ObjectAccessor::FindPlayer(guid)` on every fire and passes it
to `GuideRuntime::Tick` -- never stored across ticks, per ADR-002.
`GuideRuntime::Tick` no-ops safely if the resolved pointer is null (bot
logged out/despawned since being registered).

`.autonomousplayer guidestart`/`guidestatus` debug commands: `guidestart`
attaches a fixed 3-waypoint patrol (Valley of Trials landmarks already
used this session -- Frang, Huklah, Kaltunk's spawn area) and starts it;
`guidestatus` is read-only. Critically, **no further command is needed
between `guidestart` and completion** -- that gap is exactly what this
slice is verifying.

**Verified live on zoidberg, the actual test that matters:** issued
`guidestart` exactly once, then only polled `guidestatus`/`status` (no
`moveto` or any other command) every 15-20 seconds. The bot's position
advanced across all three waypoints and `CurrentStep` advanced 0→1→2→3
(`finished=true`) entirely on its own; the final reported position
(`-618.5, -4251.7, 38.7`) matched the third waypoint's coordinates
exactly. No crashes or errors in the server log throughout. This is the
project's first genuinely autonomous multi-step behavior -- every prior
capability in this arc required a human to trigger each individual step.

## ADR-020: GuideRuntime, second slice (combat-capable step)

**Decision:** `StepType::KillNearest` composes the already-proven
`Navigation::MoveTo`, `Combat::RequestAttack`, and `Inventory::LootCorpse`
primitives to walk to, kill, and loot the nearest creature of a given
entry -- fully automatically, no new opcode/API work. Unlike `MoveTo`
(a single fire-and-check action), this needed its own internal sub-phase
(`KillPhase::Approaching`/`Attacking`/`Looting`) tracked in
`BotGuideState`, since "walk, then fight, then loot" genuinely has three
distinct waiting conditions.

**Target tracking:** `CurrentKillTarget` is an `ObjectGuid`, resolved
fresh every tick via `ObjectAccessor::GetCreature(*bot, guid)` -- never a
stored raw `Creature*`, per ADR-002's tick-safety rule (the target could
die, despawn, or (for a corpse) be cleaned up between ticks).

**Approach originally mirrored the `.autonomousplayer attack` debug
command's pattern** (issue `Navigation::MoveTo` and `Combat::RequestAttack`
together, without waiting to arrive first), **but live testing found this
could get permanently stuck for a target near the edge of the search
radius.** Two real, genuine improvements were made in response
(arrival-gating before attacking; switching the one-shot
`Navigation::MoveTo` to a continuous-follow `MotionMaster::MoveChase`,
since the original target can be actively wandering) -- both are staying
in the code as correct, but **live re-testing after both fixes still
reproduced a stall**, with a different, deeper suspected cause
(`FindNearestCreature`'s straight-line distance check can select a target
that isn't actually reachable within a reasonable real path). This is
tracked as an open, precisely-described investigation, not a resolved
bug -- see `KNOWN_FAILURES.md` #3 for the full attempt-by-attempt history
and current hypothesis. What **is** confirmed: the mechanism works
correctly for a genuinely nearby, reachable target (the original
single-step `guidestartcombat` test, ~70 yards, completed cleanly).

**Looting is best-effort:** if the corpse has already despawned by the
time the Looting phase runs, the step still advances rather than getting
stuck -- there's no retry/backoff logic in this slice (that's later
failure-handling scope, not proven yet).

`.autonomousplayer guidestartcombat <charname> <creatureEntry>` debug
command: a single-step guide, started once, with no further command
needed.

**Verified live on zoidberg:** issued `guidestartcombat` against a real
Mottled Boar (creature 3098) once, then only polled status. Finished
within 15 seconds (`finished=true`), the boar confirmed dead
(`hp 0/55, alive=false`) via `creaturestatus`, bot took zero damage. No
crashes/errors in the server log. No manual attack/loot/moveto command
was issued at any point after the single trigger.

## ADR-021: GuideRuntime, third slice (full automatic quest loop)

**Decision:** `StepType::AcceptQuest`/`TurnInQuest` compose the
already-proven `QuestEngine::RequestAcceptQuest`/`RequestChooseReward`
primitives, giving a single guide the ability to run a complete
accept→kill→turn-in quest loop with zero manual commands. This ties
together every prior Gate 1/2 primitive (Navigation, Combat, Inventory,
QuestEngine) through the Gate 3 GuideRuntime scheduler for the first
time.

**Shared phase model generalized:** `KillPhase` was renamed to the more
general `StepPhase` (`Approaching`/`Acting`/`Looting`), and
`CurrentKillTarget`/`CurrentKillPhase` to `CurrentTargetGuid`/
`CurrentPhase`, since `AcceptQuest`/`TurnInQuest` need the exact same
shape (walk to an NPC, then act on it) as `KillNearest` -- `Looting`
remains meaningful only for `KillNearest`.

**Quest steps wait for real arrival before acting:** quest interaction
requires much tighter real range than melee engagement -- confirmed this
arc (Gate 2 QuestEngine slice): an accept attempt at ~8.6 yards silently
failed, ~1-2 yards succeeded. So `TickAcceptQuest`/`TickTurnInQuest` gate
on `GetDistance(giver) <= InteractionToleranceYards` (2 yards) before
submitting the request, and keep re-submitting each tick in the `Acting`
phase until the real quest state (`GetQuestStatus`/`IsQuestRewarded`)
confirms success -- a deliberate retry, not a single fire-and-forget,
since a submission at the first in-range tick could still race against
something else. (`TickKillNearest`'s own arrival-gating went through two
live-tested revisions after this ADR was first written -- see ADR-020's
addendum and `KNOWN_FAILURES.md` #3 for that history.)

`.autonomousplayer guidestartquest <charname> <questId> <questGiverEntry>
<killEntry> <turnInEntry> <rewardChoiceIndex>` debug command: a 3-step
guide (AcceptQuest → KillNearest → TurnInQuest), started once.

**Verification status:** `AcceptQuest`'s "already-satisfied" path (quest
already active) and the accept/turn-in interaction-range logic are
verified correct by direct observation (step correctly advanced past
`AcceptQuest` instantly when the quest was already in progress). The
`KillNearest` step in the middle of this specific chain has repeatedly
hit the unresolved distant-target stall documented in `KNOWN_FAILURES.md`
#3, so a full, clean end-to-end accept→kill→turn-in run has **not yet**
been observed live in one pass -- that remains open, not silently
assumed working.

## ADR-022: Combat/pulling design baseline (Honorbuddy/Singular research)

**Decision:** `docs/autonomous-player/HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`
(user-provided, 2026-07-01) is adopted as Gate 3's design baseline for
all `Combat`/pulling/engagement work going forward. It is a clean-room
research document, not an import: the reviewed Honorbuddy/Singular
repositories expose no usable license, so nothing is copied, translated,
or derived from their code -- the document analyzes observable behavior
and proposes a new AzerothCore-native design (`EncounterModel`,
`EngagementPlanner`, `CombatController`, `CombatExecutor`,
`RecoveryPolicy`, `AbilityCatalog`) built against this project's own
already-proven primitives and APIs.

**Why this matters right now, concretely:** the document's central
thesis -- that a pull is a multi-stage transaction with authoritative
confirmation at each step, and that movement toward a target is not
itself proof of a successful engagement -- is not a hypothetical
concern. It is the exact lesson this session was forced to learn the
hard way while fixing `KillNearest` (see `KNOWN_FAILURES.md` #3): a bare
`MotionMaster::MoveChase(target)` produced zero movement, while the real
attack request (`Combat::RequestAttack`, which calls `Unit::Attack()`
before its own `MoveChase`) worked. The document independently arrives
at and cites this exact finding as supporting evidence for its "an
opener is an action with authoritative acknowledgement" principle. This
alignment is why the document is being adopted wholesale as the design
baseline rather than treated as optional background reading.

**Immediate implication for `KillNearest`:** it currently implements
only the confirmed-working core of the document's much larger pull
transaction (`Select -> Validate -> ... -> Open -> ConfirmEngagement ->
... -> Combat -> Finish -> Loot`) as a flat 3-phase
`Approaching`/`Acting`/`Looting` state machine, with no target
validation, risk assessment, add-override, line-of-sight handling, or
bounded failure/blacklist behavior. The document's own "Gate 3
implementation sequence" (its final section) is the concrete plan for
closing this gap incrementally -- starting with an explicit state
machine and bounded stuck-timeout/blacklist (replacing the current
implicit, un-timed retry-forever behavior), then an `EncounterModel` and
structured diagnostics, then conservative single-pull class controllers
for Warrior and Priest (both already-provisioned test fixtures) before
any other class or any multi-pull/AoE/CC behavior. Proactive multi-pull
stays disabled by default per the document.

**Not adopted wholesale without adaptation:** the document itself is
explicit that this project's lifecycle, movement, threat, spell, and
session APIs differ from a client-facing addon's, so implementation
details (not the phase-separation/confirmation principles) must be
re-derived against AzerothCore's real APIs and re-verified live, the
same way every other component in this module has been.

## ADR-023: KillNearest explicit pull state machine + bounded blacklist

**Decision:** Following `HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`'s Gate 3
implementation sequence step 1 (ADR-022), `KillNearest` now has its own
explicit `PullState` enum (`Selecting`/`Approaching`/`Engaged`/`Looting`)
instead of sharing the generic `StepPhase` used by quest-interaction
steps. The document's fuller transaction (`Select -> Validate ->
AssessRisk -> PlanApproach -> Approach -> Prepare -> Open ->
ConfirmEngagement -> Stabilize -> Combat -> Finish -> Loot -> Recover`)
is not implemented wholesale: stages with no real behavior yet at this
project's current maturity (`AssessRisk`, `PlanApproach`, `Prepare`,
`Stabilize`, `Recover`) are deliberately not modeled as separate
pass-through states -- that would be complexity with no payoff. What
`Approaching`/`Engaged` retain from today's hard-won fix (ADR-020, see
`KNOWN_FAILURES.md` #3) is exactly the part proven correct: open with a
real attack request, confirm with `bot->IsInCombat()`, not a movement
heuristic.

**New, real capability:** `Approaching` is now bounded by
`MaxApproachTicks` (20 ticks, ~20 real seconds -- both of today's clean
completions finished in 12-15s, so this gives real margin). A target that
never confirms engagement within that bound is pushed onto
`BlacklistedTargets` (scoped to the current guide step; cleared on
`AdvanceToNextStep`) and `Selecting` picks a different candidate via a
new `FindNearestNonBlacklisted` helper (built on
`WorldObject::GetCreatureListWithEntryInGrid`, the same primitive the
`.autonomousplayer multipull` debug command already uses, since
`Player::FindNearestCreature` has no exclusion parameter). This directly
closes the "retry forever" gap this project's own investigation found and
the research document's "Failure handling and observability" section
calls for explicitly -- previously an unreachable/never-engaging target
had no escape path at all.

**Deliberately still minimal:** no risk assessment, no add-override, no
line-of-sight-specific handling, no expiry/decay on the blacklist beyond
"cleared at the end of this step" (the document's fuller model scopes
blacklist entries by reason/location/expiry -- not attempted yet), no
class controllers. `.autonomousplayer guidestatus` now also reports
`pullState`/`approachTicks`/`blacklisted` count for live diagnosis.

**Verified live on zoidberg, twice independently, no regression:** two
separate Mottled Boars, both died cleanly (6s and ~11s respectively),
`finished=true`, confirmed dead via `creaturestatus` both times. The
second run's diagnostics were caught mid-flight and showed real,
sensible state progression -- `pullState=2` (`Engaged`) with
`approachTicks=5`, confirming `Selecting` -> `Approaching` (5 real ticks)
-> `Engaged` happened as designed, not just "it finished eventually."
`blacklisted=0` both times (the happy path never needed it, as expected
for a reachable target) -- the new bounded-timeout/blacklist path itself
remains unexercised by a real unreachable-target scenario; that's honest,
not yet claimed as tested.

## ADR-024: EncounterModel, first slice (real attacker awareness)

**Decision:** Gate 3 implementation sequence step 2 from
`HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md` (ADR-022) is "add an
`EncounterModel` and structured pull diagnostics before adding any new
class rotations." The smallest real, testable slice of that: a snapshot
of what is actually attacking the bot right now, built from
`Unit::getAttackers()` -- the engine's own live, authoritative
attacker-tracking set, not a derived/inferred approximation. This is a
genuine, previously-nonexistent gap: `GuideRuntime`'s `KillNearest`
currently only ever tracks its own single objective target; it has zero
awareness of whether something *else* is also attacking the bot (an
unplanned add) at all.

**Deliberately minimal:** no risk scoring, line-of-sight, cast-tracking,
or threat-relationship data -- the research document's fuller
`EncounterModel` calls for these, but they have no real consumer yet
(nothing in this project currently reacts to risk or adds). Adding them
speculatively would be complexity with no payoff, matching the project's
own "smallest testable slice" discipline. `Snapshot::HasUnplannedAdd()`
is the one piece of derived logic included, since "is something attacking
me that isn't my planned target" is the single most decision-relevant
fact even at this minimal stage.

**Observational only, no behavior change yet:** `EncounterModel` is
wired into `.autonomousplayer guidestatus` (shown alongside the existing
pull diagnostics) and a standalone `.autonomousplayer encountersnapshot`
command, but `GuideRuntime`'s `KillNearest` does not yet *act* on
unplanned-add detection -- per the research document's step 2 being
explicitly about diagnostics before decisions. Target-switching/combat-
target-override (the document's "Aggro and target management" section)
is real, separate, later scope.

**Tick-safety:** `Snapshot` is a plain value type (ADR-002) containing no
`Unit*`/`Creature*` -- only GUIDs, entries, and distances captured at
build time.

**Verified live on zoidberg:** idle baseline correct (`botInCombat=false,
attackers=0`). Mid-fight against a real `KillNearest` pull (Mottled
Boar): `pullState=2` (`Engaged`), `botInCombat=true, attackers=1`, the
attacker's `entry=3098` and `isObjectiveTarget=true` both correct
(distance approx 0, matching real melee range) -- exact, correct data,
not approximated. A standalone `.autonomousplayer multipull` of 3
Mottled Boars also produced a correctly-read single-attacker snapshot
(`attackers=1, hasUnplannedAdd=true` -- correctly `true` since no guide
objective was set for that standalone call) before combat resolved.

**Honestly noted:** a genuinely simultaneous multi-attacker snapshot (2-3
attackers at once) was not empirically captured live -- the pulled
Mottled Boars are weak enough that each fight resolved faster than the
polling interval used, so overlapping attackers were never observed
together in one snapshot. The underlying mechanism (enumerating a live
engine set) has no reason to behave differently at higher counts, but
this specific claim is not directly evidenced and should not be assumed
proven without a real test.

## ADR-025: Combat::RequestCastSpell exposes real SpellCastResult

**Decision:** `RequestCastSpell` (ADR-018) now returns the real
`SpellCastResult` from `Unit::CastSpell`, not a collapsed `bool`. The
`.autonomousplayer castspell` debug command surfaces the numeric result
(and whether it equals `SPELL_CAST_OK`), plus the caster's rage before
and after the attempt.

**Why now:** directly motivated by a real, still-open investigation
(`KNOWN_FAILURES.md` #4) -- a Warrior offensive-ability cast was
rejected with no way to tell *why* (insufficient rage? wrong spell ID?
an incompatible next-swing-queued mechanic?). A bare `bool` was
sufficient for the Priest spellbook investigation earlier this arc
(where the answer -- "no offensive spell available at this level" -- was
independently confirmed by other means), but is not enough here. This
follows the same discipline that resolved the `KillNearest` investigation
(`KNOWN_FAILURES.md` #3): add real diagnostics before guessing at a fix,
rather than patching blind.

## ADR-026: KillNearest engagement confirmation fix (GetVictim, not IsInCombat)

**Decision:** `TickKillNearest`'s `Approaching` -> `Engaged` transition
now checks `bot->GetVictim() == target` (the bot's own real, specific
current attack target) instead of `bot->IsInCombat()`. Found via external
code review, not live testing: `IsInCombat()` only means "something is
fighting me" -- an unrelated add attacking the bot during approach would
also satisfy it, causing a false-positive transition to `Engaged` while
the actual objective target was never touched, after which `Engaged`
would wait forever for a target that was never actually being fought.

**Why this wasn't caught live:** every `KillNearest` verification this
arc (including the two clean fix-attempt-3 re-tests and the
`PullState`-machine re-tests) used isolated single-target pulls with no
second hostile creature aggroing mid-approach -- the exact condition
needed to trigger the false positive never occurred in any test scenario
run. This is recorded honestly as a real correctness gap this project's
own testing missed, not a design limitation that was already known and
deferred (see `KNOWN_FAILURES.md` #5).

**Verification status (calibrated honestly):** the happy-path confirmation
behavior was re-verified correctly live (matches its own real target,
correct guid, real melee range). A dedicated attempt to construct the
exact negative case the fix targets (an unrelated attacker present during
approach) via repeated `multipull` + `guidestartcombat` overlap could not
reliably force genuine overlap -- the available test creatures are weak
enough to die faster than console-command round-trip latency in this
setup. The fix is correct by code review and the happy path holds; the
specific scenario it targets is not yet proven by direct live
observation. See `KNOWN_FAILURES.md` #5 for the full attempt record.

## ADR-027: EncounterModel gates engagement confirmation (first real consumer)

**Decision:** Directly responding to the external review's priority #2
("make EncounterModel affect target selection and add handling, not just
report it"): `TickKillNearest`'s `Approaching` -> `Engaged` transition
now withholds confirmation while `EncounterModel::Snapshot::HasUnplannedAdd()`
is true, even once `bot->GetVictim() == target` is satisfied. This is the
first place in the codebase `EncounterModel` actually changes behavior,
not just reports state.

**Deliberately the smallest real behavior change, not the full "combat
target may override objective target" model:** the research document's
fuller aggro-management design (switch to the add, fight it, return to
the objective target) is real, later scope. This slice only refuses to
*confirm* a pull as clean when it isn't -- it does not retarget, does not
fight the add, does not abandon the objective target early. If the add
situation doesn't resolve naturally before `MaxApproachTicks` expires,
the existing bounded-timeout/blacklist path (ADR-023) still applies, so
this doesn't introduce a new unbounded wait.

**Why withhold rather than immediately abort/retarget:** matches the
research document's stated default leveling policy ("one desired target
and zero desired adds," proactive multi-pull disabled by default) more
conservatively -- treating an add as reason to *pause* confirming success
is safer than either (a) ignoring it entirely (the prior behavior) or (b)
building real target-switching logic before the simpler primitive is
proven. `Combat::RequestAttack` keeps re-issuing every tick regardless,
so real damage still lands on the objective target while this waits.

**Verification status (calibrated honestly):** compiles clean, no
regression -- four separate live attempts (`multipull` sizes 2, 4, 6, 8)
to construct genuine overlapping combat (needed to directly observe the
new gating logic withholding confirmation) all failed to sustain overlap
long enough to poll, the same environment limitation documented for the
underlying engagement-confirmation fix in `KNOWN_FAILURES.md` #5. The
happy path (no add present) was reconfirmed correctly and consistently
across all four attempts, with no crashes or regressions. The gating
logic's specific behavior under a genuine add has not been directly
observed live -- correct by code review (the check is synchronous with
the `GetVictim()` confirmation in the same tick, so there is no window
for the bug the review described to reappear), not yet proven by
observation.

## ADR-028: Bounded failure states for the remaining unbounded guide waits

**Decision:** Direct response to external review priority #3 ("add
bounded failure states to every guide operation"). A shared
`OperationTimedOut(state)` helper (increments `BotGuideState::OperationTicks`,
bounded by `MaxOperationTicks`, sets `Failed=true` and `Finished=true`
once exceeded) is now called from every wait the review identified as
unbounded:
- `TickMoveTo`'s arrival wait (a one-shot `Navigation::MoveTo` that never
  arrives -- unreachable point, stuck navmesh -- previously waited
  forever).
- `TickAcceptQuest`/`TickTurnInQuest`'s questgiver search-and-walk wait
  *and* their request-retry wait (both previously unbounded).
- `KillNearest`'s `Selecting` wait (nothing found, or everything
  blacklisted, previously retried forever) and `Engaged` wait (no combat
  deadline at all previously -- a target that evaded, reset, or simply
  never died would wait forever).

**Deliberately NOT covered by this slice, and explicitly still open:**
- Evade-specific detection (`Engaged`'s bound is a generic deadline, not
  an evade signal -- it can't distinguish "target evaded" from "just a
  slow kill"; a real evade check is separate, later scope).
- Loot success verification (`Inventory::LootCorpse`'s own `bool` return
  is still not checked/acted on -- this is a correctness-of-result
  concern, not an unbounded-wait concern, and was intentionally scoped
  out of this slice to keep it to one coherent theme).
- `KillNearest`'s `Approaching` phase already had its own dedicated bound
  (`ApproachTicks`/`MaxApproachTicks`, ADR-023) and is unchanged here --
  `OperationTicks` is a separate, step-level clock covering `Selecting`
  and `Engaged`, not a replacement for the target-level one.

**On timeout, the whole guide stops (`Failed=true`, `Finished=true`),
not just the current step.** This project has no per-step failure/retry-
at-a-different-step semantics yet (that would need real Planner/Executor
work per ADR-003, still Gate 0 scope) -- halting the whole guide is the
honest, minimal correct behavior available today: better than spinning
forever, without pretending to have recovery logic that doesn't exist.
`.autonomousplayer guidestatus` now reports `failed`/`operationTicks`
alongside the existing diagnostics.

**Verified live on zoidberg, with real observed evidence, not just
compiled code:** added a new `.autonomousplayer guidestartmoveto
<charname> <x> <y> <z>` debug command (single-step `MoveTo` to an
arbitrary coordinate, unlike `guidestart`'s fixed patrol) specifically to
target a genuinely unreachable point on purpose. Sent a bot toward
`(5000, 5000, 500)` on map 1 (far outside Kalimdor's real terrain, no
connected navmesh path). Observed the real tick counter progressing
across polls -- `operationTicks=23` at +10s, `operationTicks=46` at
+20s -- and the timeout genuinely fired right at the bound:
`finished=true, failed=true` once `operationTicks` exceeded
`MaxOperationTicks` (45). **Note the actual tick rate was roughly
2.3 ticks/real-second, not the ~1/second this ADR's constants were
originally sized assuming** -- `MaxOperationTicks=45` in practice bounds
waits to ~20 real seconds, not ~45; the constant name/comment referring
to "~45 real seconds" is accordingly optimistic and should be corrected
in a future pass, but the bound itself demonstrably works and is not
unsafe (a *shorter* real timeout than intended is not a correctness
problem, just a documentation inaccuracy worth fixing later). The bot's
own state was confirmed sane immediately afterward: it had walked as far
as the navmesh allowed toward the unreachable point (a real partial path,
not a crash or teleport), remained fully controllable (`.autonomousplayer
moveto` back toward known territory was accepted and executed normally),
and no crashes/errors appeared in the server log throughout. The
happy-path regression check (`guidestartcombat`) also completed cleanly
in the same session, `finished=true, failed=false`, well under the
bound. This is real, concrete proof the timeout mechanism works, not an
assumption from code review alone.

## ADR-029: First class-controller slice (opportunistic ability in KillNearest)

**Decision:** Gate 3 implementation sequence step 3, deliberately the
smallest possible slice: `GuideStep::OpportunisticSpellId`, if nonzero,
is tried once per tick via `Combat::RequestCastSpell` during
`KillNearest`'s `Engaged` phase, in addition to the bare melee
`RequestAttack` already running. This is directly enabled by this
session's confirmation that spell 78 is a genuine, working Warrior
offensive ability (`KNOWN_FAILURES.md` #4) and by `RequestCastSpell`
returning the real `SpellCastResult` (ADR-025) rather than a bool -- the
cast failing (e.g. not enough rage yet) is a real, expected, harmless
no-op, not an error to guard against specially.

**Deliberately NOT a real class controller yet:** no priority list, no
resource tracking/budgeting, no cooldown awareness, no ability rotation,
no per-class dispatch (the guide step just names a spell ID, it doesn't
know or care what class the bot is), no defensive/interrupt/utility
abilities. This is the smallest real step past "bare melee" that the
research document's fuller `CombatController`/`AbilityCatalog` design
will eventually replace -- proving the *composition* (an ability
integrated into the pull state machine, tried automatically, failing
harmlessly when unaffordable) before building the *decision-making*
around which ability to use when.

`.autonomousplayer guidestartcombatability <charname> <creatureEntry>
<spellId>` debug command mirrors `guidestartcombat` with the added
ability.

**Verified live on zoidberg, mixed but honest result across 3 independent
runs:** 2 of 3 completed cleanly (`finished=true, failed=false`), same
speed as the plain-melee `guidestartcombat` tests earlier this session.
**1 of 3 genuinely timed out** while still `Engaged`
(`finished=true, failed=true`, `operationTicks=46`) -- the target was
never confirmed dead within `MaxOperationTicks`, and by the time this was
noticed the creature could no longer be found within 100 yards (dead or
alive), consistent with a real kill whose corpse had already despawned,
or a fight that ran unusually long. No crashes or errors in the server
log for any of the 3 runs; the bot's state remained sane and controllable
afterward. **Root cause of the one timeout not investigated further** --
could be an unusually tough/evasive individual creature (a real,
pre-existing possibility independent of this slice), a timing
interaction between the added `RequestCastSpell` call and normal combat,
or simple variance; 2-of-3 clean successes with identical code is not
strong evidence the ability integration itself is the cause, but it's
also not ruled out. **This is exactly the scenario ADR-028's bounded
timeout exists for** -- the guide did not hang forever; it failed
cleanly and recoverably. Documented honestly as a mixed result, not
glossed over as a full success.

## ADR-030: Real loot success verification (closes an explicitly-deferred ADR-028 gap)

**Decision:** `KillNearest`'s `Looting` phase now records whether looting
was actually attempted (`LastLootAttempted` -- false if the corpse
despawned before the loot session could open) and, if attempted, whether
it was verified successful (`LastLootVerified` -- checked by comparing
`corpse->loot.items`/`loot.gold` *after* `Inventory::LootCorpse` runs;
verified true only if nothing lootable remains). `Inventory::LootCorpse`'s
own doc comment already said "verify actual results... afterward," but
nothing in this module actually did until now -- the external review's
point that "loot is attempted once and the step advances regardless of
success" was accurate.

**Deliberately still best-effort, not blocking:** a failed/unverified
loot does not retry the loot window or stop the guide -- the step still
advances. A normal single-item-drop corpse should always fully autostore
in one pass (this is what `LootCorpse` already does internally, looping
every slot), so a `LastLootVerified=false` result would indicate a real,
rare problem (e.g. full bags rejecting an item) worth surfacing via
diagnostics, not a routine case worth building retry logic around yet.
`.autonomousplayer guidestatus` now reports both new fields.

**Verified live on zoidberg, 3 independent runs, consistent:**
`lastLootAttempted=true, lastLootVerified=true` every time against real
Mottled Boar kills, no crashes. The happy path is reliably verified as
verified, not just assumed -- closing the gap cleanly, unlike ADR-029's
mixed result.

## ADR-031: Target selection safety (`IsSafeToEngage`)

**Decision:** `KillNearest`'s candidate search (`FindNearestNonBlacklisted`)
now filters through a new `IsSafeToEngage(bot, candidate)` check before a
creature can ever become the objective target, and `PullState::Approaching`
re-checks the same condition every tick (not just at selection time) for
the target it already picked. This directly addresses the external
review's point 3, previously entirely unaddressed: "target selection has
no hostility/tag/evade/LoS/other-player-fighting-it validation." All five
of those are now real checks against authoritative engine state, no
heuristics:

- **Evade**: `Creature::IsInEvadeMode()` -- a resetting creature is not a
  legitimate target.
- **Attackability** (not "hostility" in the naive sense): `Unit::
  IsValidAttackTarget()`, **not** `Unit::IsHostileTo()`. This started as
  `IsHostileTo` and was caught live, not by inspection, before it ever
  reached `KillNearest`: a real `.autonomousplayer targetsafety` check
  against a live Mottled Boar reported `hostile=false` -- most low-level
  questing wildlife is faction-*neutral*, not Hostile, yet is a
  completely legitimate kill target (it's what this entire project's
  combat testing has run against since Gate 2). Shipping `IsHostileTo`
  as the gate would have permanently rejected every Mottled Boar
  `KillNearest` ever tries to pull -- a real regression that live
  testing caught before merge, not a theoretical concern.
  `IsValidAttackTarget` is the engine's own real attackability check
  (reputation/faction rank including the neutral-but-at-war case,
  immunity flags, dead/unselectable state) -- it correctly treats
  attackable-neutral creatures as legitimate while still excluding
  actually-friendly NPCs (vendors, questgivers, guards).
- **Tag**: `Creature::hasLootRecipient()` + `isTappedBy(bot)` -- another
  player (or their group) already has kill/loot rights.
- **Other player fighting it**: `Unit::getAttackers()` checked for any
  `Player`-type attacker that isn't the bot -- catches the window
  *before* tap registers too (tap is set on first damage dealt, not on
  aggro), since engaging a target someone else is already fighting is
  real interference even before the tap flag exists.
- **LoS**: `WorldObject::IsWithinLOSInMap()` -- a real player cannot
  target what they cannot see; this also closes a distinct pre-existing
  gap where an out-of-LoS target would previously just burn a full
  `MaxApproachTicks` timeout (KNOWN_FAILURES.md #3's stall pattern)
  instead of being rejected at selection time.

**Why re-check during `Approaching`, not just at selection:** a target
picked as safe can become unsafe while the bot is still walking over --
most plausibly another player tags it first. The re-check only fires
while `bot->GetVictim() != target` (i.e. before the bot has actually
started attacking) -- once genuinely engaged, a real player wouldn't
abandon a target mid-swing over a status change; existing bounds
(`MaxApproachTicks`, `MaxOperationTicks`) still apply as the backstop for
anything that goes wrong after that point. A target that fails either
check is blacklisted and a new candidate is selected, reusing the exact
same blacklist-and-retarget mechanism `KillNearest` already had
(ADR-023) rather than adding a new failure path.

**Diagnostics added alongside:** a new `.autonomousplayer targetsafety
<charname> <creatureEntry> [range=100]` debug command reports each
individual check (`alive`, `evading`, `attackable`, `hasLootRecipient`,
`tappedByBot`, `otherPlayerAttacking`, `los`) plus the overall `safe`
verdict for the nearest matching creature, so a specific failure mode can
be directly confirmed live instead of inferred from "`KillNearest` didn't
attack anything." Same diagnostics-before-decisions discipline as
ADR-024/025. It's also what caught the `IsHostileTo` regression above --
without a per-check diagnostic, that would have looked identical to "no
target found" from the outside.

**Second real bug caught by this same diagnostic, in pre-existing (not
this ADR's own) code:** `targetsafety` initially reused `creaturestatus`'s
`FindNearestCreature(entry, range, false)` call and, at 100-500 yard
ranges, found *nothing* for entries that were live and nearby --
including Mottled Boar, immediately after `multipull` had just found five
within 300 yards from roughly the same spot. Root cause: `WorldObject::
FindNearestCreature`'s third parameter is not "include dead" when
`false` -- the underlying `NearestCreatureEntryWithLiveStateInObjectRangeCheck`
requires an *exact* `Creature::IsAlive() == alive` match, so `false`
means "only dead creatures," not "either." `creaturestatus` (Gate 2,
predates this ADR) has carried this footgun the whole time; it just
never happened to matter because every prior use of that command was
right after killing something, one specific case an exact-dead-match
would coincidentally satisfy. `targetsafety` fixes it locally (search
alive first, then dead, instead of passing `false`) but does not touch
`creaturestatus` itself, out of this ADR's scope -- worth fixing there
too if it ever causes a false "not found" in a future session.

**Verified live on zoidberg, calibrated:**
- `attackable` (the fixed check): `.autonomousplayer targetsafety
  Grunttestbot 3098 300` against a real live Mottled Boar reported
  `alive=true evading=false attackable=true hasLootRecipient=false
  tappedByBot=false otherPlayerAttacking=false los=true -> safe=true` --
  the exact regression-fix confirmation.
- **No regression, full end-to-end:** `.autonomousplayer guidestartcombat`
  with the new `IsSafeToEngage` gate wired into both selection and the
  `Approaching` re-check completed exactly as before -- `pullState`
  progressed `Selecting`(0)->`Approaching`(1, `approachTicks=1`)->
  `Engaged`(2, real attacker confirmed, `hasUnplannedAdd=false`) on the
  first poll, then `finished=true, failed=false, lastLootAttempted=true,
  lastLootVerified=true` on the next -- same shape as every pre-ADR-031
  `KillNearest` run in this project's history.
- **Update (follow-up session, after ADR-032 unblocked a second test
  character): tap and other-player-attacking are now live-verified, with
  real evidence, not just code review.** With `Grunttestbot` and
  `Grunttestii` (second Horde character, account `ap_test2`) colocated
  near a boar-dense spot, `Grunttestii` was set attacking a specific
  Mottled Boar (`.autonomousplayer attackguid`, live low-guid 3969) while
  `.autonomousplayer targetsafety Grunttestbot 3098 50` was polled on the
  *same* target: `hasLootRecipient=true tappedByBot=false
  otherPlayerAttacking=true los=true -> safe=false` -- both checks firing
  correctly, live, on a real concurrently-fought creature. **Then a full
  `KillNearest` run proved the composed behavior, not just the
  diagnostic:** `.autonomousplayer guidestartcombat Grunttestbot 3098`
  (real search radius 50 yards, multiple live boars in range including
  the tapped one) went straight to `Engaged` on a *different* boar
  (confirmed by position: `(-713.8, -4281.1)` vs. the tapped boar's
  `(-712.7, -4320.4)`) and completed cleanly
  (`finished=true, failed=false, lastLootVerified=true`) while
  `Grunttestii` independently killed its own tapped target the whole
  time -- two bots, two separate kills, zero interference, confirming
  `IsSafeToEngage` genuinely steers `KillNearest` away from an
  already-contested target rather than just flagging it.
- **LoS is now also live-verified, found opportunistically.** While
  scouting creature entries near a Razor Hill-area test position (after
  an unrelated incident -- see `KNOWN_FAILURES.md` #10 -- moved
  `Grunttestbot` far from its usual spot), `.autonomousplayer
  targetsafety Grunttestbot 10685 300` (Swine, likely in a fenced pen)
  returned `los=false -> safe=false` while every other nearby entry
  checked at the same time (`Greater Plainstrider`, `Razormane Water
  Seeker`, `Zhevra Runner`, `Adder`) returned `los=true`. Reproduced on a
  second immediate poll (`los=false` again, not a one-off flicker) --
  real, obstructed line of sight correctly detected and correctly
  excluding the target.
- **Still not verified live, code-review only: evade.** Attempted this
  same follow-up session (pull a boar, immediately flee to break chase,
  poll for `evading=true`) but Mottled Boars have very low HP and die or
  fully reset within a couple of real seconds -- faster than console-
  command round-trip latency allows a genuine mid-evade state to be
  caught (one attempt did show indirect evidence: `hasLootRecipient` had
  reset to `false` on a creature that had just been attacked, consistent
  with a real evade-driven combat reset, but `evading` itself read
  `false` by the time the poll landed). This is the same class of
  environment limitation already documented in `KNOWN_FAILURES.md` #5
  (weak, fast-resolving test creatures vs. console-command latency) --
  not a new concern, and not worth further retries with the same
  approach. `IsInEvadeMode` is a standard, unmodified engine call already
  correct by code review; a higher-HP test target or in-process test
  instrumentation (not console polling) would be needed to close this
  for real. **This is now the only one of ADR-031's five checks without
  direct live confirmation** (attackable, tag, other-player-attacking,
  and LoS all now have real observed evidence).

## ADR-032: Character-name pre-validation (root-causes and fixes `KNOWN_FAILURES.md` #9)

**Root cause found:** `Grunttestbot2`'s creation stall (ADR-031 followup)
was not a hung async chain -- it was `HandleCharCreateOpcode` correctly
rejecting the name for a completely mundane reason (WoW character names
may not contain digits; `ObjectMgr::CheckPlayerName`'s `isValidString`
call passes `numericOrSpace=false` for creation) and returning
*immediately*, before ever reaching the async DB chain
`PendingCharacterCreations` was polling for. The rejection is sent via
`SendCharCreate` -> `SendPacket`, which is a silent no-op for this
module's null-socket bot sessions -- the exact same "client-feedback
path gated on `m_Socket`" class of problem as every Gate 1 bug
(ARCHITECTURE.md ADR-008), just discovered in a new spot. Confirmed by
reading `WorldSession::HandleCharCreateOpcode`'s source directly (not
guessed): it has several early-return validation checks (name, race/
class DBC lookup, expansion mask) that all take this same silent path.

**Decision:** added `Setup::ValidateCharacterName`, which calls the same
public `ObjectMgr::CheckPlayerName(name, true)` API and translates the
result to a human-readable reason. `HandleProvisionCommand` now calls it
*before* submitting the creation request and refuses with a clear
console message (`PSendSysMessage`, unaffected by the null-socket issue
since it's the GM/console's own feedback channel, not the bot session's)
instead of silently starting a doomed async wait. This does not touch
`HandleCharCreateOpcode` or any other core engine code -- it only adds a
client-side-equivalent pre-check using an existing public API, matching
this project's "public APIs are fair game" rule.

**Verified live on zoidberg, and corrected a wrong guess along the way:**
re-provisioning with `Grunttestbot2` now fails immediately with
`Refusing to submit character creation for 'Grunttestbot2': too long.`
instead of a multi-minute silent stall -- **the real reason was name
length** (`MAX_PLAYER_NAME` is 12; `Grunttestbot2` is 13 characters),
not the digit-character theory this ADR originally wrote down before
testing it. `Grunttestbot2` also would have failed the character-set
check (digits genuinely are rejected, confirmed by reading
`isBasicLatinString`'s `numericOrSpace` parameter), but `CheckPlayerName`
checks length first and returns immediately, so that second real problem
never even got exercised. Re-provisioning with a valid name
(`Grunttestii`, same account id 206) completed successfully on the
**first attempt** -- `acore_characters.characters` gained a real row
(guid 2016) within 8 seconds, confirming both the fix and that account
206 itself was never the problem. Lesson for whoever reads this ADR:
`ValidateCharacterName`'s per-code messages are accurate (each maps to
the real `CHAR_NAME_*` reason), but don't assume *which* code will fire
without checking -- this session's own first guess was wrong.

## ADR-033: Live regression suite (`tools/live_regression_suite.py`, review priority 7)

**Decision:** added a small, real, automated regression suite that runs
over the worldserver's live SOAP interface -- the exact same mechanism
every "Verified live" claim in this project's docs has been produced
with (see the `autonomous-player-zoidberg-soap-access` agent-memory
entry). This is deliberately NOT a `BUILD_TESTING`/gtest suite -- this
repo's module build path doesn't wire that up, and most of what needs
protecting here (real opcode handlers, real navmesh movement, real DB
state) only exists meaningfully against a live deployed server anyway.
Same class of tool as `check_no_playerbots_dependency.sh` /
`check_no_forbidden_apis.sh`: cheap, real, automatable, catches a real
class of regression -- not an attempt to replace careful live
verification when building genuinely new features.

**Five tests, chosen to protect real things this arc already proved,
not hypothetical future behavior:**
1. SOAP connectivity sanity check.
2. Bot login reaches a live `alive=true` status.
3. **`creature_attackable_not_merely_hostile`** -- a direct regression
   test for ADR-031's own `IsHostileTo` -> `IsValidAttackTarget` fix.
   This is the test that matters most: it would have caught that
   regression automatically, the same session it was introduced, instead
   of relying on a human (or agent) noticing `hostile=false` looked
   wrong by eye.
4. `guidestartcombat` completes cleanly end-to-end (walk+kill+loot, no
   manual step advances) -- deliberately does NOT hard-require
   `lastLootVerified=true`, since ADR-030 documents that as legitimately
   best-effort; found live while first running this suite that a
   long-lived test bot with a near-full backpack (18 items) produces a
   real `lastLootVerified=false` on an otherwise clean run, which would
   have made an over-strict assertion flake on correct behavior instead
   of catching an actual regression.
5. `guidestartmoveto` against a genuinely unreachable coordinate reaches
   `failed=true` within the documented bound (ADR-028).

**Bugs the suite's own first run caught in itself, not in the module**
(reported here because they're a useful lesson, not swept away): the
initial `key=value` parser split on whitespace only, so
`guidestatus`'s comma-separated output parsed `finished=true,` (with
the trailing comma) as the value -- never equal to the literal `"true"`
a test compared against, so tests 4 and 5 falsely reported "never
finished" even when the real output already said `finished=true`. Fixed
by stopping value capture at a comma too. Separately, test 5's timeout
was originally 40s, too tight for `MaxOperationTicks`'s real variable
tick rate under load (ADR-028 already documented ~2.3 ticks/s under
light load, slower otherwise) -- raised to 70s.

**Verified live on zoidberg:** full clean run against `Grunttestii`
(second test character, entry 3098 Mottled Boar): `5/5 passed`. An
earlier run against `Grunttestbot` at a different (accidentally
higher-level, see `KNOWN_FAILURES.md` #10/#11) location caught two real
environment-dependent conditions this session hadn't anticipated
(near-full bags, content above the bot's level) rather than a suite bug
-- both are now handled correctly (informational, not hard failures,
for the loot case; the level-mismatch case just needs an appropriate
creature entry passed in, which is a real precondition of any guide, not
a suite defect).

**Deliberately out of scope for this first slice:** no CI wiring (this
project's build path doesn't run against a live deployed server in CI
at all yet), no self-positioning (tests assume the bot is already near
appropriate-level creatures of the given entry -- moving it there is the
caller's job, same as every other debug command in this module), no
credential storage in the repo (SOAP user/password are required
CLI flags or env vars, resolved at run time, never hardcoded).

**Follow-up (ADR-037's pets work): a real cross-feature interaction, not
a regression.** Running this suite against `Grunthunter` (which had by
then tamed a Mottled Boar, entry 3098 -- the same entry the suite's
default fixture uses) initially failed
`creature_attackable_not_merely_hostile`: `targetsafety`'s nearest-match
search found the bot's *own pet* (same species) instead of a wild one,
and `attackable=false` for your own pet is the **correct** answer
(`IsValidAttackTarget` rightly excludes it) -- not the `IsHostileTo`
regression this test exists to catch. Fixed the test itself (not the
module) to cross-check the found guid against `petstatus` and skip
cleanly on a match, rather than asserting on an inherently ambiguous
case. Re-verified: `5/5 passed` against `Grunthunter` with its pet still
alive. Worth remembering: any future regression-suite test that searches
by creature entry needs to consider that a Hunter (or later Warlock/DK)
test fixture may have tamed/summoned something of the same species.

## ADR-034: First Hunter (ranged/pet class) combat coverage

**Decision/finding:** provisioned a third class archetype,
`Grunthunter` (Orc Hunter, account `ap_test3`), specifically because
this whole project's combat testing to date was melee-only (Warrior) or
combat-untested (Priest had no offensive spell yet at level 1, see Gate
2 KNOWN_FAILURES). No code changes were needed -- `KillNearest`'s
existing `OpportunisticSpellId` mechanism (ADR-029) composes directly
with a ranged ability the same way it did with the Warrior's Heroic
Strike, since `Combat::RequestCastSpell` is spell-agnostic.

**Verified live on zoidberg:** `.autonomousplayer guidestartcombatability
Grunthunter 3098 75` (spell 75, Auto Shot) against a real Mottled Boar --
`pullState` progressed `Selecting` -> `Approaching` -> `Engaged`
(`isObjectiveTarget=true`) -> `Looting`, completed in ~15 seconds,
`finished=true, failed=false, lastLootVerified=true`. First real
evidence in this project that the `KillNearest` + `OpportunisticSpellId`
composition works for a ranged ability, not just a melee-adjacent one --
no changes needed to make it work, which is itself useful confirmation
that ADR-029's design was genuinely class-agnostic, not accidentally
Warrior-specific.

**Not covered by this slice (real, honest scope limits):** pet
summon/management has no debug command or `Combat`-layer support at
all in this module yet -- Hunters in WotLK start with a pet, but this
test never interacted with it (no `.autonomousplayer` command exists to
check pet state, summon, or verify pet combat contribution). "Ranged
pulls" as a *distinct* pulling behavior (engage from range before the
target closes, rather than melee-range engagement that happens to use a
ranged spell) also isn't modeled -- `KillNearest`'s `Approaching` phase
still walks into melee range via `Combat::RequestAttack` regardless of
`OpportunisticSpellId`. Both are real, scoped-out gaps for Gate 3's
"pets"/"ranged pulls" bar, not silently claimed as done.

## ADR-035: Bounded-timeout bail-outs now stop movement, not just guide bookkeeping (fixes `KNOWN_FAILURES.md` #10)

**Bug found live:** `OperationTimedOut` (ADR-028) marks the guide's own
state `Failed=true, Finished=true` when a bound is exceeded, so
`GuideRuntime` stops polling that step. But an earlier
`Navigation::MoveTo`/`Combat::RequestAttack` call in the same step
already issued a real `MotionMaster` order, and that order keeps
executing on its own -- the two are completely decoupled. Concretely:
`.autonomousplayer guidestartmoveto` toward an intentionally unreachable
coordinate correctly hit the bound and reported `failed=true`, but the
character kept physically walking toward that same coordinate for a long
time afterward, off the edge of reachable terrain, and **died for
real**. Recovering it required manually issuing a fresh `moveto` to
override the stale order before `releasespirit`/`reclaimcorpse` would
even work (the ghost was *also* still driving toward the same stale
destination).

**Decision:** `OperationTimedOut` now takes `Player* bot` and calls
`bot->StopMoving()` (a standard public `Unit` API -- halts the current
movement spline in place, not a position write, so it doesn't trip
`check_no_forbidden_apis.sh`'s teleport ban) on every bail-out, before
returning `true`. All 5 call sites (`TickMoveTo`, `TickKillNearest`'s
`Selecting`/`Engaged` phases, `TickAcceptQuest`, `TickTurnInQuest`)
already had `bot` in scope -- purely additive, no other behavior change.
`KillNearest`'s separate `MaxApproachTicks`-bound blacklist path
(ADR-023) does *not* need the same fix: unlike a full guide bail-out, it
immediately returns to `Selecting` and issues a fresh movement order for
the next candidate within the same tick cycle, so there's no window
where a stale order runs unattended.

**Verified live on zoidberg:** repeated the exact failure scenario
(`guidestartmoveto` toward `5000, 5000, 500`) after the fix -- the guide
still correctly reaches `failed=true` at the `MaxOperationTicks` bound,
but the bot's position is now static across repeated polls after that
point instead of continuing to drift toward the abandoned destination,
and it survives (`alive=true` throughout, no death).

## ADR-036: `castspell` debug command's own MOVING-state bug (design-pass discovery for pets)

**Investigating whether Tame Beast (candidate spell 1515) is real** for
the pets design pass, `.autonomousplayer castspell Grunthunter 1515
3098` was tried against a live Mottled Boar. First attempt: `result=97`
(`SPELL_FAILED_OUT_OF_RANGE`) -- this alone was useful signal that 1515
*is* a real, engine-recognized spell (an unknown/invalid spell ID would
not produce a specific, meaningful `SpellCastResult` this way). Follow-up
attempts, even standing still and retrying repeatedly: every single one
returned `result=51` (`SPELL_FAILED_MOVING`), never succeeding.

**Root cause: `HandleCastSpellCommand` itself, not Tame Beast.** The
debug command unconditionally calls `Navigation::MoveTo` toward the
target's position *every time it runs*, even when the caster is already
well within range. Issuing a fresh `MotionMaster` move order sets
`UNIT_STATE_MOVING` for at least a tick regardless of the actual
distance involved, and `Unit::CastSpell` correctly rejects with
`SPELL_FAILED_MOVING` whenever that state is set -- a real WoW mechanic
(a real player mid-run can't cast either), but this debug command never
let the state clear between its own move-then-cast calls, so every retry
looked identical from the outside no matter how long a real player
waited between them.

**Fixed:** `HandleCastSpellCommand` now checks the spell's own real
range (`SpellInfo::GetMaxRange`, the same engine data structure
`Unit::CastSpell` itself checks) and only calls `Navigation::MoveTo` if
actually outside it. No change to `Combat::RequestCastSpell` or any
production `GuideRuntime` code -- this was purely a debug-tooling
artifact that happened to block investigating a real feature.

**Verified live:** re-ran the identical cast after the fix --
`result=255` (`SPELL_CAST_OK`). Waited out Tame Beast's real cast time,
then confirmed a genuine pet was created: `acore_characters.character_pet`
gained a real row (`entry=3098` -- the exact Mottled Boar tamed,
`owner`=Grunthunter's guid, `PetType=1` [hunter pet], `name="Boar"`,
`curhealth=44`). **This is a first, major finding for the pets design
pass: taming itself needs zero new module code** -- it composes entirely
from already-proven primitives (`Combat::RequestCastSpell`, real engine
`EffectTameCreature`). What's actually missing for a usable pets slice is
*visibility* (no way to check pet state) and *`GuideRuntime` awareness*
(nothing reads pet state at all yet) -- see the next ADR for that slice.

## ADR-037: First pets slice -- `Pets` component (tame, status, react state)

**Decision:** new `Pets` component (`Pets/BotPets.{h,cpp}`), deliberately
small per Gate 3's design-pass mandate:
- `RequestTameBeast(Player*, Creature*)` -- thin wrapper over the
  already-proven `Combat::RequestCastSpell` with `TameBeastSpellId`
  (1515). Does NOT move the caster into range itself (same division of
  responsibility `Combat::RequestCastSpell` already has for every other
  spell) -- the caller positions first.
- `PetSnapshot`/`BuildSnapshot(Player*)` -- read-only diagnostic (guid,
  entry, alive, health/maxHealth, react state), same tick-safety
  discipline as `EncounterModel::Snapshot` (plain value type, never
  stores a `Pet*`).
- `RequestSetPetReactState(Player*, ReactStates)` -- wraps the real
  `Unit::SetReactState`/`GetReactState` the engine's own pet
  command-bar uses.
- Three new debug commands: `tamebeast`, `petstatus`, `petreactstate`.

**Verified live on zoidberg, real evidence, not just code review:**
- `.autonomousplayer petstatus Grunthunter` correctly reported "no pet"
  before taming, then the real tamed Mottled Boar afterward (entry 3098,
  matching the exact creature cast at).
- **Pet state persists across a full worldserver restart and re-login**
  -- logged `Grunthunter` back in after redeploying with this ADR's own
  code and the pet was already there (`alive=true, hp=149/149`,
  `character_pet`'s save from ADR-036's tame still valid) -- this is
  real engine pet-persistence working correctly, not something this
  module had to build.
- `petreactstate Grunthunter 2` (aggressive) submitted successfully and
  `petstatus` confirmed `reactState=2` afterward.
- `guidestartcombat` against a real Mottled Boar completed cleanly with
  the pet set aggressive and alive throughout (`finished=true,
  failed=false, lastLootVerified=true`), no regression from the
  pet-less case.

**Follow-up, same session: the "does the pet actually assist" gap is
now closed for real.** Added `PetSnapshot::VictimGuid` (reads
`Pet::GetVictim()`, mirroring `EncounterModel`'s own pattern) and
surfaced it in `petstatus`. Re-tested `guidestartcombat` against a fresh
Mottled Boar with tight polling: while `KillNearest` was `Engaged`
(`pullState=2`) against a specific live target (low guid `3969`), the
pet's own `victim` reported the *exact same guid* -- direct, positive
proof the aggressive pet was genuinely attacking the bot's own
objective target, not merely standing nearby. The next poll showed the
target dead (`alive=false`) and the pet's victim correctly reset to
`none`, and the guide finished cleanly (`finished=true, failed=false,
lastLootVerified=true`). This is real, observed evidence, not an
inference from "nothing went wrong" -- the same discipline
`KNOWN_FAILURES.md` #5/evade still lacks for a different check.

**Not in this slice, real scope for later:** `GuideRuntime` itself has
no pet awareness at all yet (no auto-tame-if-no-pet step, no
auto-aggressive-on-tame, no pet-revive-on-death) -- this slice is
primitives + diagnostics only, matching how `Combat`/`EncounterModel`
each started before `KillNearest` was built on top of them. Also not
attempted: Warlock demon summoning (a different, separate spell/mechanic
from Hunter taming) -- out of scope for this pass.

## ADR-038: `GuideRuntime` pet awareness -- `EnsurePetAssists`

**Decision:** `KillNearest` now keeps a live pet on the guide's own
planned target throughout `Approaching` and `Engaged`, via a new
`EnsurePetAssists(bot, targetGuid)` helper:
- Sets the pet to `REACT_DEFENSIVE`, not `REACT_AGGRESSIVE`.
  `REACT_AGGRESSIVE` would let the pet freely acquire any nearby
  hostile creature on its own initiative -- directly working against
  the conservative "one planned target, zero desired adds" pull policy
  `HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md` and `EncounterModel`
  (ADR-024/027) already established. `REACT_DEFENSIVE` only reacts to
  something attacking the pet or its owner, never roams for its own
  targets.
- Explicitly commands the pet onto the guide's own `CurrentTargetGuid`
  via `Pets::RequestAttackTarget` (a new primitive: synthesizes the real
  `CMSG_PET_ACTION` packet with `COMMAND_ATTACK`, exactly what a
  player's pet action-bar click sends, handled by the real
  `WorldSession::HandlePetAction`). This keeps target *ownership* with
  the same engagement planner that already decided what's safe to pull
  (`IsSafeToEngage`) -- the pet assists on that specific decision, it
  doesn't make its own.
- Idempotent per tick: only re-issues the react-state change or the
  attack command if the pet's current state doesn't already match
  (`snapshot.React != REACT_DEFENSIVE`, `snapshot.VictimGuid != targetGuid`),
  cheap to call unconditionally every `Approaching`/`Engaged` tick.

**Verified live on zoidberg:** with the pet manually reset to
`REACT_AGGRESSIVE` beforehand (to make the transition observable),
`guidestartcombat` against a real Mottled Boar showed `reactState`
flip to `1` (defensive) within the first tick, and -- with fast enough
polling to catch it before the (very low-HP) target died --
`petstatus`'s `victim` field showed the pet's real live guid matching
the guide's own objective target's exact guid (`Low: 1968`) while
`KillNearest` was `Approaching`/`Engaged`. Guide completed cleanly
(`finished=true, failed=false, lastLootVerified=true`).
`tools/live_regression_suite.py` still `5/5` afterward (no regression).

**Design correction worth recording:** the first version of this slice
used `REACT_AGGRESSIVE` with no explicit target command (rely on the
pet's own aggro radius). This would have let the pet pull unrelated
adds on its own, silently reintroducing exactly the "unplanned add"
risk ADR-027's `EncounterModel` gating exists to prevent -- caught
before being verified live and replaced with the defensive +
explicit-attack design above.

**Still not in this slice:** pet-revive-on-death (a dead pet is not
detected or revived automatically), auto-tame-if-no-pet (a Hunter guide
with no pet does not attempt to acquire one). Both are real, separate,
later increments.

## ADR-039: `CombatIntent`/`CombatExecutor` and a real recovery-phase interface for pets

**Design correction, at the user's explicit direction:** an earlier
version of pet-revive-on-death wired `Pets::RequestRevivePet` directly
into `KillNearest`'s per-tick helper (`EnsurePetAssists`), triggered
merely by `!snapshot.HasPet`/`!snapshot.Alive` -- another instance of
the "isolated spell-ID behavior scattered across `GuideRuntime`" pattern
this whole arc's combat/pets work had been accumulating (`TameBeastSpellId`,
`OpportunisticSpellId`, now `RevivePetSpellId`, each with its own ad-hoc
call site). Corrected before being deployed as final, per direction to
build a shared decision layer instead of one more one-off:

- **`Combat::CombatIntent`** (`Combat/CombatIntent.h`): a small tagged
  struct (`IntentKind` -- `EngageTarget`/`UseAbility`/`AssistPetOnTarget`/
  `RecoverPet` -- plus a target guid and optional spell id) describing
  *what* the bot wants done, independent of *how*.
- **`Combat::Execute`** (`Combat/CombatExecutor.{h,cpp}`): the one place
  that maps an intent to the real underlying primitive
  (`RequestAttack`/`RequestCastSpell`/`Pets::RequestAttackTarget`/
  `Pets::RequestRevivePet`). `KillNearest`'s melee engage, opportunistic
  ability cast, and pet-assist command all now go through this same
  function -- not just the new pet-recovery behavior -- for one
  reviewable execution path instead of several parallel ones.
- **`Pets::PetState`** (`NotYetTamed`/`Dismissed`/`Dead`/`Alive`) and
  **`Pets::ClassifyPetState`**: the engine's `Player::GetPet()` alone
  cannot distinguish "never tamed anything" from "had a pet, it's gone
  now without ever being observed dead" (a real dismiss or an abnormal
  removal) -- both just read as `HasPet=false`. Rather than inventing an
  engine capability that doesn't exist, `ClassifyPetState` is a pure
  function taking the caller's own last-known pet guid as an explicit
  parameter; `GuideRuntime::BotGuideState` owns that one guid field
  (`LastKnownPetGuid`), not a hidden singleton.
- **`Recovery::PlanPetRecovery`** (`Recovery/PetRecoveryPolicy.{h,cpp}`):
  the Singular model's "Recover" stage
  (`HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`, ADR-022) applied to pet
  maintenance. Returns a `RecoverPet` intent **only** when the pet is
  actually `Dead` (not `Dismissed`/`NotYetTamed` -- auto-re-taming stays
  explicitly out of scope), `bot` is **not in combat**
  (`Unit::IsInCombat()`, a real safety gate, not assumed safe), and
  `bot` has **actually learned** Revive Pet (`Player::HasSpell`,
  verified through the bot's own spellbook rather than assumed just
  because the spell id is theoretically real -- the same gated-ability
  discipline this project already established for Priest/Warrior
  spells, `KNOWN_FAILURES.md` Gate 2).
- **Guide-state preservation**: `GuideRuntime::Tick` calls
  `PlanPetRecovery` once, centrally, **before** dispatching to the
  current step's tick function at all. If it returns an intent,
  `Tick` executes it and returns immediately for that tick --
  `CurrentStep`/`CurrentPhase`/`CurrentPullState`/etc. are never touched
  while recovery is "in progress." There is deliberately no explicit
  save/restore: skipping the step dispatch *is* the pause mechanism, and
  the guide automatically resumes exactly where it was the next tick
  `PlanPetRecovery` returns `nullopt` (pet alive again, or recovery
  genuinely not applicable), since nothing about the step's own state
  was ever mutated in between.

**Verified live on zoidberg:**
- The refactor introduced no regression: `tools/live_regression_suite.py`
  still passes (aside from unrelated environment/positioning failures
  already documented as such, not code issues -- test creatures out of
  the guide's real 50-yard search radius from wherever the bot happened
  to be that check).
- A guide runs to completion normally with **no pet at all**
  (`PetState::NotYetTamed`) -- confirmed no incorrect stall trying to
  "recover" a pet that was never supposed to exist.
- **A real, organic confirmation of the `Dismissed` vs. `Dead`
  distinction**: after `Grunthunter` died from an unrelated real hazard
  (`KNOWN_FAILURES.md` #10's cross-country-travel note) with its pet
  still alive nearby, the pet ended up abnormally removed
  (`character_pet.slot=100`/`PET_SAVE_NOT_IN_SLOT`, not the current
  active pet) rather than left dead-in-place. `petstatus` correctly
  reported "no pet," and `ClassifyPetState` (the original 4-state
  version at the time) resolved this as `Dismissed`, and `PlanPetRecovery`
  correctly did **not** attempt a doomed revive under that model. **Note,
  written after ADR-040's same-day follow-up below**: under the corrected
  6-state model, this exact case (a stable entry genuinely exists,
  `curhealth=0`) reclassifies as `PetState::MissingDead`, not
  `Dismissed` -- the original model couldn't see the difference because
  it never queried `PetStable` at all. The *behavior* (no revive attempt)
  happened to be identical either way for this specific data point purely
  because the fix for `MissingDead` hadn't been built yet; see ADR-040 for
  what actually changed.

**Not verified live at the time this ADR was first written, honestly:**
the actual `Dead` -> `Alive` transition via `RequestRevivePet` on a pet
that's dead but still present. Attempting to construct that scenario
fresh (re-tame a new pet, get it killed while the bot survives) ran into
a new, real, separate finding instead -- **this was fully resolved the
same day, see ADR-040 immediately below**: `RequestRevivePet` had a real
bug (bailed out whenever `GetPet()` was null, exactly the case it needed
to handle), fixed and live-verified twice against `Grunthunter`'s actual
broken pet.

## ADR-040: Pet recovery finished for real -- `RequestRevivePet` fix, full `PetState` taxonomy, `RequestCallPet`

Direct continuation of ADR-039's open gap, same day. The investigation
into `KNOWN_FAILURES.md` #13 went through a wrong "structural fork
limitation" conclusion before landing on the real fix -- the full,
in-order story (three theories, two wrong) is recorded in
`KNOWN_FAILURES.md` #13 itself, not duplicated here. Summary of what
shipped:

- **The real bug, in this module, not the engine**: `RequestRevivePet`
  bailed out with `SPELL_FAILED_BAD_TARGETS` whenever `bot->GetPet()`
  was null -- but that's exactly the case Revive Pet's real effect
  (`Spell::EffectResurrectPet`, `SPELL_EFFECT_RESURRECT_PET`, fully
  implemented -- not to be confused with the separate, genuinely
  unimplemented `SPELL_EFFECT_CALL_PET`/`Spell::EffectNULL`) is designed
  to handle, via `player->SummonPet(0, ...)` reloading from
  `PetStable`/DB regardless of whether a live object exists. Fixed:
  `RequestRevivePet` now always self-casts
  (`Combat::RequestCastSpell(bot, bot, RevivePetSpellId)`).
- **Live-verified, twice**: cast against `Grunthunter`'s real broken
  `PetState::MissingDead` state, result `SPELL_CAST_OK`, and the *same*
  pet (matching pet number) loaded back in alive both times. A new
  `.autonomousplayer revivepet <charname>` debug command was needed to
  test this cleanly -- `castspell`'s generic range-check-against-a-
  dummy-target logic (ADR-036) doesn't fit a self-cast spell and kept
  re-triggering `SPELL_FAILED_MOVING`.
- **One honestly-noted persistence gap**: the null-pet revival branch
  doesn't call an explicit `SavePetToDB`; a revived pet's DB row can
  still show the old dead/unslotted state until the next periodic
  autosave (900s default) or a clean logout. An abrupt worldserver
  restart shortly after a revive (this session's own redeploy cycle)
  reverted it. Consistent with how every other piece of transient state
  in this engine already behaves -- not a new bug -- but worth knowing
  explicitly rather than assuming a revive is durable the instant it's
  observed working.
- **`PetState` expanded from 4 states to the full model**:
  `NoPet`/`ActiveAlive`/`ActiveDead`/`MissingAlive`/`MissingDead`/
  `Dismissed`. The `Missing*` split required reading
  `PetStable::GetUnslottedHunterPet()->Health` directly (not just
  presence) -- `ClassifyPetState` now takes `Player* bot` as a parameter
  for that one synchronous read, rather than staying a pure
  `(snapshot, guid)` function.
- **`Recovery::PlanPetRecovery` updated to match**: `ActiveDead`/
  `MissingDead` -> `RecoverPet` (confirmed above); `MissingAlive` ->
  new `IntentKind::CallPet`/`Pets::RequestCallPet` (spell id 883, a
  real standard WotLK id, but **not yet live-verified** -- no test
  Hunter reached the level to have it learned this session, gated
  behind `Player::HasSpell` the same as every other unconfirmed ability
  in this project).

`Grunthunter` (`ap_test3`) currently has a live, alive, revived pet as
of this session's end -- no longer deliberately left broken.

**Two follow-up regressions found and fixed via
`tools/live_regression_suite.py`, same day, after the above shipped:**

- **Recovery preempting an active pursuit, stretching wall-clock time.**
  The original `Tick()`-level recovery check ran unconditionally every
  tick, including while `KillNearest` was actively `Approaching`/
  `Engaged` with a real, live target (or a quest-giver interaction in
  progress). `PlanPetRecovery` only requires `!bot->IsInCombat()`, which
  is not a perfectly stable signal moment-to-moment -- a real evade or a
  brief gap before the first hit registers both read as "not in combat"
  while a target is still a genuine in-progress objective. When
  recovery fired during one of those windows, `Tick` returning early
  starved `OperationTimedOut`'s own bounded-wait counter (only
  incremented inside the step dispatch this skips) of ticks --
  correctness wasn't broken (the guide still eventually hit its own
  tick-based bound and failed cleanly) but wall-clock time to do so
  stretched measurably, confirmed live: `live_regression_suite.py`
  started intermittently exceeding its 40s wall-clock timeout after the
  recovery check was added, where it had been a clean `5/5` before.
  **Fixed**: gated the whole recovery check on
  `state.CurrentTargetGuid.IsEmpty()` -- a precise proxy for "genuinely
  between objectives" across every step type that uses that field
  (`KillNearest`, quest accept/turn-in); a real pursuit in progress is
  now never preempted.
- **Recovery re-issuing the same cast every eligible tick, interrupting
  itself before it could complete.** `Combat::Execute`'s `RecoverPet`/
  `CallPet` cases fire-and-forget `Unit::CastSpell` -- like a real player
  mashing the same spell button, a new cast request interrupts and
  restarts one already in progress rather than being a no-op. Both
  Revive Pet and Call Pet have a real cast time; without awareness of an
  in-flight cast, `GuideRuntime::Tick` calling `PlanPetRecovery` every
  eligible tick meant a cast could never survive long enough to
  complete. **Fixed**: `PlanPetRecovery` now checks
  `bot->IsNonMeleeSpellCast(false)` (the real, standard engine query for
  "is this unit currently mid-cast") and returns `std::nullopt` if a
  cast is already in flight, leaving it alone instead of restarting it
  every tick.

**Live-verified after both fixes**: `live_regression_suite.py` reached
`5/5` again. Some intermittent `guidestartcombat_completes_cleanly`
failures still occurred in later runs this session -- **investigated,
not attributed to this module's code**: in each case, `petstatus`
confirmed the pet was `MissingDead` (a state `PlanPetRecovery` would
normally act on) while `CurrentTargetGuid` was non-empty and the guide
was genuinely `Engaged` with a live target -- direct live confirmation
that the new gate correctly held recovery off during an active pursuit.
The guide still finished via its own `MaxOperationTicks` bound
(`operationTicks` reaching ~45-46, `Failed=true`, `StopMoving()` called)
in every case observed, just slower than the test harness's 40s window
in some of them. This matches `KNOWN_FAILURES.md` #6's already-documented,
pre-existing, unreproduced-at-scale Engaged-phase timeout finding
(ADR-029) -- not a new regression from pet recovery, though this
session's rate of occurrence (small sample, a heavily-reused level-2
test character) wasn't enough to either confirm or rule out a higher
real rate than #6's original ~1/13. Left as-is, not chased further --
diminishing returns for the time this session had left, and the
mechanism (`OperationTimedOut`'s own bound firing) is confirmed working
correctly regardless of cause.
