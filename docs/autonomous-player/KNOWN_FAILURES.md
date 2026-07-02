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

### 4. Warrior offensive-ability cast (spell 78) rejected -- RESOLVED (root cause confirmed: insufficient rage)
Attempted, as part of Gate 3 implementation sequence step 3 (conservative
single-pull Warrior controller), to cast spell 78 (a candidate for
"Heroic Strike" based on ID matching well-known retail WoW numbering --
**not confirmed by name against this fork's actual spell data**, since
`spell_dbc` was already found incomplete/uninformative for this purpose
during the earlier Priest spellbook investigation this arc) against a
live Mottled Boar via `.autonomousplayer castspell`. Rejected on the
first two attempts (tried very early in each fight, before real rage had
accumulated).

**Resolved with real diagnostics** (`Combat::RequestCastSpell` now
returns the actual `SpellCastResult` instead of a bool, plus caster rage
before/after -- see ADR-025): re-attempted after engaging combat for a
few real seconds first (letting rage build from auto-attack), and the
cast was accepted -- `result=255` (`SPELL_CAST_OK` -- confirmed this is
the real, correct sentinel value for success in this codebase's
`SharedDefines.h`, not a garbage/error read), with real rage available
(179, then 234 on a second independent attempt). The target creature was
confirmed dead shortly after each successful cast (checked via
`creaturestatus`), consistent with a real "queued for next melee swing"
mechanic (`Unit::CastSpell` accepting the request immediately, the actual
extra damage landing on the following auto-attack swing, not
instantaneously at cast time -- HP was unchanged in the same command's
immediate before/after read, then confirmed dead moments later).

**Conclusion: hypothesis 1 (insufficient rage) was the real cause, both
prior times.** Spell 78 is a genuine, working Warrior offensive ability
beyond bare melee, castable via the existing `Combat::RequestCastSpell`
primitive with no changes needed to that function -- the earlier
rejections were a legitimate real-game-mechanic outcome (not enough rage
yet), exactly the same class of honest, correct-behavior finding as the
Economy/Growth insufficient-funds cases earlier in this arc, not a
defect. Verified twice, independently, both times ending in a real kill.

### 5. KillNearest's engagement confirmation checked the wrong condition — FIXED
Found via external code review (not live testing -- this project's own
test scenarios never happened to trigger it), not this session's own
investigation: `TickKillNearest`'s `Approaching` phase confirmed
engagement with `bot->IsInCombat()`, which only means "something is
fighting me" -- an unrelated add attacking the bot during approach would
also satisfy it, causing a false-positive transition to `Engaged` while
the actual objective target was never touched. The `Engaged` phase would
then wait for that untouched, still-alive target to die -- forever, since
nothing was actually fighting it. **Fixed:** confirmation now checks
`bot->GetVictim() == target` -- the bot's own real, specific current
attack target (set by `Unit::Attack()` inside `Combat::RequestAttack`),
not generic combat state. This is a genuine correctness bug, not a
missing-scope item -- recorded here rather than as a design gap.

**Verification attempted live, result honest and calibrated:** tried to
construct the exact failure condition (an unrelated attacker present
while `KillNearest` approaches its own target) via
`.autonomousplayer multipull` (2, 4, 6, then 8 simultaneous Mottled
Boars -- four separate attempts, increasing pull size each time)
immediately followed by `guidestartcombat` for a new target, polling
`guidestatus` as fast as possible to catch genuine overlap. **Could not
reliably force sustained overlap in any of the four attempts** -- these
are weak, low-HP test creatures (42-55 HP vs. a level-2+ Warrior) that
die within a few real seconds regardless of pull size, faster than the
console-command round-trip latency in this test setup (every attempt,
including the 8-boar one, showed `hasUnplannedAdd=false` and often
`attackers=0-1` by the time a poll landed, meaning the earlier fights had
already resolved). The happy-path confirmation behavior *was*
re-verified correctly and consistently across all four attempts
(`isObjectiveTarget=true`, correct guid, real melee-range engagement, no
crashes, no regression), and the fix's logic is unambiguously more
precise than before by code review -- but the specific negative case
(does it correctly *ignore*/withhold-confirmation-for an unrelated
attacker) was **not conclusively demonstrated live** after four honest
attempts. This is now treated as a confirmed environment limitation, not
worth further retries with the same approach: a higher-HP test target,
a deliberately-throttled/paused test harness, or direct in-process test
instrumentation (rather than console-command polling) would be needed to
close this gap for real in a future session.

