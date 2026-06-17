# mod-playerbots patches required by mod-idlebot

mod-playerbots is a git **submodule**, so these changes can't live in the
mod-idlebot commit. They are kept here as patches and must be applied to the
`modules/mod-playerbots` working tree before building the worldserver image.

## playerbots-skip-managed-bot-idle.patch
Stops `RandomPlayerbotMgr` from auto-**randomizing** (re-gear/re-bag/re-spec) and
random-**teleporting** externally-managed bots (mod-idlebot's, or any altbot not in
the random-bot pool) when they look "idle". Without it, the idle path
(`ProcessBot()`) re-equips ~96 bag slots and yanks the bot to a random map, wrecking
guide progress. Config-gated by `AiPlayerbot.RandomizeManagedBots` (default 1 =
original behaviour); set it to **0** in the live `playerbots.conf` to enable the
guard. This is what makes idlebot bots acquire gear/bags "like a player" (loot +
vendor) per `IdleBot.AutoGear = 0`.

Apply:
```
cd modules/mod-playerbots
git apply ../mod-idlebot/patches/playerbots-skip-managed-bot-idle.patch
```
(Idempotent check: the guard line `RandomizeManagedBots` should appear once in
`src/Bot/RandomPlayerbotMgr.cpp` near the `if (idleBot)` block.)

For permanent/Komodo builds, fold this into the pinned mod-playerbots fork rather
than relying on a working-tree patch.
