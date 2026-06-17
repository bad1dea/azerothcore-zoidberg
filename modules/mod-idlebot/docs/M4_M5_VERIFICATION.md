# M4/M5 verification checklist

Coded 2026-06-17. The module is **static** — nothing here is testable until a
Komodo build of the worldserver target is deployed. Run this after deploy.

## 0. Build / migrate
- [ ] Komodo build `ac-worldserver-zoidberg` (worldserver target) succeeds with
      the M4/M5 patch (this is the first real compile — watch for errors).
- [ ] `ac-module-sql` one-shot applied `data/sql/characters/base/idlebot_tables.sql`
      and `.../updates/2026_06_17_00_idlebot_progress_death.sql`.
- [ ] `DESCRIBE idlebot_bots;` shows new columns: `guide_id`, `step_index`,
      `step_state`, `death_count_total`, `death_count_current_step`, `updated_at`.
      (Migration is idempotent — safe if it ran twice.)
- [ ] worldserver boots clean; log shows `[IdleBot] initialized` + `loaded N
      persisted bot(s)` + `registered M builtin guide(s)`.

## 1. Config
- [ ] `IdleBot.DeathHandling.*`, `IdleBot.Inventory.*`, `IdleBot.TownMaintenance.*`
      read from `mod_idlebot.conf` (copy from `.dist`).

## 2. Commands
- [ ] `.idlebot summary <bot>` shows level/XP, location, bags, durability, guide
      step/objective, deaths.
- [ ] `.idlebot log <bot> 20` tails the per-bot event log.
- [ ] `.idlebot guide current/reset/step <bot>` work; `step` is 1-based.

## 3. Guide run (horde-1-12-tirisfal-glades)
- [ ] Bot accepts/turns in Q364/376/3901 around Deathknell (TRAVEL/QUEST/COMBAT
      events in the log).
- [ ] **Q3902 Scavenging Deathknell completes**: bot walks to the equipment-box
      cluster (~1900,1545,88), uses boxes (GO 164662), quest goes Complete, turns
      in to Saltain. (If it stalls, confirm GO entry 164662 + spawn presence in
      `gameobject` on this DB; adjust search coords/radius in the guide.)

## 4. Death recovery
- [ ] Kill the bot (`.die`/pull a pack). Log shows `[DEATH]`, then the playerbots
      DeadStrategy releases + corpse-runs; on success `[RECOVERY] back on its feet`.
- [ ] Force a stall (block the corpse run): after `GhostStallTicks` the log shows
      `[RECOVERY] corpse-run nudge`, then graveyard/`direct-resurrected`.
- [ ] Repeated deaths on one step (≥ `MaxDeathsPerStep`) → `[FAILURE] … pausing`;
      bot is `[paused]`. `.idlebot resume <bot>` clears the block and retries.

## 5. Persistence
- [ ] Mid-guide, restart worldserver. `.idlebot guide current <bot>` shows the
      SAME step index (not reset to 1). `death_count_total` preserved.

## 6. Inventory / maintenance
- [ ] During Brill grind legs near a vendor/repairer, bags-low/durability-low
      triggers `[VENDOR]`/`[REPAIR]` events. Away from town it silently proceeds
      (town routing is a later milestone — expected).

## Known limitations to expect
- Long-haul movement uses raw MovePoint (can wander); TravelMgr integration TBD.
- Repair/vendor/trainer/maintenance only succeed near the relevant NPC.
- Guides are still builtin C++ (YAML loader pending).