### Non-blocker, resolved: "isolated kills not incrementing quest counter"
Earlier in this arc, `character_queststatus.mobcount1` for quest 788
appeared stuck at 3/8 immediately after an isolated `guidestartcombat`
kill, leading to a documented (but unconfirmed) concern that kill credit
might not be registering correctly. Re-checked later in the same
session, after several more kills accumulated across multiple
tests: `mobcount1` had reached 8/8 and the quest had genuinely
auto-transitioned to `QUEST_STATUS_COMPLETE`. Running the quest through
to a real `guidestartquest` turn-in confirmed full, genuine completion:
`status=6` (`QUEST_STATUS_REWARDED`), `rewarded=true`, real XP granted.
Kill credit was working correctly the whole time -- the earlier
observation was a premature read of an in-progress count, not a defect.

### 6. Opportunistic-ability KillNearest occasionally times out in Engaged -- RE-TESTED, NOT REPRODUCED (larger sample)
`.autonomousplayer guidestartcombatability` (ADR-029, `KillNearest`'s
`Engaged` phase also trying a real ability alongside melee) was
originally tested 3 independent times against Mottled Boars: 2/3
completed cleanly, 1/3 genuinely hit the `MaxOperationTicks` bound while
still `Engaged` (`failed=true`), root cause not investigated at the time
(too small a sample to distinguish a real interaction from bad luck).

