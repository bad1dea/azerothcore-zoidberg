# Known Failures

Reproducible failures, evidence, and classification. Append-only within a
gate; prune entries once a fix is verified and cite the fixing commit
instead of deleting silently.

## Gate 0

None yet — no runtime behavior exists to fail. This file will start
recording `PATH_FAILED` / `TRANSPORT_FAILED` / `TARGET_UNAVAILABLE` /
`OBJECTIVE_NO_PROGRESS` / `QUEST_ACCEPT_FAILED` / `QUEST_TURNIN_FAILED` /
`COMBAT_TOO_HARD` / `UNSAFE_PACK_DENSITY` / `CORPSE_RECOVERY_FAILED` /
`INVENTORY_BLOCKED` / `TRAINING_BLOCKED` / `GUIDE_INVALID` /
`SCRIPTED_OBJECTIVE_UNSUPPORTED` / `STATE_MIGRATION_FAILED` class failures
starting in Gate 1, once there is a Planner/Executor loop that can produce
them.
