# Active-Bot Crash/Freeze Analysis — Visibility / Combat / Threat

Root-cause writeup for the server crashes that occur when idlebot drives multiple
**force-active** playerbots in heavy combat. For a dedicated core-stability effort
(or Codex deep-research). Pairs with `AUDIT.md` (module overview) and `NOTES.md`.

Date: 2026-06-20. Branch: `idlebot-contested-go-deploy`. Fork:
bad1dea/azerothcore-zoidberg (Playerbot). Host: `khuong@10.10.30.20`, compose
project `azerothcore-wotlk`.

---

## STATUS: RESOLVED (2026-06-21)

All 4 force-active bots run stably and level (verified ~30 min continuous, RC=0,
CPU healthy, Idlebot reached L6). Both crash classes fixed:
- **Visibility — FIXED** (validate via live registry / softened asserts).
- **Combat / threat — FIXED** via a **lock-free** guard in
  `CombatManager::EndCombatBeyondRange`: the combat map is keyed by the other
  unit's GUID (`it->first`), so we re-validate it against the **per-map creature
  store** (`_owner->GetMap()->GetCreature(guid)`) **without dereferencing the
  (possibly dangling) pointer and without any global registry lock**. Stale refs
  are dropped before `EndCombat` can touch freed memory. Runs every
  `Player::Update`, so it self-cleans — and since combat⟺threat are linked, this
  also stops the threat-side UAF (no threat crash observed). The earlier
  `ObjectAccessor`-based attempt deadlocked (global player-registry lock vs
  `MapUpdater::wait`); the per-map store avoids that. The deployed image now
  contains this fix and is safe with bots active.

Below is the original investigation (kept for reference).

## TL;DR (original)

Running ~4 force-active idlebot bots fighting constantly crashed the worldserver
~95–100s after they went active. It was **not one bug** — a **class of
use-after-free / desync bugs** in this fork's bookkeeping, which stores **raw
`WorldObject*` / `Unit*` pointers** in per-object maps. Under bot churn (units
freed/removed/relocated while still referenced) those pointers dangle, and
different subsystems fault on them.

---

## Reproduction

1. `UPDATE acore_characters.idlebot_bots SET active=1;` (4 bots, strict mode, L1 at
   their race spawns), ensure `IdleBot.MaxActiveBots >= 4` in `mod_idlebot.conf`.
2. Recreate worldserver. Bots log in, quest, fight, loot.
3. ~95–100s later: SIGSEGV (pre-combat-fix) or freeze (with the reverted-out combat fix).
   Disable bots (`active=0`) + restart → stable.

Cores land at `/azerothcore/core.1` (6+ GB, fixed name — each crash overwrites).
gdb is NOT in the image (wiped on every full rebuild); `apt-get update && apt-get
install -y gdb` inside the container to analyze. Live `gdb -p` attach is blocked
(no `CAP_SYS_PTRACE`); use `kill -ABRT <pid>` to force a core of a frozen process
(but the abort handler itself can deadlock in malloc — see Freeze section).

---

## The four faults observed (with backtraces)

### 1. Visibility UAF — `VisibleNotifier::SendToSelf` (FIXED)
```
Object::GetGuidValue (this=freed)            Object.cpp:329
 ← Object::GetGUID / GetDebugInfo
 ← Player::IsWorldObjectOutOfSightRange       Player.cpp:16409
 ← Acore::VisibleNotifier::SendToSelf         GridNotifiers.cpp (visibleWorldObjects loop)
 ← Unit::ExecuteDelayedUnitRelocationEvent    Unit.cpp:16416
 ← Map::HandleDelayedVisibility / Map::Update Map.cpp
 ← MapUpdater::WorkerThread
```
A `Creature` (killed mob) removed-from-world (`m_inWorld=false`, value arrays freed)
was still in the bot's `ObjectVisibilityContainer::_visibleWorldObjectsMap` (a
`std::unordered_map<ObjectGuid, WorldObject*>` of **raw pointers**). The relocation
visibility scan dereferenced it.

### 2. Visibility UAF — `DoForAllVisibleWorldObjects` (FIXED)
```
Object::HasFlag (this=freed) → Unit::HasNpcFlag
 ← Player::UpdateForQuestWorldObjects lambda   PlayerUpdates.cpp:1807
 ← WorldObject::DoForAllVisibleWorldObjects     Object.h
 ← Player::EquipItem → SwapItem
 ← EquipAction::EquipItem (mod-playerbots)
```
Same dangling container entry, dereferenced by a *different* reader (triggered when
a bot equips an item). Confirms: **every** reader of that container is vulnerable.

