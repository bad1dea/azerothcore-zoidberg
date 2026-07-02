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

### 8. `FindNearestCreature`'s `alive=false` means "only dead," not "either" — FIXED in both call sites now, live-verified
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
**Fixed in `targetsafety` first** (search `alive=true` first, then
`alive=false` as a fallback). **Now also fixed in `creaturestatus`
itself**, same pattern -- this bug was hit for real multiple times during
this session's pet-recovery regression-suite verification runs (reported
false "not found" for creatures confirmed alive and within range via
direct SQL distance queries, repeatedly causing wasted investigation time
chasing what looked like bot-positioning drift). **Live-verified**: found
a real, live, nearby creature (`Spirit Healer`, entry 6491, `alive=true`)
that the unfixed version would have missed.

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

**Update (2026-07-02, follow-up session): #12 had a second half.**
The ADR-036 fix only moved when out of the spell's max range -- but a
SELF-cast spell (found live with Summon Imp, 688) has max range 0, so
the `maxRange <= 0 -> walk anyway` fallback re-issued `MoveTo` every
invocation and re-created the identical permanent
`SPELL_FAILED_MOVING` (51) loop for the whole self-cast class. Fixed
(range-0 spells never move; commit `f3072ac`) and live-verified: the
same cast went 51 -> `SPELL_CAST_OK` with a real Imp produced.

