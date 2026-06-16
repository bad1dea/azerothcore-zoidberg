# Architecture

```
User command (.idlebot ...) / later: Discord / Web UI
    -> IdleBotCommandScript            (parse, GM-gated)
    -> IdleBotManager                  (registry + non-blocking tick loop)
    -> Guide planner (GuideLoader)     (YAML -> in-memory Guide)
    -> IdleBotDecisionEngine           (choose next action; stub until M7)
    -> Step executor                   (one small action per tick)
    -> IdleBotPlayerbotBridge          (THE seam; chat-cmd impl first)
    -> mod-playerbots / Player object
    -> AzerothCore worldserver simulation
    -> idlebot_events / snapshots      (telemetry + resumable state)
```

## Key principles

- **Bridge is the seam.** Nothing above it knows whether bots are driven by chat
  commands or direct mod-playerbots calls. Swapping the implementation must not
  touch the manager, executor, or decision engine.
- **Never block the world thread.** The tick does at most one small action per
  bot. State advances across ticks via a state machine, never via loops/sleeps.
- **Guide is advisory.** In strict mode the bot follows it closely; wider
  decision modes let the engine deviate safely.
- **World DB is the source of truth** for quest/NPC/object IDs. Questie data is
  enrichment only and is cross-checked, never blindly trusted.

## Bot tick (per active, unpaused bot)
1. Ensure bot online (bridge).
2. Load current goal + guide step.
3. Assemble DecisionContext from bridge reads + failure tracker.
4. Decision engine returns one Decision (stub: ContinueCurrentStep).
5. Executor performs ONE small action toward that decision.
6. Snapshot state; emit telemetry.
