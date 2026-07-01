# mod-autonomous-player

A clean-room AzerothCore module that builds autonomous player characters:
bots that level legitimately from character creation and their first
racial starting quest to a configured level cap (60 Classic / 80 Wrath),
playing like a real human player — questing, fighting with class abilities,
looting, equipping, training, traveling by every legitimate means, dying and
recovering, and resuming correctly after server restarts.

This module does **not** copy, call, wrap, link, depend on, or adapt
`mod-playerbots`, `PlayerbotAI`, or any Playerbots-derived code. See
`tools/check_no_playerbots_dependency.sh`.

Full project documentation, architecture decisions, roadmap, and the current
session handoff live in `docs/autonomous-player/` at the repo root (not
inside this module directory, since they describe project process rather
than shippable module content):

- `docs/autonomous-player/PROJECT.md` — charter, constraints, player-like
  policy summary.
- `docs/autonomous-player/ARCHITECTURE.md` — component boundaries and ADRs.
- `docs/autonomous-player/ROADMAP.md` — gate structure and active weekly
  outcome.
- `docs/autonomous-player/HANDOFF.md` — current session state and the next
  bounded task.
- `docs/autonomous-player/KNOWN_FAILURES.md`, `TEST_MATRIX.md`.

## Status

Gate 0 (project foundation): module builds and loads standalone, with
`Lifecycle`/`Perception`/`Telemetry` skeletons. No gameplay behavior yet —
see `HANDOFF.md` for the next task (Gate 1: online Orc Warrior + read-only
perception snapshot).

## Config

See `conf/mod_autonomous_player.conf.dist`.

## Checks

`tools/check_no_playerbots_dependency.sh` — greps this module's own source
for any Playerbots dependency (include paths, symbol usage, copy-paste
markers). Run it from the repo root or from this directory; exits non-zero
on a hit.
