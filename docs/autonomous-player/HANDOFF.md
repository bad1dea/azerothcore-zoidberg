# Session Handoff

## Current milestone
Gate 1, first slice: bring one configured level-1 Orc Warrior online and
expose a read-only perception snapshot for it. **Not complete** — blocked
on a session-lifetime bug found via live testing (see Known failures).

## Completed this session
- Researched and implemented a bot account/character/session model
  (ARCHITECTURE.md ADR-008): a dedicated bot-owning account, a
  `sock = nullptr, is_bot = true` `WorldSession` (this fork's core already
  supports null-socket sessions structurally), and character
  creation/login driven through the same public production opcode
  handlers (`HandleCharCreateOpcode`/`HandlePlayerLoginOpcode`) a real
  game client uses, via synthesized packets — not direct DB writes or a
  reimplemented login/creation path.
- `Setup/BotProvisioning.{h,cpp}`: `EnsureBotAccount`, `CreateBotSession`,
  `SubmitCharacterCreate`. `Lifecycle/BotLogin.{h,cpp}`: `TryLoginBot`.
  `Commands/cs_autonomousplayer.cpp`: `.autonomousplayer
  provision|login|status` admin/console commands (`SEC_ADMINISTRATOR` /
  `SEC_GAMEMASTER`).
- Wired `BotLifecycleMgr` registration to the real `PLAYERHOOK_ON_LOGIN`/
  `_LOGOUT` hooks, gated on `WorldSession::IsBot()`.
- **Live-tested on zoidberg and found two real bugs, both fixed and
  reverified by rebuild:**
  1. `WorldSession::IsBot()` is not exclusive to this module —
     mod-playerbots sets it on its own large random-bot pool too, so the
     login hook was registering hundreds of their bots and spamming
     perception logs. Fixed with account-name-prefix ownership
     (`Setup::AccountPrefix`, `Setup::IsAutonomousPlayerAccount`).
  2. The original prefix (`"autonomous_player_"`, 19 chars) was longer
     than `AccountMgr::MAX_ACCOUNT_STR` (17) by itself. Shortened to
     `"ap_"`.
  3. Also fixed `check_no_playerbots_dependency.sh` to strip `//` comments
     before matching (it was flagging bug-1's own explanatory comments as
     a false-positive "Playerbots dependency").
- **Found a third, more fundamental bug that is not yet fixed** (see Known
  failures / ARCHITECTURE.md ADR-008): the `sock = nullptr` bot session
  does not survive past one `WorldSessionMgr::UpdateSessions` tick in this
  fork's current core, which silently orphans the async character-creation
  (and would orphan login) DB query chain. This is why the character was
  never actually created despite the command reporting success.
- Rolled back the live zoidberg deploy to the pre-session image once this
  was diagnosed (saved digest, retagged, force-recreated
  `ac-worldserver`). Verified healthy afterward (back on
  `idlebot-contested-go-deploy` @ `ade9279`, "ready...", no errors).

## Files changed
- `docs/autonomous-player/ARCHITECTURE.md`: ADR-008 (bot account/
  character/session model) plus all three live-testing findings above,
  including the unresolved one and its two candidate fixes.
- `docs/autonomous-player/KNOWN_FAILURES.md`: the unresolved session-
  survival bug, with reproduction evidence.
- `modules/mod-autonomous-player/src/Setup/BotProvisioning.{h,cpp}` (new),
  `Lifecycle/BotLogin.{h,cpp}` (new),
  `Commands/cs_autonomousplayer.cpp` (new),
  `Lifecycle/BotLifecycleMgr.{h,cpp}` (added `GetRegisteredBotGuids`),
  `AutonomousPlayerModule.cpp` (login/logout hooks, periodic perception
  logging), `mod_autonomous_player_loader.cpp` (registers the new command
  script).
- `modules/mod-autonomous-player/tools/check_no_playerbots_dependency.sh`:
  comment-aware matching.