### 3. Combat assert — `CombatManager::PutReference` (softened; see Status)
```
ASSERT(!inMap) "Duplicate combat state ... memory leak!"  CombatManager.cpp:395
 ← CombatManager::SetInCombatWith
 ← Spell::HandleLaunchPhase (a bot casting a spell entering combat)
```
`SetInCombatWith` only checks `_owner`'s side for an existing reference, not the
target's side. Under churn the combat link goes **asymmetric** (target references
the bot but not vice-versa), so it creates a new ref and inserting into the
target's map trips the duplicate assert.

### 4. Combat/threat UAF — `EndCombat` → `ClearThreat` (NOT FIXED)
```
Object::GetGuidValue (this=freed)             Object.cpp:328
 ← ThreatManager::ClearThreat(target=freed)    ThreatManager.cpp:562
 ← CombatReference::EndCombat                   CombatManager.cpp:78
 ← CombatManager::EndCombatBeyondRange          CombatManager.cpp:303
 ← Player::Update                               PlayerUpdates.cpp:403
 ← Map::Update ← MapUpdater::WorkerThread
```
A `CombatReference` (raw `Unit* first/second`, NO GUIDs) survives in the bot's
combat map pointing at a unit that was freed without the reference being torn down.
`EndCombatBeyondRange` then derefs the freed unit. **This is the current blocker.**

### 5. FREEZE (regression from the attempted combat fix — REVERTED)
The attempted fix made `EndCombat` re-resolve endpoints via
`ObjectAccessor::FindPlayer`/`GetUnit`. Those take the player-registry / map locks.
`EndCombat` runs on the **map-update worker thread**, while the **main thread**
blocks in `MapUpdater::wait()` (and elsewhere holds registry write locks during
logout/add). Result: lock-ordering **deadlock** → server froze (CPU ~4%, no logs,
no restart) for 73 min. A SIGABRT core showed the worker stuck in
`je_malloc_mutex_lock` and main in `MapUpdater::wait` (partly an abort-handler
artifact). **Lesson: do NOT call `ObjectAccessor`/registry-locking APIs from the
combat/threat/visibility hot path on the map thread.** A freeze is worse than a
crash (no auto-recovery).

---

## Root cause

This fork stores **raw object pointers** in bidirectional bookkeeping maps:
- `ObjectVisibilityContainer`: `VisibleWorldObjectsMap` (player→objects) and
  `VisiblePlayersMap` (object→players).
- `CombatManager`: `_pveRefs` / `_pvpRefs` (guid→`CombatReference*`), each
  `CombatReference` holding raw `Unit* first/second`.
- `ThreatManager`: threat refs by guid.

When a unit/object is freed under heavy bot activity, these cross-references are
not always torn down symmetrically/completely, leaving dangling pointers that the
map-update thread later dereferences. Triggers include: killed-mob despawn, bot
relocation, and (most aggressively) **force-active** bots, which run full AI every
tick and churn combat/visibility far harder than throttled random bots.

Threading note: `MapUpdate.Threads=1` (one map worker thread); `MapMgr::Update`
schedules to it and `wait()`s. Bot AI runs in `Player::Update` on that worker;
bot logout/removal is queued to the main thread (PlayerbotWorldThreadProcessor in
`WorldScript::OnUpdate`). The hot map-thread paths must not take locks the main
thread can hold across the `wait()` — that is what deadlocked.

---

## What is fixed (kept in source, NOT yet deployed)

All in the core tree; rebuild required (static build).

- **`GridNotifiers.cpp`** — `VisibleNotifier::SendToSelf`: before dereferencing each
  `visibleWorldObjects` entry, re-resolve its GUID via
  `ObjectAccessor::GetWorldObject` (the notifier is NOT on the deadlock-prone
  path); drop stale entries. (Visibility notify runs on the worker but this
  particular lookup did not deadlock in testing — re-verify under load.)
- **`Object.h` / `Object.cpp`** — `DoForAllVisibleWorldObjects` template guards every
  entry via new helper `WorldObject::GetValidatedVisibleObject(guid, stored)`
  (defined in `Object.cpp`), covering all 4 `DoForAll` callers. `DestroyForVisiblePlayers`
  validates each `Player*` via `ObjectAccessor::FindPlayer` before deref.
