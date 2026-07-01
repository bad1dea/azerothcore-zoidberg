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
`.autonomousplayer gossiphello`/`gossiptrain` debug commands for live
verification.
