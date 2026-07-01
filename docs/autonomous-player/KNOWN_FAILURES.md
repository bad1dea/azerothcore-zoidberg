# Known Failures

Reproducible failures, evidence, and classification. Append-only within a
gate; prune entries once a fix is verified and cite the fixing commit
instead of deleting silently.

## Gate 0

None — no runtime behavior existed to fail.

## Gate 1

### QUEST_ACCEPT_FAILED-adjacent: bot session doesn't survive past one tick

**Not yet fixed — this is the Gate 1 blocker.**

- **Symptom:** `.autonomousplayer provision <account> <password> <charname>
  <race> <class> <gender>` returns "Submitted character creation..." but no
  character row is ever written to `acore_characters.characters`, and no
  error or success log line appears anywhere (`entities.player.character`,
  `module.autonomous_player`, or otherwise).
- **Evidence:** Reproduced live on zoidberg 2026-06-30, account `ap_test1`
  (id 204) created successfully, character `GruntTestbot` never appeared
  in the DB after 8+ seconds. Confirmed via direct
  `SELECT ... FROM acore_characters.characters WHERE account=204` —
  0 rows.
- **Root cause:** `WorldSession::Update()`
  (`src/server/game/Server/WorldSession.cpp`, ~line 605) has an
  unconditional `if (!m_Socket) { return false; }` after
  `ProcessQueryCallbacks()`. `WorldSessionMgr::UpdateSessions` deletes any
  session whose `Update()` returns false. A `sock = nullptr` bot session
  (per ADR-008) survives exactly one `WorldSessionMgr` tick, then is
  destroyed — orphaning `HandleCharCreateOpcode`'s multi-hop async DB
  query chain before its results return. The same would block
  `Lifecycle::TryLoginBot` once exercised for the same reason.
- **Classification:** Architecture gap in ADR-008's session model, not a
  simple bug. See ARCHITECTURE.md ADR-008's "third live-testing catch" for
  the two candidate fixes and the open question (how does mod-playerbots'
  own bot pool survive this same code path?) to resolve before picking
  one.
- **Status:** Live deploy rolled back to the pre-session image. Fixing
  this is the Gate 1 `NEXT TASK` (see HANDOFF.md).

---

This file will also start recording `PATH_FAILED` / `TRANSPORT_FAILED` /
`TARGET_UNAVAILABLE` / `OBJECTIVE_NO_PROGRESS` / `QUEST_TURNIN_FAILED` /
`COMBAT_TOO_HARD` / `UNSAFE_PACK_DENSITY` / `CORPSE_RECOVERY_FAILED` /
`INVENTORY_BLOCKED` / `TRAINING_BLOCKED` / `GUIDE_INVALID` /
`SCRIPTED_OBJECTIVE_UNSUPPORTED` / `STATE_MIGRATION_FAILED` class failures
once there is a Planner/Executor loop and a working online bot that can
produce them.