**Follow-up session: re-tested with a much larger sample (13 valid
trials, careful this time to poll for `finished=true` before starting
the next trial -- an earlier attempt in this same batch was invalidated
by starting a new trial before the prior one had actually finished,
which restarts the guide's state via `StartGuide` and silently abandons
the in-flight fight; that contaminated trial was discarded, not counted
either way).** Result: **12/13 completed cleanly** (`finished=true,
failed=false`, including two that were caught mid-fight via `guidestatus`
polling with real `Engaged`/`isObjectiveTarget=true` state before
finishing normally), **1/13 failed, but in `Selecting`, not `Engaged`**
-- `pullState=0` the whole time, hit `MaxOperationTicks` because no
creature of the target entry was found within the guide's 50-yard search
radius (the bot had wandered ~150 yards from the dense area across the
prior 9 kills' `MoveChase` pursuit) -- an entirely different, already-
understood failure mode (see ADR-028's bounded-Selecting-wait), not a
recurrence of the original `Engaged`-phase timeout at all.

**Conclusion: the original 1/3 `Engaged`-phase timeout did not reproduce
across 13 further trials.** This is now good evidence (not just a shrug)
that it was an unlucky individual creature/timing coincidence rather
than a systemic interaction between `Combat::RequestCastSpell` and
melee-swing timing -- the hypothesis this entry originally flagged for
investigation is not supported by the larger sample. Not fully closing
this out (13 trials is good, not exhaustive), but downgrading from "open
concern" to "low-priority, unreproduced."

### 7. Target-safety's first hostility check would have rejected every real questing target — CAUGHT PRE-DEPLOY, FIXED
While implementing target-selection safety (ADR-031), the first version
of `IsSafeToEngage`'s hostility check used `Unit::IsHostileTo()`. Live
diagnostics (`.autonomousplayer targetsafety`) against a real Mottled
Boar reported `hostile=false` -- most low-level questing wildlife is
faction-*neutral*, not Hostile, in this game's actual faction model, yet
is completely legitimate to kill (every Gate 2/3 combat test in this
project has run against exactly this creature). Shipping `IsHostileTo`
unchanged would have made `KillNearest` reject its own most-tested
target entirely -- a real regression, caught by testing the diagnostic
command itself before it was ever wired into `KillNearest`'s live path.
**Fixed:** switched to `Unit::IsValidAttackTarget()`, the engine's real
attackability check (handles the neutral-but-attackable case via
faction/reputation rank, plus immunity/unselectable/dead-state flags) --
re-verified live afterward, `attackable=true` for the same Mottled Boar
(see `TEST_MATRIX.md`).

### 8. `FindNearestCreature`'s `alive=false` means "only dead," not "either" — found via targetsafety, not fixed at the source
Investigating why `targetsafety`/`creaturestatus` couldn't find *any*
creature (friendly or hostile, at ranges up to 500 yards) that `multipull`
found trivially seconds earlier: `WorldObject::FindNearestCreature`'s
third parameter is an *exact* match
(`NearestCreatureEntryWithLiveStateInObjectRangeCheck::operator()`
requires `Creature::IsAlive() == alive`), not "include dead when false."
`creaturestatus` (a Gate 2 debug command, unrelated to this session's own
work) passes `false` intending "dead or alive" but actually means "only
dead" -- it happened to never matter in past sessions because
`creaturestatus` was typically called right after killing something.
**Fixed locally in `targetsafety`** (search `alive=true` first, then
`alive=false` as a fallback) but **not fixed in `creaturestatus` itself**
-- out of this session's scope, worth a one-line fix if it ever produces
a false "not found" in a future session.

### 9. Second test account's character creation reproducibly stalled -- ROOT-CAUSED AND FIXED (ADR-032)
Attempted to provision a second Horde character (`ap_test2`/
`Grunttestbot2`, same race/class args as the already-working
`Grunttestbot`) specifically to construct a real "another player already
fighting/tapped this target" scenario for ADR-031's target-safety
verification. Account creation itself succeeded (id 206, confirmed via
`SELECT` and via later login attempts correctly resolving the account),
but character creation never completed across three separate attempts:
`PendingCharacterCreations` logged "'Grunttestbot2' did not appear after
6001 ticks" each time, and `acore_characters.characters` never gained a
row for account 206. This is a different failure from Gate 2's
documented one-time transient *account*-creation hiccup (`KNOWN_FAILURES.md`
Gate 2, "one-time transient provision failure") -- that one resolved on
a single retry; this one reproduced 3/3 tries.

**Root cause (found by reading `WorldSession::HandleCharCreateOpcode`'s
real source, then confirmed live):** not a hung async chain at all --
`ObjectMgr::CheckPlayerName` correctly rejected `Grunttestbot2` for being
13 characters (`MAX_PLAYER_NAME` is 12), and `HandleCharCreateOpcode`
returns immediately on that rejection, *before* the async DB chain
`PendingCharacterCreations` was polling for ever starts. The rejection
packet (`SendCharCreate`) is a silent no-op for this module's
null-socket bot sessions -- the same "client-feedback gated on
`m_Socket`" class of bug as every Gate 1 finding, just newly discovered
in the character-creation path instead of login. (`Grunttestbot2` would
also have failed the separate character-set check -- digits are
genuinely disallowed -- but the length check fires first and
short-circuits, so that second issue was never actually exercised.)

**Fixed (ADR-032):** `Setup::ValidateCharacterName` runs the same
`ObjectMgr::CheckPlayerName` check *before* submitting and reports the
real reason immediately via the console's own feedback channel (not
gated on the bot session's null socket). **Verified live:** re-submitting
`Grunttestbot2` now fails in under a second with "too long" instead of a
multi-minute silent stall; a validly-named second character
(`Grunttestii`, same account 206) was created successfully on the first
attempt (guid 2016, confirmed via `SELECT`) -- account 206 itself was
never the problem. This also unblocks the live two-character scenario
ADR-031's evade/tap/other-player-attacking checks still need (see
`ARCHITECTURE.md` ADR-031's "not verified live" note).

### 10. Bounded-timeout guides stop their own bookkeeping but not an already-issued physical movement order -- FIXED (ADR-035)
While first running the new `live_regression_suite.py` (ADR-033) against
`Grunttestbot`, the `guidestartmoveto` regression test (an intentionally
unreachable coordinate, `5000, 5000, 500`) correctly hit
`MaxOperationTicks` and reported `finished=true, failed=true` as
designed (ADR-028) -- but the character kept physically walking toward
that same coordinate for a long time afterward, off the edge of
reachable terrain, and **died for real** along the way (confirmed:
`alive=false, hp=0/108`). Root cause: `TickMoveTo`'s one-shot
`Navigation::MoveTo` issues a real `MotionMaster` path order exactly
once; `OperationTimedOut` marking the *guide's own* state as
`failed=true` stops `GuideRuntime` from polling/caring about that step
any further, but never issues a stop-movement/return-to-safety order to
the character itself -- the two are decoupled, and only the first one is
bounded. **Recovered live** via the already-proven death/recovery
primitives (`releasespirit` then a fresh `moveto` back to the corpse,
since the ghost was *also* still driving toward the same stale
destination and had to be explicitly redirected before `reclaimcorpse`
would succeed -- confirming the stale movement order persists across
death/ghost-state too, not just the live character).

**Fixed same session (ADR-035):** `OperationTimedOut` now calls
`bot->StopMoving()` (standard public `Unit` API, not a position write)
on every bail-out, immediately before returning. **Verified live**:
repeated the exact same `guidestartmoveto` toward `5000, 5000, 500` --
the guide still correctly hits `failed=true` at the bound, but the
bot's position is now static across 5 subsequent polls (~30 real
seconds) instead of continuing to drift, and it survives
(`alive=true` throughout). `tools/live_regression_suite.py` still
passes `5/5` afterward (no regression).

**Separately, an unrelated real hazard found while re-testing this
fix** (not the same bug, not fixed, explicitly out of scope): manually
walking `Grunttestbot` back from a distant test location via a plain
`.autonomousplayer moveto` (not a guide, no `OperationTimedOut` involved
at all) resulted in a second, independent death partway through a very
long (~1500+ yard) unescorted cross-country walk through unfamiliar,
dangerous terrain. This is a real hazard of *this session's own test
methodology* (accidentally dragging a low-level character far from its
intended zone, then manually walking it back a huge distance) -- not a
module defect. Regional/long-distance travel safety is explicitly
Gate 4 scope ("regional travel," `ROADMAP.md`), not yet built; recovered
via the same proven death/recovery cycle. Worth remembering: don't drag
low-level test characters cross-country manually in future sessions --
if a bot ends up far from appropriate content, either provision a fresh
one near the target area or accept a long walk's real risk.

### 11. First real live encounter of `COMBAT_TOO_HARD` (a class this file already anticipated, see the closing section below) -- not a bug
As a direct consequence of #10 (`Grunttestbot`, level 3, dragged ~1500
yards from Valley of Trials into the Razor Hill area by the runaway
movement), `guidestartcombat` was tried against a `Greater Plainstrider`
(entry 3244, real Razor Hill-area wildlife -- higher-level content than
a level-3 character is meant to fight). Result: `pullState=2` (`Engaged`)
correctly reached and held for the full `MaxOperationTicks` bound
(`operationTicks` climbing 8->44 across polls, `botInCombat=true`,
`distance=0.5` the whole time -- genuinely fighting, not stuck/confused),
but the target was still `alive=true` when the bound fired
(`finished=true, failed=true`) -- a real level-3 character's melee DPS
alone genuinely could not kill it in time. **Not a defect** -- this is
`ADR-028`'s bounded-timeout working exactly as designed against a
real "content above the bot's level" mismatch, and it's the first live,
organic occurrence of the `COMBAT_TOO_HARD` failure class this file's
closing section already reserved a name for, before any Planner/Executor
loop exists to classify it formally. Worth remembering for later gates:
`KillNearest`'s guide-authoring layer (whatever picks `creatureEntry`
values for real leveling routes) needs to keep targets appropriate to
the bot's level, since `MaxOperationTicks` alone won't distinguish "genuinely
stuck" from "fighting something too tough" -- both look identical from
the guide's own state.

