# CLAUDE.md — mod-idlebot

This file governs how you (Claude Code) work in this repository. Read it fully before doing anything.

## What this project is

`mod-idlebot` is a **private-server-only** AzerothCore (WoW 3.3.5a) C++ module. It is an
interactive "guide-assisted playerbot controller": it drives a real, visible playerbot
character around the world to level up using a recommended leveling *guide*, but the guide
is **advisory, not mandatory** — the long-term goal is a player-like decision agent.

This runs against a local AzerothCore server the owner controls. It does NOT touch retail
WoW, retail auth, anti-cheat, protected services, or any third-party server. Do not write
anything that does.

## The single most important rule: DO NOT HALLUCINATE APIs

You do not reliably know the exact AzerothCore or mod-playerbots API surface, and your
training data may be stale or wrong for the user's specific fork/commit. Therefore:

- **Never invent a function signature and present it as real.** If you need an AzerothCore
  or mod-playerbots call and you are not certain it exists with that exact signature in
  THIS checkout, mark it `// TODO(verify): likely <Class>::<method>(...) — confirm in local source at <path>`.
- Before using any AzerothCore API, **grep the actual source tree** (it should be at
  `../..` relative to this module, i.e. the `azerothcore-wotlk` root, or wherever the user
  points you). Confirm the symbol exists. Cite the file/line you found it in.
- Before using any mod-playerbots behavior, **grep `../mod-playerbots`** for the actual
  command names and entry points. Forks differ. Do not assume `liyunfan1223` syntax without
  checking.
- If you cannot find it, say so plainly and leave a TODO. Do not fake success.

## Control strategy for the prototype: CHAT COMMANDS

The first prototype drives bots via mod-playerbots **chat commands**, not direct internal
calls. Reason: it is the most stable surface and does not require recompiling worldserver
to iterate on bot logic. `IdleBotPlayerbotBridge` is the seam — its public interface
(MoveTo, AcceptQuest, etc.) stays stable; the prototype implementation issues chat commands
underneath. A later implementation can swap in direct calls WITHOUT changing the decision
engine or executor. Keep that boundary clean.

When you implement the bridge, first grep mod-playerbots for the exact command strings and
how a master issues them programmatically. Do not guess the command syntax.

## Build / environment facts (zoidberg stack)

- Host: `zoidberg` (Site A), Komodo-managed Docker Compose. AC + modules compiled
  **static** into a custom worldserver image.
- AzerothCore: **Playerbot fork**, `github.com/bad1dea/azerothcore-zoidberg` @ branch
  `Playerbot`. Last-built AC revision: `c1267b09` (built 2026-06-15, Ubuntu 24.04,
  clang, RelWithDebInfo, `-DSCRIPTS=static -DMODULES=static`, Boost static).
  Re-run `git log -1` to confirm the current commit before integration work.
- mod-playerbots: ike3 lineage, bundled in the fork's `modules/mod-playerbots/`,
  drives `acore_playerbots` DB. Autologin on (`AC_AI_PLAYERBOT_RANDOM_BOT_AUTOLOGIN=1`).
- This module lives at `modules/mod-idlebot/` inside that fork.
- Build image: Komodo Build `ac-worldserver-zoidberg` → `apps/docker/Dockerfile`,
  `worldserver` target → `ghcr.io/bad1dea/ac-worldserver-zoidberg:latest`.

## CRITICAL: this is a STATIC-MODULE build — no hot-loading

Modules are compiled into the worldserver image at build time (`-DMODULES=static`).
Editing source here does NOT change a running server. The loop is:
  edit modules/mod-idlebot/ → Komodo Build (worldserver target) → redeploy stack.
There is no runtime module reload. Plan changes in batches; a rebuild is required
to test anything. Module SQL is applied by the `ac-module-sql` one-shot, which
loops modules/*/data/sql/{world,characters}/{base,updates}/ — idlebot's tables
are at data/sql/characters/base/ to match.

## Hard coding constraints

- **Never block the worldserver thread.** No long loops, no sleeps, no synchronous waits in
  the tick path. Do one small action per tick. If you think you need to block, you're wrong —
  redesign as a state machine that advances across ticks.
- SQL must be MySQL/MariaDB compatible.
- C++ style consistent with AzerothCore (their formatting, their ScriptMgr registration pattern).
- Small files, clean interfaces. Don't dump giant guide data into code.
- Don't fake success anywhere. A TODO that's honest beats code that looks done but isn't.

## Build order (do not skip ahead)

Work milestone by milestone. Get each one actually running on the user's server before the next.

- **M1**: Empty module compiles, config loads, `.idlebot help` works in-game.
- **M2**: Bot registry — add/list/remove/status. Show position, level, HP/mana, quests.
- **M3**: Single-step executor: move_to, talk_to_npc, accept_quest, turn_in_quest.
- **M4**: Combat objective: kill_mobs, loot, recover, death handling.
- **M5**: First Northshire guide runs 2–3 quests; progress persists across restart.
- **M6+**: Randomization, then the decision engine, then social/grouping (disabled by default), then web/Discord.

Do NOT build the decision engine or social features until a bot can take and turn in one
real quest. The interface stubs exist now; the logic comes later.

## When in doubt

Ask the user, or grep the source, or leave an honest TODO. Never paper over uncertainty.
