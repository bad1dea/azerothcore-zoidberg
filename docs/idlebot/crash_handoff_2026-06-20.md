# IdleBot Crash Handoff - 2026-06-20

## Summary

`ac-worldserver` was crash-looping with `exitCode=139` whenever too many persisted IdleBots were allowed to run concurrently.

The working mitigation now in place is:

- code fix: `IdleBot.MaxActiveBots` is enforced in the normal runtime tick path
- runtime config on zoidberg: `IdleBot.MaxActiveBots = 3`
- result: only 3 bots are allowed online/ticking at once, the 4th stays in standby, and the worldserver remains stable past the old crash window

This does **not** prove the underlying AzerothCore / playerbot visibility bug is fully solved. It proves the old IdleBot cap was ineffective, and enforcing it avoids the crash on this stack.

## What Was Happening

Observed behavior on zoidberg:

- container: `ac-worldserver`
- restart policy: `unless-stopped`
- repeated restarts roughly every 84-112 seconds
- docker events showed repeated `die` events with `exitCode=139`

Typical event pattern:

- startup
- IdleBot queues multiple persisted bots
- bots begin combat / loot / movement in several starter zones
- worldserver segfaults
- container restarts automatically

## Backtrace Evidence

Fresh and older cores both pointed into delayed visibility processing:

- `Object::GetGuidValue`
- `Object::GetGUID`
- `Object::GetDebugInfo`
- `Object::AddToWorld`
- `WorldObject::AddToWorld`
- `Player::IsWorldObjectOutOfSightRange`
- `Acore::VisibleNotifier::SendToSelf`
- `Unit::ExecuteDelayedUnitRelocationEvent`
- `Map::HandleDelayedVisibility`

The inspected object state was corrupted / torn down:

- bogus or unreadable object memory
- stale `TYPEID_UNIT`-looking object
- `m_uint32Values == 0` on one captured core
- later core showed even more corrupted memory and null jump at frame 0

Interpretation:

- this is consistent with a stale world object / use-after-free in visibility handling
- the crash looked like an engine/runtime interaction problem, not a YAML parse issue

## What Was Ruled Out

I tested these cases directly on zoidberg:

1. All bots inactive
- stable
- no restarts across multiple old crash windows

2. Single active bot
- `Idleshaman`: stable
- `Idlepaladin`: stable
- `Idletest`: stable
- `Idlebot`: stable

3. Multi-bot subsets
- dwarf pair: stable
- dwarf pair + shaman: stable
- dwarf pair + undead: stable

4. Four active bots with the old code path
- unstable
- worldserver eventually crash-looped again

Conclusion:

- this was not one broken character, one broken guide row, or one single quest step
- it was a concurrency/load problem in the active-bot runtime path
- the configured cap existed, but it was not actually limiting persisted bots during normal execution

## Root Cause

`IdleBot.MaxActiveBots` only applied to `.idlebot add`.

It did **not** apply in the normal world update loop for persisted bots loaded from `idlebot_bots`.

Before the fix:

- `LoadBots()` restored all persisted active bots
- `Tick()` iterated every active bot
- each active bot could call `EnsureBotOnline()`
- all persisted active bots could end up online and running together
- the configured cap did not protect the server

So the config looked like a concurrency limit, but in practice it was not one.

## Code Fix

Files changed:

- `modules/mod-idlebot/src/IdleBotManager.h`
- `modules/mod-idlebot/src/IdleBotManager.cpp`

Behavior after the fix:

- `Tick()` gathers active, unpaused bots
- sorts them deterministically by bot name
- only the first `IdleBot.MaxActiveBots` records are granted runtime
- bots over the limit are kept in standby
- if an over-limit bot is already online, IdleBot releases it cleanly
- over-limit bots no longer silently exceed the configured concurrency

## Current Live Mitigation on Zoidberg

Applied on remote host:

- `idlebot_bots.active` rows restored to `1` for all 4 test bots
- `env/dist/etc/modules/mod_idlebot.conf` set to:
  - `IdleBot.MaxActiveBots = 3`

Observed after redeploy:

- log shows `maxActiveBots=3`
- only three bots are being queued online
- worldserver has remained stable past the earlier segfault window
- the fourth bot stays out of the active runtime set

## Important Caveat

This is a **working mitigation and correctness fix for IdleBot's own cap semantics**.

It is **not** a proof that the engine-side stale visibility bug is gone under arbitrary higher bot counts.

What we know:

- 4 active concurrent bots was enough to trigger the crash on this environment
- 3 active concurrent bots is stable in current testing
- IdleBot was previously violating its own configured limit

What remains unknown:

- the exact engine-level object lifetime race in visibility handling
- whether a different mix of 4 bots could still crash even with other behavioral changes
- whether the real long-term fix belongs in AzerothCore / playerbots visibility, corpse, or relocation handling

## Recommended Next Work

1. Keep `IdleBot.MaxActiveBots = 3` for now on zoidberg.
2. Leave the runtime cap fix in code.
3. If we want 4+ stable concurrent bots, investigate engine-side visibility/object lifetime further.
4. Add stronger IdleBot telemetry around:
   - bot online/offline transitions
   - standby release decisions
   - playerbot-control attach/detach
5. Consider a future scheduler that rotates standby bots instead of lexicographic first-N.

## Useful Verification Commands

Check server state:

```bash
ssh khuong@10.10.30.20 'docker inspect ac-worldserver --format "state={{.State.Status}} restartCount={{.RestartCount}} startedAt={{.State.StartedAt}}"'
```

Check IdleBot startup/cap behavior:

```bash
ssh khuong@10.10.30.20 'docker logs --since 5m ac-worldserver 2>&1 | grep "\[IdleBot\]" | tail -n 120'
```

Check current active rows:

```bash
ssh khuong@10.10.30.20 'docker exec ac-database bash -lc '\''mysql -N -uroot -p"$MYSQL_ROOT_PASSWORD" acore_characters -e "SELECT id,bot_name,active,guide_id,step_index,step_state FROM idlebot_bots ORDER BY id;"'\'''
```

Check remote cap config:

```bash
ssh khuong@10.10.30.20 'grep -n "^IdleBot.MaxActiveBots" /home/khuong/azerothcore-wotlk/env/dist/etc/modules/mod_idlebot.conf'
```