### 13. A pet removed abnormally (owner death by real environmental hazard) needed a real fix to `RequestRevivePet` itself -- FIXED and live-verified; earlier "structural limitation" conclusion in this same investigation was WRONG and is superseded below
While verifying `Recovery::PlanPetRecovery`/`RequestRevivePet` (ADR-039)
against a genuinely dead-in-place pet: `Grunthunter` died from the
already-documented cross-country-travel hazard (`KNOWN_FAILURES.md` #10's
closing note) while its pet was alive nearby. Afterward, `petstatus`
reported "has no pet" -- `acore_characters.character_pet` showed the pet
row with `curhealth=0` and `slot=100` (`PET_SAVE_NOT_IN_SLOT`, the
engine's own sentinel for "not the current active pet"), not `slot=0`.

**This investigation went through three theories before landing on the
real, working fix. Recorded in order because the wrong turns are
instructive, not just the ending:**

1. **Stale `Unit::GetPetGUID()` (real, but not the cause here).**
   `Player::GetPet()`'s own source resolves the guid and returns null on
   failure *without* clearing it (confirmed by reading `Player::GetPet()`
   directly, including a commented-out fix in the engine's own code:
   `//const_cast<Player*>(this)->SetPetGUID(0);`). Added
   `Pets::HasStalePetSlot`/`RequestClearStalePetSlot` to detect and clear
   exactly that condition -- genuinely real and kept as its own defensive
   check -- but live-testing disproved it as the cause of the specific
   `SPELL_FAILED_DONT_REPORT` rejection seen when re-taming: after a full
   worldserver restart (`rawPetGuid` confirmed empty via a new `petstatus`
   diagnostic), re-taming was still rejected identically.
2. **`PetStable::GetUnslottedHunterPet()` correctly blocking *Tame
   Beast*.** Found by reading the actual attached spell script
   (`spell_script_names` names `spell_hun_tame_beast` for spell 1515;
   `src/server/scripts/Spells/spell_hunter.cpp`): `CheckCast()` refuses
   to tame a *new* pet while the old, abnormally-removed one is still
   sitting unslotted -- real, correct WoW pet-management logic. **This
   part of the analysis was correct** -- Tame Beast genuinely is and
   should be blocked here.
3. **Wrong conclusion drawn from #2: that this made the state
   unrecoverable at all.** This was a mistake, caught by the user before
   it was written up as final: taming a *new* pet was never the right
   recovery action for this state in the first place -- **reviving the
   existing pet is**, and that path was never actually tried. The
   apparent evidence for "unimplemented" (`SPELL_EFFECT_CALL_PET` maps to
   `Spell::EffectNULL` in this fork's generic effect table) was real but
   irrelevant: Revive Pet's real effect is `SPELL_EFFECT_RESURRECT_PET`
   (109), a *different*, fully implemented effect
   (`Spell::EffectResurrectPet`), and this module's own
   `RequestRevivePet` had a bug that prevented ever reaching it for this
   exact case: it bailed out early with `SPELL_FAILED_BAD_TARGETS`
   whenever `bot->GetPet()` was null -- but `GetPet()==null` is *exactly*
   the case this spell exists to handle. Reading `EffectResurrectPet`
   directly shows it explicitly branches on `!pet` and calls
   `player->SummonPet(0, ..., damage)`, which (per `Player::SummonPet`'s
   own source comment, `"petentry == 0 for hunter 'call pet' (current pet
   summoned if any)"`) reloads the pet from `PetStable`/DB via
   `Pet::LoadPetFromDB` regardless of whether a live object currently
   exists.

**Fix and live verification:** changed `RequestRevivePet` to always
self-cast (`Combat::RequestCastSpell(bot, bot, RevivePetSpellId)`)
instead of requiring `bot->GetPet()` to resolve first. Also added a
dedicated `.autonomousplayer revivepet <charname>` debug command --
`castspell`'s generic out-of-range-then-move logic (ADR-036) doesn't
make sense for a self-cast spell tested against an unrelated dummy
target and kept re-triggering `SPELL_FAILED_MOVING`. **Live result:**
cast against the real broken `Grunthunter` (`PetState::MissingDead`),
result `SPELL_CAST_OK`, and polling `petstatus` showed the *same pet*
(matching "Pet number: 5914") load back in alive, starting at partial
health and regenerating normally afterward. Reproduced twice.

**One real, separate, honestly-noted gap found during this
verification:** the null-pet branch of `EffectResurrectPet` does not
call an explicit `SavePetToDB` afterward (unlike its live-but-dead-pet
branch, which does). A worldserver restart shortly after a successful
revive (this session's own redeploy-for-the-next-fix cycle) found the
DB row still showing the pre-revive `slot=100, curhealth=0` state, and
the revived pet was gone again on the next login. This is consistent
with the engine's normal periodic autosave (`PlayerSaveInterval`,
900000ms/15min by default) or a clean logout being what actually
persists it -- not a new bug, the same durability characteristic every
other piece of transient world state in this engine already has -- but
worth knowing explicitly: a revive verified moments before an abrupt
server restart (as opposed to a clean shutdown) may not survive that
specific restart. Not verified further (would need a real 15-minute
wait or a clean logout, both correctly deferred as low value for this
session's time budget) -- flagged, not claimed.

**Corrected `PetState` model, implemented and shipped (see ADR-039's
follow-up):** `NoPet`, `ActiveAlive`, `ActiveDead`, `MissingAlive`,
`MissingDead`, `Dismissed` -- the `Missing*` split reads
`PetStable::GetUnslottedHunterPet()->Health` directly (not just
presence) to tell a pet that was dismissed-while-alive from one that was
dead-and-unslotted, per the user's explicit direction. `PlanPetRecovery`
routes `ActiveDead`/`MissingDead` to `RequestRevivePet` (confirmed
above) and `MissingAlive` to a new `RequestCallPet` -- **`RequestCallPet`
itself is NOT yet live-verified**: no test Hunter reached the level to
have Call Pet learned this session, so its spell id (883, a real,
standard WotLK id, not yet cast-tested in this fork) is gated behind
`Player::HasSpell` the same way every other unconfirmed gated ability in
this project is, and should be re-verified live (cast it, read the real
`SpellCastResult`) the first time a Hunter reaches the right level.

**Two follow-up regressions found by `tools/live_regression_suite.py`
right after this shipped, both fixed same day (full detail in
`ARCHITECTURE.md` ADR-040):** the `Tick()`-level recovery check
originally ran unconditionally, which (1) could preempt an active
`KillNearest` pursuit/quest interaction whenever `!bot->IsInCombat()`
read true for a moment (a real evade, or a brief gap before the first
hit registers) -- fixed by gating the whole check on
`state.CurrentTargetGuid.IsEmpty()`; and (2) re-issued the same
`RequestCastSpell` every eligible tick with no awareness of an
already-in-flight cast, which (like a real player mashing a spell
button) interrupted and restarted Revive Pet's own real cast time
before it could ever complete -- fixed by checking
`bot->IsNonMeleeSpellCast(false)` first. Both confirmed via the
regression suite going from intermittently exceeding its 40s wall-clock
timeout back to a clean `5/5`. A residual, live-confirmed-unrelated
Engaged-phase timeout still occurred in some later runs this session --
`petstatus` confirmed the pet was `MissingDead` while the guide was
genuinely `Engaged` with a live target, i.e. the new gate correctly held
recovery off; the guide still finished via its own bound
(`MaxOperationTicks`) regardless, just slower than the 40s test window
in some runs. This matches the pre-existing, already-documented Gate 3
#6 finding (ADR-029), not a new issue from pet recovery -- not chased
further this session (small sample, a heavily-reused test character).

### 14. User-reported: `Grunthunter` observed underground (Z-clipping) via direct in-game teleport -- ROOT-CAUSED and FIXED (NOPATH straight-line spline fallback = literal flight)
The user directly teleported to `Grunthunter` in-game (something this
agent cannot do -- no visual/client access, SOAP+DB only) and reported
it was under the terrain, asking whether pathing was broken. Taken
seriously and investigated immediately, not dismissed.

**Working theory going in**: `tools/live_regression_suite.py`'s
`guidestartmoveto_unreachable_target_times_out` test targets a literal,
deliberately-unreachable `(5000, 5000, 500)` on map 1
(`live_regression_suite.py:249`). `MotionMaster::MovePoint` defaults to
`forceDestination=true` -- if real pathfinding to that point fails, this
parameter can still force the unit toward the literal target coordinates
including a `Z=500` that may not match real terrain, which read as a
plausible mechanism for terrain clipping. This test ran many times over
the session (each bounded to ~20 real seconds via `MaxOperationTicks`
before `OperationTimedOut` calls `StopMoving()`, ADR-035), which also
independently explains this session's large cumulative position drift
(started near Valley of Trials, ended over 1500 yards away).

**Live-reproduced twice, deliberately, watching `Z` every few seconds
through the entire ~20-second walk both times**: both runs showed
smooth, continuous, terrain-tracking `Z` the whole way (e.g. `91.7 ->
92.5 -> 95.3 -> 99.3 -> 104.7 -> 106.5` in run 1) with no drops, no
negative values, no discontinuities -- both ended with the bot alive,
at full health, at a plausible resting `Z` for wherever it stopped. **The
suspected `MovePoint`/`forceDestination` mechanism did not reproduce the
reported clipping.**

**Real alternative candidate, not yet investigated as thoroughly**: this
session had multiple real deaths (`KNOWN_FAILURES.md` #10's travel-hazard
note) followed by `releasespirit`/`reclaimcorpse` cycles, and at least
one of those already produced an informally-noted (never written up as
its own entry until now) stuck-ghost symptom -- a ghost's position
reading at an elevated `Z` inconsistent with reachable terrain after one
release, worked around at the time by attempting `reclaimcorpse` anyway
since the ghost happened to already be within the real 39-yard
`CORPSE_RECLAIM_RADIUS` despite the height mismatch. A corpse or ghost
resting at an odd terrain position after a real death (especially a fall
death in Durotar's rocky, uneven terrain) is a plausible, real, and
distinct mechanism from `MovePoint` -- this agent has no way to visually
distinguish "underground" from "correctly at a low point in uneven
terrain the map's own geometry produces" without client access, so this
is flagged as the more likely candidate, not confirmed.

**Honest conclusion**: real, user-reported, taken seriously, actively
investigated same day -- but **not root-caused**. The specific mechanism
originally suspected was tested directly and ruled out. Not something
this agent can conclusively resolve without either the user's own
in-game observation at the exact moment it recurs (position + whether it
was the live character or a corpse/ghost) or deeper terrain-data
inspection tooling this project doesn't have yet. Flagged open, not
claimed fixed, not claimed understood.

**Real, cheap hardening applied regardless of root cause** (see
`tools/live_regression_suite.py`): the unreachable-target test now
restores the bot to its pre-test position afterward via a second
`guidestartmoveto`, rather than leaving a real test character stranded
wherever the bounded walk happened to stop -- this doesn't address the
clipping report directly, but removes the cumulative-drift side effect
this test was independently causing, and keeps future test runs closer
to a known, safe starting position.

### 15. Auto-tame-if-no-pet's first version gated on the wrong check (`Player::HasSpell`) -- FIXED and live-verified, fully automatic firing confirmed
While building auto-tame-if-no-pet (ADR-041, `Recovery::PlanPetAcquisition`):
the first version gated on `bot->HasSpell(Pets::TameBeastSpellId)`,
mirroring the discipline this project already applies to Revive Pet/Call
Pet (never assume a gated ability is usable without confirming it
live). This turned out to be the wrong check for Tame Beast
specifically: casting it manually
(`.autonomousplayer tamebeast Huntonia 3098`) against a real level-1
Hunter whose spellbook (`.autonomousplayer spellbook`) does **not**
list spell 1515 at all still returned `SPELL_CAST_OK`, and the pet
tamed successfully. Tame Beast is a real, innate Hunter ability, not
one granted through the normal trainer/spellbook system that
`HasSpell` reflects -- had this shipped with the `HasSpell` gate, auto-
tame would have silently never fired for any Hunter, ever, a real,
permanent no-op bug that could easily have gone unnoticed without this
specific live test (Grunthunter's spellbook, checked earlier the same
session, also lacked spell 1515 despite having successfully tamed a
pet via the same command).

**Fixed**: replaced `HasSpell` with `bot->IsClass(CLASS_HUNTER,
CLASS_CONTEXT_ABILITY)` -- a cheap pre-filter only (avoids a wasted
grid search for every non-Hunter bot every tick); real class/level/
range validation still happens inside the engine's own `CheckCast`
when the cast is actually attempted, same "delegate to the real
engine" philosophy as every other primitive in this module.

**Live-verified end to end after the fix**: a fresh Hunter
(`Petulantia`, never tamed anything, confirmed `PetState::NoPet`) was
walked near a real Mottled Boar via `guidestartmoveto` and then left
completely idle -- **no manual `tamebeast` command was issued**.
`Recovery::PlanPetAcquisition` fired on its own via the `Tick()`-level
`GuideRuntime` check, and `petstatus` showed a real, live, newly-tamed
pet (pet number 5969, alive, full health) moments later. This is the
first fully-automatic (not debug-command-triggered) confirmation of
any pet-acquisition behavior in this project.

### 16. `Recovery::PlanPetRecovery`'s `RecoverPet`/`CallPet` branches had the SAME `HasSpell` bug as #15 -- also FIXED live, and this corrects an earlier over-claimed verification
Directly after fixing #15 (Tame Beast), checked whether Revive Pet (982)
and Call Pet (883) had the identical problem, since both are also
real, specific Hunter pet-management spells. **They did.** Casting both
982 and 883 against a Hunter (`Petulantia`) whose spellbook lists
neither id both returned the real, specific `SPELL_FAILED_ALREADY_
HAVE_SUMMON` (not an unknown-spell rejection) while she had an active
pet -- conclusive proof both are genuine, castable, innate abilities,
same as Tame Beast, and `Player::HasSpell` is the wrong gate for all
three Hunter pet-management spells in this fork.

**This matters more than #15 alone because it corrects an earlier,
too-confident claim.** `ARCHITECTURE.md`'s ADR-040 entry states
`RecoverPet`'s automatic `Tick()`-level firing was "fully verified
live" -- that verification only ever exercised the raw
`Pets::RequestRevivePet` primitive through the manual
`.autonomousplayer revivepet` debug command, which calls the primitive
directly and never goes through `Recovery::PlanPetRecovery`'s own
`HasSpell` gate at all. The actual *policy* -- the thing that's
supposed to decide *whether* to revive automatically -- would have
silently never returned a `RecoverPet` intent for any Hunter, ever,
identical to #15's bug, and this went unnoticed through this entire
session's earlier pet-recovery work.

**Fixed**: both branches now use `IsClass(CLASS_HUNTER,
CLASS_CONTEXT_ABILITY)` instead of `HasSpell`, same fix as #15.
**Live-verified for real this time**: logged `Grunthunter` back in with
his pet still `PetState::MissingDead` (unchanged from earlier this
session), started a trivial guide (a `guidestartmoveto` to his own
current position, purely to keep `GuideRuntime::Tick` actively firing
-- see the architectural note below) and issued **no `revivepet`
call**. Within a few seconds, `petstatus` showed the exact same pet
(matching pet number 5914) alive again, fully automatically.

**Real architectural characteristic surfaced along the way, worth
documenting plainly rather than leaving as a silent assumption**:
`BotLifecycleMgr::Update` only calls `GuideRuntime::Tick` for a bot
while `!session.Guide.Finished` -- and an idle bot with no guide
currently running (or one whose last guide already completed) has
`Finished=true`, so `Tick()` is never called for it at all. This means
**pet recovery/acquisition currently only fires as a side effect of an
active guide being ticked** -- there is no standalone "ambient"
background pet-maintenance process independent of guide activity. Not
itself a bug (every real behavior this project has built is guide-
driven by design), but worth knowing explicitly: a fully idle bot with
a dead/missing pet and no guide running will not self-heal until some
guide starts running again.

**Closed, same session (ADR-042)**: added `GuideRuntime::TickAmbient`,
called unconditionally by `BotLifecycleMgr::Update` for every
registered bot every tick regardless of guide state, containing the
pet-recovery/acquisition logic moved out of `Tick()`. **Live-verified
with zero guide commands**: `Petulantia` (`PetState::NoPet`) was simply
logged in near her spawn -- no `guidestartmoveto`, no `tamebeast`,
nothing -- and got a real, live, auto-tamed pet within seconds. A fully
idle bot now does self-heal without any guide ever being started.

### 17. `PetState::MissingAlive` cannot be constructed via the real "Abandon Pet" action -- RESOLVED (ADR-043): the real mechanism is the Dismiss Pet SPELL (2641), and the `MissingAlive` -> `Alive` transition is now directly observed live

**RESOLUTION (2026-07-02, ADR-043)**: the mechanism this entry
concluded didn't exist does exist -- it's the real Dismiss Pet *spell*
(2641), not a pet *command*. `Spell::EffectDismissPet` calls
`pet->Remove(PET_SAVE_NOT_IN_SLOT)` -- the recoverable unslot -- unlike
the abandon opcode's `PET_SAVE_AS_DELETED`. This investigation had
searched the `CommandStates` enum and the opcode vocabulary but not the
innate pet-management spell family this same arc had already
characterized (#15/#16) -- the fix was one `grep EffectDismissPet` away
the whole time. `Pets::RequestDismissPet` + `.autonomousplayer
dismisspet` now exist, and the full chain was observed live with 40ms
polling: `SPELL_CAST_OK` -> (real ~5s cast time) -> a directly-captured
`state=MissingAlive` -> `Recovery::PlanPetRecovery`'s `CallPet` intent
firing automatically on the next ambient tick -> **the same pet number
(5988) back alive within 0.6s**, zero manual recovery commands. Call
Pet spell 883 is thereby also live-confirmed for the first time. The
original entry below is kept as written -- its "no mechanism found"
conclusion was wrong, and the in-order record of *why* it was reached
(looking at commands, not spells) is the instructive part.
Attempting to finally verify `RequestCallPet`'s specific `MissingAlive`
-> `Alive` transition (the one remaining unverified pet-recovery
transition after ADR-039/040/041): added
`Pets::RequestAbandonPet`/`.autonomousplayer abandonpet`, using the
real `CMSG_PET_ABANDON` opcode handler
(`WorldSession::HandlePetAbandon`) to dismiss a genuinely alive pet
(`Petulantia`'s real, live, auto-tamed pet) as the only apparent
non-destructive way to construct that state.

**Real finding**: `HandlePetAbandon` calls `RemovePet(pet,
PET_SAVE_AS_DELETED, false)` -- confirmed by checking
`acore_characters.character_pet` immediately after: the row was gone
entirely, not present with `slot=100`/health intact. "Abandon Pet" is a
**permanent delete**, not a recoverable dismiss -- `petstatus`
correctly (if a little ambiguously in its own debug-command label,
since it doesn't have a `lastKnownGuid` to distinguish) reported
`NoPet`. This engine's own `CommandStates` enum
(`src/server/game/Entities/Unit/Unit.h`) has exactly four values --
`COMMAND_STAY`/`COMMAND_FOLLOW`/`COMMAND_ATTACK`/`COMMAND_ABANDON` --
there is no distinct "temporarily dismiss, keep recoverable" command in
this fork's pet-command vocabulary at all, unlike what the real WoW
client's pet UI implies (a separate "dismiss" vs. "abandon" distinction
from the player's perspective).

**Consequence, stated honestly**: this session found no real,
player-facing way to construct a genuine `PetState::MissingAlive`
scenario at all. The only organic `PET_SAVE_NOT_IN_SLOT` (unslotted)
state observed all session was `MissingDead` (`curhealth=0`), arising
from an unrelated owner-death mishap, not a live pet being cleanly
unslotted. `RequestCallPet`'s specific `MissingAlive` -> `Alive`
transition therefore remains **unverified**, and unlike every other gap
this session closed, this one may not be closeable through this
module's normal live-testing approach at all without either (a) finding
a different, real in-game mechanic that produces this exact state (some
forks/instances temporarily unsummon pets via
`RemovePet(pet, PET_SAVE_NOT_IN_SLOT)` internally, e.g. certain zone or
vehicle transitions -- not investigated this session, real candidate
for next time) or (b) accepting the strong code-symmetry argument
instead of direct observation: `Pets::RequestCallPet`'s underlying cast
(spell 883, self-targeted) is grounded in the same
`player->SummonPet(0, ...)` mechanism `RequestRevivePet` already
confirmed working live for the analogous `MissingDead` case -- that
code path does not distinguish alive/dead in the stable at all, only
whether `GetPet()` currently resolves. Not claimed as proof, just noted
as the strongest available evidence short of direct observation.

`Petulantia`'s pet is genuinely, permanently gone now (working as
designed, not a bug) -- she is back to `PetState::NoPet` and available
as a clean auto-tame-acquisition fixture for future sessions, not a
`MissingAlive` one.

### 18. `TickAmbient`/`Tick` split (ADR-042) could let a same-tick guide-step dispatch interrupt an ambient-issued cast -- caught by self-review, FIXED before any live symptom occurred
After ADR-042 shipped and was live-verified, re-read the change
carefully rather than moving straight on -- this project's own
established discipline of testing diagnostics against real state, not
just reading code, cuts both ways: reading code carefully after a
change also catches real things live testing didn't happen to exercise.

**The real risk**: `BotLifecycleMgr::Update` called `TickAmbient` and
`Tick` back-to-back, unconditionally, in the same fire. Before ADR-042,
the pet-recovery logic lived *inside* `Tick()` itself, so issuing a
recovery intent made the whole function return immediately -- that was
the entire mechanism preventing the same tick's guide-step dispatch
(e.g. `KillNearest`'s `Selecting` phase finding a brand new target)
from running right after and potentially interrupting a cast that
intent had just started (both Revive Pet and Tame Beast have real cast
times, ADR-040/041's `IsNonMeleeSpellCast` fix). Splitting the pet logic
into a separate function silently dropped that pause-by-skipping
behavior -- nothing connected the two now-separate calls anymore.

**Why live testing didn't catch it**: neither of this session's real
verifications happened to exercise the risky combination. The
`Grunthunter` revival test used a bare `MoveTo` guide, which has no
competing target-search logic to run in the same tick. The `Petulantia`
acquisition test had no guide running at all, so `Tick()` never fired
either. Both are real, legitimate confirmations of `TickAmbient` itself
working -- neither was positioned to reveal this specific interaction.

**Fixed**: `TickAmbient` now returns `bool` (true if it issued an
intent this call); `BotLifecycleMgr::Update` skips `Tick()` for that
same fire when it did, restoring the original pause-by-skipping
semantics explicitly across the split call sites. Compiled clean,
redeployed, `live_regression_suite.py` re-run to confirm no regression
from the fix itself (still shows the same pre-existing, already-
documented environmental flakiness pattern, not a new failure).

### 19. A dead bot could get permanently, unrecoverably stuck via ADR-042's own TickAmbient/Tick split -- FIXED at the source and with a systemic backstop
Found running the regression suite as a final health check, not by
deliberately hunting for it: a `guidestartmoveto` guide for `Huntonia`
got stuck at `operationTicks=44` (one short of the 45 bound) and
**never advanced**, confirmed by polling `guidestatus` twice with zero
change in between -- a real hang, not a slow-but-progressing run.

**Root cause**: `Huntonia` had died for real (`alive=false`) with her
pet also `PetState::MissingDead`. `Recovery::PlanPetRecovery` had no
`bot->IsAlive()` check, so it kept returning a `RecoverPet` intent every
single tick. Manually confirmed the cast itself fails instantly with
`SPELL_FAILED_CASTER_DEAD` (code 23) -- a dead caster can't cast
anything, obviously, in retrospect -- which meant `Unit::
IsNonMeleeSpellCast` (the guard added in ADR-040 specifically to stop
re-issuing a cast that's already in flight) never had anything to catch,
since the cast never actually started. Combined with ADR-042's own fix
(`TickAmbient` returning `true` skips `Tick()` for that fire, to stop a
same-tick guide-step dispatch from interrupting a cast `TickAmbient`
just started), this meant `Tick()` -- and with it, every one of its own
bounded-wait mechanisms (`OperationTimedOut`, `MaxOperationTicks`) --
never got a chance to run again, **forever**, for as long as the bot
stayed dead. A guide that should have failed cleanly within ~20 real
seconds instead would have hung indefinitely.

**This is worse than #18** (the earlier same-session self-review catch):
#18 was a narrow race window that only mattered for one tick at a time.
This is an unbounded, permanent hang with no natural recovery path --
exactly the class of failure this whole project's bounded-wait
discipline (ADR-028 and everything built on it) exists to prevent, and
it slipped in via the one place (`TickAmbient`) that doesn't go through
`Tick()`'s own bounded-wait bookkeeping at all.

**Fixed two ways**:
1. **At the source**: added `!bot->IsAlive()` to the early-return guard
   in both `Recovery::PlanPetRecovery` and `Recovery::PlanPetAcquisition`
   -- a dead bot has no business attempting any pet-recovery/acquisition
   cast, matching how a real player character can't cast anything while
   dead either. Directly prevents this specific cause from recurring.
2. **A systemic backstop**: added `BotSession::ConsecutiveAmbientSkips`
   (`Lifecycle/BotLifecycleMgr.h`), incremented whenever `TickAmbient`
   causes `Tick()` to be skipped, reset to 0 whenever it doesn't. Once
   `MaxConsecutiveAmbientSkips` (10, ~10 real seconds) is exceeded,
   `Tick()` is forced to run regardless of what `TickAmbient` reports --
   giving the guide's own bounded-wait mechanisms a chance to resolve
   even under some *other*, not-yet-found persistent-failure mode having
   the same starvation effect. Deliberately generic rather than another
   narrow, cause-specific patch: the structural risk (anything in
   `TickAmbient` that can return `true` indefinitely bypasses every
   bounded-wait guarantee `Tick()` provides) is real regardless of which
   specific condition triggers it.

**Live-verified**: `Huntonia` logged back in alive (login resurrects), a
fresh `guidestartmoveto` completed normally (`finished=true,
failed=false`) with no hang. The exact original dead-bot scenario
couldn't be re-triggered identically in the same session (she was alive
again by the time the fix was live), so this is indirect confirmation
that `Tick()` runs normally now, not a direct re-reproduction of the
original hang followed by a fix -- honestly noted, not overclaimed.
The fix itself (the `IsAlive()` check specifically) is sound by direct
code review of the confirmed root cause regardless.

### 20. Non-bug findings from the first deliberate dense-camp/cave validation (2026-07-02) -- documented characteristics, working as designed

The Burning Blade cave run (see `TEST_MATRIX.md`'s 2026-07-02 rows)
surfaced three real characteristics worth recording, none of them bugs:

1. **A killed-out area produces a bounded whole-step failure, by
   design**: `KillNearest`'s `MaxOperationTicks` budget deliberately
   spans the entire step (ADR-028's own code comment: not reset per
   target, so a pathological target-cycle still trips the bound). In a
   heavily-farmed cave with 200s respawns, one cycle spent ~35 ticks in
   a genuine no-safe-LoS-target drought, then blacklisted a flickering
   patroller, retargeted, engaged -- and hit the budget mid-`Engaged`
   (`finished=true, failed=true, operationTicks=46`), leaving the bot
   mid-fight. The bot finished that fight fine on real auto-attack+pet
   (back to full health, out of combat, no hang). A real leveling-guide
   layer above should simply re-issue/advance -- the step-level
   `failed=true` is the bounded-escape contract working, not a defect.

2. **`EncounterModel` counts only attackers of the bot itself**
   (`bot->getAttackers()`, confirmed by reading `BuildSnapshot`): a mob
   attacking the *pet* is invisible to `HasUnplannedAdd`. Deliberate
   scope for now (the research document's add policy is about the
   bot's own safety), but anyone extending multi-target policy should
   know pet-side aggro doesn't register as an add.

3. **`multipull` does not reliably construct a multi-attacker-on-bot
   scenario**: it issues `RequestAttack` per target back-to-back, and
   the bot's real melee attack can only stick on the last one -- an
   earlier target that never took damage may simply never aggro. In the
   one engineered attempt this session, `attackers` never exceeded 1.
   Tooling limitation of the debug command, not module behavior --
   `hasUnplannedAdd=true` has still never been organically observed in
   the cave (honestly flagged in `TEST_MATRIX.md`; the add-withhold
   decision itself was live-verified in the earlier review-response
   arc).

   **Update (2026-07-02, follow-up session)**: two real additions from
   a deliberate attempt at the assist-call construction above.
   (a) **Methodology trap, worth never re-tripping**: the standalone
   `.autonomousplayer encountersnapshot` command passes
   `ObjectGuid::Empty` as the objective (it is guide-independent by
   design), so with ANY attacker present it reports
   `hasUnplannedAdd=true` -- an early "organic observation" this
   session was exactly this artifact and was retracted on reading the
   command's source. The honest signal is `guidestatus`'s `encounter:`
   line, which is built with the guide's real `CurrentTargetGuid`.
   (b) The server config was checked directly rather than assumed:
   creature family assist IS enabled on this deployment
   (`CreatureFamilyAssistanceRadius = 10`,
   `CreatureFamilyAssistanceDelay = 2000`), so the construction is
   possible in principle. 17 guide cycles against Vile Familiars
   teleport-anchored on a 5.7yd spawn pair AND a ~8yd triple cluster
   (`(-40, -4227)` map 1, tele point `APFamiliarTriple`), including
   one final cycle after a full 220s camp respawn, polling the correct
   signal, produced zero organic adds -- wander (and possibly the test
   Hunter's pet absorbing neighbor attention) keeps live neighbors
   outside the 10yd assist radius at fight time far more reliably than
   spawn coordinates suggest. Still open, deliberately parked after a
   real attempt: the next idea that isn't more of the same is a
   petless melee bot fighting slower (longer assist window), or
   accepting `multipull`'s constructed version as the only practical
   reproduction.

### 21. `IsSafeToEngage`'s LoS check was stricter than the engine's own combat LoS -- on doodad-dense terrain it rejected EVERY target zone-wide -- FIXED (`ModelIgnoreFlags::M2`) and live-verified

Found by the all-races breadth run (2026-07-02), specifically the Blood
Elf slice on Sunstrider Isle: `guidestartcombatability` failed bounded
(`operationTicks=46`, `Selecting` drought) from multiple positions, and
`targetsafety` reported `los=false -> safe=false` for **every creature
tried, zone-wide** (Springpaw Cub, Mana Wyrm, from different spots) --
while the raw `.autonomousplayer attack` command walked 70yd and fought
one with no problem, and a real `castspell 75` returned `SPELL_CAST_OK`
(the engine's own cast-time LoS validation passing).

**Root cause, confirmed by reading the engine**: the module called
`IsWithinLOSInMap(candidate)` with the default
`VMAP::ModelIgnoreFlags::Nothing` -- M2 doodad models (trees, crystals,
props) block sight under those flags. The engine's own combat reality
is different: `Spell::CheckCast`'s LoS check (`Spell.cpp`) passes
`VMAP::ModelIgnoreFlags::M2`, i.e. real spells (and melee, which does
no LoS check at all) go straight through doodads. Sunstrider Isle is
blanketed in giant M2s, so the strict check starved target selection
for the entire zone -- and would have done the same in any
decoration-heavy area, silently, as "no safe target" bounded failures.
The five previously-tested zones just happen to be sparse enough that
this never fired.

**Fix**: both call sites (`IsSafeToEngage`, and the `targetsafety`
debug command whose whole job is to mirror that policy) now pass
`VMAP::ModelIgnoreFlags::M2`, matching the engine's own combat LoS
semantics; WMO buildings/terrain still block normally. **Live-verified**:
same spot went `los=false -> safe=false` to `los=true -> safe=true`
after deploy, and the previously-failing Blood Elf ranged slice then
completed cleanly (`finished=true, failed=false, lastLootVerified=
true`). Regression suite `5/5` after the fix.

**Diagnosability note from the same investigation**: three different
causes produced the *identical* `failed=true, operationTicks=46,
pullState=0` signature this session -- (a) this LoS bug, (b) no
creature of the entry within the 50yd search radius at all (the SQL
"cluster average" position turned out to be an empty centroid ringed
by spawns 64+yd away), and (c) all nearby candidates legitimately
tagged by another bot (two bots hunting the same wolf pack -- ADR-031's
tap/other-player-attacking checks working exactly as designed between
two of this module's own bots, observed live). `guidestatus` does not
currently distinguish "nothing in range" from "candidates found but
all unsafe (and why)" -- a per-rejection-reason counter would have cut
this session's diagnosis time substantially. Real, cheap improvement
for a future session.

**DONE (2026-07-02, follow-up session)**: `SelectionDiagnostics`
(ADR-045) -- every `Selecting` sweep now records per-reason rejection
counters (`candidates`/`dead`/`blacklisted`/`evading`/`notAttackable`/
`tapped`/`otherPlayerAttacking`/`noLos`) into the guide state, printed
by `guidestatus` as a `selection:` line. Paid for itself the same hour
it went live, twice: a `notAttackable=1` at the boar cluster turned
out to be the test Hunter's own tamed Mottled Boar (a Pet matching the
objective's creature entry -- correctly rejected by
`IsValidAttackTarget`, and invisible to diagnosis before the
counters); and `noLos=3` on the familiar-camp cave terrain confirmed
the M2-ignore LoS check (#21's own fix) rejecting only genuinely
WMO-blocked candidates zone-locally instead of everything.

### 22. Server-initiated teleports of a bot silently never complete (no client to ack) -- FIXED (`BotSessionMgr` teleport-ack synthesis) and live-verified on both paths

Found live (2026-07-02): `.tele name Grunttestbot ValleyOfTrials` printed
its normal success message and the bot never moved -- no error anywhere,
position simply unchanged minutes later. Root cause, confirmed by
reading the engine: every server-initiated teleport keeps the player at
the old position until the *client* acknowledges it
(`MSG_MOVE_TELEPORT_ACK` for same-map, `MSG_MOVE_WORLDPORT_ACK` across
maps; `Player::mSemaphoreTeleport_Near/_Far`), and a socketless bot
session has no client to ever send one -- the semaphore hangs forever.
Same silent-failure root shape as every Gate 1 bug (a client-feedback
path gated on a socket that doesn't exist).

Sub-finding: the hung semaphore is not even visible as a stuck bot --
server-driven `MotionMaster` movement still works while
teleport-pending, and the pending *destination* gets persisted by the
periodic character save, so a worldserver restart "completes" the
teleport hours later as a position snap-back. That made the first
observation genuinely confusing (the bot "teleported" across a restart
boundary with no code in between).

**Fix**: `BotSessionMgr::Update` now synthesizes the ack a real client
would send, once per update, for any tracked session whose player has a
pending teleport -- `WorldSession::HandleMoveWorldportAck()` (the
core's own "for server-side calls" entry point) for the far case, a
synthesized `MSG_MOVE_TELEPORT_ACK` packet through the real
`HandleMoveTeleportAck` handler for the near case. Deliberately ack
-only: the module still never *initiates* a teleport (ADR-005/ADR-046);
both entry points no-op unless the core already has one pending.

**Live-verified both paths** (2026-07-02, post-deploy): same-map
`.tele name Grunttestbot RazorHill` -> position (326.8, -4706.6) within
seconds; cross-map `.tele name Grunttestbot Stormwind` -> map 0
(-8833.4, 628.6), then back to map 1. Operational warning learned the
hard way in the same test: a level-3 Horde bot teleported into
Stormwind is guard-killed in seconds -- pick teleport test destinations
by faction.

### 23. `.kick` of a bot is a silent no-op -- a socketless bot session cannot be logged out by any normal means -- FIXED (`.autonomousplayer logout`)

Found live (2026-07-02) while trying to force-recycle `Grunttestbot`'s
session (see #24): `.kick Grunttestbot` printed "Player Grunttestbot
kicked." and the bot stayed online and registered; a subsequent
`.autonomousplayer login` for the same character was then rejected with
"character is already online". Almost certainly the same ADR-008
mechanism that makes these sessions survive at all: kick/logout
processing happens in the session-update path that `BotSessionMgr`
deliberately drives with a `MapSessionFilter` (whose `ProcessUnsafe()`
is false) specifically so the null-socket eviction path never runs --
which also means the kick-driven logout never runs. Real gap, honestly
stated: **the module currently has no way to log a bot out at runtime**
(no `.autonomousplayer logout` exists either). Any future logout
feature must route around the same filter, and must NOT delete the
session from inside its own call stack (see
`BotSessionMgr::QueueForRemoval`'s doc comment).

**FIXED (2026-07-02, same day it bit again):** mid-#29 verification, a
character needed an inventory reload and the only tool was another
full worldserver restart -- direct evidence this gap had real
operational cost. `.autonomousplayer logout <charname>` now calls
`WorldSession::LogoutPlayer(true)` directly (the same real teardown an
organic logout performs), routing around the filtered kick path
exactly as this entry prescribed; session deletion rides the existing
`OnPlayerLogout` -> `QueueForRemoval` machinery, deferred safely off
the command's call stack. Live-verified end to end: login (1
registered) -> logout ("logged out and saved", 0 registered, DB
`online=0`) -> immediate re-login (submitted, bot back in world at its
logout position) -- the precise cycle `.kick` could not perform this
morning.

### 24. `Grunttestbot` combat-inert after cross-map guard-death + GM `.revive` -- movement/selection fine, melee swing never fires; NOT root-caused, control bot unaffected

Observed live (2026-07-02, post-deploy): after #22's Stormwind teleport
test (guard-killed), a corpse-state teleport back to map 1, and a GM
`.revive`, `Grunttestbot` (alive, full hp, `ghost=false`) walks
normally, selects targets normally (`selection: candidates=4`, zero
rejections), reaches its target (2.9yd), gets `Engaged` confirmation
(`GetVictim` set) -- and then simply never swings: 46 ticks at melee
range, `botInCombat=false` throughout, the boar never retaliates
(strong evidence no swing ever landed or even started), step fails
bounded, bot unharmed. Reproduced with the raw `.autonomousplayer
attack` command too, twice, including after `.kick` (#23 -- which
didn't actually recycle anything).

Ruled out with a real control: `Petulantia`, same build, same teleport
mechanism, same boar cluster, completed full kill+loot cycles cleanly
(with and without full bags) minutes later. Weapon durability 11 (not
broken). NOT ruled out: something in the die-in-unvisited-map ->
corpse-teleported-cross-map-while-dead -> GM-`.revive` chain leaving a
stuck unit/attack state a fresh `Player` object would clear.

**Probe result (same session): the restart CLEARED it.** After a
worldserver restart + fresh `.autonomousplayer login`, the same
character ran a clean fully-automatic kill+loot cycle
(`botInCombat=true` observed mid-fight, `finished=true, failed=false,
lastLootVerified=true`). So the wedge is in-memory session/`Player`
state, not persisted character state -- root cause inside that state
still unidentified (the die-cross-map + GM-`.revive` chain is the
reproduction candidate if anyone needs it), and #23's missing-logout
gap is exactly what made it unrecoverable without a restart. If a bot
ever goes combat-inert again: restart first, root-cause second. The
regression suite's combat test uses this character -- if it
mysteriously fails `guidestartcombat_completes_cleanly`, check this
first (or point `AP_SOAP_BOT_ACCOUNT/CHAR` at `ap_test5`/`Petulantia`).

### 25. Cast-time opportunistic openers self-interrupted forever -- the exact failure an ADR-044-era code comment predicted, now observed live and FIXED

The first real cast-time ranged opener this project ever ran (Warlock
Shadow Bolt 686, `Warlocktest`, same session as the summoning probe)
hit it immediately: `KillNearest`'s `Engaged` phase re-issues the
opportunistic ability every tick, and for a spell with a real cast
time each re-issue cancels the in-flight cast -- so **zero bolts ever
landed**. Observed live end-to-end: clean ranged selection at 48.3yd,
correct approach-then-hold at 23.1yd (ADR-044 residual A, observed for
the first time in the same trace), then the boar walked over and beat
the stationary bot from 63 to 25hp across ~35 ticks while the bot
"cast" continuously, until ADR-028's bound failed the step. The old
code comment at the call site predicted this word-for-word ("a future
cast-time ranged opener would need the same `IsNonMeleeSpellCast`
guard") -- prediction confirmed, not a surprise, but now it is
evidence.

**Fix**: gate the per-tick re-issue on
`!bot->IsNonMeleeSpellCast(false, false, true)` --
`skipAutorepeat=true` keeps the Auto Shot archetype's behavior
byte-identical (an armed autorepeat doesn't count as casting; only a
genuine in-flight cast blocks the re-issue). **Live A/B verified**:
identical setup post-fix selected at 42.4yd, held at range, bolts
landed, the boar died at contact via the ADR-044 melee fallback,
`finished=true, failed=false, lastLootVerified=true`, bot at 62/63hp.
~11 operation ticks versus the pre-fix bounded failure at 46. This was
also the first Warlock combat slice -- a fourth class archetype
(melee Warrior, autorepeat-ranged Hunter, cast-time-ranged Warlock,
plus Priest's non-combat coverage) composed with zero class-specific
code.

---

This file will also start recording `PATH_FAILED` / `TRANSPORT_FAILED` /
`TARGET_UNAVAILABLE` / `OBJECTIVE_NO_PROGRESS` / `QUEST_TURNIN_FAILED` /
`COMBAT_TOO_HARD` / `UNSAFE_PACK_DENSITY` / `CORPSE_RECOVERY_FAILED` /
`INVENTORY_BLOCKED` / `TRAINING_BLOCKED` / `GUIDE_INVALID` /
`SCRIPTED_OBJECTIVE_UNSUPPORTED` / `STATE_MIGRATION_FAILED` class failures
once there is a Planner/Executor loop and a working online bot that can
produce them.

### 26. Quest drops were NEVER looted -- per-player quest-item loot slots were never requested -- FIXED, plus the loot-verification check could never read true

Found live by ADR-048's first full grind run: 8+ verified kill+loot
cycles against Plainstriders collected ZERO Plainstrider Meat (the
90%-chance quest drop) -- confirmed via forced save + DB inventory
query, not inference. Root cause: a player's quest drops live in a
separate per-player list (`Loot::PlayerQuestItems`), addressed on the
wire as `items.size() + <index in that player's list>` (the exact
encoding `SMSG_LOOT_RESPONSE` sends a real client); `LootCorpse` only
ever requested `loot.items` slots. Every quest-collection objective in
the project's history would have silently never progressed. Fixed --
same real autostore opcode handler, per-player rules apply inside it.

Same investigation, second bug: `LastLootVerified` checked
`corpse->loot.items.empty()`, but the engine FLAGS looted items rather
than erasing them -- so verification could literally never read true
for any corpse that dropped an item at all (every historical
`lastLootVerified=true` was a gold-or-nothing corpse). Now uses the
engine's own `Loot::isLooted()` (`gold == 0 && unlootedCount == 0`),
which also counts per-player quest drops. Post-fix, grind cycles read
`lastLootVerified=true` consistently for the first time -- on corpses
with real drops.

### 27. The first repeat-grind gate kept grinding forever AFTER the quest completed -- `CanCompleteQuest` is the wrong predicate -- FIXED

ADR-048's first fixed-loot run collected all 7 meat (engine flipped
the quest INCOMPLETE -> COMPLETE the instant the 7th landed), and the
grind step kept hunting an 8th plainstrider until the ADR-028 bound
killed the whole route. `Player::CanCompleteQuest` only evaluates
objectives while the status is still INCOMPLETE and returns false for
an already-COMPLETE quest -- so the gate `!CanCompleteQuest(...)`
became permanently true at the exact moment of success. The correct
keep-grinding predicate is simply
`GetQuestStatus(...) == QUEST_STATUS_INCOMPLETE`. Fixed and verified
by both archetype runs (see ADR-048). Same lesson as #15/#16: verify
what an engine helper actually computes, not what its name suggests.

### 28. A resumed grind route with already-complete objectives could not pass its own grind step -- gate was only consulted AFTER a kill cycle -- FIXED

Found widening ADR-048 to Undead (map 0) and Draenei (map 530): both
races' grind fields sit farther from their questgiver than the
turn-in step's 150yd search radius (Rattlecage field ~168yd from
Sarvis, Root Lasher field ~258yd from Botanist Taerix), so the first
route issue predictably bound-failed at the turn-in step -- with all
objectives complete. The designed recovery is re-issuing the
idempotent route with the MoveTo waypoint near the giver; but the
repeat-grind gate only existed at the END of a kill+loot cycle, so on
re-entry the grind step owed one more kill first, and a giver with no
grind targets within 50yd spun `Selecting` to its 3x bound and failed
the whole guide. Fixed by checking the same
`GetQuestStatus != QUEST_STATUS_INCOMPLETE` gate on `Selecting` entry:
an already-satisfied grind step now no-ops, which is what makes the
re-issue contract actually hold for far-field quests. Verified live:
Draenei collection quest 9293 resumed straight to turn-in -> REWARDED
after exactly this failure. Route-authoring lesson from the same runs:
one grind step covers ONE kill entry, so multi-objective quests (364
needs 5x Mindless Zombie AND 5x Scarlet Convert) are composed as one
route issue per objective -- the shared quest-complete gate makes the
second issue finish the quest.

### 29. Choice-reward turn-in is silently refused forever when the bot's bags are full -- FIXED (SellJunk step + turn-in refusal diagnosability)

Undead run, quest 3901 (choice reward, shield/dagger): bot standing at
distance 0.0 from Sarvis, quest COMPLETE 8/8 in the DB, turn-in step
stuck in `Acting` phase to the ADR-028 bound, twice. Cause: 16/16
backpack slots -- a session of verified kill+loot cycles fills the
backpack with gray drops, then `CMSG_QUESTGIVER_CHOOSE_REWARD` hits
`Player::CanRewardQuest`, which refuses because the reward item cannot
be stored, and the refusal is only reported to the (headless) client
session -- invisible to the module. Quests with NO reward items (9293,
10302) turn in fine with full bags, which is why this never bit
before. The bound contains it (bot unharmed, route re-issuable), but
no re-issue can succeed until a slot frees.

**FIXED same day, both halves.** (1) `StepType::SellJunk` +
`.autonomousplayer guidestartselljunk <char> <vendorEntry> <x> <y>
<z>`: walk to the nearest live `<vendorEntry>`, sell every gray via
one real `CMSG_SELL_ITEM` per item, finish only when a re-count reads
zero -- checked on step entry, so the step is idempotent and
re-issuable like the rest of ADR-048. Live-verified on two bots on two
continents: `Deathtestbot` at Joshua Kien (grays 11 -> 0, free slots
0 -> 11, money 143c -> 173c) and `Grunttestbot` at Duokna (grays
13 -> 0, free slots 0 -> 13, money 0c -> 189c), plus a clean re-issue
no-op with zero grays. (2) `guidestatus` now prints
`turnInEngineRefused` (the same `CanRewardQuest` predicate the opcode
handler uses, evaluated live during a stuck turn-in) and
`grayItems`/`freeBagSlots`, so this wedge reads as itself instead of a
generic timeout. Honest residuals: `turnInEngineRefused=true` has not
yet been observed live (constructing the wedge again costs a full
grind; the predicate is the handler's own and the healthy-case `false`
is verified); nothing inserts a SellJunk step automatically yet --
route authors must compose it (same authoring-responsibility line as
#28's one-grind-per-objective). One trap found while verifying: the
custom mall-vendor rows in this deployment's DB (`Weapons Vendor`
26309 et al.) are not actually spawned in the live world --
`FindNearestCreature` correctly returns nothing and the step
bound-fails with no `target:` line and `phase=0` (the exact
guidestatus signature of "vendor entry doesn't resolve"). Probe with
`.autonomousplayer creaturestatus <char> <entry>` and use a vendor the
world actually has.

### #14 resolution (2026-07-02): the user watched a bot fly, live

The missing piece #14's entry said it needed -- the user's own eyes at
the exact moment -- arrived: "grunt is floating in air, walking and
falling constantly, flying around, definitely not obeying Z," while
the server reported `Grunttestbot` STATIONARY at `(-764.3, -3825.4,
54.5)` -- 470yd from the boar field, hovering exactly where the
regression suite's unreachable-target test had bound-stopped it
(three suite runs that hour, each ending with an invisible flight).
Root cause, confirmed in core source: `PointMovementGenerator`'s
fallback when the navmesh query fails or returns `PATHFIND_NOPATH` is
`init.MoveTo(x, y, z)` -- a RAW straight-line spline to the literal
destination -- and `MovePoint`'s `forceDestination=true` default
appends unreachable endpoints even when the query partially succeeds.
A server-driven player character has no client applying gravity, so
it flies the line and then hovers. The original underground sighting
is the same mechanism pointed down instead of up. **Fix (a788bb2)**:
`Navigation::MoveTo` -- the module's single `MovePoint` choke point --
now probes the navmesh itself, walks only the reachable portion
(`GetActualEndPosition`), and refuses to move at all on NOPATH;
standing still until the ADR-028 bound fires was always the designed
unreachable-target outcome. Verified: suite 5/5 on the fixed build,
and the unreachable-target test now leaves the bot grounded at its
starting field (Z 39.8) instead of airborne 470yd away (Z 54.5).
Residual, honestly stated: the hover state itself is only entered via
pre-fix history (recovered `Grunttestbot` by GM-teleporting it to
`APBoarCluster`); user visual confirmation of normal-looking walking
post-fix is still pending.

### #29 residual closed organically (2026-07-02, Coldridge arc)

`turnInEngineRefused=true` was observed live without constructing it:
`Dwarftestbot` refilled its bags to 16/16 grinding 12 troggs for quest
170 (dual-objective, choice reward), reached Balir Frosthammer at
distance 0.0 with the quest COMPLETE 6/6+6/6, and `guidestatus` read
`turnInEngineRefused=true grayItems=14 freeBagSlots=0` -- the wedge
reading as itself, exactly as designed. The full recovery loop then
ran with module tools only: `guidestartselljunk` at Adlin Pridedrift
(grays 14 -> 0) -> re-issued quest route (entry gate skips the done
grind) -> turn-in passed, REWARDED (free slots 14 -> 13 = the choice
reward landing). `Gnometestbot` hit the same state and was recovered
the same way preemptively. Route-authoring note from the same runs:
`MoveTo` legs beyond ~200yd can exceed the ADR-028 bound mid-walk
(~20 real seconds) -- re-issuing resumes from wherever the bot got to,
by design, but authors should prefer shorter legs.
