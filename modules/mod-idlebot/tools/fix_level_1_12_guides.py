#!/usr/bin/env python3
"""
Level 1-12 smoke-roster validator / fixer.

Primary outputs:
  - reports/LEVEL_1_12_BLOCKER_INVENTORY.json
  - reports/LEVEL_1_12_BLOCKER_INVENTORY.md
  - reports/level_1_12_validation.json
  - reports/level_1_12_validation.md

This script validates the actual smoke roster guide chains against the live DB.
It also proposes high-confidence guide fixes for:
  - missing kill objective steps
  - missing item objective steps with clear loot sources
  - missing explore/area-trigger steps

It intentionally does not auto-apply low-confidence scripted quest fixes.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import hashlib
import json
import math
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

import yaml


MODULE_DIR = Path(__file__).resolve().parents[1]
GUIDES_DIR = MODULE_DIR / "data" / "guides"
REPORTS_DIR = MODULE_DIR / "reports"
RESET_SCRIPT = MODULE_DIR / "tools" / "reset_idlebot_test_roster.sh"

SSH_HOST = os.environ.get("IDLEBOT_HOST", "khuong@10.10.30.20")
DB_PASSWORD = os.environ.get(
    "IDLEBOT_DB_PASSWORD",
    "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53",
)
RUNTIME_GUIDE_ROOT = os.environ.get("IDLEBOT_RUNTIME_GUIDE_ROOT", "/home/khuong/acore-zoidberg/data/guides")
CONTAINER_GUIDE_ROOT = os.environ.get("IDLEBOT_CONTAINER_GUIDE_ROOT", "/azerothcore/env/dist/data/guides")

INVENTORY_JSON = REPORTS_DIR / "LEVEL_1_12_BLOCKER_INVENTORY.json"
INVENTORY_MD = REPORTS_DIR / "LEVEL_1_12_BLOCKER_INVENTORY.md"
VALIDATION_JSON = REPORTS_DIR / "level_1_12_validation.json"
VALIDATION_MD = REPORTS_DIR / "level_1_12_validation.md"

FAILURE_CLASSIFICATIONS = {
    "QUEST_INCOMPLETE_AT_TURNIN": "GUIDE_MISSING_OBJECTIVE_STEP",
    "QUEST_ACCEPT_FAILED": "QUEST_ACCEPT_PREREQ_FAILED",
    "QUEST_PROGRESS_STALLED": "TARGET_SELECTION_BUG",
    "QUEST_OBJECTIVE_NO_PROGRESS": "TARGET_SELECTION_BUG",
}

CLASS_MASKS = {
    1: 1 << 0,
    2: 1 << 1,
    3: 1 << 2,
    4: 1 << 3,
    5: 1 << 4,
    6: 1 << 5,
    7: 1 << 6,
    8: 1 << 7,
    9: 1 << 8,
    11: 1 << 10,
}

RACE_MASKS = {
    1: 1,
    2: 2,
    3: 4,
    4: 8,
    5: 16,
    6: 32,
    7: 64,
    8: 128,
    10: 512,
    11: 1024,
}

RACE_NAMES = {
    1: "Human",
    2: "Orc",
    3: "Dwarf",
    4: "NightElf",
    5: "Undead",
    6: "Tauren",
    7: "Gnome",
    8: "Troll",
    10: "BloodElf",
    11: "Draenei",
}

CLASS_NAMES = {
    1: "Warrior",
    2: "Paladin",
    3: "Hunter",
    4: "Rogue",
    5: "Priest",
    6: "DeathKnight",
    7: "Shaman",
    8: "Mage",
    9: "Warlock",
    11: "Druid",
}

OBJECTIVE_STEP_TYPES = {
    "kill_mobs",
    "collect_items",
    "interact_gameobject",
    "use_item_on_npc",
    "use_item_at_location",
    "move_to",
}


@dataclasses.dataclass
class SmokeBot:
    name: str
    race_id: int
    class_id: int
    start_guide: str


@dataclasses.dataclass
class Issue:
    bot: str
    guide_id: str
    guide_path: str
    quest_id: int | None
    step_id: str | None
    issue_type: str
    severity: str
    message: str
    evidence: dict[str, Any]
    suggested_fix: str | None = None

    def as_dict(self) -> dict[str, Any]:
        return dataclasses.asdict(self)


def run(cmd: list[str], check: bool = True) -> str:
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if check and proc.returncode != 0:
        raise RuntimeError(f"command failed: {' '.join(cmd)}\n{proc.stderr}")
    return proc.stdout


def run_ssh(command: str, check: bool = True) -> str:
    return run(["ssh", SSH_HOST, command], check=check)


def mysql_rows(db: str, sql: str) -> list[dict[str, str]]:
    sql = sql.replace('"', '\\"')
    cmd = (
        f"docker exec ac-database mysql -B -uroot -p{DB_PASSWORD} {db} "
        f'-e "{sql}" 2>/dev/null'
    )
    out = run_ssh(cmd)
    lines = [line for line in out.splitlines() if line.strip()]
    if not lines:
        return []
    headers = lines[0].split("\t")
    rows: list[dict[str, str]] = []
    for line in lines[1:]:
        parts = line.split("\t")
        if len(parts) < len(headers):
            parts.extend([""] * (len(headers) - len(parts)))
        rows.append(dict(zip(headers, parts)))
    return rows


def mysql_scalar(db: str, sql: str) -> str:
    sql = sql.replace('"', '\\"')
    cmd = (
        f"docker exec ac-database mysql -N -uroot -p{DB_PASSWORD} {db} "
        f'-e "{sql}" 2>/dev/null'
    )
    return run_ssh(cmd).strip()


def parse_smoke_roster() -> list[SmokeBot]:
    bots: list[SmokeBot] = []
    pattern = re.compile(r'^define_bot "([^"]+)"\s+(\d+)\s+(\d+)\s+\d+\s+.+?"([^"]+)"$')
    for line in RESET_SCRIPT.read_text().splitlines():
        m = pattern.match(line.strip())
        if not m:
            continue
        bots.append(
            SmokeBot(
                name=m.group(1),
                race_id=int(m.group(2)),
                class_id=int(m.group(3)),
                start_guide=m.group(4),
            )
        )
    return bots


def load_guides() -> tuple[dict[str, dict[str, Any]], dict[str, list[Path]]]:
    guides: dict[str, dict[str, Any]] = {}
    guide_paths: dict[str, list[Path]] = defaultdict(list)
    for root, dirs, files in os.walk(GUIDES_DIR):
        dirs[:] = [d for d in dirs if d not in ("generated", "generated_backup")]
        for fn in files:
            if not fn.endswith(".yaml"):
                continue
            path = Path(root) / fn
            data = yaml.safe_load(path.read_text()) or {}
            gid = data.get("id")
            if not gid:
                continue
            data["_path"] = str(path.relative_to(GUIDES_DIR))
            guides[gid] = data
            guide_paths[gid].append(path)
    return guides, guide_paths


def runtime_guide_occurrences() -> dict[str, list[str]]:
    cmd = (
        f"find {RUNTIME_GUIDE_ROOT} "
        f"\\( -path '*/generated_backup/*' -o -path '*/generated/*' \\) -prune -o "
        "-name '*.yaml' -print0 | "
        "while IFS= read -r -d '' f; do "
        "id=$(grep -m1 '^id:' \"$f\" | awk '{print $2}'); "
        "if [ -n \"$id\" ]; then printf '%s\\t%s\\n' \"$id\" \"$f\"; fi; "
        "done"
    )
    out = run_ssh(cmd, check=False)
    occ: dict[str, list[str]] = defaultdict(list)
    for line in out.splitlines():
        if "\t" not in line:
            continue
        gid, path = line.split("\t", 1)
        occ[gid].append(path)
    return occ


def container_guide_occurrences() -> dict[str, list[str]]:
    cmd = (
        f"docker exec ac-worldserver sh -lc 'if [ -d {CONTAINER_GUIDE_ROOT} ]; then "
        f"find {CONTAINER_GUIDE_ROOT} -name \"*.yaml\" -print0 | "
        "while IFS= read -r -d '' f; do "
        "id=$(grep -m1 '^id:' \"$f\" | awk '{print $2}'); "
        "if [ -n \"$id\" ]; then printf '%s\\t%s\\n' \"$id\" \"$f\"; fi; "
        "done; fi'"
    )
    out = run_ssh(cmd, check=False)
    occ: dict[str, list[str]] = defaultdict(list)
    for line in out.splitlines():
        if "\t" not in line:
            continue
        gid, path = line.split("\t", 1)
        occ[gid].append(path)
    return occ


def sha1_file(path: Path) -> str:
    return hashlib.sha1(path.read_bytes()).hexdigest()


def sha1_remote(path: str) -> str | None:
    out = run_ssh(f"test -f '{path}' && sha1sum '{path}' | awk '{{print $1}}' || true", check=False).strip()
    return out or None


def guide_chain(start_guide: str, guides: dict[str, dict[str, Any]], max_level: int = 12) -> list[str]:
    out: list[str] = []
    seen: set[str] = set()
    gid = start_guide
    while gid and gid in guides and gid not in seen:
        seen.add(gid)
        out.append(gid)
        g = guides[gid]
        if int(g.get("level_max") or 0) >= max_level:
            break
        gid = g.get("next_guide") or ""
    return out


def step_applies(step: dict[str, Any], bot: SmokeBot) -> bool:
    restrictions = step.get("restrictions") or {}
    class_mask = restrictions.get("class_mask")
    if class_mask is not None:
        if int(class_mask) == 0:
            return False
        if not (int(class_mask) & CLASS_MASKS.get(bot.class_id, 0)):
            return False
    race_mask = restrictions.get("race_mask")
    if race_mask is not None and not (int(race_mask) & RACE_MASKS.get(bot.race_id, 0)):
        return False
    return True


def parse_condition_index(step: dict[str, Any], quest_id: int) -> int | None:
    cond = str(step.get("completion_condition") or "")
    m = re.search(rf"quest_objective_complete:{quest_id}/(\d+)", cond)
    if m:
        return int(m.group(1))
    return None


def build_quest_cache(quest_ids: set[int]) -> dict[int, dict[str, Any]]:
    if not quest_ids:
        return {}
    ids = ",".join(str(q) for q in sorted(quest_ids))
    rows = mysql_rows(
        "acore_world",
        "SELECT q.ID, q.LogTitle, q.QuestLevel, q.MinLevel, q.AllowableRaces, "
        "q.RequiredNpcOrGo1, q.RequiredNpcOrGo2, q.RequiredNpcOrGo3, q.RequiredNpcOrGo4, "
        "q.RequiredNpcOrGoCount1, q.RequiredNpcOrGoCount2, q.RequiredNpcOrGoCount3, q.RequiredNpcOrGoCount4, "
        "q.RequiredItemId1, q.RequiredItemId2, q.RequiredItemId3, q.RequiredItemId4, q.RequiredItemId5, q.RequiredItemId6, "
        "q.RequiredItemCount1, q.RequiredItemCount2, q.RequiredItemCount3, q.RequiredItemCount4, q.RequiredItemCount5, q.RequiredItemCount6, "
        "q.ObjectiveText1, q.ObjectiveText2, q.ObjectiveText3, q.ObjectiveText4, "
        "a.PrevQuestID, a.ExclusiveGroup, a.AllowableClasses, a.SpecialFlags "
        "FROM quest_template q "
        "LEFT JOIN quest_template_addon a ON a.ID = q.ID "
        f"WHERE q.ID IN ({ids})"
    )
    cache: dict[int, dict[str, Any]] = {}
    for row in rows:
        qid = int(row["ID"])
        cache[qid] = {
            "id": qid,
            "title": row["LogTitle"],
            "quest_level": int(row["QuestLevel"] or 0),
            "min_level": int(row["MinLevel"] or 0),
            "allowable_races": int(row["AllowableRaces"] or 0),
            "allowable_classes": int(row.get("AllowableClasses") or 0),
            "prev_quest_id": int(row.get("PrevQuestID") or 0),
            "exclusive_group": int(row.get("ExclusiveGroup") or 0),
            "special_flags": int(row.get("SpecialFlags") or 0),
            "npc_go": [
                int(row.get(f"RequiredNpcOrGo{i}") or 0) for i in range(1, 5)
            ],
            "npc_go_counts": [
                int(row.get(f"RequiredNpcOrGoCount{i}") or 0) for i in range(1, 5)
            ],
            "item_ids": [
                int(row.get(f"RequiredItemId{i}") or 0) for i in range(1, 7)
            ],
            "item_counts": [
                int(row.get(f"RequiredItemCount{i}") or 0) for i in range(1, 7)
            ],
            "objective_texts": [
                row.get(f"ObjectiveText{i}") or "" for i in range(1, 5)
            ],
        }
    at_rows = mysql_rows(
        "acore_world",
        f"SELECT quest, id FROM areatrigger_involvedrelation WHERE quest IN ({ids})"
    )
    at_map: dict[int, list[int]] = defaultdict(list)
    for row in at_rows:
        at_map[int(row["quest"])].append(int(row["id"]))
    for qid, q in cache.items():
        q["area_triggers"] = at_map.get(qid, [])
    return cache


def build_relation_cache(table: str, quest_ids: set[int]) -> dict[int, set[int]]:
    if not quest_ids:
        return {}
    ids = ",".join(str(q) for q in sorted(quest_ids))
    rows = mysql_rows("acore_world", f"SELECT id, quest FROM {table} WHERE quest IN ({ids})")
    out: dict[int, set[int]] = defaultdict(set)
    for row in rows:
        out[int(row["quest"])].add(int(row["id"]))
    return out


def build_creature_spawn_cache(entries: set[int]) -> dict[int, list[dict[str, Any]]]:
    if not entries:
        return {}
    ids = ",".join(str(x) for x in sorted(entries))
    rows = mysql_rows(
        "acore_world",
        f"SELECT id1, map, position_x, position_y, position_z FROM creature WHERE id1 IN ({ids})"
    )
    out: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        out[int(row["id1"])].append(
            {
                "map": int(row["map"]),
                "x": float(row["position_x"]),
                "y": float(row["position_y"]),
                "z": float(row["position_z"]),
            }
        )
    return out


def build_go_spawn_cache(entries: set[int]) -> dict[int, list[dict[str, Any]]]:
    if not entries:
        return {}
    ids = ",".join(str(x) for x in sorted(entries))
    rows = mysql_rows(
        "acore_world",
        f"SELECT id, map, position_x, position_y, position_z FROM gameobject WHERE id IN ({ids})"
    )
    out: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        out[int(row["id"])].append(
            {
                "map": int(row["map"]),
                "x": float(row["position_x"]),
                "y": float(row["position_y"]),
                "z": float(row["position_z"]),
            }
        )
    return out


def build_areatrigger_cache(entries: set[int]) -> dict[int, dict[str, Any]]:
    if not entries:
        return {}
    ids = ",".join(str(x) for x in sorted(entries))
    rows = mysql_rows(
        "acore_world",
        f"SELECT entry, map, x, y, z, radius, length, width, height, orientation "
        f"FROM areatrigger WHERE entry IN ({ids})"
    )
    out: dict[int, dict[str, Any]] = {}
    for row in rows:
        out[int(row["entry"])] = {
            "map": int(row["map"]),
            "x": float(row["x"]),
            "y": float(row["y"]),
            "z": float(row["z"]),
            "radius": float(row["radius"] or 0),
            "length": float(row["length"] or 0),
            "width": float(row["width"] or 0),
            "height": float(row["height"] or 0),
            "orientation": float(row["orientation"] or 0),
        }
    return out


def build_item_source_cache(item_ids: set[int]) -> tuple[dict[int, set[int]], dict[int, set[int]]]:
    if not item_ids:
        return {}, {}
    ids = ",".join(str(x) for x in sorted(item_ids))
    creature_rows = mysql_rows(
        "acore_world",
        f"SELECT Entry, Item FROM creature_loot_template WHERE Item IN ({ids})"
    )
    go_rows = mysql_rows(
        "acore_world",
        f"SELECT Entry, Item FROM gameobject_loot_template WHERE Item IN ({ids})"
    )
    c_map: dict[int, set[int]] = defaultdict(set)
    g_map: dict[int, set[int]] = defaultdict(set)
    for row in creature_rows:
        c_map[int(row["Item"])].add(int(row["Entry"]))
    for row in go_rows:
        g_map[int(row["Item"])].add(int(row["Entry"]))
    return c_map, g_map


def average_spawn(spawns: list[dict[str, Any]]) -> dict[str, Any]:
    if not spawns:
        return {"map": 0, "x": 0.0, "y": 0.0, "z": 0.0, "radius": 30.0}
    map_id = spawns[0]["map"]
    x = sum(s["x"] for s in spawns) / len(spawns)
    y = sum(s["y"] for s in spawns) / len(spawns)
    z = sum(s["z"] for s in spawns) / len(spawns)
    max_d = max(math.dist((x, y), (s["x"], s["y"])) for s in spawns) if len(spawns) > 1 else 15.0
    return {"map": map_id, "x": round(x, 2), "y": round(y, 2), "z": round(z, 2), "radius": max(30.0, round(max_d + 20.0, 1))}


def has_nearby_spawn(step: dict[str, Any], spawns: list[dict[str, Any]]) -> bool:
    coords = step.get("coordinates") or {}
    map_id = int(coords.get("map_id", coords.get("map", 0)) or 0)
    x = float(coords.get("x", 0) or 0)
    y = float(coords.get("y", 0) or 0)
    radius = float(coords.get("radius", 0) or 0)
    max_range_sq = max(radius * 2.0, 200.0) ** 2
    for spawn in spawns:
        if int(spawn["map"]) != map_id:
            continue
        dx = float(spawn["x"]) - x
        dy = float(spawn["y"]) - y
        if dx * dx + dy * dy <= max_range_sq:
            return True
    return False


def quest_objectives(quest: dict[str, Any]) -> list[dict[str, Any]]:
    objectives: list[dict[str, Any]] = []
    for idx, (entry, count, text) in enumerate(zip(quest["npc_go"], quest["npc_go_counts"], quest["objective_texts"]), start=1):
        if entry or count:
            if entry > 0:
                objectives.append({"index": idx, "kind": "creature", "entry": entry, "count": count, "text": text})
            elif entry < 0:
                objectives.append({"index": idx, "kind": "gameobject", "entry": abs(entry), "count": count, "text": text})
    for idx, (item_id, count) in enumerate(zip(quest["item_ids"], quest["item_counts"]), start=1):
        if item_id or count:
            objectives.append({"index": idx, "kind": "item", "item_id": item_id, "count": count, "text": ""})
    for at in quest.get("area_triggers", []):
        objectives.append({"index": None, "kind": "areatrigger", "area_trigger_id": at, "count": 1, "text": ""})
    return objectives


def classify_current_failure(event_code: str | None, issue_types: set[str], is_stale: bool) -> str:
    if is_stale:
        return "DASHBOARD_STALE_LIVE_STATE"
    if event_code in FAILURE_CLASSIFICATIONS:
        mapped = FAILURE_CLASSIFICATIONS[event_code]
        if mapped == "QUEST_ACCEPT_PREREQ_FAILED":
            if "GUIDE_BAD_ACCEPT_NPC" in issue_types:
                return "GUIDE_BAD_ACCEPT_NPC"
            if "GUIDE_BAD_PREREQ_CHAIN" in issue_types:
                return "GUIDE_BAD_PREREQ_CHAIN"
            if "GUIDE_DUPLICATE_STALE_COPY" in issue_types:
                return "GUIDE_DUPLICATE_STALE_COPY"
        if mapped == "GUIDE_MISSING_OBJECTIVE_STEP":
            if "SCRIPTED_QUEST_UNSUPPORTED" in issue_types:
                return "SCRIPTED_QUEST_UNSUPPORTED"
            if "GUIDE_MISSING_OBJECTIVE_STEP" in issue_types:
                return "GUIDE_MISSING_OBJECTIVE_STEP"
        return mapped
    if issue_types:
        return sorted(issue_types)[0]
    return "UNKNOWN"


def validate_roster(apply_fixes: bool = False) -> tuple[list[dict[str, Any]], list[Issue], list[dict[str, Any]]]:
    bots = parse_smoke_roster()
    guides, local_occ = load_guides()
    runtime_occ = runtime_guide_occurrences()
    container_occ = container_guide_occurrences()

    roster_chains: dict[str, list[str]] = {
        bot.name: guide_chain(bot.start_guide, guides) for bot in bots
    }
    involved_guides = {gid for chain in roster_chains.values() for gid in chain}

    quest_ids: set[int] = set()
    creature_entries: set[int] = set()
    go_entries: set[int] = set()
    for gid in involved_guides:
        guide = guides[gid]
        for step in guide.get("steps", []):
            if step.get("quest_id"):
                quest_ids.add(int(step["quest_id"]))
            for cid in step.get("creature_ids") or []:
                creature_entries.add(int(cid))
            if step.get("gameobject_id"):
                go_entries.add(int(step["gameobject_id"]))
            for goid in step.get("source_gameobject_entries") or []:
                go_entries.add(int(goid))

    quest_cache = build_quest_cache(quest_ids)
    starter_creatures = build_relation_cache("creature_queststarter", quest_ids)
    starter_gos = build_relation_cache("gameobject_queststarter", quest_ids)
    ender_creatures = build_relation_cache("creature_questender", quest_ids)
    ender_gos = build_relation_cache("gameobject_questender", quest_ids)

    item_ids = {
        item_id
        for q in quest_cache.values()
        for item_id in q["item_ids"]
        if item_id > 0
    }
    creature_sources, go_sources = build_item_source_cache(item_ids)
    creature_entries.update({e for vals in creature_sources.values() for e in vals})
    go_entries.update({e for vals in go_sources.values() for e in vals})

    areatrigger_ids = {at for q in quest_cache.values() for at in q.get("area_triggers", [])}

    creature_spawns = build_creature_spawn_cache(creature_entries)
    go_spawns = build_go_spawn_cache(go_entries)
    areatriggers = build_areatrigger_cache(areatrigger_ids)

    guide_users: dict[str, list[SmokeBot]] = defaultdict(list)
    for bot in bots:
        for gid in roster_chains[bot.name]:
            guide_users[gid].append(bot)

    issues: list[Issue] = []
    fix_candidates: list[dict[str, Any]] = []

    for bot in bots:
        completed_in_chain: set[int] = set()
        for gid in roster_chains[bot.name]:
            guide = guides[gid]
            path = guide["_path"]
            if len(runtime_occ.get(gid, [])) > 1:
                issues.append(Issue(bot.name, gid, path, None, None, "GUIDE_DUPLICATE_STALE_COPY", "BLOCKER",
                                    f"runtime guide id '{gid}' exists in multiple host files",
                                    {"runtime_paths": runtime_occ[gid]}))
            if len(local_occ.get(gid, [])) > 1:
                issues.append(Issue(bot.name, gid, path, None, None, "GUIDE_DUPLICATE_STALE_COPY", "BLOCKER",
                                    f"source guide id '{gid}' exists in multiple local files",
                                    {"source_paths": [str(p) for p in local_occ[gid]]}))

            active_steps = []
            for s in guide.get("steps", []):
                if not step_applies(s, bot):
                    continue
                qid = int(s.get("quest_id") or 0)
                if qid:
                    quest = quest_cache.get(qid)
                    if quest and (quest["min_level"] > 12 or quest["quest_level"] > 12):
                        continue
                if int(s.get("level_min") or 0) > 12:
                    continue
                active_steps.append(s)
            by_quest: dict[int, dict[str, list[dict[str, Any]]]] = defaultdict(lambda: {"accept": [], "turnin": [], "objective": []})
            for step in active_steps:
                qid = step.get("quest_id")
                if not qid:
                    continue
                qid = int(qid)
                stype = step.get("type")
                if stype == "accept_quest":
                    by_quest[qid]["accept"].append(step)
                elif stype == "turn_in_quest":
                    by_quest[qid]["turnin"].append(step)
                elif stype in OBJECTIVE_STEP_TYPES and (step.get("completion_condition") or stype != "move_to" or step.get("area_trigger_id")):
                    by_quest[qid]["objective"].append(step)

            for qid, grouped in by_quest.items():
                quest = quest_cache.get(qid)
                if not quest:
                    issues.append(Issue(bot.name, gid, path, qid, None, "QUEST_NOT_FOUND", "BLOCKER",
                                        f"quest {qid} missing from DB", {}))
                    continue

                allowed_classes = quest["allowable_classes"]
                if allowed_classes and not (allowed_classes & CLASS_MASKS.get(bot.class_id, 0)):
                    issues.append(Issue(bot.name, gid, path, qid, None, "GUIDE_BAD_PREREQ_CHAIN", "BLOCKER",
                                        f"quest {qid} '{quest['title']}' not valid for class {CLASS_NAMES[bot.class_id]}",
                                        {"allowable_classes": allowed_classes}))

                allowed_races = quest["allowable_races"]
                if allowed_races not in (0, -1, 4294967295) and not (allowed_races & RACE_MASKS.get(bot.race_id, 0)):
                    issues.append(Issue(bot.name, gid, path, qid, None, "GUIDE_BAD_PREREQ_CHAIN", "BLOCKER",
                                        f"quest {qid} '{quest['title']}' not valid for race {RACE_NAMES[bot.race_id]}",
                                        {"allowable_races": allowed_races}))

                prev = quest["prev_quest_id"]
                if prev > 0 and prev not in completed_in_chain and grouped["accept"]:
                    issues.append(Issue(bot.name, gid, path, qid, grouped["accept"][0].get("id"), "GUIDE_BAD_PREREQ_CHAIN", "BLOCKER",
                                        f"quest {qid} '{quest['title']}' requires prev quest {prev} earlier in chain",
                                        {"prev_quest_id": prev}))

                for step in grouped["accept"]:
                    npc = int(step.get("npc_id") or 0)
                    goid = int(step.get("gameobject_id") or 0)
                    valid_starters = starter_creatures.get(qid, set()) | starter_gos.get(qid, set())
                    if npc and npc not in starter_creatures.get(qid, set()):
                        issues.append(Issue(bot.name, gid, path, qid, step.get("id"), "GUIDE_BAD_ACCEPT_NPC", "BLOCKER",
                                            f"accept step uses npc {npc} not in creature_queststarter for quest {qid}",
                                            {"db_starters": sorted(starter_creatures.get(qid, set()))}))
                    if goid and goid not in starter_gos.get(qid, set()):
                        issues.append(Issue(bot.name, gid, path, qid, step.get("id"), "GUIDE_BAD_ACCEPT_NPC", "BLOCKER",
                                            f"accept step uses gameobject {goid} not in gameobject_queststarter for quest {qid}",
                                            {"db_starters": sorted(starter_gos.get(qid, set()))}))
                    if not npc and not goid and not valid_starters:
                        issues.append(Issue(bot.name, gid, path, qid, step.get("id"), "GUIDE_BAD_ACCEPT_NPC", "BLOCKER",
                                            f"quest {qid} has no starter in DB", {}))

                for step in grouped["turnin"]:
                    npc = int(step.get("npc_id") or 0)
                    goid = int(step.get("gameobject_id") or 0)
                    if npc and npc not in ender_creatures.get(qid, set()):
                        issues.append(Issue(bot.name, gid, path, qid, step.get("id"), "GUIDE_BAD_TURNIN_NPC", "BLOCKER",
                                            f"turn-in step uses npc {npc} not in creature_questender for quest {qid}",
                                            {"db_enders": sorted(ender_creatures.get(qid, set()))}))
                    if goid and goid not in ender_gos.get(qid, set()):
                        issues.append(Issue(bot.name, gid, path, qid, step.get("id"), "GUIDE_BAD_TURNIN_NPC", "BLOCKER",
                                            f"turn-in step uses gameobject {goid} not in gameobject_questender for quest {qid}",
                                            {"db_enders": sorted(ender_gos.get(qid, set()))}))

                covered_indices: set[int] = set()
                covered_items: set[int] = set()
                covered_triggers: set[int] = set()
                unsupported = False
                for step in grouped["objective"]:
                    idx = parse_condition_index(step, qid)
                    if idx is not None:
                        covered_indices.add(idx)
                    if step.get("item_id"):
                        covered_items.add(int(step["item_id"]))
                    if step.get("area_trigger_id"):
                        covered_triggers.add(int(step["area_trigger_id"]))
                    if "not implemented" in str(step.get("name", "")).lower() or step.get("_unsafe"):
                        unsupported = True
                    stype = str(step.get("type") or "")
                    if stype == "kill_mobs":
                        step_spawns: list[dict[str, Any]] = []
                        for entry in step.get("creature_ids") or []:
                            step_spawns.extend(creature_spawns.get(int(entry), []))
                        if step_spawns and not has_nearby_spawn(step, step_spawns):
                            issues.append(Issue(bot.name, gid, path, qid, step.get("id"), "GUIDE_BAD_OBJECTIVE_COORDS", "BLOCKER",
                                                f"objective step '{step.get('id')}' is not near any matching creature spawns for quest {qid}",
                                                {"creature_ids": step.get("creature_ids") or [], "step_coordinates": step.get("coordinates")} ))
                    elif stype == "collect_items":
                        step_spawns = []
                        for entry in step.get("source_creature_entries") or []:
                            step_spawns.extend(creature_spawns.get(int(entry), []))
                        for entry in step.get("source_gameobject_entries") or []:
                            step_spawns.extend(go_spawns.get(int(entry), []))
                        if step_spawns and not has_nearby_spawn(step, step_spawns):
                            issues.append(Issue(bot.name, gid, path, qid, step.get("id"), "GUIDE_BAD_OBJECTIVE_COORDS", "BLOCKER",
                                                f"objective step '{step.get('id')}' is not near any matching source spawns for quest {qid}",
                                                {"step_coordinates": step.get("coordinates")} ))

                for obj in quest_objectives(quest):
                    if obj["kind"] == "creature":
                        if obj["index"] not in covered_indices:
                            issues.append(Issue(bot.name, gid, path, qid, None, "GUIDE_MISSING_OBJECTIVE_STEP", "BLOCKER",
                                                f"quest {qid} missing creature objective step for entry {obj['entry']} x{obj['count']}",
                                                {"objective": obj},
                                                "insert kill_mobs step before turn-in"))
                            spawns = creature_spawns.get(obj["entry"], [])
                            if spawns:
                                fix_candidates.append({
                                    "bot": bot.name, "guide_id": gid, "guide_path": path, "quest_id": qid,
                                    "kind": "creature", "objective": obj, "insert_before": grouped["turnin"][0]["id"] if grouped["turnin"] else None,
                                    "coords": average_spawn(spawns),
                                })
                    elif obj["kind"] == "gameobject":
                        if obj["index"] not in covered_indices and obj["entry"] not in covered_items:
                            issues.append(Issue(bot.name, gid, path, qid, None, "GUIDE_MISSING_OBJECTIVE_STEP", "BLOCKER",
                                                f"quest {qid} missing gameobject objective step for entry {obj['entry']}",
                                                {"objective": obj},
                                                "insert interact_gameobject or collect_items step before turn-in"))
                    elif obj["kind"] == "item":
                        support_steps = []
                        for step in grouped["objective"]:
                            idx = parse_condition_index(step, qid)
                            if idx == obj["index"] or int(step.get("item_id") or 0) == obj["item_id"]:
                                support_steps.append(step)
                        if obj["item_id"] not in covered_items and obj["index"] not in covered_indices:
                            issues.append(Issue(bot.name, gid, path, qid, None, "GUIDE_MISSING_OBJECTIVE_STEP", "BLOCKER",
                                                f"quest {qid} missing item objective step for item {obj['item_id']} x{obj['count']}",
                                                {"objective": obj},
                                                "insert item objective step before turn-in"))
                            csrc = sorted(creature_sources.get(obj["item_id"], set()))
                            gsrc = sorted(go_sources.get(obj["item_id"], set()))
                            if csrc:
                                spawns = []
                                for entry in csrc[:5]:
                                    spawns.extend(creature_spawns.get(entry, []))
                                if spawns:
                                    fix_candidates.append({
                                        "bot": bot.name, "guide_id": gid, "guide_path": path, "quest_id": qid,
                                        "kind": "item_creature", "objective": obj, "insert_before": grouped["turnin"][0]["id"] if grouped["turnin"] else None,
                                        "creature_ids": csrc[:5], "coords": average_spawn(spawns),
                                    })
                            elif gsrc:
                                spawns = []
                                for entry in gsrc[:5]:
                                    spawns.extend(go_spawns.get(entry, []))
                                if spawns:
                                    fix_candidates.append({
                                        "bot": bot.name, "guide_id": gid, "guide_path": path, "quest_id": qid,
                                        "kind": "item_go", "objective": obj, "insert_before": grouped["turnin"][0]["id"] if grouped["turnin"] else None,
                                        "gameobject_ids": gsrc[:5], "coords": average_spawn(spawns),
                                    })
                        needs_loot = any(str(step.get("type")) in {"kill_mobs", "collect_items"} for step in support_steps)
                        if needs_loot and not creature_sources.get(obj["item_id"]) and not go_sources.get(obj["item_id"]):
                            issues.append(Issue(bot.name, gid, path, qid, None, "LOOT_BUG", "BLOCKER",
                                                f"quest {qid} item {obj['item_id']} has no creature/gameobject loot source in DB",
                                                {"objective": obj}))
                    elif obj["kind"] == "areatrigger":
                        if obj["area_trigger_id"] not in covered_triggers:
                            issues.append(Issue(bot.name, gid, path, qid, None, "GUIDE_MISSING_OBJECTIVE_STEP", "BLOCKER",
                                                f"quest {qid} missing area trigger step for areatrigger {obj['area_trigger_id']}",
                                                {"objective": obj},
                                                "insert move_to + area_trigger_id step before turn-in"))
                            at = areatriggers.get(obj["area_trigger_id"])
                            if at:
                                fix_candidates.append({
                                    "bot": bot.name, "guide_id": gid, "guide_path": path, "quest_id": qid,
                                    "kind": "areatrigger", "objective": obj, "insert_before": grouped["turnin"][0]["id"] if grouped["turnin"] else None,
                                    "coords": {"map": at["map"], "x": at["x"], "y": at["y"], "z": at["z"], "radius": at["radius"] or 8.0},
                                })

                if unsupported:
                    issues.append(Issue(bot.name, gid, path, qid, None, "SCRIPTED_QUEST_UNSUPPORTED", "WARNING",
                                        f"quest {qid} includes an objective step marked unsupported/not implemented",
                                        {"quest_title": quest["title"]},
                                        "replace with supported quest action or remove optional class quest from route"))

                if grouped["turnin"]:
                    completed_in_chain.add(qid)

    if apply_fixes:
        apply_high_confidence_fixes(guides, fix_candidates)

    inventory = build_blocker_inventory(bots, guides, issues)
    return inventory, issues, fix_candidates


def build_blocker_inventory(bots: list[SmokeBot], guides: dict[str, dict[str, Any]], issues: list[Issue]) -> list[dict[str, Any]]:
    issue_map: dict[tuple[str, int | None], list[Issue]] = defaultdict(list)
    for issue in issues:
        issue_map[(issue.bot, issue.quest_id)].append(issue)

    bot_rows = mysql_rows(
        "acore_characters",
        "SELECT b.bot_name, b.guide_id, b.step_index, b.step_state, b.blocked_reason, b.last_failure_code, "
        "b.soak_run_id, b.bot_session_id, b.reset_id, b.updated_at AS manager_updated_at, "
        "ls.level, ls.map_id, ls.x, ls.y, ls.z, ls.state AS live_state, ls.step_name, ls.quest_id, ls.objective_text, "
        "ls.target_name, ls.target_entry, ls.updated_at AS live_updated_at, "
        "c.class AS class_id, c.race AS race_id "
        "FROM idlebot_bots b "
        "LEFT JOIN idlebot_live_state ls ON ls.bot_name = b.bot_name "
        "LEFT JOIN characters c ON c.name = b.bot_name "
        "ORDER BY b.bot_name"
    )
    failure_rows = mysql_rows(
        "acore_characters",
        "SELECT ib.bot_name, ie.event_code, ie.detail, ie.created_at, ie.soak_run_id, ie.bot_session_id, ie.reset_id "
        "FROM idlebot_events ie "
        "JOIN idlebot_bots ib ON ib.id = ie.bot_id "
        "WHERE ie.event_type='FAILURE' "
        "ORDER BY ie.created_at DESC"
    )
    latest_failure: dict[str, dict[str, str]] = {}
    for row in failure_rows:
        latest_failure.setdefault(row["bot_name"], row)

    inventory: list[dict[str, Any]] = []
    now = dt.datetime.now(dt.timezone.utc)
    for row in bot_rows:
        name = row["bot_name"]
        gid = row.get("guide_id") or ""
        guide = guides.get(gid)
        guide_path = guide["_path"] if guide else ""
        source_hash = sha1_file(GUIDES_DIR / guide_path) if guide_path else None
        runtime_path = None
        runtime_hash = None
        if gid:
            occ = runtime_guide_occurrences().get(gid, [])
            if occ:
                runtime_path = occ[0]
                runtime_hash = sha1_remote(runtime_path)
        qid = int(row["quest_id"] or 0) or None
        live_updated_at = row.get("live_updated_at") or ""
        live_age = None
        is_stale = False
        if live_updated_at:
            stamp = dt.datetime.fromisoformat(str(live_updated_at).replace(" ", "T") + "Z")
            live_age = (now - stamp).total_seconds()
            is_stale = live_age > 120
        failure = latest_failure.get(name)
        related = issue_map.get((name, qid), []) + issue_map.get((name, None), [])
        issue_types = {i.issue_type for i in related}
        classification = classify_current_failure((failure or {}).get("event_code"), issue_types, is_stale)
        inventory.append(
            {
                "bot_name": name,
                "race": RACE_NAMES.get(int(row["race_id"] or 0), str(row["race_id"])),
                "class": CLASS_NAMES.get(int(row["class_id"] or 0), str(row["class_id"])),
                "level": int(row["level"] or 0),
                "guide_id": gid,
                "guide_path": guide_path,
                "loaded_guide_hash": runtime_hash,
                "source_guide_hash": source_hash,
                "runtime_guide_path": runtime_path,
                "current_step_index": int(row["step_index"] or 0),
                "current_step_id": step_id_for_guide(guide, int(row["step_index"] or 0)),
                "current_step_name": row.get("step_name"),
                "current_quest_id": qid,
                "current_objective": row.get("objective_text"),
                "current_position": {
                    "map_id": int(row["map_id"] or 0),
                    "x": float(row["x"] or 0),
                    "y": float(row["y"] or 0),
                    "z": float(row["z"] or 0),
                },
                "live_state_age_seconds": live_age,
                "current_target": {
                    "name": row.get("target_name"),
                    "entry": int(row["target_entry"] or 0) or None,
                },
                "active_failure": failure,
                "failure_status": "stale" if is_stale else ("active" if failure and row.get("step_state") in ("blocked", "paused") else "resolved"),
                "step_state": row.get("step_state"),
                "blocked_reason": row.get("blocked_reason"),
                "last_failure_code": row.get("last_failure_code"),
                "root_cause_classification": classification,
                "validator_issues": [i.as_dict() for i in related],
            }
        )
    return inventory


def step_id_for_guide(guide: dict[str, Any] | None, index: int) -> str | None:
    if not guide:
        return None
    steps = guide.get("steps", [])
    if 0 <= index < len(steps):
        return steps[index].get("id")
    return None


def apply_high_confidence_fixes(guides: dict[str, dict[str, Any]], candidates: list[dict[str, Any]]) -> None:
    by_path: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for cand in candidates:
        if cand.get("insert_before"):
            by_path[cand["guide_path"]].append(cand)
    for rel_path, path_cands in by_path.items():
        abs_path = GUIDES_DIR / rel_path
        guide = yaml.safe_load(abs_path.read_text())
        steps = guide.get("steps", [])
        inserted = 0
        for cand in sorted(path_cands, key=lambda c: (c["quest_id"], c["kind"])):
            before = cand["insert_before"]
            if any(str(step.get("id")) == f"q{cand['quest_id']}_auto_{cand['kind']}" for step in steps):
                continue
            idx = next((i for i, step in enumerate(steps) if step.get("id") == before), None)
            if idx is None:
                continue
            obj = cand["objective"]
            coords = cand["coords"]
            if cand["kind"] == "areatrigger":
                new_step = {
                    "id": f"q{cand['quest_id']}_auto_areatrigger",
                    "name": f"Quest {cand['quest_id']} explore objective",
                    "type": "move_to",
                    "quest_id": cand["quest_id"],
                    "area_trigger_id": obj["area_trigger_id"],
                    "completion_condition": f"quest_objective_complete:{cand['quest_id']}/1",
                    "coordinates": {
                        "x": coords["x"], "y": coords["y"], "z": coords["z"],
                        "radius": coords["radius"], "map_id": coords["map"],
                    },
                }
            elif cand["kind"] == "creature":
                new_step = {
                    "id": f"q{cand['quest_id']}_auto_objective{obj['index']}",
                    "name": f"Quest {cand['quest_id']} objective {obj['index']}",
                    "type": "kill_mobs",
                    "quest_id": cand["quest_id"],
                    "creature_ids": [obj["entry"]],
                    "completion_condition": f"quest_objective_complete:{cand['quest_id']}/{obj['index']}",
                    "coordinates": {
                        "x": coords["x"], "y": coords["y"], "z": coords["z"],
                        "radius": coords["radius"], "map_id": coords["map"],
                    },
                }
            elif cand["kind"] == "item_creature":
                new_step = {
                    "id": f"q{cand['quest_id']}_auto_item{obj['item_id']}",
                    "name": f"Quest {cand['quest_id']} kill for item {obj['item_id']}",
                    "type": "kill_mobs",
                    "quest_id": cand["quest_id"],
                    "creature_ids": cand["creature_ids"],
                    "item_id": obj["item_id"],
                    "item_count": obj["count"],
                    "completion_condition": f"item_count:{obj['item_id']}/{obj['count']}",
                    "coordinates": {
                        "x": coords["x"], "y": coords["y"], "z": coords["z"],
                        "radius": coords["radius"], "map_id": coords["map"],
                    },
                }
            elif cand["kind"] == "item_go":
                new_step = {
                    "id": f"q{cand['quest_id']}_auto_item{obj['item_id']}",
                    "name": f"Quest {cand['quest_id']} collect item {obj['item_id']} from GO",
                    "type": "collect_items",
                    "quest_id": cand["quest_id"],
                    "item_id": obj["item_id"],
                    "item_count": obj["count"],
                    "source_gameobject_entries": cand["gameobject_ids"],
                    "gameobject_id": cand["gameobject_ids"][0],
                    "completion_condition": f"item_count:{obj['item_id']}/{obj['count']}",
                    "coordinates": {
                        "x": coords["x"], "y": coords["y"], "z": coords["z"],
                        "radius": coords["radius"], "map_id": coords["map"],
                    },
                }
            else:
                continue
            steps.insert(idx + inserted, new_step)
            inserted += 1
        if inserted:
            abs_path.write_text(yaml.safe_dump(guide, sort_keys=False))


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2))


def write_inventory_md(path: Path, inventory: list[dict[str, Any]]) -> None:
    lines = [
        "# Level 1-12 Blocker Inventory",
        "",
        f"Generated: {dt.datetime.now().isoformat()}",
        "",
        "| Bot | Level | Guide | Step | Quest | Failure | Status | Classification | Live age(s) |",
        "|---|---:|---|---:|---|---|---|---|---:|",
    ]
    for row in inventory:
        failure = row["active_failure"]["event_code"] if row.get("active_failure") else ""
        quest = row["current_quest_id"] or ""
        age = "" if row["live_state_age_seconds"] is None else f"{row['live_state_age_seconds']:.0f}"
        lines.append(
            f"| {row['bot_name']} | {row['level']} | {row['guide_id']} | {row['current_step_index']} | "
            f"{quest} | {failure} | {row['failure_status']} | {row['root_cause_classification']} | {age} |"
        )
    lines.append("")
    for row in inventory:
        lines.append(f"## {row['bot_name']}")
        lines.append("")
        lines.append(f"- Race/Class/Level: {row['race']} {row['class']} {row['level']}")
        lines.append(f"- Guide: `{row['guide_id']}`")
        lines.append(f"- Guide path: `{row['guide_path']}`")
        lines.append(f"- Source guide hash: `{row['source_guide_hash']}`")
        lines.append(f"- Runtime guide path: `{row.get('runtime_guide_path')}`")
        lines.append(f"- Runtime guide hash: `{row.get('loaded_guide_hash')}`")
        lines.append(f"- Step: `{row['current_step_id']}` / {row['current_step_index']}")
        lines.append(f"- Quest: `{row['current_quest_id']}`")
        lines.append(f"- Objective: `{row['current_objective']}`")
        lines.append(f"- Position: `{row['current_position']}`")
        lines.append(f"- Target: `{row['current_target']}`")
        lines.append(f"- Failure status: `{row['failure_status']}`")
        lines.append(f"- Root cause: `{row['root_cause_classification']}`")
        if row["active_failure"]:
            lines.append(f"- Latest failure: `{row['active_failure']['event_code']}` at `{row['active_failure']['created_at']}`")
            lines.append(f"  - {row['active_failure']['detail']}")
        if row["validator_issues"]:
            lines.append("- Validator issues:")
            for issue in row["validator_issues"]:
                lines.append(f"  - `{issue['issue_type']}`: {issue['message']}")
        lines.append("")
    path.write_text("\n".join(lines))


def write_validation_md(path: Path, issues: list[Issue], fixes: list[dict[str, Any]]) -> None:
    lines = [
        "# Level 1-12 Guide Validation",
        "",
        f"Generated: {dt.datetime.now().isoformat()}",
        "",
        f"Total issues: {len(issues)}",
        f"Fix candidates: {len(fixes)}",
        "",
        "| Severity | Bot | Guide | Quest | Type | Message |",
        "|---|---|---|---:|---|---|",
    ]
    for issue in issues:
        lines.append(
            f"| {issue.severity} | {issue.bot} | {issue.guide_id} | {issue.quest_id or ''} | "
            f"{issue.issue_type} | {issue.message} |"
        )
    lines.append("")
    lines.append("## High-confidence fix candidates")
    lines.append("")
    for cand in fixes:
        lines.append(f"- `{cand['guide_id']}` q{cand['quest_id']} `{cand['kind']}` -> before `{cand.get('insert_before')}`")
    path.write_text("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--inventory-only", action="store_true")
    parser.add_argument("--apply-high-confidence", action="store_true")
    args = parser.parse_args()

    inventory, issues, fixes = validate_roster(apply_fixes=args.apply_high_confidence)

    write_json(INVENTORY_JSON, inventory)
    write_inventory_md(INVENTORY_MD, inventory)
    write_json(VALIDATION_JSON, {"issues": [i.as_dict() for i in issues], "fix_candidates": fixes})
    write_validation_md(VALIDATION_MD, issues, fixes)

    blockers = [i for i in issues if i.severity == "BLOCKER"]
    print(f"Wrote {INVENTORY_JSON}")
    print(f"Wrote {INVENTORY_MD}")
    print(f"Wrote {VALIDATION_JSON}")
    print(f"Wrote {VALIDATION_MD}")
    print(f"Blockers: {len(blockers)}  Warnings: {len(issues) - len(blockers)}")
    return 1 if blockers else 0


if __name__ == "__main__":
    sys.exit(main())
