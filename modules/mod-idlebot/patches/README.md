# mod-playerbots patches — NONE REQUIRED (stock upstream)

As of 2026-06-22, mod-idlebot builds against **stock upstream mod-playerbots**
(submodule pinned at `085e127e`, a real fetchable upstream commit). There are **no
out-of-tree playerbots patches** anymore — `git submodule update` works normally.

The fork (`557a75b`) previously carried five changes; here is where each went:

- **force-active** (`PlayerbotAI::SetForceActive` / `_forceActiveBots` + the
  `AllowActive` check) — **removed**. Unnecessary: bots stay active via stock
  `AllowActive` (in-combat bots are always active) plus the module's per-tick drive.
  `IdleBotPlayerbotBridge::SetForceActive` is now a no-op.
- **quest-first** (`SetRpgQuestFirst` / `GetRpgStatusWeight`) — **removed**. Only
  affected organic mode; strict mode drives explicit guide steps.
- **RandomizeManagedBots guard** (`RandomPlayerbotMgr`) — **moved into the module**.
  `IdleBotPlayerbotBridge::MarkBotManaged` sets `SetValue(randomize/teleport/
  change_strategy, 1)` (TTL = `maxRandomBotInWorldTime`, hours) before login and
  refreshes it every tick, so the stock idle-management gates skip our bots.
  Verified live: bots stayed in their starting zones (not random-teleported).
- **OnBotLogin AI re-create** (the "online, no AI" wedge fix) — **moved into the
  module**. `GetLiveStatus` clears the stale entry via the stock public
  `RemoveFromPlayerbotsMap(guid)` then calls `OnBotLogin(p)` to rebuild the AI.
- **botLoading stale-entry expiry** (`AddPlayerBot`) — **DROPPED**, not movable:
  `botLoading` is a private static with no public reset. Risk: a login whose async
  callback leaks strands that bot offline until a worldserver restart. Low frequency
  (the original login-churn cause — the AH-bot GUID collision — is fixed by config).
  Re-add as a ~10-line playerbots patch only if it bites in practice.
