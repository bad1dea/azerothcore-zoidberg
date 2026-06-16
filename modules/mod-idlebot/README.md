# mod-idlebot

Private-server-only AzerothCore (3.3.5a) module: a guide-assisted, player-like
playerbot controller. A bot drives a real, visible character to level up using a
recommended leveling **guide** — but the guide is advisory, not a rigid script.

> Scope: local AzerothCore server only. No retail WoW, no auth/anti-cheat bypass,
> no third-party servers.

## Status

Early scaffold. The code here is **interface-and-skeleton**, with honest TODOs
where it must be verified/wired against your actual AzerothCore + mod-playerbots
checkout. It is not yet a working build target.

## Quick start (for Claude Code / contributors)

1. Read `CLAUDE.md` first — it sets the working rules (do not hallucinate APIs).
2. Place this module at `azerothcore-wotlk/modules/mod-idlebot/`.
3. Fill in the version facts in `CLAUDE.md` (AC commit, mod-playerbots fork/commit).
4. Work milestone by milestone (see `docs/ROADMAP.md`), confirming each on the
   live server before moving on.

## Layout

- `src/` — module C++ (manager, command script, bridge, guide model, decision engine stub)
- `conf/` — `mod_idlebot.conf.dist`
- `data/guides/` — YAML leveling guides (start: Northshire 1-6)
- `sql/base/db_characters/` — runtime tables
- `docs/` — architecture, guide format, decision engine, adaptive, social, roadmap
- `tools/questie_export/` — offline Questie data importer (later)

## Control strategy

Prototype drives bots via mod-playerbots **chat commands** behind the
`IdleBotPlayerbotBridge` interface. Direct internal calls can replace the
implementation later without touching code above the bridge.