- **`ObjectVisibilityContainer.cpp`** — `CleanVisibilityReferences` validates pointers
  before deref; the fatal `ASSERT`s (`DirectRemoveVisibilityReference`,
  `UnlinkVisibilityFromPlayer`, destructor) softened to defensive guards so a stale
  ref no longer aborts the server (notably on shutdown `KickAll` → `LogoutAllBots`).
- **`CombatManager.cpp`** — `PutReference`: the duplicate-state `ASSERT`s replaced with
  "replace the stale entry + LOG_DEBUG" so asymmetric combat state no longer aborts.

⚠️ Caveat: the visibility fixes also call `ObjectAccessor` (read locks). They did
NOT deadlock in testing, but the same lock-ordering hazard exists in principle.
Re-verify the visibility-fixed build under load BEFORE trusting it, and watch for a
freeze (not just a crash).

## What was reverted (deadlocked)

- **`CombatManager.h` / `.cpp`** — the `CombatReference` GUID members + the `EndCombat`
  rewrite that validated via `ObjectAccessor`. Reverted to original. See fault #5.

---

## Remaining work (combat/threat) — recommended directions

The combat/threat UAF (fault #4) is unsolved. Options, roughly in order of
soundness:

1. **Fix the source of dangling combat refs (best).** Ensure that when ANY unit is
   removed/freed, ALL its combat + threat references are torn down symmetrically
   *before* the object is freed. Audit `Unit::CleanupBeforeRemoveFromMap` /
   `RemoveFromWorld` / `CombatStop` / `setDeathState(JUST_DIED)` and creature
   despawn/grid-unload paths for the gap that lets a ref survive its referent. This
   is lock-free (runs in the normal removal flow) and fixes the root.
2. **Lock-free validation in `EndCombat`.** Store `firstGuid`/`secondGuid` on
   `CombatReference` (constructor must be defined in `.cpp`; `Unit` is only
   forward-declared in `CombatManager.h`) and validate **without** `ObjectAccessor`
   — e.g. a generation counter / "alive" epoch on Unit set at destruction, checked
   lock-free. Avoid any registry/map lock on the map thread.
3. **Serialize the churn.** Investigate whether routing idlebot bots through
   `RandomPlayerbotMgr` (master-less `AddPlayerBot(guid,0)`) + force-active causes
   avoidable removal/re-add churn; reducing churn shrinks the dangling-ref window
   (mitigation, not a fix).

Also re-verify there are no further subsystems with the same raw-pointer pattern
(grep for maps of `Unit*`/`WorldObject*` keyed by guid).

## Constraints learned (do NOT repeat)

- **No `ObjectAccessor` / registry-locking calls on the map-update hot path**
  (visibility/combat/threat) — deadlocks against `MapUpdater::wait()` + main-thread
  registry writes. Validate lock-free or fix at the removal source.
- A **freeze is worse than a crash** here (crash auto-restarts in ~100s; freeze
  hangs until manual restart). Prefer the crash over a risky lock-based "fix".
- Header changes (`Object.h`, `CombatManager.h`) trigger ~20–30 min full rebuilds;
  `.cpp`-only changes are faster. Build with `docker compose build --progress=plain`
  and grep for `error:` — buildkit hides compiler errors behind "failed to solve".

---

## Build / deploy / debug runbook

```bash
# sync changed core files (from repo) to the build host
tar -cf - src/server/game/<paths> | ssh khuong@10.10.30.20 'tar -xf - -C /home/khuong/azerothcore-wotlk'
# build (capture errors!)
ssh khuong@10.10.30.20 'cd /home/khuong/azerothcore-wotlk && docker compose build --progress=plain ac-worldserver 2>&1 | grep -iE "error:|Built|exit code"'
# deploy
ssh khuong@10.10.30.20 'cd /home/khuong/azerothcore-wotlk && docker compose up -d --no-deps --force-recreate ac-worldserver'
# bots on/off (acore_characters DB)
docker exec ac-database mysql -uroot -p<PW> -e "UPDATE acore_characters.idlebot_bots SET active=0;"
# analyze a core
docker exec -u root ac-worldserver sh -c 'apt-get update -qq && apt-get install -y -qq gdb; cd /azerothcore && gdb -batch -nx -ex "bt 16" env/dist/bin/worldserver core.1'
```

## Immediate next step for whoever picks this up

1. Rebuild from current source (visibility fixes + PutReference soften; combat
   EndCombat reverted) and deploy — this removes the freeze regression from the live
   image. Keep bots off until verified.
2. With that deployed, the open blocker is the combat/threat UAF (fault #4). Pursue
   "Remaining work" option 1 (symmetric teardown at the removal source).
