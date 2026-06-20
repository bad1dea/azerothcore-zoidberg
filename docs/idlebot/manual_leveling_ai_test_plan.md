# Manual Leveling AI Test Plan

## Scope

This checklist is for the current state of the repo after the first review and one-guide conversion pass.

It does not assume runtime loading of generated guides exists yet.

## 1. Review Generated Guide Artifacts

Run:

```bash
python3 tools/zygor_to_idlebot/convert_zygor.py \
  --input ~/Zygor.zip \
  --output data/idlebot/guides/generated \
  --limit 1 \
  --faction alliance \
  --min-level 1 \
  --max-level 12 \
  --report-file docs/idlebot/zygor_conversion_report.md \
  --unknown-file docs/idlebot/zygor_unknown_commands.md \
  --force
```

Check:

- `docs/idlebot/zygor_conversion_report.md` exists
- `docs/idlebot/zygor_unknown_commands.md` exists
- one generated guide file exists under `data/idlebot/guides/generated/`

## 2. Validate Converter Syntax

Run:

```bash
python3 -m py_compile tools/zygor_to_idlebot/convert_zygor.py
```

Expected:

- no output
- exit code `0`

## 3. Validate Generated Guide Structure

Current generated `.yaml` files are emitted in a JSON-compatible YAML subset.

Run:

```bash
python3 - <<'PY'
import json, pathlib
path = pathlib.Path("data/idlebot/guides/generated/alliance/dwarf/zygor_s_alliance_leveling_guides_eastern_kingdoms_1_60_dun_morogh_1_10_dwarf_1_5.yaml")
obj = json.loads(path.read_text())
print(obj["id"])
print(len(obj["steps"]))
print(obj["steps"][0]["type"])
PY
```

Expected:

- guide id prints
- non-zero step count prints
- first step type prints `accept_quest`

## 4. Review Unknown Command Coverage

Open:

- `docs/idlebot/zygor_unknown_commands.md`

Check:

- class tutorial text lines are listed as unknown/instructional
- unmapped `from`-style hint lines are listed
- the report is small enough to guide the next parser increment

## 5. Review Runtime Gap Before Wiring Generated Guides

Open:

- `docs/idlebot/current_idlebot_review.md`
- `docs/idlebot/playerbot_bridge_gap_report.md`
- `docs/idlebot/guide_executor_design.md`

Check:

- loader is still correctly called out as stubbed
- bridge reuse is preferred over new direct systems
- next runtime milestone is guide ingestion, not more one-off quest logic

## 6. Future Runtime Validation Once Loader Exists

When generated guide loading is implemented, validate:

1. `.idlebot guide set <bot> <generated-guide-id>`
2. bot loads first step without crash
3. accept quest step fires one bridge action only
4. objective progress uses `GetQuestObjectiveProgress`
5. restart worldserver and confirm guide resume state persists

This section is intentionally deferred until the runtime loader path exists.
