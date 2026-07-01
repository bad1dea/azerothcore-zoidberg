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

---

This file will also start recording `PATH_FAILED` / `TRANSPORT_FAILED` /
`TARGET_UNAVAILABLE` / `OBJECTIVE_NO_PROGRESS` / `QUEST_TURNIN_FAILED` /
`COMBAT_TOO_HARD` / `UNSAFE_PACK_DENSITY` / `CORPSE_RECOVERY_FAILED` /
`INVENTORY_BLOCKED` / `TRAINING_BLOCKED` / `GUIDE_INVALID` /
`SCRIPTED_OBJECTIVE_UNSUPPORTED` / `STATE_MIGRATION_FAILED` class failures
once there is a Planner/Executor loop and a working online bot that can
produce them.
