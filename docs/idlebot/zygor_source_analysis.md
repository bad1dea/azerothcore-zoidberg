# Zygor Source Analysis

## Archive Inspected

Local reference archive:

- `~/Zygor.zip`

Extracted for inspection to:

- `/tmp/zygor_extract`

## Archive Structure

Top-level addon directory:

- `ZygorGuidesViewer/`

Relevant leveling guide files discovered:

- `ZygorGuidesViewer/Guides/Leveling/ZygorLevelingAllianceCATA.lua`
- `ZygorGuidesViewer/Guides/Leveling/ZygorLevelingHordeCATA.lua`
- additional MoP-era leveling files also exist

Supporting reference files also exist under `ZygorGuidesViewer/Data/`, including NPC and chain data.

## Important Compatibility Caveat

This archive is Cataclysm-era or later leveling content, not native Wrath-era vanilla 1-60 content.

That matters because:

- many 1-60 starter quests were revamped after 3.3.5a
- IDs and route order may not match the AzerothCore 3.3.5a world DB
- the repo's existing `zygor_validate.py` already assumes DB filtering is mandatory

So this source is usable as a local factual reference for:

- guide ordering
- quest/objective grouping
- NPC/object/item IDs
- rough zone/area hints

It is not safe as direct runtime truth for 3.3.5a without validation.

## Guide Container Syntax

Each guide is registered as a Lua block in the form:

```lua
ZygorGuidesViewer:RegisterGuide("Guide Title", [[
...
]])
```

The existing repo parser in `modules/mod-idlebot/tools/zygor_parse.py` already extracts these with a `RegisterGuide(...)` regex.

## Observed Guide Metadata Commands

Observed non-action metadata commands include:

- `author`
- `image`
- `condition suggested ...`
- `next ...`
- `startlevel ...`
- `endlevel ...`
- `dynamic on`

These are suitable for:

- source metadata
- level range extraction
- faction/race/class filtering
- guide chaining

## Observed Action Syntax

The main action syntax observed in the leveling files:

- `step`
- `goto Zone 33.6,53.0`
- `.talk 197`
- `..accept 28757`
- `..turnin 28757`
- `.kill 6 Blackrock Battle Worg##49871+ |q 28757/1`
- `.get 5 Forgotten Dwarven Artifact |q 24477/1`
- `.collect ...`
- `.click Keg of Gnomenbrau##319`
- `.home ...`
- `.fpath ...`
- `only Human Mage`
- inline `|only Human Mage`

The current repo parser already handles several of these:

- `accept`
- `turnin`
- `kill`
- `collect`
- `click`
- `fpath`
- `home`
- `only`

But it currently ignores `goto` coordinates and many instructional lines.

## Human 1-5 Sample

Observed sample from:

- `Zygor's Alliance Leveling Guides\Eastern Kingdoms 1-60\Elwynn Forest (1-10)\Human (1-5)`

Important patterns from that sample:

- class-specific starter accept/turn-in steps are grouped at the same NPC
- `goto` establishes local area hints before action lines
- objective lines often bind to quest/objective indices with `|q quest/objective`
- some class tutorial objectives are expressed as free text lines, not `.kill` or `.get` actions

Those plain-text tutorial lines are not safely convertible in the first pass without richer interpretation logic.

## Starter Guide Order Found

Early alliance guide order in the file begins with:

1. `Dun Morogh (1-10)\Dwarf (1-5)`
2. `Dun Morogh (1-10)\Gnome (1-5)`
3. `Elwynn Forest (1-10)\Human (1-5)`

For the first-pass converter milestone, a level 1-5 starter guide is available immediately.

## Existing Repo Tooling

The repo already contains a partial Zygor pipeline:

- `modules/mod-idlebot/tools/zygor_parse.py`
- `modules/mod-idlebot/tools/zygor_validate.py`

What that pipeline already does:

- parse `RegisterGuide(...)` blocks
- extract quest-route backbone actions
- validate quest coverage against the live DB
- emit cleaned route JSON files under `modules/mod-idlebot/data/routes/`

What it does not do:

- produce runtime YAML guides
- preserve `goto` location hints
- normalize class/race restrictions cleanly
- connect output to runtime loader/executor

## First-Pass Converter Implications

The initial converter should:

- accept zip or extracted input
- detect `ZygorLeveling*.lua`
- preserve guide title, faction, level range, and recognized action data
- preserve `goto` as `source_hint`, not runtime world coordinates
- record unknown commands instead of inventing behavior
- write into a generated guide folder only

That is enough for one validated starter-guide conversion without pretending the source is 3.3.5a-correct runtime truth.
