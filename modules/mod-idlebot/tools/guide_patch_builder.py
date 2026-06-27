#!/usr/bin/env python3
"""Build and optionally apply small, DB-validated guide patches.

This tool compares live idlebot YAML guides against:
  - external Zygor-derived donor guides
  - the live AzerothCore world DB

It generates small reviewable patch descriptors by default and only edits live
guides when --apply is supplied. External guides are treated as route-shape
donors only, never as canonical truth.

Outputs:
  - <out>/patch_candidates/**/*.patch.yml
  - <module>/reports/guide_patch_summary.json
  - <module>/reports/problem_quests.json
  - <module>/reports/hunt_areas.json
  - <module>/reports/guide_patch_apply_<timestamp>.json (when --apply)
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any

import yaml


MODULE_DIR = Path(__file__).resolve().parents[1]
DEFAULT_LIVE_GUIDES = MODULE_DIR / "data" / "guides"
DEFAULT_EXTERNAL_GUIDES = Path.home() / "acore_external_guides_1_12" / "external_guides"
DEFAULT_OUT = MODULE_DIR / "patch_candidates"
REPORTS_DIR = MODULE_DIR / "reports"
BLOCKED_QUESTS_PATH = DEFAULT_LIVE_GUIDES / "blocked_quests.yml"
MAP_AREAS_PATH = MODULE_DIR.parent.parent / "tools" / "dashboard" / "frontend" / "public" / "map-tiles" / "map-areas.json"
SYNC_RELOAD = MODULE_DIR / "tools" / "sync_and_reload_guide.sh"

DEFAULT_DB_HOST = os.environ.get("IDLEBOT_HOST", "khuong@10.10.30.20")
DEFAULT_DB_CONTAINER = os.environ.get("IDLEBOT_DB_CONTAINER", "ac-database")
DEFAULT_DB_PASSWORD = os.environ.get(
    "IDLEBOT_DB_PASSWORD",
    "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53",
)
DEFAULT_WORLD_DB = os.environ.get("IDLEBOT_WORLD_DB", "acore_world")
DEFAULT_CHAR_DB = os.environ.get("IDLEBOT_CHAR_DB", "acore_characters")
DEFAULT_TARGET_QUESTS = {87, 218, 789, 1462, 1581}


def now_stamp() -> str:
    return dt.datetime.now().strftime("%Y%m%d_%H%M%S")


def norm(text: str | None) -> str:
    if not text:
        return ""
    return re.sub(r"[^a-z0-9]+", "", text.lower())


def dist2d(x1: float, y1: float, x2: float, y2: float) -> float:
    return math.sqrt((x1 - x2) ** 2 + (y1 - y2) ** 2)


def median(values: list[float]) -> float:
    if not values:
        return 0.0
    vals = sorted(values)
    n = len(vals)
    mid = n // 2
    if n % 2:
        return vals[mid]
    return (vals[mid - 1] + vals[mid]) / 2.0


def parse_csv_set(raw: str | None) -> set[str]:
    if not raw:
        return set()
    return {item.strip().lower() for item in raw.split(",") if item.strip()}


def parse_csv_ints(raw: str | None) -> set[int]:
    if not raw:
        return set()
    out: set[int] = set()
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        out.add(int(item))
    return out


def run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(cmd, text=True, capture_output=True, errors="replace")
    if check and proc.returncode != 0:
        raise RuntimeError(f"command failed: {' '.join(cmd)}\n{proc.stderr}")
    return proc


class DB:
    def __init__(self, host: str, container: str, password: str, world_db: str, char_db: str):
        self.host = host
        self.container = container
        self.password = password
        self.world_db = world_db
        self.char_db = char_db

    def query(self, db_name: str, sql: str) -> list[dict[str, str]]:
        sql = sql.replace('"', '\\"')
        remote = (
            f"docker exec {self.container} mysql -B -uroot -p'{self.password}' {db_name} "
            f'-e "{sql}" 2>/dev/null'
        )
        proc = run(["ssh", self.host, remote], check=False)
        if proc.returncode != 0 or not proc.stdout.strip():
            return []
        lines = [line for line in proc.stdout.splitlines() if line.strip()]
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

    def scalar(self, db_name: str, sql: str) -> str:
        sql = sql.replace('"', '\\"')
        remote = (
            f"docker exec {self.container} mysql -N -uroot -p'{self.password}' {db_name} "
            f'-e "{sql}" 2>/dev/null'
        )
        proc = run(["ssh", self.host, remote], check=False)
        return proc.stdout.strip()


@dataclass
class ExternalHint:
    quest_id: int
    guide_file: str
    faction: str
    race: str
    cls: str
    source_step: int
    action_types: list[str]
    raw: list[str]
    local_coord: dict[str, Any] | None
    npc_ids: list[int]
    item_name: str | None
    objective_index: int | None


@dataclass
class CandidatePatch:
    guide_rel: str
    guide_id: str
    quest_id: int | None
    patch_name: str
    status: str
    confidence: str
    reason: str
    operations: list[dict[str, Any]]
    validation: dict[str, Any]
    external_sources: list[str]
    live_confirmed: bool = False


class PatchBuilder:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.live_root = Path(args.live_guides).resolve()
        self.external_root = Path(args.external_guides).resolve()
        self.out_root = Path(args.out).resolve()
        self.report_dir = REPORTS_DIR
        self.db = DB(args.db_host, args.db_container, args.db_password, args.world_db, args.char_db)
        self.timestamp = now_stamp()
        self.blocked = self.load_blocked_quests()
        self.map_areas = self.load_map_areas()
        self.external_hints = self.load_external_hints()
        self.summary: list[dict[str, Any]] = []
        self.problem_quests: list[dict[str, Any]] = []
        self.hunt_areas: list[dict[str, Any]] = []
        self.applied: list[dict[str, Any]] = []
        self.changed_guides: set[str] = set()
        self.quest_filter = parse_csv_ints(args.quest) if args.quest else set(DEFAULT_TARGET_QUESTS)
        self.race_filter = parse_csv_set(args.race)
        self.class_filter = parse_csv_set(args.cls)

    def load_blocked_quests(self) -> dict[int, dict[str, Any]]:
        data = yaml.safe_load(BLOCKED_QUESTS_PATH.read_text()) or {}
        out: dict[int, dict[str, Any]] = {}
        for item in data.get("quests", []):
            qid = int(item["quest_id"])
            out[qid] = item
        return out

    def load_map_areas(self) -> dict[str, dict[str, Any]]:
        if not MAP_AREAS_PATH.exists():
            return {}
        rows = json.loads(MAP_AREAS_PATH.read_text())
        out: dict[str, dict[str, Any]] = {}
        for row in rows:
            key = norm(row.get("displayName") or row.get("areaName"))
            if key and key not in out:
                out[key] = row
        return out

    def load_external_hints(self) -> dict[int, list[ExternalHint]]:
        hints: dict[int, list[ExternalHint]] = defaultdict(list)
        if not self.external_root.exists():
            return hints
        for path in sorted(self.external_root.rglob("*.yml")):
            doc = yaml.safe_load(path.read_text()) or {}
            req = doc.get("requirements", {})
            faction = (req.get("faction") or "").lower()
            race = (req.get("race") or "").lower()
            cls = (req.get("class") or "any").lower()
            for step in doc.get("steps", []):
                actions = step.get("actions", [])
                action_types = [a.get("type", "") for a in actions]
                quest_ids: set[int] = set()
                npc_ids: list[int] = []
                item_name = None
                objective_index = None
                local_coord = None
                for action in actions:
                    if action.get("local_coord") and not local_coord:
                        local_coord = action["local_coord"]
                    quest = action.get("quest") or {}
                    if quest.get("id"):
                        quest_ids.add(int(quest["id"]))
                    quest_ref = action.get("quest_ref") or {}
                    if quest_ref.get("quest"):
                        quest_ids.add(int(quest_ref["quest"]))
                        if quest_ref.get("objective") is not None:
                            objective_index = int(quest_ref["objective"])
                    for npc in action.get("npcs", []):
                        if npc.get("id"):
                            npc_ids.append(int(npc["id"]))
                    npc = action.get("npc") or {}
                    if npc.get("id"):
                        npc_ids.append(int(npc["id"]))
                    if action.get("item_name"):
                        item_name = action["item_name"]
                for qid in sorted(quest_ids):
                    hints[qid].append(
                        ExternalHint(
                            quest_id=qid,
                            guide_file=str(path.relative_to(self.external_root)),
                            faction=faction,
                            race=race,
                            cls=cls,
                            source_step=int(step.get("source_step") or 0),
                            action_types=action_types,
                            raw=step.get("raw", []),
                            local_coord=local_coord,
                            npc_ids=sorted(set(npc_ids)),
                            item_name=item_name,
                            objective_index=objective_index,
                        )
                    )
        return hints

    def live_guides(self) -> list[tuple[Path, dict[str, Any]]]:
        guides: list[tuple[Path, dict[str, Any]]] = []
        for path in sorted(self.live_root.rglob("*.yaml")):
            if "generated_backup" in path.parts:
                continue
            rel = path.relative_to(self.live_root)
            if rel.parts[:1] == ("generated",):
                continue
            if path.name == "blocked_quests.yml":
                continue
            try:
                data = yaml.safe_load(path.read_text()) or {}
            except Exception as exc:
                self.problem_quests.append({"guide": str(rel), "problem": f"yaml_parse_failed: {exc}"})
                continue
            if not data.get("id"):
                continue
            if self.race_filter and (data.get("race") or "").lower() not in self.race_filter:
                continue
            if self.class_filter and (data.get("class") or "any").lower() not in self.class_filter and "any" not in self.class_filter:
                continue
            guides.append((path, data))
        return guides

    def local_to_world(self, local_coord: dict[str, Any] | None) -> dict[str, Any] | None:
        if not local_coord:
            return None
        zone_key = norm(local_coord.get("zone"))
        area = self.map_areas.get(zone_key)
        if not area:
            return None
        x_pct = float(local_coord["x"])
        y_pct = float(local_coord["y"])
        loc_left = float(area["locLeft"])
        loc_right = float(area["locRight"])
        loc_top = float(area["locTop"])
        loc_bottom = float(area["locBottom"])
        world_y = loc_left + (loc_right - loc_left) * (x_pct / 100.0)
        world_x = loc_top + (loc_bottom - loc_top) * (y_pct / 100.0)
        return {
            "map_id": int(area["mapId"]),
            "x": round(world_x, 2),
            "y": round(world_y, 2),
            "z": None,
            "zone": local_coord.get("zone"),
            "source_raw": local_coord.get("raw"),
        }

    def fetch_quest_rows(self, quest_ids: set[int]) -> dict[int, dict[str, Any]]:
        if not quest_ids:
            return {}
        ids = ",".join(str(q) for q in sorted(quest_ids))
        rows = self.db.query(
            self.db.world_db,
            "SELECT ID, LogTitle, QuestLevel, MinLevel, PrevQuestID, NextQuestID, "
            "RequiredNpcOrGo1, RequiredNpcOrGo2, RequiredNpcOrGo3, RequiredNpcOrGo4, "
            "RequiredNpcOrGoCount1, RequiredNpcOrGoCount2, RequiredNpcOrGoCount3, RequiredNpcOrGoCount4, "
            "RequiredItemId1, RequiredItemId2, RequiredItemId3, RequiredItemId4, "
            "RequiredItemCount1, RequiredItemCount2, RequiredItemCount3, RequiredItemCount4 "
            f"FROM quest_template WHERE ID IN ({ids})",
        )
        return {int(row["ID"]): row for row in rows}

    def fetch_creature_names(self, creature_ids: set[int]) -> dict[int, str]:
        if not creature_ids:
            return {}
        ids = ",".join(str(c) for c in sorted(creature_ids))
        rows = self.db.query(self.db.world_db, f"SELECT entry, name FROM creature_template WHERE entry IN ({ids})")
        return {int(r["entry"]): r["name"] for r in rows}

    def fetch_spawns(self, creature_ids: set[int]) -> dict[int, list[dict[str, Any]]]:
        if not creature_ids:
            return {}
        ids = ",".join(str(c) for c in sorted(creature_ids))
        rows = self.db.query(
            self.db.world_db,
            f"SELECT id1, map, position_x, position_y, position_z FROM creature WHERE id1 IN ({ids})",
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

    def fetch_loot_sources(self, item_ids: set[int]) -> dict[int, list[dict[str, Any]]]:
        if not item_ids:
            return {}
        ids = ",".join(str(i) for i in sorted(item_ids))
        rows = self.db.query(
            self.db.world_db,
            f"SELECT Entry, Item, Chance, QuestRequired, MinCount, MaxCount FROM creature_loot_template WHERE Item IN ({ids})",
        )
        out: dict[int, list[dict[str, Any]]] = defaultdict(list)
        for row in rows:
            out[int(row["Item"])].append(
                {
                    "entry": int(row["Entry"]),
                    "chance": float(row["Chance"]),
                    "quest_required": int(row["QuestRequired"]),
                    "min": int(row["MinCount"]),
                    "max": int(row["MaxCount"]),
                }
            )
        return out

    def compute_hunt_area(self, quest_id: int, step: dict[str, Any], quest_row: dict[str, Any], spawns_by_creature: dict[int, list[dict[str, Any]]]) -> dict[str, Any] | None:
        creature_ids = set(int(c) for c in step.get("creature_ids", []))
        if not creature_ids:
            for idx in range(1, 5):
                val = int(quest_row.get(f"RequiredNpcOrGo{idx}", "0") or 0)
                if val > 0:
                    creature_ids.add(val)
        map_id = int(step.get("map_id", step.get("coordinates", {}).get("map_id", 0)) or 0)
        spawns: list[dict[str, Any]] = []
        for cid in creature_ids:
            spawns.extend([s for s in spawns_by_creature.get(cid, []) if s["map"] == map_id])
        if not spawns:
            return None
        xs = [s["x"] for s in spawns]
        ys = [s["y"] for s in spawns]
        zs = [s["z"] for s in spawns]
        centroid_x = sum(xs) / len(xs)
        centroid_y = sum(ys) / len(ys)
        centroid_z = sum(zs) / len(zs)
        radius = max(dist2d(centroid_x, centroid_y, s["x"], s["y"]) for s in spawns)
        hunt_area = {
            "quest_id": quest_id,
            "map": map_id,
            "centroid": {
                "x": round(centroid_x, 2),
                "y": round(centroid_y, 2),
                "z": round(centroid_z, 2),
                "o": 0.0,
            },
            "radius": round(radius + 20.0, 2),
            "spawn_count": len(spawns),
            "creature_ids": sorted(creature_ids),
            "source_quest_id": quest_id,
        }
        self.hunt_areas.append(hunt_area)
        return hunt_area

    def transition_target(self, guide: dict[str, Any]) -> str | None:
        faction = (guide.get("faction") or "").lower()
        race = (guide.get("race") or "").lower()
        level_max = int(guide.get("level_max") or 0)
        if level_max < 12:
            return None
        if faction == "alliance" and race in {"human", "dwarf", "gnome", "nightelf"}:
            return "alliance-dwarf-1-18"
        if faction == "horde" and race in {"orc", "troll", "tauren"}:
            return "horde-tauren-1-18"
        if faction == "horde" and race == "undead":
            return "horde-undead-1-18"
        return None

    def make_approach_step(self, quest_id: int, hint: ExternalHint, kill_step: dict[str, Any], world_coord: dict[str, Any]) -> dict[str, Any]:
        cavey = any(re.search(r"(enter|inside|cave|mine|lower entrance)", raw, re.I) for raw in hint.raw)
        npc_name = None
        if hint.raw:
            for raw in hint.raw:
                m = re.search(r"\.from\s+([^#|]+)", raw)
                if m:
                    npc_name = m.group(1).strip().rstrip("+")
                    break
        if cavey and hint.local_coord and hint.local_coord.get("zone"):
            name = f"Enter {hint.local_coord['zone']} lower entrance"
        elif npc_name:
            name = f"Approach {npc_name}"
        else:
            name = f"Approach quest {quest_id} objective"
        step = {
            "id": f"q{quest_id}_approach",
            "name": name,
            "type": "move_to",
            "quest_id": quest_id,
            "coordinates": {
                "x": world_coord["x"],
                "y": world_coord["y"],
                "z": round(float(kill_step.get("coordinates", {}).get("z") or 0.0), 2),
                "radius": 20.0 if cavey else 18.0,
                "map_id": world_coord["map_id"],
            },
        }
        return step

    def live_has_equivalent_approach(self, steps: list[dict[str, Any]], quest_id: int, world_coord: dict[str, Any]) -> bool:
        for step in steps:
            if step.get("quest_id") != quest_id or step.get("type") != "move_to":
                continue
            coords = step.get("coordinates", {})
            if int(coords.get("map_id", 0) or 0) != world_coord["map_id"]:
                continue
            if dist2d(float(coords.get("x", 0.0)), float(coords.get("y", 0.0)), world_coord["x"], world_coord["y"]) <= 35.0:
                return True
        return False

    def find_relevant_hint(self, guide: dict[str, Any], quest_id: int) -> ExternalHint | None:
        hints = self.external_hints.get(quest_id, [])
        if not hints:
            return None
        race = (guide.get("race") or "").lower()
        faction = (guide.get("faction") or "").lower()
        cls = (guide.get("class") or "any").lower()
        for hint in hints:
            if hint.faction and hint.faction != faction:
                continue
            if hint.race and hint.race != race:
                continue
            if cls != "any" and hint.cls not in {cls, "any"}:
                continue
            return hint
        return hints[0]

    def validate_candidate(self, guide: dict[str, Any], patch: CandidatePatch, quest_rows: dict[int, dict[str, Any]], spawns_by_creature: dict[int, list[dict[str, Any]]]) -> dict[str, Any]:
        result = {"ok": True, "checks": []}
        qid = patch.quest_id
        if qid and qid in quest_rows:
            result["checks"].append(f"quest {qid} exists")
        else:
            result["ok"] = False
            result["checks"].append(f"quest {qid} missing from DB")
        for op in patch.operations:
            if op["op"] == "insert_before":
                step = op["step"]
                coords = step.get("coordinates", {})
                if int(coords.get("map_id", -1)) < 0:
                    result["ok"] = False
                    result["checks"].append("insert step has invalid map")
            elif op["op"] == "update_step":
                step = op["value"]
                for cid in step.get("creature_ids", []):
                    if not spawns_by_creature.get(int(cid)):
                        result["ok"] = False
                        result["checks"].append(f"creature {cid} has no spawns")
        if patch.status == "blocked_excluded":
            result["checks"].append("blocked quest registry enforced")
        return result

    def build_candidates(self) -> list[CandidatePatch]:
        quest_ids_seen: set[int] = set()
        candidates: list[CandidatePatch] = []
        guides = self.live_guides()
        quest_ids_needed = set(self.quest_filter)
        for _, guide in guides:
            for step in guide.get("steps", []):
                qid = step.get("quest_id")
                if qid:
                    quest_ids_needed.add(int(qid))
        quest_rows = self.fetch_quest_rows(quest_ids_needed)
        creature_ids_needed: set[int] = set()
        item_ids_needed: set[int] = set()
        for _, guide in guides:
            for step in guide.get("steps", []):
                creature_ids_needed.update(int(c) for c in step.get("creature_ids", []))
                if step.get("item_id"):
                    item_ids_needed.add(int(step["item_id"]))
        for row in quest_rows.values():
            for idx in range(1, 5):
                val = int(row.get(f"RequiredNpcOrGo{idx}", "0") or 0)
                if val > 0:
                    creature_ids_needed.add(val)
                item = int(row.get(f"RequiredItemId{idx}", "0") or 0)
                if item > 0:
                    item_ids_needed.add(item)
        spawns_by_creature = self.fetch_spawns(creature_ids_needed)
        creature_names = self.fetch_creature_names(creature_ids_needed)
        loot_sources = self.fetch_loot_sources(item_ids_needed)

        for path, guide in guides:
            rel = str(path.relative_to(self.live_root))
            guide_id = guide["id"]
            next_target = self.transition_target(guide) if self.args.continue_next_section else None
            if next_target and guide.get("next_guide") != next_target:
                patch = CandidatePatch(
                    guide_rel=rel,
                    guide_id=guide_id,
                    quest_id=None,
                    patch_name=f"{guide_id}--next-guide.patch.yml",
                    status="candidate",
                    confidence="medium",
                    reason=f"starter handoff should continue into {next_target}",
                    operations=[{"op": "set_top_level", "field": "next_guide", "value": next_target}],
                    validation={"ok": True, "checks": [f"next_guide target {next_target} selected"]},
                    external_sources=[],
                )
                candidates.append(patch)

            steps = guide.get("steps", [])
            indexed_steps = {step.get("id"): idx for idx, step in enumerate(steps) if step.get("id")}
            quests_in_guide = sorted({int(step.get("quest_id")) for step in steps if step.get("quest_id")})
            for qid in quests_in_guide:
                if qid not in self.quest_filter:
                    continue
                quest_ids_seen.add(qid)
                patch_name = f"{guide_id}--q{qid}.patch.yml"
                if qid in self.blocked:
                    blocked_steps = [s for s in steps if int(s.get("quest_id") or 0) == qid]
                    status = "blocked_excluded"
                    ops: list[dict[str, Any]] = []
                    if blocked_steps:
                        ops.append({
                            "op": "remove_quest_steps",
                            "quest_id": qid,
                            "step_ids": [s.get("id") for s in blocked_steps if s.get("id")],
                        })
                    patch = CandidatePatch(
                        guide_rel=rel,
                        guide_id=guide_id,
                        quest_id=qid,
                        patch_name=patch_name,
                        status=status,
                        confidence="high",
                        reason=self.blocked[qid]["reason"],
                        operations=ops,
                        validation={"ok": True, "checks": ["blocked quest handled by registry"]},
                        external_sources=[hint.guide_file for hint in self.external_hints.get(qid, [])],
                    )
                    candidates.append(patch)
                    continue

                live_steps = [s for s in steps if int(s.get("quest_id") or 0) == qid]
                kill_step = next((s for s in live_steps if s.get("type") == "kill_mobs"), None)
                hint = self.find_relevant_hint(guide, qid)
                external_sources = [hint.guide_file] if hint else []
                ops = []
                status = "no_change"
                confidence = "low"
                reason = "no donor delta found"

                if kill_step and qid in {87, 218} and hint and hint.local_coord:
                    world_coord = self.local_to_world(hint.local_coord)
                    if world_coord:
                        if self.live_has_equivalent_approach(steps, qid, world_coord):
                            status = "live_confirmed_fixed" if qid == 218 else "no_change"
                            confidence = "high"
                            reason = "live guide already has equivalent approach step"
                        else:
                            anchor_id = kill_step.get("id")
                            if anchor_id:
                                ops.append({
                                    "op": "insert_before",
                                    "anchor_step_id": anchor_id,
                                    "step": self.make_approach_step(qid, hint, kill_step, world_coord),
                                    "meta": {
                                        "requires_clear_to_objective": True,
                                        "source": "external_route_shape",
                                    },
                                })
                                status = "candidate"
                                confidence = "high"
                                reason = "external guide has better approach/cave pathing"
                    else:
                        self.problem_quests.append({
                            "quest_id": qid,
                            "guide": rel,
                            "problem": f"missing WorldMapArea conversion for {hint.local_coord.get('zone')}",
                        })

                if kill_step and qid == 789:
                    qrow = quest_rows.get(qid)
                    if qrow:
                        hunt_area = self.compute_hunt_area(qid, kill_step, qrow, spawns_by_creature)
                        if hunt_area:
                            current_coords = kill_step.get("coordinates", {})
                            current_dist = dist2d(
                                float(current_coords.get("x", 0.0)),
                                float(current_coords.get("y", 0.0)),
                                hunt_area["centroid"]["x"],
                                hunt_area["centroid"]["y"],
                            )
                            if current_dist > 140.0:
                                new_step = json.loads(json.dumps(kill_step))
                                new_step["coordinates"]["x"] = hunt_area["centroid"]["x"]
                                new_step["coordinates"]["y"] = hunt_area["centroid"]["y"]
                                new_step["coordinates"]["z"] = hunt_area["centroid"]["z"]
                                new_step["coordinates"]["radius"] = max(float(current_coords.get("radius", 0.0)), hunt_area["radius"])
                                ops.append({
                                    "op": "update_step",
                                    "step_id": kill_step["id"],
                                    "value": new_step,
                                    "meta": {"hunt_area": hunt_area},
                                })
                                status = "candidate"
                                confidence = "medium"
                                reason = "DB-derived Scorpid Worker hunt area is more specific than current anchor"
                            else:
                                status = "no_change"
                                confidence = "high"
                                reason = "live guide already uses DB-consistent Scorpid Worker hunting grounds"

                patch = CandidatePatch(
                    guide_rel=rel,
                    guide_id=guide_id,
                    quest_id=qid,
                    patch_name=patch_name,
                    status=status,
                    confidence=confidence,
                    reason=reason,
                    operations=ops,
                    validation={},
                    external_sources=external_sources,
                    live_confirmed=(qid == 218 and status == "live_confirmed_fixed"),
                )
                patch.validation = self.validate_candidate(guide, patch, quest_rows, spawns_by_creature)
                candidates.append(patch)

                if qid in {87, 218, 789} and qid in quest_rows:
                    self.problem_quests.append({
                        "quest_id": qid,
                        "guide": rel,
                        "title": quest_rows[qid]["LogTitle"],
                        "live_status": status,
                        "loot_sources": loot_sources.get(int(kill_step.get("item_id", 0) or 0), []),
                        "creature_names": {str(cid): creature_names.get(cid, "") for cid in kill_step.get("creature_ids", [])} if kill_step else {},
                    })

        for qid in sorted(self.quest_filter - quest_ids_seen):
            self.problem_quests.append({"quest_id": qid, "problem": "quest not present in scanned live guides"})

        return candidates

    def write_candidates(self, candidates: list[CandidatePatch]) -> None:
        for candidate in candidates:
            out_path = self.out_root / "patch_candidates" / candidate.guide_rel
            out_path = out_path.with_name(f"{out_path.stem}--{candidate.patch_name}")
            out_path.parent.mkdir(parents=True, exist_ok=True)
            with open(out_path, "w") as f:
                yaml.safe_dump(asdict(candidate), f, sort_keys=False, width=120)
            self.summary.append({
                "guide": candidate.guide_rel,
                "guide_id": candidate.guide_id,
                "quest_id": candidate.quest_id,
                "status": candidate.status,
                "confidence": candidate.confidence,
                "reason": candidate.reason,
                "patch_file": str(out_path),
            })

        self.report_dir.mkdir(parents=True, exist_ok=True)
        (self.report_dir / "guide_patch_summary.json").write_text(json.dumps(self.summary, indent=2))
        (self.report_dir / "problem_quests.json").write_text(json.dumps(self.problem_quests, indent=2))
        (self.report_dir / "hunt_areas.json").write_text(json.dumps(self.hunt_areas, indent=2))

    def backup_file(self, path: Path) -> Path:
        backup = path.with_suffix(path.suffix + f".bak.{self.timestamp}")
        shutil.copy2(path, backup)
        return backup

    def apply_candidate(self, candidate: CandidatePatch) -> dict[str, Any]:
        path = self.live_root / candidate.guide_rel
        guide = yaml.safe_load(path.read_text()) or {}
        steps = guide.get("steps", [])
        backup = self.backup_file(path)
        changed = False
        for op in candidate.operations:
            if op["op"] == "insert_before":
                anchor = op["anchor_step_id"]
                idx = next((i for i, s in enumerate(steps) if s.get("id") == anchor), None)
                if idx is None:
                    continue
                steps.insert(idx, op["step"])
                changed = True
            elif op["op"] == "update_step":
                step_id = op["step_id"]
                idx = next((i for i, s in enumerate(steps) if s.get("id") == step_id), None)
                if idx is None:
                    continue
                steps[idx] = op["value"]
                changed = True
            elif op["op"] == "set_top_level":
                guide[op["field"]] = op["value"]
                changed = True
            elif op["op"] == "remove_quest_steps":
                qid = op["quest_id"]
                new_steps = [s for s in steps if int(s.get("quest_id") or 0) != qid]
                if len(new_steps) != len(steps):
                    guide["steps"] = new_steps
                    steps = new_steps
                    changed = True
        if not changed:
            return {"guide": candidate.guide_rel, "status": "skipped", "reason": "no changes applied"}
        guide["steps"] = steps
        path.write_text(yaml.safe_dump(guide, sort_keys=False, width=120))
        try:
            yaml.safe_load(path.read_text())
        except Exception as exc:
            shutil.copy2(backup, path)
            return {"guide": candidate.guide_rel, "status": "reverted", "reason": f"yaml invalid after apply: {exc}"}
        self.changed_guides.add(candidate.guide_rel)
        return {
            "guide": candidate.guide_rel,
            "status": "applied",
            "backup": str(backup),
            "patch": candidate.patch_name,
            "quest_id": candidate.quest_id,
        }

    def reload_changed_guides(self) -> list[dict[str, Any]]:
        results = []
        for guide_rel in sorted(self.changed_guides):
            if self.args.reload != "guide":
                continue
            proc = run([str(SYNC_RELOAD), guide_rel], check=False)
            if proc.returncode != 0 and self.args.reload_fallback_restart:
                restart = run(
                    [
                        "ssh",
                        self.args.db_host,
                        "docker compose -p zoidberg-stack --env-file ~/secrets/shared.env --env-file ~/secrets/zoidberg.env "
                        "-f ~/homelab/compose/zoidberg/compose.yml restart ac-worldserver",
                    ],
                    check=False,
                )
                results.append({
                    "guide": guide_rel,
                    "reload": "restart" if restart.returncode == 0 else "failed",
                    "stdout": restart.stdout,
                    "stderr": restart.stderr,
                })
            else:
                results.append({
                    "guide": guide_rel,
                    "reload": "guide" if proc.returncode == 0 else "failed",
                    "stdout": proc.stdout,
                    "stderr": proc.stderr,
                })
        return results

    def watch_bots(self, quest_ids: set[int]) -> list[dict[str, Any]]:
        if not quest_ids:
            return []
        sql = (
            "SELECT bot_name, state, guide_id, step_index, step_name, quest_id, objective_text, "
            "target_entry, target_name, target_distance, x, y, z, updated_at "
            "FROM idlebot_live_state "
            f"WHERE quest_id IN ({','.join(str(q) for q in sorted(quest_ids))}) "
            "ORDER BY bot_name"
        )
        snapshots = []
        for _ in range(max(1, self.args.watch_iterations)):
            rows = self.db.query(self.db.char_db, sql)
            snapshots.append({"captured_at": dt.datetime.utcnow().isoformat() + "Z", "rows": rows})
            if self.args.watch_iterations > 1:
                import time
                time.sleep(self.args.watch_interval)
        return snapshots

    def write_apply_log(self, reload_results: list[dict[str, Any]], watches: list[dict[str, Any]]) -> None:
        payload = {
            "timestamp": self.timestamp,
            "applied": self.applied,
            "reload_results": reload_results,
            "watch": watches,
        }
        (self.report_dir / f"guide_patch_apply_{self.timestamp}.json").write_text(json.dumps(payload, indent=2))

    def run(self) -> int:
        candidates = self.build_candidates()
        self.write_candidates(candidates)
        if not self.args.apply:
            return 0

        for candidate in candidates:
            if candidate.status not in {"candidate", "blocked_excluded"}:
                continue
            if not candidate.validation.get("ok", False):
                continue
            if candidate.confidence not in {"high", "medium"}:
                continue
            if not candidate.operations:
                continue
            self.applied.append(self.apply_candidate(candidate))

        reload_results: list[dict[str, Any]] = []
        if self.changed_guides:
            reload_results = self.reload_changed_guides()
        watches = self.watch_bots({c.quest_id for c in candidates if c.quest_id})
        self.write_apply_log(reload_results, watches)
        return 0


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--live-guides", default=str(DEFAULT_LIVE_GUIDES))
    p.add_argument("--external-guides", default=str(DEFAULT_EXTERNAL_GUIDES))
    p.add_argument("--world-db", default=DEFAULT_WORLD_DB)
    p.add_argument("--char-db", default=DEFAULT_CHAR_DB)
    p.add_argument("--db-host", default=DEFAULT_DB_HOST)
    p.add_argument("--db-container", default=DEFAULT_DB_CONTAINER)
    p.add_argument("--db-password", default=DEFAULT_DB_PASSWORD)
    p.add_argument("--out", default=str(DEFAULT_OUT))
    p.add_argument("--quest", help="Comma-separated quest IDs")
    p.add_argument("--race", help="Comma-separated race filter")
    p.add_argument("--class", dest="cls", help="Comma-separated class filter")
    p.add_argument("--apply", action="store_true", help="Apply candidate patches to live YAML")
    p.add_argument("--reload", choices=["none", "guide"], default="none", help="Reload changed guides after apply")
    p.add_argument("--reload-fallback-restart", action="store_true", help="Restart worldserver if guide reload fails")
    p.add_argument("--watch-bots", action="store_true", help="Capture bot telemetry for touched quests")
    p.add_argument("--watch-iterations", type=int, default=1)
    p.add_argument("--watch-interval", type=int, default=15)
    p.add_argument("--continue-next-section", action="store_true", help="Generate handoff patches for starter->next-section guides")
    return p


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()
    builder = PatchBuilder(args)
    if not args.watch_bots:
        args.watch_iterations = 0
    return builder.run()


if __name__ == "__main__":
    sys.exit(main())
