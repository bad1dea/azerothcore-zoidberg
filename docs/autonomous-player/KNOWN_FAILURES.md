# Known Failures

Reproducible failures, evidence, and classification. Append-only within a
gate; prune entries once a fix is verified and cite the fixing commit
instead of deleting silently.

## Gate 0

None — no runtime behavior existed to fail.

## Gate 1

All five bugs below were found via live testing on zoidberg (2026-06-30)
and are now fixed. Kept here (not deleted) because the debugging pattern
is reusable: several of these looked identical from the outside (silent
failure, no character/no login, no error log) despite having completely
different root causes, because so much of the normal client-feedback path
(`SendPacket`, `KickPlayer`'s log line) is gated on `m_Socket` being
non-null — always false for our sessions. Lesson: verify against the DB
directly, don't infer success/failure from log silence alone.

### 1. Playerbots-bot false positive in the login hook — FIXED (`5d5840f`'s predecessor)
`WorldSession::IsBot()` is not exclusive to this module; mod-playerbots
sets it on its entire random-bot pool, so an `IsBot()`-only login hook
registered hundreds of their bots. Fixed with account-name-prefix
ownership (`Setup::AccountPrefix`, `Setup::IsAutonomousPlayerAccount`).

### 2. Account prefix longer than `MAX_ACCOUNT_STR` — FIXED
The original prefix (`"autonomous_player_"`, 19 chars) exceeded
`AccountMgr::MAX_ACCOUNT_STR` (17) by itself. Shortened to `"ap_"`.

### 3. Session doesn't survive past one world tick — FIXED (`5d5840f`)
`WorldSession::Update()` has an unconditional `if (!m_Socket) return
false;` after `ProcessQueryCallbacks()`; `WorldSessionMgr::UpdateSessions`
deletes any session whose `Update()` returns false. A `sock = nullptr`
session registered via `sWorldSessionMgr->AddSession` survived exactly one
tick, orphaning async DB work (character creation, login) before it could
complete. Root-caused by reading how mod-playerbots' own identically-
constructed bot sessions avoid this (they never register with
`WorldSessionMgr`, driving sessions from their own update loop instead —
read-only inspection, not a dependency). Fixed with new
`Lifecycle::BotSessionMgr`, this module's own from-scratch equivalent:
ticks tracked sessions via `Update(diff, MapSessionFilter)`, which skips
the null-socket eviction path while still draining
`ProcessQueryCallbacks()`.

### 4. Use-after-free in the creation-timeout path — FIXED (`3d49e7f`)
The first version of fix #3's `PendingCharacterCreations` deleted a
session on a polling timeout even though its async DB chain could still
be in flight (confirmed: a character row appeared in the DB *after* the
"timed out" log line — the completion callback ran against freed memory,
which happened not to crash, but that's luck). Fixed: the timeout path now
only stops polling and logs an error; it never deletes the session, since
completion can't be ruled out. A stuck session is a resource leak to
investigate, not something to guess-and-delete.

### 5. Login rejected by `IsLegitCharacterForAccount` — FIXED (`8ada2d4`)
`HandlePlayerLoginOpcode` requires the login GUID to already be in
`_legitCharacters`, a set populated only by the character-list-
*enumeration* flow (`HandleCharEnum`/`BuildEnumData`) a real client runs
before login. A headless bot that never enumerates always failed with
"Account can't login with that character." Fixed by bypassing
`HandlePlayerLoginOpcode` and calling the also-public
`WorldSession::HandlePlayerLoginFromDB` directly via our own resolved
`LoginQueryHolder` — the real login-finalization code, minus the
client-only gate ahead of it.

### 6. Case-sensitive account-prefix comparison — FIXED (`d83ea84`)
`AccountMgr::CreateAccount` uppercases every stored username, so
`ap_test1` is stored as `AP_TEST1`. `IsAutonomousPlayerAccount`'s
`starts_with("ap_")` against that stored uppercase name was always false.
This made a login that had *actually succeeded* core-side (confirmed via
`characters.online` being set to `1`, which only happens after
`AddPlayerToMap` succeeds) look identical to a stuck one, since the
`PLAYERHOOK_ON_LOGIN` ownership check silently rejected it. Fixed with a
case-insensitive prefix comparison.

**Final live verification:** `Grunttestbot` (Orc Warrior, level 1) online
at Valley of Trials (map 1, `-618.5, -4251.7`), registered, correct
perception snapshot. See `HANDOFF.md`.

## Gate 2

### 1. Combat stalls when the target moves out of melee range — FIXED (`bf10901`)
`Unit::Attack()` (called by the reused `HandleAttackSwingOpcode`) only
sets combat state — it does not add any follow/chase movement generator
for the attacker. A real human player's client keeps them in range via
their own movement input; a headless bot has none, so the fight silently
stalled the instant the target moved even slightly (confirmed live: bot
stuck at 66/70 hp, `combat=true`, frozen position, for 20+ seconds, no
further damage in either direction — a fled Mottled Boar). Fixed by
issuing `MotionMaster::MoveChase` on the target alongside the attack
request. Verified live after the fix: two clean kills, zero damage taken,
`combat` correctly returned to `false` both times.

### Non-bug: loot correctly respects quest-gating
Investigated (not assumed) why looting two Scorpid Workers produced no
items despite a 90%-chance loot-table entry (`Scorpid Worker Tail`, item
4862). That loot-table row has `QuestRequired = 1` — it only drops for a
player with an active quest needing it, and this session's bot had
already turned that quest in. Confirmed via
`SELECT * FROM creature_loot_template WHERE Entry = ...`. This is correct
game behavior faithfully reproduced by `Inventory::LootCorpse`, not a
defect in it.

### Non-bug: release-spirit leaves the ghost in place with no graveyard nearby
After a genuine death (triggered live via deliberate multi-pulling, see
`HANDOFF.md`), `Recovery::RequestReleaseSpirit` did not move the ghost
anywhere — it stayed exactly at the death coordinates instead of
appearing at a graveyard. Investigated via code review:
`Player::RepopAtGraveyard()`'s own comment states "if no grave found,
stay at the current location," and `sGraveyard->GetClosestGraveyard(...)`
returned null because the death happened in open wilderness far from any
registered graveyard zone. This is a faithful reproduction of real core
behavior (the same thing would happen to a human player who died in the
same remote spot), not a defect in `Recovery::RequestReleaseSpirit`.

### Tooling wart (not a module bug): `attackguid`'s low-GUID guessing is unreliable
The `.autonomousplayer attackguid` debug command takes a creature's
low-GUID and reconstructs a full `ObjectGuid` to resolve it, on the
assumption that the static `creature` spawn table's `guid` column matches
the live in-game low-GUID. It does not, in this fork (confirmed live:
every guess using DB `guid` values returned "no creature found"). Not
fixed -- `attackguid` is left in place as-is since it's a pure test
convenience, not module logic, but `.autonomousplayer multipull` (built
on `WorldObject::GetCreatureListWithEntryInGrid`, which returns live
`Creature*` pointers directly, no GUID reconstruction) is the reliable
tool for targeting specific/multiple creatures in tests going forward.

### 2. Most-vexing-parse compile error in Economy — FIXED (`c7caab9`)
`WorldPackets::Item::BuyItem packet(WorldPacket(CMSG_BUY_ITEM));` was
parsed by the compiler as a function declaration (a function named
`packet` taking a `WorldPacket` parameter, returning
`WorldPackets::Item::BuyItem`), not object construction -- the classic
C++ "most vexing parse." Caught immediately by the zoidberg compile check
(`packet.VendorGuid = ...` failed to compile against a function type).
Fixed with brace-initialization.

### Non-bug: vendor buy correctly rejects insufficient funds
`Economy::BuyItem`/`RepairAll` both submitted cleanly against a real
vendor+repair NPC (Huklah, creature 3160) with the bot's real 0-copper
balance. Neither had any visible effect (no money spent, no item
received) -- confirmed this is `Player::BuyItemFromVendorSlot`'s real
insufficient-funds check running correctly, not a defect. No
crashes/errors in the server log either way.

### Non-bug: trainer spell-learning correctly rejects insufficient funds
Same class of finding as vendor buy: `Growth::FindLearnableTrainerSpell`
found spell 6673 (Battle Shout, real level-1 Warrior trainer spell) as
eligible via `Trainer::CanTeachSpell`, but `RequestLearnSpell` had no
effect (bot still didn't have the spell afterward) because it costs 10
copper and the bot has 0. Confirmed via the live world DB
(`trainer_spell.MoneyCost`). Worth remembering: `CanTeachSpell` checks
race/class/level/skill/profession-point eligibility only, **not**
affordability -- money is checked separately inside `Trainer::TeachSpell`
itself. Not a bug in either component.

### Non-bug: level-1 Human Priest has no offensive spell to cast
`Growth`'s new sibling test for `Combat::RequestCastSpell` (broader
race/class coverage slice): read the bot's full 42-entry starting
spellbook live via the new `.autonomousplayer spellbook` command, tried
casting a plausible early-Priest damage-spell candidate (spell 585)
against a live Diseased Young Wolf -- rejected (`accepted=false`, target
HP unchanged) both before and after confirming the bot had actually
arrived in range. Consistent with the real vanilla/WotLK Priest leveling
curve (no offensive spell until several levels past 1) rather than a
`Combat`/`Unit::CastSpell` defect. Fell back to melee (already-proven
`Combat::RequestAttack`) for this bot's kill test instead -- itself a
realistic choice a real level-1 Priest player would also make. A positive
"spell deals damage" test is deferred until a bot has an actual offensive
spell (later level or after training).

### Minor anomaly (not investigated, non-blocking): one-time transient provision failure
`.autonomousplayer provision ap_priest1 ...` failed on its very first
invocation this session with "Failed to create/find account", even
though the account (id 205) already existed in the DB and a manual
`SELECT id FROM account WHERE username='ap_priest1'` found it
immediately (case-insensitive collation confirmed working correctly).
Retrying the identical command right after succeeded. Happened right
after a fresh worldserver redeploy; `AccountMgr::GetId` is a synchronous
blocking `LoginDatabase.Query` with no retry, so a brief post-restart DB
connection hiccup is the leading suspicion, but this wasn't root-caused.
Not blocking -- this is a debug/test-provisioning path only, never
exercised by the (not-yet-existing) runtime bot loop. Worth a look if it
recurs.

## Gate 3

### No bugs found -- GuideRuntime first slice verified clean
`.autonomousplayer guidestart` (3-waypoint automatic patrol) worked
correctly on the first live attempt: `CurrentStep` advanced 0→1→2→3
(`finished=true`) with zero manual commands issued after `guidestart`,
final position exactly matched the last waypoint, no crashes/errors in
the server log. No bug to record -- noted here only because this was the
first time `BotLifecycleMgr::Update`'s per-bot dispatch call actually did
anything observable (it was pure bookkeeping before), so a wiring mistake
would have been a real risk worth calling out if one had been found.

### No bugs found -- GuideRuntime combat-capable step (KillNearest) verified clean
`.autonomousplayer guidestartcombat` against a real Mottled Boar (creature
3098) worked correctly on the first live attempt: the 3-phase sub-state
machine (Approaching → Attacking → Looting) ran to completion in under 15
seconds with zero manual commands after the single trigger, the boar was
confirmed dead (`hp 0/55`) via `creaturestatus`, bot took zero damage, no
crashes/errors in the server log. No bug to record.

### 3. KillNearest can get permanently stuck approaching a distant target — RESOLVED (fix attempt 3, verified live twice)
While testing the full quest-loop slice (`guidestartquest`, ADR-021),
`KillNearest`'s Approaching phase found a Mottled Boar and issued
`Navigation::MoveTo` + `Combat::RequestAttack` together immediately (the
same pattern as the already-proven `.autonomousplayer attack` debug
command). The bot walked toward the target's search-time position and
stopped there, `combat=false`, full health, permanently.

**Fix attempt 1 (insufficient on its own):** hypothesized
`Combat::RequestAttack` silently declined to engage from too far away;
changed the Approaching phase to wait for real arrival
(`MeleeEngageToleranceYards`, 5 yards) via a one-shot `Navigation::MoveTo`
before attacking. **Re-tested live: identical stall, same position.**

**Fix attempt 2 (a genuine, real improvement -- but still not sufficient
alone):** `Navigation::MoveTo`/`MotionMaster::MovePoint` is a *one-shot*
walk order that doesn't re-path if the destination moves, and Mottled
Boars have real wandering AI -- switched to
`bot->GetMotionMaster()->MoveChase(target)`, the same continuous-follow
generator `Combat::RequestAttack` itself already uses once attacking
(ADR-012). This is unambiguously more correct than a one-shot MoveTo for
a moving target, and is staying in the code. **But re-tested live a third
time and the bot still got stuck** -- at a *different* position this
time, again with no Mottled Boar findable within 100 yards afterward
(dead or alive), after being frozen in place for 60+ seconds.

**Investigation with real diagnostics (not guessing anymore):** added
live target-position/distance reporting to `.autonomousplayer
guidestatus` instead of inferring from bot-position snapshots, plus
reduced the guide commands' search radius from 100 to 50 yards as a
parallel mitigation attempt. Re-tested fix attempt 2 (bare `MoveChase`)
live with these diagnostics running: **the target resolved correctly on
every single poll, `alive=true`, and was visibly wandering (its reported
position changed between polls) -- but the bot's own position was
completely frozen across 35+ continuous, uninterrupted seconds with zero
manual commands in between.** This is conclusive, not inferred: issuing a
*bare* `MotionMaster::MoveChase(target)` with no preceding
`Combat::RequestAttack`/`Unit::Attack()` call produces **no bot movement
at all** in this context, regardless of target reachability. (An earlier
poll in this same investigation showed contaminated results from a
manually-issued `.autonomousplayer moveto` colliding with the guide's own
automatic movement order -- that data was discarded; the clean,
uninterrupted 35-second observation above is the one that matters.)

**Fix attempt 3 (the actual fix):** reverted `TickKillNearest`'s
Approaching phase to call `Combat::RequestAttack` immediately/repeatedly
on finding a target -- the *original* Gate 3 slice 2 pattern, which was
proven working in that slice's very first live test before any of these
three fix attempts existed. `RequestAttack`'s own internal `Unit::Attack()`
(called before its `MoveChase`) is apparently required for the chase to
actually produce movement -- a bare `MoveChase` alone, as fix attempt 2
used, does not. The phase now transitions to `Acting` on
`bot->IsInCombat()` becoming true (the real signal that engagement
succeeded) rather than a distance check. `MeleeEngageToleranceYards` (fix
attempt 1's constant) is now unused and was removed.

**Verified live, twice, independently, with a genuinely clean test
protocol this time** (relocate *before* starting the guide, zero manual
commands issued *during* guide execution -- earlier attempts in this
investigation were sometimes contaminated by a manual `moveto` colliding
with the guide's own movement order, which this protocol avoids): two
separate Mottled Boars, at two different locations, both died cleanly
within 12-15 seconds of `guidestartcombat`, `finished=true`, confirmed
dead via `creaturestatus` (`hp 0/42`, `alive=false`) both times, bot took
zero damage both times, no crashes/errors in the server log either time.
**This is now genuinely resolved**, not just theorized -- three prior
"fixed" claims in this same investigation were each disproven on
re-test, so this one earns the label specifically because it held up
under repeated, clean, adversarial re-verification.

**What was confirmed working throughout:** the original `.autonomousplayer
guidestartcombat` test (single-step `KillNearest`, no quest chain, no
prior movement) against a Mottled Boar found within ~70 yards completed
cleanly in ~15 seconds on its first attempt, before any of these three
fix attempts existed -- using exactly the pattern fix attempt 3 reverts
to. The bot's underlying movement system itself was independently
confirmed working throughout this investigation (a manual
`.autonomousplayer moveto` to a fresh location succeeded cleanly), ruling
out any general navmesh/motion-system failure -- the issue was
specifically about how `KillNearest`'s Approaching phase invoked
movement, not the movement system itself.

---

This file will also start recording `PATH_FAILED` / `TRANSPORT_FAILED` /
`TARGET_UNAVAILABLE` / `OBJECTIVE_NO_PROGRESS` / `QUEST_TURNIN_FAILED` /
`COMBAT_TOO_HARD` / `UNSAFE_PACK_DENSITY` / `CORPSE_RECOVERY_FAILED` /
`INVENTORY_BLOCKED` / `TRAINING_BLOCKED` / `GUIDE_INVALID` /
`SCRIPTED_OBJECTIVE_UNSUPPORTED` / `STATE_MIGRATION_FAILED` class failures
once there is a Planner/Executor loop and a working online bot that can
produce them.
