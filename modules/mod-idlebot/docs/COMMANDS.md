# Commands

GM-gated when `IdleBot.AllowGMOnly = 1`.

## Implemented (M1–M5)
- `.idlebot help`
- `.idlebot list`
- `.idlebot add <botName>`
- `.idlebot remove <botName>`
- `.idlebot status <botName>` — live position, level, HP/mana, quest count
- `.idlebot summary <botName>` — IdleRPG summary: level/XP, location, bags,
  durability, current guide step/objective, death totals
- `.idlebot log <botName> [lines]` — tail the bot's per-bot event log (default 15,
  max 50)
- `.idlebot goto <botName>` / `.idlebot teleport <botName>` — teleport yourself
  to a live registered bot
- `.idlebot pause <botName>`
- `.idlebot resume <botName>` — also clears a death-loop `blocked` state
- `.idlebot guide set <botName> <guideId>`
- `.idlebot guide clear <botName>`
- `.idlebot guide current <botName>` — current guide, step index, objective
- `.idlebot guide reset <botName>` — restart the guide at step 1
- `.idlebot guide step <botName> <n>` — jump to step n (1-based)

Guide progress, step state, and death counters persist to `idlebot_bots` and
resume across a worldserver restart.

## Later milestones
- `.idlebot goal <botName> level zone "Elwynn Forest" guide human_elwynn_1_10`
- `.idlebot mode <botName> strict|assisted|autonomous|sandbox`
- `.idlebot step <botName>`
- `.idlebot stop <botName>`
- `.idlebot debug <botName>`
- `.idlebot decision <botName>`
- `.idlebot failures <botName>`
- `.idlebot guide list`
- `.idlebot guide validate <guideId>`
- `.idlebot social <botName> on|off`
