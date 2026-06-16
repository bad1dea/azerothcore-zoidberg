# Decision engine

Sits between the guide planner and the executor. Reads a `DecisionContext`,
returns one `Decision`. Pure, non-blocking, unit-testable off-server.

Until M7 it is a **stub** that always returns `ContinueCurrentStep` (strict
behavior). This keeps M1-M6 simple while the interface is already wired.

## Modes
- **STRICT_GUIDE** — follow guide; deviate only for death/stuck/full bags/broken state.
- **GUIDE_ASSISTED** — allow alternate objectives, grinding, vendor/trainer, safe skips.
- **AUTONOMOUS_LEVELING** — guide is a suggestion; freely choose quests/grind/group.
- **SANDBOX_IDLE** — bot picks its own activities from allowed goals.

## Scoring inputs (M7)
guide priority, quest XP reward, travel distance, mob level vs bot level,
deaths in area, recent stuck failures, nearby player density, quest progress,
drop-rate/time, bag space, durability, class role, available spells, gear
quality, nearby player/group on same objective, required-for-chain, whether
skipping blocks future steps.

## Actions
continue_current_step, retry_with_alternate_area, grind_until_level,
vendor_repair_train, skip_optional_step, mark_quest_blocked, ask_for_group,
accept_group_invite, leave_group, return_to_town, pause_for_manual_review,
switch_to_fallback_guide_branch.

## Safety constraints
Never attack gray/friendly/evade-bug targets; avoid elites unless the guide
allows; avoid hostile camps above a level threshold; pause after too many
deaths; pause if repeatedly stuck; never use GM teleport as normal pathing
(debug only).