## Verification
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`,
  `codestyle-cpp.py`: all pass, every commit this session.
- Compiled clean on zoidberg 4 times this session (once per fix
  iteration); final state (commit `97c3013`) compiles clean and was
  deployed live.
- **Live behavior does not yet meet Gate 1's acceptance criteria** — see
  Known failures. The bot account exists (`ap_test1`, id 204); the
  character does not.

## Current repository state
- Branch: `mod-autonomous-player`, 4 new commits this session
  (`bb7e2f1`, `b7b6258`, `97c3013`, plus this handoff-finalization commit),
  pushed to origin.
- zoidberg's live `ac-worldserver` is back on the pre-session image
  (`idlebot-contested-go-deploy` @ `ade9279`) — this session's code is
  **not** currently deployed.
- zoidberg's `acore_auth` DB has one extra row: account `ap_test1` (id
  204), no characters, no password known to anyone but this session's
  command (harmless test fixture, safe to leave or delete next session).
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing, not from this session) are unchanged — same list
  as Gate 0's handoff: `modules/mod-ah-bot-plus`, `modules/mod-playerbots`
  (submodule pointer), `tools/dashboard/backend/routers/bots.py`,
  `tools/dashboard/**` untracked, `reports/`, and the stray heredoc-
  artifact file.

## Known failures
See `docs/autonomous-player/KNOWN_FAILURES.md` Gate 1 section: bot
sessions don't survive past one world tick, orphaning async character
creation/login. Root-caused to an unconditional `if (!m_Socket) return
false;` in `WorldSession::Update()`. Not yet fixed.

## Decisions made
- Live-tested on zoidberg per user's explicit approval this session ("it
  can run on the live realm, it's just a testing server" — create a test
  account and characters there). Chose account name `ap_test1` (short
  prefix `ap_`, not the `idlebot` name the user first suggested, to avoid
  confusion with the separate, removed idlebot project — flagged and
  confirmed with the user before creating the branch; same reasoning
  applied here without re-asking since it's the same naming concern).
- Rolled back the live deploy rather than leaving the broken build
  running, since it has no working bot functionality yet and there's no
  reason to keep an unverified build live over the known-good one.
- Did not attempt to patch core this session to fix the session-survival
  bug — it's a real architectural decision (which of the two candidate
  fixes in ADR-008, or a third option) that deserves its own session
  rather than a rushed patch at the end of an already-long one.

## NEXT TASK
Fix bot session survival across world ticks, then re-verify the full
Gate 1 slice live on zoidberg.

Scope for that session:
1. Read ARCHITECTURE.md ADR-008's "third live-testing catch" section in
   full before writing any code.
2. First, spend a short amount of time (read-only) trying to understand
   *why* mod-playerbots' own bot sessions survive the same
   `WorldSession::Update()` code path — this should make the fix choice
   obvious rather than guessed. This is inspection of core behavior, not
   a Playerbots dependency (see ADR-006) — reading how a public core code
   path behaves in the presence of Playerbots' bots (e.g. via
   `docker logs`, or reading Playerbots' own source *only* to understand
   what session-construction pattern it uses, not to copy it) is fair
   game; writing any code that includes or calls Playerbots source is
   not.
3. Implement whichever fix that inspection points to: most likely a
   small, explicitly-documented core patch to
   `src/server/game/Server/WorldSession.cpp` exempting `_isBot` sessions
   from the null-socket eviction (candidate fix 1 in ADR-008), unless the
   Playerbots inspection reveals they use a real/loopback socket instead
   (candidate fix 2).
4. Re-run the exact same live sequence that failed this session:
   `.autonomousplayer provision ap_test1 <password> GruntTestbot 2 1 0`
   (account already exists from this session, so this will skip straight
   to character creation) → confirm the character actually appears in
   `acore_characters.characters` this time → `.autonomousplayer login
   ap_test1 GruntTestbot` → confirm via `.autonomousplayer status` and the
   perception log that it's online, level 1, alive, at the correct Orc
   starting position (Valley of Trials, Durotar, mapId 1).
5. Update ADR-008 with the actual fix and why; update
   `docs/autonomous-player/TEST_MATRIX.md`; move the Gate 1
   `KNOWN_FAILURES.md` entry to fixed (cite the commit) once verified.

## Next-session acceptance criteria
- A configured level-1 Orc Warrior bot logs in at its correct racial
  starting location without any teleport call, and the character/session
  remain alive across multiple world ticks (not destroyed after one).
- `BotLifecycleMgr::IsRegistered` is true for that bot's GUID after login
  and false after logout.
- A `PerceptionSnapshot` built for that bot on a live tick reports the
  correct `CharacterGuid`, `Level` (1), `MapId`, position matching the
  spawn location, and `IsAlive == true` — observed via the `.autonomousplayer
  status` command and/or the periodic perception log line, live on
  zoidberg.
- `check_no_playerbots_dependency.sh` and `check_no_forbidden_apis.sh`
  still pass.
- Change is committed on the `mod-autonomous-player` branch;
  `docs/autonomous-player/HANDOFF.md` updated with this session's
  completed work and the next bounded task.

## Recommended next-session prompt
Read the project files and complete the NEXT TASK in this handoff. Build,
test, commit, and update this handoff. Do not begin later roadmap work.
