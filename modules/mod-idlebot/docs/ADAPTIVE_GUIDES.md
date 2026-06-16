# Adaptive guide metadata

Each step may carry an `adaptive` block giving the decision engine permission to
deviate. Strict mode ignores it.

```yaml
adaptive:
  optional: true
  skippable: true
  required_for_chain: false
  max_attempt_minutes: 15
  max_deaths: 2
  allow_alternate_areas: true
  allow_grouping: true
  allow_grind_fallback: true
  fallback_steps:
    - grind_until_level_5
    - alternate_kobold_area
  alternate_objectives:
    - type: kill_mobs
      creature_ids: [475, 476]
      area: northshire_alt_kobold_camp
  failure_policy:
    on_too_hard: grind_until_level
    on_no_progress: try_alternate_area
    on_repeated_death: skip_or_pause
    on_pathing_failed: use_alternate_waypoints
```

The engine consults this plus live state and failure history to choose an action.
