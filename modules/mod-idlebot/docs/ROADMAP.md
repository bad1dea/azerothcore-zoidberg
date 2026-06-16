# Roadmap

Confirm each milestone running on the live server before starting the next.

- **M0 Environment** — AC builds in Docker; mod-playerbots builds; a bot spawns
  and a connected client can see it move. Record AC + mod-playerbots commits.
- **M1 Empty module** — CMake compiles the module; config loads; `.idlebot help`
  responds in-game.
- **M2 Registry** — `.idlebot add/list/remove/status`; status shows live
  position, level, HP/mana, quest log summary (via bridge).
- **M3 Single-step executor** — move_to, talk_to_npc, accept_quest, turn_in_quest.
- **M4 Combat** — kill_mobs, loot, rest/recover, death handling.
- **M5 First Northshire guide** — run 2-3 quests; persist progress; resume after
  worldserver restart.
- **M6 Randomization** — bounded random delays, alternate target selection,
  varied waypoints.
- **M7 Adaptive decisions** — decision engine goes live: no-progress detection,
  alternate areas, reroute/pause after repeated deaths, skip optional, grind if
  underleveled.
- **M8 Social/grouping** — OFF by default; nearby player detection; invite/accept
  with cooldowns; shared objective detection.
- **M9 Web/Discord controller** — read status, push goals, stream events.
