# Roadmap

Confirm each milestone running on the live server before starting the next.

- **M0 Environment** — AC builds in Docker; mod-playerbots builds; a bot spawns
  and a connected client can see it move. Record AC + mod-playerbots commits.
- **M1 Empty module** — CMake compiles the module; config loads; `.idlebot help`
  responds in-game.
- **M2 Registry** — `.idlebot add/list/remove/status`; status shows live
  position, level, HP/mana, quest log summary (via bridge).
- **M3 Single-step executor** — move_to, talk_to_npc, accept_quest, turn_in_quest. ✅
- **M4 Combat + recovery** — kill_mobs (grind nudge), loot (playerbots `+loot`
  strategy), death handling state machine (observe/count/persist deaths; let the
  playerbots DeadStrategy recover; nudge revive + graveyard/direct-resurrect
  fallback on stall; pause after a per-step death loop), inventory awareness
  (free-slot + durability guard → repair/sell/maintenance via playerbots actions).
  GO interaction (Q3902 Scavenging Deathknell via GO entry 164662). ✅
- **M5 First guide persists** — `horde-1-12-tirisfal-glades` runs; guide id, step
  index, step state, and death counters persist to `idlebot_bots` and resume after
  a worldserver restart. IdleRPG event feed (TRAVEL/QUEST/COMBAT/LEVEL/DEATH/
  RECOVERY/REPAIR/VENDOR/FAILURE) to per-bot logs + `idlebot_events`; `.idlebot
  summary` / `.idlebot log`. ✅ (verify on live server)
  NOTE: YAML guide loading is still pending — guides remain builtin in C++.
- **M6 Randomization** — bounded random delays, alternate target selection,
  varied waypoints.
- **M7 Adaptive decisions** — decision engine goes live: no-progress detection,
  alternate areas, reroute/pause after repeated deaths, skip optional, grind if
  underleveled.
- **M8 Social/grouping** — OFF by default; nearby player detection; invite/accept
  with cooldowns; shared objective detection.
- **M9 Web/Discord controller** — read status, push goals, stream events.