### 12. `castspell` debug command always re-triggers `SPELL_FAILED_MOVING` on retry -- FIXED (ADR-036)
While investigating whether Tame Beast (spell 1515) was real ahead of
the pets design pass: `.autonomousplayer castspell` unconditionally
calls `Navigation::MoveTo` before every cast, even when the caster is
already in range. This sets `UNIT_STATE_MOVING` for a tick every single
invocation, so `Unit::CastSpell` correctly rejects with
`SPELL_FAILED_MOVING` (51) -- and since every retry re-triggers the same
move order, this looked identical (and identically broken) no matter how
many times or how long between attempts, initially suggesting the spell
itself might not work. **Fixed:** the command now only moves if actually
outside the spell's real range (`SpellInfo::GetMaxRange`). No production
code (`Combat::RequestCastSpell`, `GuideRuntime`) was affected --
`RequestCastSpell` itself never issues a move order, so this bug only
existed in the debug-command wrapper. **Verified live:** identical cast
after the fix succeeded (`result=255 SPELL_CAST_OK`), and Tame Beast's
real cast completed into a genuine pet (see ADR-036/037).

### 13. A pet removed abnormally (owner death by real environmental hazard) can leave a stale summon-slot reference that blocks re-taming -- found, not root-caused
While verifying `Recovery::PlanPetRecovery`/`RequestRevivePet` (ADR-039)
against a genuinely dead-in-place pet: `Grunthunter` died from the
already-documented cross-country-travel hazard (`KNOWN_FAILURES.md` #10's
closing note) while its pet was alive nearby. Afterward, `petstatus`
reported "has no pet" (not "dead pet") -- `acore_characters.character_pet`
showed the pet row with `curhealth=0` and, critically, `slot=100`
(`PET_SAVE_NOT_IN_SLOT`, the engine's own sentinel for "not the current
active pet"), not `slot=0` (`PET_SAVE_AS_CURRENT`). `Player::GetPet()`
correctly resolves to null for this state (confirmed live: this is a
real `PetState::Dismissed`, not `PetState::Dead`, and `PlanPetRecovery`
correctly did *not* attempt a doomed revive on it -- a genuine positive
confirmation of the Dismissed/Dead distinction working as designed).

**The real, unresolved finding:** attempting to re-tame a *fresh* pet on
the same bot afterward was rejected every time
(`SPELL_FAILED_DONT_REPORT`, code 27) -- via both `.autonomousplayer
tamebeast` and raw `.autonomousplayer castspell 1515`, consistently,
across multiple retries with waits in between (ruling out GCD/cooldown).
`EffectTameCreature`'s own real source has an early, silent return if
`m_caster->GetPetGUID()` is non-empty (checked by reading
`SpellEffects.cpp` directly) -- `GetPetGUID()` reads a raw summon-slot
guid separately from `GetPet()`'s object resolution, so it's plausible
the abnormal pet removal left that raw guid stale/non-cleared even
though `GetPet()` itself correctly returns null. **Not confirmed root
cause** -- no debug command currently exposes `GetPetGUID()` directly to
check this theory, and a relogin (which might clear it) couldn't be
forced live (`.autonomousplayer login` refuses an already-registered
bot; no `logout` command exists yet). Left as an open, real, reproduced-
once finding rather than guessed at further. Consequence: full live
verification of `RequestRevivePet` actually reviving a dead-in-place pet
was **not achieved this session** -- the implementation is grounded in
a confirmed-real spell id (982, rejected with a semantically-consistent
`SPELL_FAILED_ALREADY_HAVE_SUMMON` while a pet is alive, see ADR-039) and
correct `PetState` classification (verified for the Dismissed case
above), but the specific Dead->Alive transition was not directly
observed firing. Same honest calibration as `KNOWN_FAILURES.md` #5.

---

This file will also start recording `PATH_FAILED` / `TRANSPORT_FAILED` /
`TARGET_UNAVAILABLE` / `OBJECTIVE_NO_PROGRESS` / `QUEST_TURNIN_FAILED` /
`COMBAT_TOO_HARD` / `UNSAFE_PACK_DENSITY` / `CORPSE_RECOVERY_FAILED` /
`INVENTORY_BLOCKED` / `TRAINING_BLOCKED` / `GUIDE_INVALID` /
`SCRIPTED_OBJECTIVE_UNSUPPORTED` / `STATE_MIGRATION_FAILED` class failures
once there is a Planner/Executor loop and a working online bot that can
produce them.
