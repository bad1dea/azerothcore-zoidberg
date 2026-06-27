#!/usr/bin/env python3
"""
Offline IdleBot leveling simulator / validator.

Roleplays a pretend character through the YAML guide chain against the live
AzerothCore DB (via pre-exported JSON snapshots) and reports plain-English
validation results.

Usage examples:

  # Single bot:
  python3 simulate_leveling_offline.py \
    --race Human --class Priest --faction Alliance \
    --start-level 1 --target-level 12 \
    --guide alliance/human/00_northshire-1-6.yaml

  # Smoke roster (reads from reset_idlebot_test_roster.sh):
  python3 simulate_leveling_offline.py --profile smoke --target-level 12

  # Fail on any error (pre-soak gate):
  python3 simulate_leveling_offline.py --profile smoke --target-level 12 --fail-on-error

  # Write YAML patches:
  python3 simulate_leveling_offline.py --profile smoke --target-level 12 --write-patches

Stages:
  Stage 1: smoke roster 1→12      (--profile smoke --target-level 12)
  Stage 2: starting-zone 1→12     (--profile starting-zone --target-level 12)
  Stage 3: full matrix 1→20       (--profile full-matrix --target-level 20)
  Stage 4: full path 1→80         (--profile full-matrix --target-level 80)

Death Knights are excluded.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import os
import re
import sys
import textwrap
from collections import defaultdict
from pathlib import Path
from typing import Any, Optional

import yaml

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

MODULE_DIR   = Path(__file__).resolve().parents[1]
GUIDES_DIR   = MODULE_DIR / "data" / "guides"
DATA_DIR     = MODULE_DIR / "data" / "generated"
REPORTS_DIR  = MODULE_DIR / "reports" / "offline-sim"
TOOLS_DIR    = MODULE_DIR / "tools"
ROSTER_SH    = TOOLS_DIR / "reset_idlebot_test_roster.sh"

# ---------------------------------------------------------------------------
# WoW Constants
# ---------------------------------------------------------------------------

RACE_IDS = {
    "human": 1, "orc": 2, "dwarf": 3, "nightelf": 4, "undead": 5,
    "tauren": 6, "gnome": 7, "troll": 8, "bloodelf": 10, "draenei": 11,
}
RACE_NAMES = {v: k.title() for k, v in RACE_IDS.items()}
RACE_MASKS = {1: 1, 2: 2, 3: 4, 4: 8, 5: 16, 6: 32, 7: 64, 8: 128, 10: 512, 11: 1024}

CLASS_IDS = {
    "warrior": 1, "paladin": 2, "hunter": 3, "rogue": 4, "priest": 5,
    "shaman": 7, "mage": 8, "warlock": 9, "druid": 11,
}
CLASS_NAMES = {v: k.title() for k, v in CLASS_IDS.items()}
CLASS_MASKS = {
    1: 1 << 0, 2: 1 << 1, 3: 1 << 2, 4: 1 << 3, 5: 1 << 4,
    7: 1 << 6, 8: 1 << 7, 9: 1 << 8, 11: 1 << 10,
}
# Death Knight excluded per spec.
EXCLUDED_CLASSES = {6}

ALLIANCE_RACES = {1, 3, 4, 7, 11}
HORDE_RACES    = {2, 5, 6, 8, 10}

FACTION_RACES = {"alliance": ALLIANCE_RACES, "horde": HORDE_RACES}
RACE_FACTION  = {r: "alliance" for r in ALLIANCE_RACES}
RACE_FACTION.update({r: "horde" for r in HORDE_RACES})

# Valid race/class combos in 3.3.5a (excluding DK).
VALID_RACE_CLASS: dict[int, set[int]] = {
    1:  {1, 2, 4, 5, 8, 9},        # Human
    2:  {1, 3, 4, 7, 8, 9},        # Orc
    3:  {1, 2, 3, 4, 5},           # Dwarf
    4:  {1, 3, 4, 5, 11},          # NightElf
    5:  {1, 4, 5, 8, 9},           # Undead
    6:  {1, 3, 7, 11},             # Tauren
    7:  {1, 4, 8, 9},              # Gnome
    8:  {1, 3, 4, 7, 8},           # Troll
    10: {2, 4, 5, 8, 9},           # BloodElf
    11: {1, 2, 3, 5, 7},           # Draenei
}

# WoW 3.3.5a: XP required to reach each level (from the previous level).
XP_TO_LEVEL: dict[int, int] = {
    2: 400, 3: 900, 4: 1400, 5: 2100, 6: 2800, 7: 3600,
    8: 4500, 9: 5400, 10: 6500, 11: 7600, 12: 8800,
    13: 10100, 14: 11400, 15: 12900, 16: 14400, 17: 16000,
    18: 17700, 19: 19400, 20: 21300, 21: 23200, 22: 25200,
    23: 27300, 24: 29400, 25: 31700, 26: 34000, 27: 36400,
    28: 38900, 29: 41400, 30: 44300,
    35: 72000, 40: 130000, 50: 300000, 60: 670000, 70: 1500000, 80: 2700000,
}

# Quest XP reward table [quest_level][RewardXPDifficulty 0-6]
# Approximate values from QuestXPFact.dbc for 3.3.5a.
_QXP: dict[int, list[int]] = {
    1:  [50,  125,  160,  200,  250,  320,  400],
    2:  [90,  230,  290,  360,  450,  575,  720],
    3:  [135, 340,  425,  530,  670,  840,  1050],
    4:  [180, 450,  560,  710,  890,  1110, 1390],
    5:  [225, 565,  710,  890,  1110, 1395, 1740],
    6:  [270, 670,  840,  1050, 1320, 1645, 2060],
    7:  [315, 790,  990,  1240, 1555, 1940, 2430],
    8:  [360, 900,  1130, 1415, 1775, 2215, 2775],
    9:  [405, 1015, 1270, 1590, 1995, 2495, 3120],
    10: [450, 1130, 1415, 1770, 2220, 2770, 3470],
    11: [540, 1355, 1695, 2120, 2660, 3320, 4155],
    12: [630, 1580, 1975, 2470, 3095, 3865, 4840],
}

ACTION_NAMES: dict[str, str] = {
    "accept_quest":          "Accept quest",
    "turn_in_quest":         "Turn in quest",
    "kill_mobs":             "Kill enemies",
    "collect_items":         "Collect items",
    "interact_gameobject":   "Click/use object",
    "use_item_at_location":  "Use quest item",
    "use_item":              "Use quest item",
    "move_to":               "Move to location",
    "explore_area":          "Discover area",
    "area_trigger":          "Explore/discover area",
    "train":                 "Train new spells",
    "vendor":                "Sell or buy from vendor",
    "repair":                "Repair gear",
    "grind":                 "Grind XP",
    "hearth":                "Use Hearthstone",
    "class_travel":          "Use class travel ability",
    "transport":             "Use boat/zeppelin/tram",
    "flight_path":           "Take flight path",
}

SKIP_DIRS = {"generated", "generated_backup"}

# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class BotProfile:
    name: str
    race_id: int
    class_id: int
    faction: str
    start_guide_id: str
    start_level: int = 1
    start_x: float = 0.0
    start_y: float = 0.0
    start_z: float = 0.0
    start_map: int = 0


@dataclasses.dataclass
class SimFailure:
    code: str
    severity: str  # "FAIL" | "WARN" | "INFO"
    step_index: int
    step_id: str
    quest_id: Optional[int]
    guide_id: str
    guide_path: str
    title: str
    objective: str
    problem: str
    suggested_fix: str
    debug: dict


@dataclasses.dataclass
class VirtualChar:
    bot_name: str
    race_id: int
    class_id: int
    faction: str
    level: float
    xp: int
    map_id: int
    pos: tuple[float, float, float]
    active_quests: dict    # qid -> {"objectives_done": set[int], "accepted_step": int}
    rewarded_quests: set   # quest IDs turned in with reward
    known_quests_started: set  # all accepted
    inventory: dict        # item_id -> count
    failures: list
    warnings: list
    filtered_steps: list
    log: list

    @property
    def race_name(self) -> str:
        return RACE_NAMES.get(self.race_id, f"Race{self.race_id}")

    @property
    def class_name(self) -> str:
        return CLASS_NAMES.get(self.class_id, f"Class{self.class_id}")

    @property
    def race_mask(self) -> int:
        return RACE_MASKS.get(self.race_id, 0)

    @property
    def class_mask(self) -> int:
        return CLASS_MASKS.get(self.class_id, 0)

    def xp_to_next(self) -> int:
        lvl = int(self.level)
        return XP_TO_LEVEL.get(lvl + 1, 99999999)

    def gain_xp(self, amount: int):
        self.xp += amount
        while True:
            lvl = int(self.level)
            needed = XP_TO_LEVEL.get(lvl + 1)
            if needed is None or self.xp < needed:
                break
            self.xp -= needed
            self.level = float(lvl + 1)


# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------

class DB:
    def __init__(self, data_dir: Path):
        self.quests: dict[str, dict]         = self._load(data_dir / "quests.json")
        self.npc_spawns: dict[str, list]     = self._load(data_dir / "npc_spawns.json")
        self.go_spawns: dict[str, list]      = self._load(data_dir / "go_spawns.json")
        self.quest_starters: dict[str, list] = self._load(data_dir / "quest_starters.json")
        self.quest_enders: dict[str, list]   = self._load(data_dir / "quest_enders.json")
        self.item_sources: dict[str, dict]   = self._load(data_dir / "item_sources.json")
        self.event_quests: set[int]          = self._load_event_quests(data_dir / "event_quests.txt")

    @staticmethod
    def _load(path: Path) -> dict | list:
        if not path.exists():
            return {}
        with open(path) as f:
            return json.load(f)

    @staticmethod
    def _load_event_quests(path: Path) -> set[int]:
        if not path.exists():
            return set()
        result = set()
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line.isdigit():
                    result.add(int(line))
        return result

    def quest(self, qid: int) -> Optional[dict]:
        return self.quests.get(str(qid))

    def npc_name(self, entry: int) -> str:
        spawns = self.npc_spawns.get(str(entry), [])
        if spawns:
            return spawns[0].get("name", f"NPC #{entry}")
        return f"NPC #{entry}"

    def go_name(self, entry: int) -> str:
        spawns = self.go_spawns.get(str(entry), [])
        if spawns:
            return spawns[0].get("name", f"Object #{entry}")
        return f"Object #{entry}"

    def get_npc_spawns_on_map(self, entry: int, map_id: int) -> list[dict]:
        return [s for s in self.npc_spawns.get(str(entry), []) if s.get("map") == map_id]

    def get_go_spawns_on_map(self, entry: int, map_id: int) -> list[dict]:
        return [s for s in self.go_spawns.get(str(entry), []) if s.get("map") == map_id]

    def quest_title(self, qid: int) -> str:
        q = self.quest(qid)
        if q:
            return q.get("LogTitle") or f"Quest #{qid}"
        return f"Quest #{qid}"


# ---------------------------------------------------------------------------
# Guide loading
# ---------------------------------------------------------------------------

class GuideDB:
    """Loads all YAML guides and detects duplicates."""

    def __init__(self, guides_dir: Path):
        self.guides: dict[str, dict]         = {}   # id -> guide data
        self.guide_paths: dict[str, list[Path]] = defaultdict(list)  # id -> [path, ...]
        self.duplicates: dict[str, list[Path]] = {}  # id -> paths (len>=2)
        self._load(guides_dir)

    def _load(self, guides_dir: Path):
        for root, dirs, files in os.walk(guides_dir):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
            for fn in sorted(files):
                if not fn.endswith(".yaml"):
                    continue
                path = Path(root) / fn
                try:
                    data = yaml.safe_load(path.read_text()) or {}
                except Exception as e:
                    print(f"[WARN] Failed to load {path}: {e}", file=sys.stderr)
                    continue
                gid = data.get("id")
                if not gid:
                    continue
                data["_path"] = str(path.relative_to(guides_dir))
                data["_abs_path"] = str(path)
                if gid not in self.guides:
                    self.guides[gid] = data
                self.guide_paths[gid].append(path)

        for gid, paths in self.guide_paths.items():
            if len(paths) > 1:
                self.duplicates[gid] = paths

    def get(self, guide_id: str) -> Optional[dict]:
        return self.guides.get(guide_id)

    def get_chain(self, start_id: str, max_level: int) -> list[dict]:
        """Follow next_guide links up to max_level_max."""
        chain = []
        seen = set()
        current_id = start_id
        while current_id:
            if current_id in seen:
                break  # cycle guard
            seen.add(current_id)
            guide = self.guides.get(current_id)
            if not guide:
                break
            chain.append(guide)
            level_max = guide.get("level_max", 0) or 0
            if level_max >= max_level:
                break
            current_id = guide.get("next_guide", "")
        return chain


# ---------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------

def dist2d(x1: float, y1: float, x2: float, y2: float) -> float:
    return math.sqrt((x1 - x2) ** 2 + (y1 - y2) ** 2)


def spawn_coverage(spawns: list[dict], cx: float, cy: float, radius: float) -> tuple[int, int]:
    """Return (inside_radius, total_spawns)."""
    inside = sum(1 for s in spawns if dist2d(s["x"], s["y"], cx, cy) <= radius)
    return inside, len(spawns)


def best_cluster_centroid(spawns: list[dict]) -> tuple[float, float]:
    if not spawns:
        return 0.0, 0.0
    xs = [s["x"] for s in spawns]
    ys = [s["y"] for s in spawns]
    return sum(xs) / len(xs), sum(ys) / len(ys)


def nearest_spawn(spawns: list[dict], cx: float, cy: float) -> tuple[float, Optional[dict]]:
    if not spawns:
        return float("inf"), None
    best = min(spawns, key=lambda s: dist2d(s["x"], s["y"], cx, cy))
    return dist2d(best["x"], best["y"], cx, cy), best


# ---------------------------------------------------------------------------
# Quest XP estimation
# ---------------------------------------------------------------------------

def estimate_quest_xp(quest: dict, char_level: int) -> int:
    qlevel = max(1, quest.get("QuestLevel", 1) or 1)
    difficulty = min(6, max(0, quest.get("RewardXPDifficulty", 5) or 5))
    row = _QXP.get(qlevel)
    if row is None:
        # Extrapolate for high levels
        row = [int(qlevel * v / 12) for v in _QXP[12]]
    xp = row[difficulty]
    # Grey quest penalty
    grey_offset = char_level - qlevel
    if grey_offset >= 9:
        return 0
    if grey_offset >= 6:
        xp = int(xp * 0.1)
    elif grey_offset >= 4:
        xp = int(xp * 0.3)
    return max(0, xp)


# ---------------------------------------------------------------------------
# Profile loading
# ---------------------------------------------------------------------------

def load_smoke_roster(roster_sh: Path) -> list[BotProfile]:
    """Parse define_bot lines from reset_idlebot_test_roster.sh."""
    profiles = []
    pattern = re.compile(
        r'^define_bot\s+"([^"]+)"\s+(\d+)\s+(\d+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+"([^"]+)"'
    )
    try:
        text = roster_sh.read_text()
    except FileNotFoundError:
        print(f"[WARN] Roster script not found: {roster_sh}", file=sys.stderr)
        return []
    for line in text.splitlines():
        m = pattern.match(line.strip())
        if not m:
            continue
        name, race_id, class_id, map_id, x, y, z, start_guide = (
            m.group(1), int(m.group(2)), int(m.group(3)),
            int(m.group(4)), float(m.group(5)), float(m.group(6)),
            float(m.group(7)), m.group(8),
        )
        faction = RACE_FACTION.get(race_id, "unknown")
        profiles.append(BotProfile(
            name=name,
            race_id=race_id,
            class_id=class_id,
            faction=faction,
            start_guide_id=start_guide,
            start_map=map_id,
            start_x=x,
            start_y=y,
            start_z=z,
        ))
    return profiles


def load_starting_zone_profiles() -> list[BotProfile]:
    """One bot per race, using each race's starting guide."""
    RACE_START_GUIDES = {
        1:  "human-northshire-1-6",
        2:  "orc-valley_of_trials-1-6",
        3:  "dwarf-coldridge-1-6",
        4:  "nightelf-shadowglen-1-6",
        5:  "undead-deathknell-1-6",
        6:  "tauren-camp_narache-1-6",
        7:  "gnome-gnomeregan-1-6",
        8:  "troll-valley_of_trials-1-6",
        10: "bloodelf-sunstrider-1-6",
        11: "draenei-ammen_vale-1-6",
    }
    profiles = []
    for race_id, guide_id in RACE_START_GUIDES.items():
        # Pick the first valid class for that race
        valid = sorted(VALID_RACE_CLASS.get(race_id, {1}))
        class_id = valid[0]
        faction = RACE_FACTION.get(race_id, "alliance")
        profiles.append(BotProfile(
            name=f"{RACE_NAMES.get(race_id, str(race_id))}{CLASS_NAMES.get(class_id, str(class_id))}",
            race_id=race_id,
            class_id=class_id,
            faction=faction,
            start_guide_id=guide_id,
        ))
    return profiles


def load_full_matrix_profiles() -> list[BotProfile]:
    """All valid race/class combos (excluding DK)."""
    RACE_START_GUIDES = {
        1:  "human-northshire-1-6",
        2:  "orc-valley_of_trials-1-6",
        3:  "dwarf-coldridge-1-6",
        4:  "nightelf-shadowglen-1-6",
        5:  "undead-deathknell-1-6",
        6:  "tauren-camp_narache-1-6",
        7:  "gnome-gnomeregan-1-6",
        8:  "troll-valley_of_trials-1-6",
        10: "bloodelf-sunstrider-1-6",
        11: "draenei-ammen_vale-1-6",
    }
    profiles = []
    for race_id, classes in sorted(VALID_RACE_CLASS.items()):
        guide_id = RACE_START_GUIDES.get(race_id, "")
        if not guide_id:
            continue
        for class_id in sorted(classes):
            if class_id in EXCLUDED_CLASSES:
                continue
            faction = RACE_FACTION.get(race_id, "alliance")
            profiles.append(BotProfile(
                name=f"{RACE_NAMES.get(race_id,'?')}{CLASS_NAMES.get(class_id,'?')}",
                race_id=race_id,
                class_id=class_id,
                faction=faction,
                start_guide_id=guide_id,
            ))
    return profiles


# ---------------------------------------------------------------------------
# Simulation engine
# ---------------------------------------------------------------------------

class Simulator:

    def __init__(self, db: DB, guide_db: GuideDB, output_mode: str = "human"):
        self.db       = db
        self.guide_db = guide_db
        self.output_mode = output_mode

    def _make_char(self, profile: BotProfile) -> VirtualChar:
        return VirtualChar(
            bot_name=profile.name,
            race_id=profile.race_id,
            class_id=profile.class_id,
            faction=profile.faction,
            level=float(profile.start_level),
            xp=0,
            map_id=profile.start_map,
            pos=(profile.start_x, profile.start_y, profile.start_z),
            active_quests={},
            rewarded_quests=set(),
            known_quests_started=set(),
            inventory={},
            failures=[],
            warnings=[],
            filtered_steps=[],
            log=[],
        )

    def simulate_bot(self, profile: BotProfile, target_level: int) -> dict:
        """Simulate one bot. Returns result dict."""
        char = self._make_char(profile)
        chain = self.guide_db.get_chain(profile.start_guide_id, target_level)

        if not chain:
            return self._bot_result(char, profile, target_level, chain, blocked=True,
                                    block_reason=f"Start guide '{profile.start_guide_id}' not found")

        # Load class guides (race+class specific parallel guides)
        class_guides = self._find_class_guides(profile)

        # Pre-collect all quest IDs across the entire chain (zone + class guides).
        # Used to distinguish "ordering bug" (prereq IS in chain) from
        # "genuinely missing" (prereq NOT in chain anywhere).
        chain_quest_ids: set[int] = set()
        for g in chain + class_guides:
            for step in (g.get("steps") or []):
                if step.get("quest_id"):
                    chain_quest_ids.add(step["quest_id"])
        self._chain_quest_ids = chain_quest_ids  # expose to handlers

        # Check duplicate guide IDs
        dup_failures = self._check_duplicates(chain, profile)
        char.failures.extend(dup_failures)

        step_count = 0
        blocked = False
        block_step = None

        for guide in chain:
            guide_id   = guide.get("id", "")
            guide_path = guide.get("_path", "")
            guide_min  = guide.get("level_min", 0) or 0
            guide_max  = guide.get("level_max", 0) or 0
            guide_race = (guide.get("race") or "").lower()
            guide_cls  = (guide.get("class") or "").lower()

            # Basic guide-level race check
            if guide_race and guide_race != RACE_NAMES.get(char.race_id, "").lower():
                if guide_race not in ("any", ""):
                    pass  # guide restriction; continue (class guides have race)

            steps = guide.get("steps") or []
            for si, step in enumerate(steps):
                step_count += 1
                result = self._simulate_step(char, step, si, guide_id, guide_path, char.level)
                if result.get("blocked"):
                    blocked = True
                    block_step = (guide_id, guide_path, si, step)
                    break

            if blocked:
                break

            # Level check at end of guide
            if guide_max and char.level < guide_max * 0.9:
                char.warnings.append({
                    "code": "LEVEL_GAP",
                    "severity": "WARN",
                    "title": f"Possible level gap at end of {guide.get('name', guide_id)}",
                    "problem": f"Pretend {char.race_name} {char.class_name} is only level "
                               f"{char.level:.1f} but guide expects to reach {guide_max}.",
                    "suggested_fix": "Add grind steps or optional quests to fill the gap.",
                    "debug": {"guide_id": guide_id, "char_level": char.level,
                              "guide_max": guide_max},
                })

            if float(target_level) <= char.level:
                break

        # Also simulate class guides (they run in parallel)
        for cguide in class_guides:
            cguide_id   = cguide.get("id", "")
            cguide_path = cguide.get("_path", "")
            for si, step in enumerate(cguide.get("steps") or []):
                self._simulate_step(char, step, si, cguide_id, cguide_path, char.level,
                                    is_class_guide=True)

        return self._bot_result(char, profile, target_level, chain, blocked=blocked,
                                block_step=block_step)

    def _find_class_guides(self, profile: BotProfile) -> list[dict]:
        """Find class-specific guides for this race/class."""
        race_key  = RACE_NAMES.get(profile.race_id, "").lower()
        class_key = CLASS_NAMES.get(profile.class_id, "").lower()
        results = []
        for gid, g in self.guide_db.guides.items():
            g_race  = (g.get("race") or "").lower()
            g_class = (g.get("class") or "").lower()
            if g_race == race_key and g_class == class_key:
                results.append(g)
        return results

    def _check_duplicates(self, chain: list[dict], profile: BotProfile) -> list[dict]:
        failures = []
        for guide in chain:
            gid = guide.get("id", "")
            paths = self.guide_db.guide_paths.get(gid, [])
            if len(paths) > 1:
                path_strs = [str(p.relative_to(GUIDES_DIR)) for p in paths]
                failures.append({
                    "code": "DUPLICATE_GUIDE_ID",
                    "severity": "FAIL",
                    "title": f"Duplicate guide ID: {gid}",
                    "problem": f"Guide ID '{gid}' exists in {len(paths)} files. "
                               f"Runtime may load the wrong copy.",
                    "suggested_fix": f"Remove or rename stale copies. Active files: {path_strs}",
                    "debug": {"guide_id": gid, "paths": path_strs},
                })
        return failures

    def _simulate_step(self, char: VirtualChar, step: dict, si: int,
                       guide_id: str, guide_path: str, char_level: float,
                       is_class_guide: bool = False) -> dict:
        stype  = step.get("type", "")
        qid    = step.get("quest_id")
        coords = step.get("coordinates") or {}
        cx     = float(coords.get("x", step.get("x", 0)) or 0)
        cy     = float(coords.get("y", step.get("y", 0)) or 0)
        cz     = float(coords.get("z", step.get("z", 0)) or 0)
        radius = float(coords.get("radius", step.get("radius", 30)) or 30)
        map_id = int(coords.get("map_id", coords.get("map",
                     step.get("map_id", step.get("map", char.map_id)))) or char.map_id)

        ctx = dict(si=si, guide_id=guide_id, guide_path=guide_path,
                   step=step, char=char, cx=cx, cy=cy, radius=radius, map_id=map_id)

        # ── Check guide-level restrictions ────────────────────────────────
        restrictions = step.get("restrictions") or {}
        if restrictions:
            if not self._passes_restrictions(char, restrictions, qid):
                char.filtered_steps.append({
                    "step_id": step.get("id", ""),
                    "guide_id": guide_id,
                    "reason": "class/race restriction",
                })
                return {"ok": True}

        ctx["is_class_guide"] = is_class_guide
        if stype == "accept_quest":
            return self._handle_accept(ctx)
        elif stype in ("turn_in_quest", "turnin_quest"):
            return self._handle_turnin(ctx)
        elif stype == "kill_mobs":
            return self._handle_kill(ctx)
        elif stype in ("collect_items", "interact_gameobject"):
            return self._handle_collect(ctx)
        elif stype == "use_item_at_location":
            return self._handle_use_item(ctx)
        elif stype == "grind":
            return self._handle_grind(ctx)
        else:
            return {"ok": True}

    def _passes_restrictions(self, char: VirtualChar, restrictions: dict,
                             qid: Optional[int]) -> bool:
        """Return True if the character passes step restrictions."""
        race_mask  = restrictions.get("race_mask", 0) or 0
        faction    = (restrictions.get("faction") or "").lower()
        if race_mask and not (race_mask & char.race_mask):
            return False
        if faction and faction != "any" and faction != char.faction:
            return False
        # class_mask: 0 is the guide convention for "disabled for all classes".
        # Distinguish "key absent" (unrestricted) from "key present but 0" (skip).
        if "class_mask" in restrictions:
            class_mask = int(restrictions["class_mask"] or 0)
            if class_mask == 0 or not (class_mask & char.class_mask):
                return False
        return True

    # ── Step handlers ──────────────────────────────────────────────────────

    def _handle_accept(self, ctx: dict) -> dict:
        char         = ctx["char"]
        step         = ctx["step"]
        qid          = step.get("quest_id")
        si           = ctx["si"]
        gid          = ctx["guide_id"]
        gpath        = ctx["guide_path"]
        is_class_guide = ctx.get("is_class_guide", False)

        if not qid:
            return {"ok": True}

        quest = self.db.quest(qid)
        if not quest:
            char.failures.append(self._fail(
                code="QUEST_NOT_FOUND",
                severity="FAIL",
                si=si, step=step, guide_id=gid, guide_path=gpath, quest=None, qid=qid,
                title=f"Quest #{qid} not found in DB",
                objective="",
                problem=f"The guide tells the pretend {char.race_name} {char.class_name} to accept "
                        f"quest #{qid}, but this quest ID does not exist in the database.",
                fix="Remove all steps for this quest from the guide.",
                debug={"questId": qid},
            ))
            return {"blocked": True}

        qtitle = quest.get("LogTitle", f"Quest #{qid}")

        # Already rewarded — skip silently
        if qid in char.rewarded_quests:
            char.log.append(f"  [skip] {qtitle} (#{qid}) already rewarded")
            return {"ok": True}

        # Already active — skip silently
        if qid in char.active_quests:
            return {"ok": True}

        # Race check
        ar = quest.get("AllowableRaces", 0) or 0
        if ar not in (0, -1, 4294967295) and not (ar & char.race_mask):
            char.filtered_steps.append({
                "step_id": step.get("id", ""),
                "guide_id": gid,
                "quest_id": qid,
                "reason": f"GUIDE_FILTERED: {qtitle} (#{qid}) race restriction "
                          f"(AR={ar}, char mask={char.race_mask})",
                "human": f"Quest {qtitle} (#{qid}) is not available to {char.race_name}s. "
                         f"Pretend {char.race_name} {char.class_name} correctly ignores it.",
            })
            return {"ok": True}

        # Class check
        ac = quest.get("AllowableClasses", 0) or 0
        if ac not in (0, -1, 4294967295) and not (ac & char.class_mask):
            char.filtered_steps.append({
                "step_id": step.get("id", ""),
                "guide_id": gid,
                "quest_id": qid,
                "reason": f"GUIDE_FILTERED: {qtitle} (#{qid}) class restriction "
                          f"(AC={ac}, char mask={char.class_mask})",
                "human": f"Quest {qtitle} (#{qid}) is {CLASS_NAMES.get(self._single_class_from_mask(ac), 'class')}-only. "
                         f"Pretend {char.race_name} {char.class_name} correctly ignores it.",
            })
            return {"ok": True}

        # Level check
        min_level = quest.get("MinLevel", 1) or 1
        if char.level < min_level:
            # Class quests above the simulation target level are expected to be locked.
            # Downgrade these to WARN so they don't block the pre-soak gate.
            severity = "WARN" if is_class_guide else "FAIL"
            container = char.warnings if severity == "WARN" else char.failures
            container.append(self._fail(
                code="QUEST_ACCEPT_LEVEL_TOO_LOW",
                severity=severity,
                si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                title=f"Level too low to accept {qtitle} (#{qid})",
                objective=f"Accept {qtitle}",
                problem=f"Pretend {char.race_name} {char.class_name} is level {char.level:.1f} "
                        f"but {qtitle} requires level {min_level}."
                        + (" (Class quest — bot will accept this when it reaches the right level.)"
                           if is_class_guide else ""),
                fix=f"Add grind steps or optional quests before this step to reach level {min_level}."
                    if not is_class_guide else "No fix needed — class quests unlock automatically at the right level.",
                debug={"questId": qid, "minLevel": min_level, "charLevel": char.level,
                       "isClassGuide": is_class_guide},
            ))
            return {"blocked": False}  # non-blocking; bot will wait

        # Prereq check
        prev_qid = quest.get("PrevQuestID", 0) or 0
        if prev_qid > 0 and prev_qid not in char.rewarded_quests:
            prev_quest  = self.db.quest(prev_qid)
            prev_title  = prev_quest.get("LogTitle", f"Quest #{prev_qid}") if prev_quest else f"Quest #{prev_qid}"
            # Check if the prereq grants this quest via RewardNextQuest (auto-chain):
            # If Q_prev has RewardNextQuest=qid, then turning in Q_prev auto-grants this quest.
            # In that case the explicit accept step is redundant (quest is auto-granted on turnin).
            auto_granted = prev_quest and (prev_quest.get("RewardNextQuest", 0) == qid)
            # Is the prereq somewhere in the guide chain (ordering issue) or totally absent?
            chain_ids = getattr(self, "_chain_quest_ids", set())
            prereq_in_chain = prev_qid in chain_ids
            if auto_granted or prereq_in_chain:
                # Ordering issue or auto-grant: WARN — quest likely succeeds at runtime
                # (either auto-granted by prereq turnin, or guide puts them in wrong order
                # but the bot eventually gets the quest via another path).
                note = (" This quest is auto-granted when the prerequisite is turned in, "
                        "so the explicit accept step here is redundant but not blocking."
                        if auto_granted else
                        " The prerequisite quest IS elsewhere in the guide but appears "
                        "after this step (ordering bug). The bot may still complete it "
                        "if the prerequisite is auto-granted or encountered later.")
                char.warnings.append(self._fail(
                    code="GUIDE_BAD_PREREQ_CHAIN",
                    severity="WARN",
                    si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                    title=f"Prerequisite ordering issue for {qtitle} (#{qid})",
                    objective=f"Accept {qtitle}",
                    problem=f"Pretend {char.race_name} {char.class_name} tries to accept "
                            f"{qtitle} (#{qid}), but {prev_title} (#{prev_qid}) "
                            f"has not been rewarded yet at this point in the guide.{note}",
                    fix=f"Move the accept/objective/turnin steps for {prev_title} (#{prev_qid}) "
                        f"BEFORE this step in the guide."
                        + (" Or remove the explicit accept step if the quest is auto-granted by "
                           f"{prev_title}'s turn-in." if auto_granted else ""),
                    debug={"questId": qid, "prevQuestId": prev_qid, "prevTitle": prev_title,
                           "autoGranted": auto_granted, "prereqInChain": prereq_in_chain},
                ))
            else:
                # Prereq NOT in the guide at all → bot will be permanently blocked
                char.failures.append(self._fail(
                    code="GUIDE_BAD_PREREQ_CHAIN",
                    severity="FAIL",
                    si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                    title=f"Missing prerequisite quest for {qtitle} (#{qid})",
                    objective=f"Accept {qtitle}",
                    problem=f"Pretend {char.race_name} {char.class_name} is trying to accept "
                            f"{qtitle} (#{qid}), which requires completing {prev_title} (#{prev_qid}) "
                            f"first. That prerequisite quest is NOT in any guide the bot follows, "
                            f"so the bot will never be able to accept {qtitle}.",
                    fix=f"Add accept + objective + turn-in steps for {prev_title} (#{prev_qid}) "
                        f"before this step, or remove {qtitle} from the guide.",
                    debug={"questId": qid, "prevQuestId": prev_qid, "prevTitle": prev_title,
                           "prereqInChain": False},
                ))

        # Exclusive group (negative PrevQuestID)
        excl = quest.get("ExclusiveGroup", 0) or 0
        if excl < 0:
            # Mutually exclusive with quest with the same |ExclusiveGroup| value
            pass  # TODO: validate exclusive group logic

        # NPC validation
        npc_id = step.get("npc_id")
        if npc_id:
            self._validate_accept_npc(char, ctx, quest, qid, qtitle, npc_id)

        # Mark quest active
        char.active_quests[qid] = {"obj_step_count": 0, "step": si}
        char.known_quests_started.add(qid)
        char.log.append(f"  [accept] {qtitle} (#{qid}) at level {char.level:.1f}")
        return {"ok": True}

    def _validate_accept_npc(self, char: VirtualChar, ctx: dict, quest: dict,
                              qid: int, qtitle: str, npc_id: int):
        si = ctx["si"]; gid = ctx["guide_id"]; gpath = ctx["guide_path"]
        step = ctx["step"]; cx = ctx["cx"]; cy = ctx["cy"]; map_id = ctx["map_id"]

        # Check quest_starters DB
        starters = self.db.quest_starters.get(str(qid), [])
        if starters:
            npc_ids_db = [s["entry"] for s in starters if s.get("type") == "creature"]
            go_ids_db  = [s["entry"] for s in starters if s.get("type") == "gameobject"]
            if npc_ids_db and npc_id not in npc_ids_db:
                db_name = self.db.npc_name(npc_ids_db[0])
                guide_name = self.db.npc_name(npc_id)
                char.failures.append(self._fail(
                    code="QUEST_ACCEPT_WRONG_STARTER",
                    severity="FAIL",
                    si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                    title=f"Wrong NPC to accept {qtitle} (#{qid})",
                    objective=f"Accept {qtitle} from {guide_name} (#{npc_id})",
                    problem=f"The guide says to accept {qtitle} (#{qid}) from "
                            f"{guide_name} (#{npc_id}), but the DB says the quest starter is "
                            f"{db_name} (#{npc_ids_db[0]}).",
                    fix=f"Update the accept step to use NPC {db_name} (#{npc_ids_db[0]}).",
                    debug={"questId": qid, "guideNpc": npc_id, "dbStarters": npc_ids_db},
                ))

        # Check spawn exists and is on the right map
        spawns = self.db.npc_spawns.get(str(npc_id), [])
        if not spawns:
            char.failures.append(self._fail(
                code="QUEST_ACCEPT_STARTER_MISSING",
                severity="FAIL",
                si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                title=f"Accept NPC not found for {qtitle} (#{qid})",
                objective=f"Accept {qtitle}",
                problem=f"The guide says to accept {qtitle} (#{qid}) from NPC #{npc_id}, "
                        f"but that NPC has no world spawns in the DB.",
                fix=f"Remove this quest from the guide or update to the correct NPC.",
                debug={"questId": qid, "npcId": npc_id},
            ))
            return

        same_map = [s for s in spawns if s.get("map") == map_id]
        if cx != 0 and same_map:
            d, nearest = nearest_spawn(same_map, cx, cy)
            if d > 50:
                npc_name = spawns[0].get("name", f"NPC #{npc_id}")
                char.failures.append(self._fail(
                    code="BAD_ACCEPT_COORDS",
                    severity="WARN",
                    si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                    title=f"Accept step for {qtitle} (#{qid}) has wrong coordinates",
                    objective=f"Accept {qtitle} from {npc_name}",
                    problem=f"The guide coordinates for accepting {qtitle} are "
                            f"{d:.0f} yards from the nearest {npc_name} spawn.",
                    fix=f"Update coordinates to ({nearest['x']:.2f}, {nearest['y']:.2f}, "
                        f"{nearest.get('z', 0):.2f}).",
                    debug={"questId": qid, "npcId": npc_id,
                           "guideCoords": (cx, cy), "nearestSpawn": (nearest["x"], nearest["y"]),
                           "distance": round(d, 1)},
                ))

    def _handle_turnin(self, ctx: dict) -> dict:
        char   = ctx["char"]
        step   = ctx["step"]
        qid    = step.get("quest_id")
        si     = ctx["si"]
        gid    = ctx["guide_id"]
        gpath  = ctx["guide_path"]

        if not qid:
            return {"ok": True}

        quest = self.db.quest(qid)
        if not quest:
            return {"ok": True}

        qtitle = quest.get("LogTitle", f"Quest #{qid}")

        # Skip if already rewarded
        if qid in char.rewarded_quests:
            return {"ok": True}

        # Check quest is active
        if qid not in char.active_quests and qid not in char.known_quests_started:
            # It's possible the quest was filtered or not in chain
            pass

        # Check all objectives complete
        active = char.active_quests.get(qid, {})
        obj_step_count = active.get("obj_step_count", 0)
        required = self._get_required_objectives(quest)
        n_required = len(required)

        if n_required > 0 and obj_step_count == 0:
            obj_texts = [self._format_objective(quest, obj) for obj in required]
            char.failures.append(self._fail(
                code="QUEST_INCOMPLETE_AT_TURNIN",
                severity="FAIL",
                si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                title=f"Pretend {char.race_name} {char.class_name} tries to turn in "
                      f"{qtitle} (#{qid}) but the quest is not complete",
                objective=f"Turn in {qtitle}",
                problem=f"The guide reaches the turn-in step for {qtitle} (#{qid}), but "
                        f"there are no objective steps in the guide before this turn-in.\n"
                        f"Required objectives:\n"
                        + "\n".join(f"  • {t}" for t in obj_texts),
                fix="Add objective steps (kill_mobs, collect_items, interact_gameobject, "
                    "or use_item_at_location) before the turn-in.",
                debug={"questId": qid, "requiredObjectiveCount": n_required,
                       "guideObjStepCount": obj_step_count},
            ))
        elif n_required > 0 and obj_step_count < n_required:
            obj_texts = [self._format_objective(quest, obj) for obj in required]
            char.warnings.append(self._fail(
                code="QUEST_POSSIBLY_INCOMPLETE",
                severity="WARN",
                si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                title=f"Fewer objective steps than required for {qtitle} (#{qid})",
                objective=f"Turn in {qtitle}",
                problem=f"The quest has {n_required} DB objectives but the guide has only "
                        f"{obj_step_count} objective step(s). This may be fine if one step "
                        f"covers multiple objectives, but worth verifying.\n"
                        f"DB objectives: " + ", ".join(obj_texts),
                fix="Verify each quest objective has a corresponding guide step.",
                debug={"questId": qid, "requiredObjectiveCount": n_required,
                       "guideObjStepCount": obj_step_count},
            ))

        # Validate turn-in NPC
        npc_id = step.get("npc_id")
        if npc_id:
            self._validate_turnin_npc(char, ctx, quest, qid, qtitle, npc_id)

        # Mark complete
        char.rewarded_quests.add(qid)
        char.active_quests.pop(qid, None)
        xp = estimate_quest_xp(quest, int(char.level))
        char.gain_xp(xp)
        char.log.append(f"  [turnin] {qtitle} (#{qid}) +{xp}xp → level {char.level:.1f}")

        # Auto-grant follow-up quest via RewardNextQuest
        next_qid = quest.get("RewardNextQuest", 0) or 0
        if next_qid and next_qid not in char.rewarded_quests:
            char.active_quests[next_qid] = {"obj_step_count": 0, "step": si,
                                             "auto_granted": True}
            char.known_quests_started.add(next_qid)
            next_title = self.db.quest_title(next_qid)
            char.log.append(f"  [auto-granted] {next_title} (#{next_qid}) via RewardNextQuest")
        return {"ok": True}

    def _validate_turnin_npc(self, char: VirtualChar, ctx: dict, quest: dict,
                              qid: int, qtitle: str, npc_id: int):
        si = ctx["si"]; gid = ctx["guide_id"]; gpath = ctx["guide_path"]
        step = ctx["step"]; cx = ctx["cx"]; cy = ctx["cy"]; map_id = ctx["map_id"]

        enders = self.db.quest_enders.get(str(qid), [])
        if enders:
            npc_enders = [e["entry"] for e in enders if e.get("type") == "creature"]
            go_enders  = [e["entry"] for e in enders if e.get("type") == "gameobject"]
            if npc_enders and npc_id not in npc_enders and not step.get("go_id"):
                db_name    = self.db.npc_name(npc_enders[0])
                guide_name = self.db.npc_name(npc_id)
                char.failures.append(self._fail(
                    code="GUIDE_BAD_TURNIN_NPC",
                    severity="FAIL",
                    si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                    title=f"Wrong NPC to turn in {qtitle} (#{qid})",
                    objective=f"Turn in {qtitle} to {guide_name} (#{npc_id})",
                    problem=f"The guide says to turn in {qtitle} (#{qid}) to "
                            f"{guide_name} (#{npc_id}), but the DB says the quest ender is "
                            f"{db_name} (#{npc_enders[0]}).",
                    fix=f"Update the turn-in step to use NPC {db_name} (#{npc_enders[0]}).",
                    debug={"questId": qid, "guideNpc": npc_id, "dbEnders": npc_enders},
                ))

        # Coord check
        spawns = self.db.npc_spawns.get(str(npc_id), [])
        if spawns and cx != 0:
            same_map = [s for s in spawns if s.get("map") == map_id]
            if same_map:
                d, nearest = nearest_spawn(same_map, cx, cy)
                if d > 50:
                    npc_name = spawns[0].get("name", f"NPC #{npc_id}")
                    char.warnings.append(self._fail(
                        code="BAD_TURNIN_COORDS",
                        severity="WARN",
                        si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                        title=f"Turn-in step for {qtitle} (#{qid}) has wrong coordinates",
                        objective=f"Turn in {qtitle} to {npc_name}",
                        problem=f"Guide coordinates are {d:.0f} yards from nearest {npc_name}.",
                        fix=f"Update to ({nearest['x']:.2f}, {nearest['y']:.2f}, "
                            f"{nearest.get('z', 0):.2f}).",
                        debug={"questId": qid, "npcId": npc_id, "distance": round(d, 1)},
                    ))

    def _handle_kill(self, ctx: dict) -> dict:
        char    = ctx["char"]
        step    = ctx["step"]
        qid     = step.get("quest_id")
        si      = ctx["si"]
        gid     = ctx["guide_id"]
        gpath   = ctx["guide_path"]
        cx, cy  = ctx["cx"], ctx["cy"]
        radius  = ctx["radius"]
        map_id  = ctx["map_id"]

        if not qid:
            return {"ok": True}

        quest   = self.db.quest(qid)
        qtitle  = self.db.quest_title(qid)
        item_id = step.get("item_id")
        cids    = step.get("creature_ids") or []

        # Auto-start item-started quests: picking up StartItem auto-presents the
        # quest accept dialog in WoW, so no explicit accept_quest step is needed.
        if item_id and quest and qid not in char.active_quests and qid not in char.rewarded_quests:
            start_item = quest.get("StartItem", 0) or 0
            if start_item == item_id:
                char.active_quests[qid] = {"obj_step_count": 0, "step": si, "item_started": True}
                char.known_quests_started.add(qid)
                char.log.append(f"  [item-start] {qtitle} (#{qid}) auto-started by picking up item #{item_id}")

        # Mark objective done (count approach — avoids index mismatches)
        if qid in char.active_quests:
            char.active_quests[qid]["obj_step_count"] += 1

        # Loot source check
        if item_id:
            item_name = f"Item #{item_id}"  # no item name table, use ID
            sources = self.db.item_sources.get(str(item_id), {})
            source_creatures = sources.get("creatures", [])
            if not source_creatures and not sources.get("gameobjects"):
                char.warnings.append(self._fail(
                    code="ITEM_LOOT_SOURCE_MISSING",
                    severity="WARN",
                    si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                    title=f"No loot source for {item_name} in {qtitle} (#{qid})",
                    objective=f"Collect {step.get('item_count', '?')} of {item_name}",
                    problem=f"The item (#{item_id}) needed for {qtitle} (#{qid}) has no "
                            f"known loot source in the DB.",
                    fix=f"Verify item #{item_id} drops from a creature or gameobject. "
                        f"Check item_loot_template and reference_loot_template.",
                    debug={"questId": qid, "itemId": item_id, "guideCids": cids},
                ))
            elif source_creatures and cids:
                # Check if guide targets can actually drop the item.
                # Downgraded to WARN: item_sources.json misses skinning/reference loot tables,
                # so guide targets (e.g. skinnable beasts) may provide the item via paths not
                # captured here. Flag as WARN for human review rather than hard FAIL.
                valid_cids = [c for c in cids if c in source_creatures]
                if not valid_cids:
                    source_names = [self.db.npc_name(c) for c in source_creatures[:3]]
                    guide_names  = [self.db.npc_name(c) for c in cids[:3]]
                    char.warnings.append(self._fail(
                        code="TARGET_DOES_NOT_DROP_REQUIRED_ITEM",
                        severity="WARN",
                        si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                        title=f"Guide targets may not drop {item_name} for {qtitle} (#{qid})",
                        objective=f"Collect {step.get('item_count', '?')} × {item_name} (#{item_id})",
                        problem=f"The guide targets {', '.join(guide_names)} "
                                f"for {item_name} (#{item_id}), but those creatures are not in the "
                                f"known direct-loot source list.\n"
                                f"Note: loot source data may be incomplete (skinning/reference loot not indexed).\n"
                                f"DB direct-loot sources: {', '.join(source_names)}.",
                        fix=f"Verify {item_name} actually drops from the guide targets. "
                            f"If not, update creature_ids to: "
                            f"{', '.join(self.db.npc_name(c)+f' (#{c})' for c in source_creatures[:3])}",
                        debug={"questId": qid, "itemId": item_id, "guideCids": cids,
                               "validSources": source_creatures[:5]},
                    ))

        # Spawn coverage — for multi-creature steps, report per-step (not per-creature):
        # only flag if the STEP has no viable coverage (at least one creature must be in range).
        if cx != 0 and cids:
            # Gather coverage data for all creatures
            no_spawn_cids   = []  # zero spawns anywhere
            wrong_map_cids  = []  # spawns exist but not on step_map
            has_coverage_cid = None   # first cid with spawns inside radius
            best_sparse: Optional[tuple] = None  # (inside, total, centroid_x, centroid_y, cid)

            for cid in cids:
                all_spawns = self.db.npc_spawns.get(str(cid), [])
                if not all_spawns:
                    no_spawn_cids.append(cid)
                    continue
                spawns_on_map = [s for s in all_spawns if s.get("map") == map_id]
                if not spawns_on_map:
                    wrong_map_cids.append((cid, list({s.get("map") for s in all_spawns})))
                    continue
                inside, total = spawn_coverage(spawns_on_map, cx, cy, radius)
                if inside > 0:
                    has_coverage_cid = cid
                    break  # at least one creature is well-covered — step is OK
                # Track best option for "sparse" reporting
                cov_pct = inside / total * 100 if total else 0
                centroid_x, centroid_y = best_cluster_centroid(spawns_on_map)
                d, _ = nearest_spawn(spawns_on_map, cx, cy)
                if best_sparse is None or total > best_sparse[1]:
                    best_sparse = (inside, total, centroid_x, centroid_y, cid, d)

            # Only flag if no creature in the step has any coverage
            if has_coverage_cid is None:
                # Determine the primary issue: wrong map, missing spawns, or bad coords
                viable_cids = [c for c in cids if c not in no_spawn_cids
                               and c not in [w[0] for w in wrong_map_cids]]
                primary_names = ", ".join(self.db.npc_name(c) + f" (#{c})" for c in cids[:3])

                if best_sparse:
                    inside, total, centroid_x, centroid_y, cid, d = best_sparse
                    severity = "FAIL" if d > 500 else "FAIL"
                    char.failures.append(self._fail(
                        code="GUIDE_BAD_OBJECTIVE_COORDS",
                        severity=severity,
                        si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                        title=f"Objective area covers no valid spawns for {qtitle} (#{qid})",
                        objective=f"Kill {primary_names}",
                        problem=f"The guide objective area covers 0 spawns of any listed creature "
                                f"for {qtitle} (#{qid}).\n"
                                f"Nearest usable spawn ({self.db.npc_name(cid)}) is {d:.0f} yards away.",
                        fix=f"Use center ({centroid_x:.2f}, {centroid_y:.2f}) with radius "
                            f"{max(radius, 100):.0f} to cover more spawns.",
                        debug={"questId": qid, "cids": cids, "guideCx": cx, "guideCy": cy,
                               "guideRadius": radius, "nearestSpawnDist": round(d, 1),
                               "bestCentroid": (centroid_x, centroid_y), "bestCid": cid},
                    ))
                elif no_spawn_cids and not wrong_map_cids and len(no_spawn_cids) == len(cids):
                    char.failures.append(self._fail(
                        code="NPC_NO_SPAWN",
                        severity="FAIL",
                        si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                        title=f"All kill targets for {qtitle} (#{qid}) have no spawns",
                        objective=f"Kill {primary_names}",
                        problem=f"None of the listed kill targets have world spawns in the DB.",
                        fix="Verify creature entry IDs or remove this quest from the guide.",
                        debug={"questId": qid, "cids": cids},
                    ))
                elif wrong_map_cids and not best_sparse:
                    bad_maps = list({m for _, maps in wrong_map_cids for m in maps})
                    char.failures.append(self._fail(
                        code="OBJECTIVE_WRONG_MAP",
                        severity="FAIL",
                        si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                        title=f"Kill targets for {qtitle} (#{qid}) not on map {map_id}",
                        objective=f"Kill {primary_names}",
                        problem=f"All listed creatures spawn on map(s) {bad_maps}, "
                                f"not on step map {map_id}.",
                        fix=f"Update map_id to {bad_maps[0]} or remove this step.",
                        debug={"questId": qid, "cids": cids, "stepMap": map_id, "spawnMaps": bad_maps},
                    ))
            elif cx != 0:
                # Has coverage, but check for sparseness of the covered creature
                spawns_on_map = self.db.get_npc_spawns_on_map(has_coverage_cid, map_id)
                if spawns_on_map:
                    inside, total = spawn_coverage(spawns_on_map, cx, cy, radius)
                    cov_pct = inside / total * 100 if total else 0
                    if cov_pct < 20 and total >= 8:
                        centroid_x, centroid_y = best_cluster_centroid(spawns_on_map)
                        char.warnings.append(self._fail(
                            code="SPARSE_OBJECTIVE_COVERAGE",
                            severity="WARN",
                            si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                            title=f"Objective area covers only {inside}/{total} "
                                  f"{self.db.npc_name(has_coverage_cid)} spawns",
                            objective=f"Kill {self.db.npc_name(has_coverage_cid)} (#{has_coverage_cid})",
                            problem=f"Only {cov_pct:.0f}% coverage of nearest usable creature. "
                                    f"A better cluster center exists.",
                            fix=f"Use ({centroid_x:.2f}, {centroid_y:.2f}) radius "
                                f"{max(radius, 120):.0f}.",
                            debug={"cid": has_coverage_cid, "inside": inside, "total": total,
                                   "coveragePct": round(cov_pct, 1)},
                        ))

        return {"ok": True}

    def _handle_collect(self, ctx: dict) -> dict:
        char    = ctx["char"]
        step    = ctx["step"]
        qid     = step.get("quest_id")
        si      = ctx["si"]
        gid     = ctx["guide_id"]
        gpath   = ctx["guide_path"]
        cx, cy  = ctx["cx"], ctx["cy"]
        radius  = ctx["radius"]
        map_id  = ctx["map_id"]

        if not qid:
            return {"ok": True}

        quest  = self.db.quest(qid)
        qtitle = self.db.quest_title(qid)
        if qid in char.active_quests:
            char.active_quests[qid]["obj_step_count"] += 1

        go_entries = step.get("source_gameobject_entries") or []
        if not go_entries and step.get("gameobject_id"):
            go_entries = [step["gameobject_id"]]

        for go_entry in go_entries:
            spawns_on_map = self.db.get_go_spawns_on_map(go_entry, map_id)
            if not spawns_on_map:
                all_spawns = self.db.go_spawns.get(str(go_entry), [])
                if not all_spawns:
                    char.failures.append(self._fail(
                        code="COLLECT_GO_NO_SPAWNS",
                        severity="FAIL",
                        si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                        title=f"Gameobject #{go_entry} for {qtitle} (#{qid}) has no spawns",
                        objective=f"Interact with {self.db.go_name(go_entry)} (#{go_entry})",
                        problem=f"The guide says to click/loot {self.db.go_name(go_entry)} "
                                f"(#{go_entry}) for {qtitle} (#{qid}), but that object has "
                                f"no world spawns in the DB.",
                        fix="Remove this step or use the correct gameobject entry.",
                        debug={"questId": qid, "goEntry": go_entry},
                    ))
                else:
                    maps = list({s.get("map") for s in all_spawns})
                    char.failures.append(self._fail(
                        code="OBJECTIVE_WRONG_MAP",
                        severity="FAIL",
                        si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                        title=f"Gameobject {self.db.go_name(go_entry)} is not on map {map_id}",
                        objective=f"Interact with {self.db.go_name(go_entry)} (#{go_entry})",
                        problem=f"The object only spawns on map(s) {maps}.",
                        fix=f"Update map_id to {maps[0]}.",
                        debug={"questId": qid, "goEntry": go_entry, "spawnMaps": maps},
                    ))
                continue

            if cx != 0:
                inside, total = spawn_coverage(spawns_on_map, cx, cy, radius)
                if total > 0 and inside == 0:
                    centroid_x, centroid_y = best_cluster_centroid(spawns_on_map)
                    d, _ = nearest_spawn(spawns_on_map, cx, cy)
                    char.failures.append(self._fail(
                        code="GUIDE_BAD_OBJECTIVE_COORDS",
                        severity="WARN",
                        si=si, step=step, guide_id=gid, guide_path=gpath, quest=quest, qid=qid,
                        title=f"Object area misses {self.db.go_name(go_entry)} for {qtitle} (#{qid})",
                        objective=f"Interact with {self.db.go_name(go_entry)} (#{go_entry})",
                        problem=f"Guide covers 0/{total} object spawns. Nearest is {d:.0f} yards away.",
                        fix=f"Use ({centroid_x:.2f}, {centroid_y:.2f}) radius {max(radius,80):.0f}.",
                        debug={"questId": qid, "goEntry": go_entry, "inside": inside, "total": total,
                               "dist": round(d, 1), "bestCentroid": (centroid_x, centroid_y)},
                    ))

        return {"ok": True}

    def _handle_use_item(self, ctx: dict) -> dict:
        char  = ctx["char"]
        step  = ctx["step"]
        qid   = step.get("quest_id")
        si    = ctx["si"]
        gid   = ctx["guide_id"]
        gpath = ctx["guide_path"]

        if qid and qid in char.active_quests:
            char.active_quests[qid]["obj_step_count"] += 1

        return {"ok": True}

    def _handle_grind(self, ctx: dict) -> dict:
        char = ctx["char"]
        step = ctx["step"]
        # Rough XP estimate for grind steps
        count = step.get("kill_count", 0) or step.get("count", 0) or 0
        lvl   = int(char.level)
        mob_xp = max(20, lvl * 25 - 30)  # very rough approximation
        xp = count * mob_xp
        if xp > 0:
            char.gain_xp(xp)
            char.log.append(f"  [grind] ~{count} kills +{xp}xp → level {char.level:.1f}")
        return {"ok": True}

    # ── Utility ─────────────────────────────────────────────────────────────

    def _get_required_objectives(self, quest: dict) -> list[dict]:
        objectives = []
        for j in range(1, 5):
            ng = quest.get(f"RequiredNpcOrGo{j}", 0) or 0
            nc = quest.get(f"RequiredNpcOrGoCount{j}", 0) or 0
            if ng != 0 and nc > 0:
                objectives.append({"type": "kill" if ng > 0 else "go", "entry": abs(ng),
                                   "count": nc, "index": j - 1})
        start_item = quest.get("StartItem", 0) or 0
        for j in range(1, 7):
            ii = quest.get(f"RequiredItemId{j}", 0) or 0
            ic = quest.get(f"RequiredItemCount{j}", 0) or 0
            if ii and ii != start_item and ic > 0:
                objectives.append({"type": "item", "entry": ii, "count": ic,
                                   "index": 4 + j - 1})
        return objectives

    def _parse_obj_index(self, condition: str, qid: int) -> int:
        """Extract objective index from completion_condition like 'quest_objective_complete:916/1'."""
        m = re.search(r'/(\d+)$', condition)
        if m:
            return int(m.group(1)) - 1  # 1-indexed in YAML, 0-indexed internally
        return 0

    def _format_objective(self, quest: dict, obj: dict) -> str:
        otype  = obj.get("type")
        entry  = obj.get("entry", 0)
        count  = obj.get("count", 0)
        if otype == "kill":
            name = self.db.npc_name(entry)
            return f"Kill {count} × {name} (#{entry})"
        elif otype == "go":
            name = self.db.go_name(entry)
            return f"Interact with {count} × {name} (#{entry})"
        elif otype == "item":
            return f"Collect {count} × Item #{entry}"
        return f"Objective (type={otype}, entry={entry}, count={count})"

    def _single_class_from_mask(self, mask: int) -> int:
        for cid, cmask in CLASS_MASKS.items():
            if mask == cmask:
                return cid
        return 0

    def _fail(self, *, code: str, severity: str, si: int, step: dict,
              guide_id: str, guide_path: str, quest: Optional[dict], qid: Optional[int],
              title: str, objective: str, problem: str, fix: str, debug: dict) -> dict:
        return {
            "code":          code,
            "severity":      severity,
            "step_index":    si,
            "step_id":       step.get("id", ""),
            "guide_id":      guide_id,
            "guide_path":    guide_path,
            "quest_id":      qid,
            "quest_title":   (quest.get("LogTitle") if quest else None) or (f"Quest #{qid}" if qid else ""),
            "human": {
                "title":        title,
                "objective":    objective,
                "problem":      problem,
                "suggestedFix": fix,
            },
            "debug": debug,
        }

    def _bot_result(self, char: VirtualChar, profile: BotProfile,
                    target_level: int, chain: list[dict],
                    blocked: bool = False, block_reason: str = "",
                    block_step: Optional[tuple] = None) -> dict:
        fail_count = len(char.failures)
        warn_count = len(char.warnings)
        filter_count = len(char.filtered_steps)

        if blocked or fail_count > 0:
            result_code = "FAIL"
        elif warn_count > 0:
            result_code = "WARN"
        else:
            result_code = "PASS"

        return {
            "bot_name":     char.bot_name,
            "race":         char.race_name,
            "class":        char.class_name,
            "faction":      char.faction,
            "start_level":  profile.start_level,
            "target_level": target_level,
            "sim_level":    round(char.level, 2),
            "sim_xp":       char.xp,
            "result":       result_code,
            "blocked":      blocked,
            "block_reason": block_reason,
            "guide_chain":  [g.get("id", "") for g in chain],
            "failures":     char.failures,
            "warnings":     char.warnings,
            "filtered_steps": char.filtered_steps,
            "log":          char.log,
        }


# ---------------------------------------------------------------------------
# Report writers
# ---------------------------------------------------------------------------

def write_human_report(results: list[dict], out_path: Path, title: str):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    total    = len(results)
    passes   = sum(1 for r in results if r["result"] == "PASS")
    warns    = sum(1 for r in results if r["result"] == "WARN")
    fails    = sum(1 for r in results if r["result"] == "FAIL")

    with open(out_path, "w") as f:
        f.write(f"# {title}\n\n")
        f.write(f"**Bots checked:** {total}  \n")
        f.write(f"**PASS:** {passes} | **WARN:** {warns} | **FAIL:** {fails}\n\n")

        if fails + warns == 0:
            f.write("All bots passed offline validation.\n\n")
        else:
            f.write("---\n\n")

        for r in results:
            icon = {"PASS": "✅", "WARN": "⚠️", "FAIL": "❌"}.get(r["result"], "?")
            f.write(f"## {icon} {r['bot_name']} — {r['race']} {r['class']}\n\n")
            f.write(f"**Result:** {r['result']}  \n")
            f.write(f"**Simulated level:** {r['sim_level']:.1f} / {r['target_level']}  \n")
            f.write(f"**Guide chain:** {' → '.join(r['guide_chain']) or '(none)'}  \n\n")

            if r.get("block_reason"):
                f.write(f"> **BLOCKED:** {r['block_reason']}\n\n")

            # Filtered steps summary
            if r["filtered_steps"]:
                f.write(f"**Filtered steps** ({len(r['filtered_steps'])} total — class/race/faction restrictions):  \n")
                for fs in r["filtered_steps"][:5]:
                    human = fs.get("human") or fs.get("reason", "")
                    f.write(f"- {human}\n")
                if len(r["filtered_steps"]) > 5:
                    f.write(f"- _(and {len(r['filtered_steps'])-5} more)_\n")
                f.write("\n")

            # Failures
            for issue in r["failures"] + r["warnings"]:
                sev = issue.get("severity", "?")
                icon2 = "❌" if sev == "FAIL" else "⚠️"
                h = issue.get("human", {})
                f.write(f"### {icon2} [{issue.get('code', 'ISSUE')}]\n\n")
                f.write(f"**{h.get('title', '')}**\n\n")
                if h.get("objective"):
                    f.write(f"**Objective:** {h['objective']}\n\n")
                if h.get("problem"):
                    f.write(f"**Problem:**\n{h['problem']}\n\n")
                if h.get("suggestedFix"):
                    f.write(f"**Suggested fix:**\n{h['suggestedFix']}\n\n")
                qid = issue.get("quest_id")
                if qid:
                    f.write(f"_Quest: {issue.get('quest_title','')} (#{qid}) · "
                            f"Guide: {issue.get('guide_path','')} step {issue.get('step_index','')+1}_\n\n")
                f.write("---\n\n")


def write_debug_report(results: list[dict], out_path: Path, title: str):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as f:
        f.write(f"# {title} (Debug)\n\n")
        for r in results:
            f.write(f"## {r['bot_name']} ({r['result']})\n\n")
            f.write(f"- race_id/class_id/faction: {r.get('race')}/{r.get('class')}/{r.get('faction')}\n")
            f.write(f"- sim_level: {r['sim_level']}\n")
            f.write(f"- target_level: {r['target_level']}\n")
            f.write(f"- guide_chain: {r['guide_chain']}\n\n")
            all_issues = r["failures"] + r["warnings"]
            for issue in all_issues:
                f.write(f"### [{issue.get('severity')}] {issue.get('code')}\n\n")
                f.write(f"- step_id: {issue.get('step_id')}\n")
                f.write(f"- guide: {issue.get('guide_path')} step {issue.get('step_index','')}\n")
                f.write(f"- quest_id: {issue.get('quest_id')}\n")
                if issue.get("debug"):
                    f.write(f"\n```json\n{json.dumps(issue['debug'], indent=2)}\n```\n\n")
            if not all_issues:
                f.write("No issues.\n\n")


def write_json_report(results: list[dict], out_path: Path):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)


def write_patches(results: list[dict], patches_dir: Path, db: DB):
    """Generate YAML patch suggestions for fixable issues."""
    patches_dir.mkdir(parents=True, exist_ok=True)
    patch_count = 0

    for r in results:
        for issue in r["failures"] + r["warnings"]:
            code = issue.get("code", "")
            qid  = issue.get("quest_id")
            debug = issue.get("debug", {})
            step_id = issue.get("step_id", "")
            guide_path = issue.get("guide_path", "")

            patch_lines = None
            confidence  = "LOW"

            if code == "GUIDE_BAD_OBJECTIVE_COORDS" and debug.get("bestCentroid"):
                cx, cy = debug["bestCentroid"]
                r_new  = max(debug.get("guideRadius", 80), 100)
                patch_lines = [
                    f"# Patch: update objective coordinates for quest {qid}",
                    f"# Guide: {guide_path}  step: {step_id}",
                    f"# Coverage before: {debug.get('inside',0)}/{debug.get('total',0)} spawns",
                    f"# Confidence: HIGH (DB spawn cluster centroid)",
                    f"coordinates:",
                    f"  x: {cx:.2f}",
                    f"  y: {cy:.2f}",
                    f"  radius: {r_new:.0f}",
                ]
                confidence = "HIGH"

            elif code == "GUIDE_BAD_TURNIN_NPC" and debug.get("dbEnders"):
                db_npc = debug["dbEnders"][0]
                spawns = db.npc_spawns.get(str(db_npc), [])
                if spawns:
                    s = spawns[0]
                    patch_lines = [
                        f"# Patch: fix turn-in NPC for quest {qid}",
                        f"# Guide: {guide_path}  step: {step_id}",
                        f"# Confidence: HIGH (DB questender)",
                        f"npc_id: {db_npc}",
                        f"coordinates:",
                        f"  x: {s['x']:.2f}",
                        f"  y: {s['y']:.2f}",
                        f"  z: {s.get('z', 0):.2f}",
                        f"  map_id: {s.get('map', 0)}",
                        f"  radius: 5.0",
                    ]
                    confidence = "HIGH"

            elif code == "QUEST_ACCEPT_WRONG_STARTER" and debug.get("dbStarters"):
                db_npc = debug["dbStarters"][0]
                spawns = db.npc_spawns.get(str(db_npc), [])
                if spawns:
                    s = spawns[0]
                    patch_lines = [
                        f"# Patch: fix accept NPC for quest {qid}",
                        f"# Guide: {guide_path}  step: {step_id}",
                        f"# Confidence: HIGH (DB queststarter)",
                        f"npc_id: {db_npc}",
                        f"coordinates:",
                        f"  x: {s['x']:.2f}",
                        f"  y: {s['y']:.2f}",
                        f"  z: {s.get('z', 0):.2f}",
                        f"  map_id: {s.get('map', 0)}",
                        f"  radius: 5.0",
                    ]
                    confidence = "HIGH"

            elif code == "TARGET_DOES_NOT_DROP_REQUIRED_ITEM" and debug.get("validSources"):
                valid = debug["validSources"]
                patch_lines = [
                    f"# Patch: fix creature_ids for quest {qid} item objective",
                    f"# Guide: {guide_path}  step: {step_id}",
                    f"# Confidence: HIGH (DB loot table)",
                    f"creature_ids:",
                ] + [f"  - {c}  # {db.npc_name(c)}" for c in valid[:3]]
                confidence = "HIGH"

            elif code == "QUEST_INCOMPLETE_AT_TURNIN":
                patch_lines = [
                    f"# Patch needed: missing objective step before turn-in for quest {qid}",
                    f"# Guide: {guide_path}  step: {step_id}",
                    f"# Confidence: LOW (requires human review to generate correct step)",
                    f"# Insert a kill_mobs / collect_items / use_item_at_location step",
                    f"# before the turn_in_quest step for quest {qid}.",
                ]
                confidence = "LOW"

            if patch_lines:
                safe_id = re.sub(r"[^a-zA-Z0-9_-]", "_", step_id or f"q{qid}")
                patch_file = patches_dir / f"{confidence.lower()}_q{qid}_{safe_id}.patch"
                with open(patch_file, "w") as pf:
                    pf.write("\n".join(patch_lines) + "\n")
                patch_count += 1

    return patch_count


def write_per_bot_reports(results: list[dict], out_dir: Path, db: DB):
    out_dir.mkdir(parents=True, exist_ok=True)
    for r in results:
        bot = r["bot_name"]
        write_human_report([r], out_dir / f"{bot}.md",
                           f"Offline Sim: {bot} ({r['race']} {r['class']})")
        write_debug_report([r], out_dir / f"{bot}.debug.md",
                           f"Offline Sim: {bot}")


# ---------------------------------------------------------------------------
# CLI helpers
# ---------------------------------------------------------------------------

def resolve_profiles(args, guide_db: GuideDB) -> list[BotProfile]:
    if args.profile == "smoke":
        profiles = load_smoke_roster(ROSTER_SH)
        if not profiles:
            sys.exit("ERROR: No bots found in smoke roster script.")
    elif args.profile == "starting-zone":
        profiles = load_starting_zone_profiles()
    elif args.profile == "full-matrix":
        profiles = load_full_matrix_profiles()
    elif args.race and getattr(args, "class_"):
        race_key  = args.race.lower()
        class_key = args.class_.lower()
        race_id   = RACE_IDS.get(race_key)
        class_id  = CLASS_IDS.get(class_key)
        if not race_id or not class_id:
            sys.exit(f"ERROR: Unknown race '{args.race}' or class '{args.class_}'")
        faction = RACE_FACTION.get(race_id, "alliance")
        guide_id = args.guide or ""
        if args.guide and not guide_id.endswith(".yaml"):
            # Already a guide ID
            pass
        elif args.guide and args.guide.endswith(".yaml"):
            # Path form — find the guide by path
            found = None
            for gid, g in guide_db.guides.items():
                if g.get("_path", "").endswith(args.guide) or args.guide in g.get("_path", ""):
                    found = gid
                    break
            guide_id = found or args.guide

        profiles = [BotProfile(
            name=f"{args.race.title()}{args.class_.title()}",
            race_id=race_id,
            class_id=class_id,
            faction=faction,
            start_guide_id=guide_id,
            start_level=getattr(args, "start_level", 1) or 1,
        )]
    else:
        sys.exit("ERROR: Specify --profile or both --race and --class.")
    return profiles


def print_summary(results: list[dict]):
    total  = len(results)
    passes = sum(1 for r in results if r["result"] == "PASS")
    warns  = sum(1 for r in results if r["result"] == "WARN")
    fails  = sum(1 for r in results if r["result"] == "FAIL")
    print(f"\n{'='*60}")
    print(f"Offline leveling simulation: {total} bot(s)")
    print(f"  PASS: {passes}  WARN: {warns}  FAIL: {fails}")
    print()
    for r in results:
        icon = {"PASS": "✓", "WARN": "⚠", "FAIL": "✗"}.get(r["result"], "?")
        nf = len(r["failures"]); nw = len(r["warnings"]); nfilt = len(r["filtered_steps"])
        print(f"  {icon} {r['bot_name']:16s}  {r['race']} {r['class']:9s}  "
              f"level {r['sim_level']:.1f}/{r['target_level']}  "
              f"{nf} fail  {nw} warn  {nfilt} filtered")
    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    # Target selection
    grp = parser.add_mutually_exclusive_group()
    grp.add_argument("--profile", choices=["smoke", "starting-zone", "full-matrix"],
                     help="Run a named profile (smoke = test roster bots)")
    parser.add_argument("--race",    help="Race (e.g. Human, Dwarf)")
    parser.add_argument("--class",   dest="class_", help="Class (e.g. Priest, Warrior)")
    parser.add_argument("--faction", help="Faction (Alliance/Horde)")
    parser.add_argument("--guide",   help="Start guide ID or path (e.g. alliance/human/00_northshire-1-6.yaml)")
    parser.add_argument("--start-level",  type=int, default=1)
    parser.add_argument("--target-level", type=int, default=12)

    # Output
    parser.add_argument("--output",     choices=["human", "debug", "both", "json"],
                        default="human")
    parser.add_argument("--output-dir", type=Path,
                        help="Output directory for reports (default: reports/offline-sim/)")
    parser.add_argument("--write-patches", action="store_true",
                        help="Generate YAML patch suggestions for fixable issues")
    parser.add_argument("--fail-on-error", action="store_true",
                        help="Exit 1 if any bot has FAIL result (for pre-soak gates)")

    # Data
    parser.add_argument("--guides-dir", type=Path, default=GUIDES_DIR)
    parser.add_argument("--data-dir",   type=Path, default=DATA_DIR)

    args = parser.parse_args()

    target_level = args.target_level
    out_dir      = args.output_dir or REPORTS_DIR

    # Load data
    print(f"Loading DB snapshots from {args.data_dir} ...")
    db = DB(args.data_dir)
    print(f"  {len(db.quests)} quests  {len(db.npc_spawns)} NPC types  "
          f"{len(db.go_spawns)} GO types  {len(db.item_sources)} item sources")

    print(f"Loading guides from {args.guides_dir} ...")
    guide_db = GuideDB(args.guides_dir)
    print(f"  {len(guide_db.guides)} guides  "
          f"{len(guide_db.duplicates)} duplicate IDs")
    if guide_db.duplicates:
        for gid, paths in guide_db.duplicates.items():
            path_strs = [str(p.relative_to(args.guides_dir)) for p in paths]
            print(f"  [DUP] {gid}: {path_strs}")

    # Resolve profiles
    profiles = resolve_profiles(args, guide_db)
    print(f"\nSimulating {len(profiles)} bot(s) to level {target_level} ...\n")

    sim     = Simulator(db, guide_db, output_mode=args.output)
    results = []

    for profile in profiles:
        profile.start_level = args.start_level
        print(f"  Simulating {profile.name} ({RACE_NAMES.get(profile.race_id,'?')} "
              f"{CLASS_NAMES.get(profile.class_id,'?')}) ...")
        result = sim.simulate_bot(profile, target_level)
        results.append(result)
        icon = {"PASS": "✓", "WARN": "⚠", "FAIL": "✗"}.get(result["result"], "?")
        nf = len(result["failures"]); nw = len(result["warnings"])
        print(f"    {icon} {result['result']}  level {result['sim_level']:.1f}  "
              f"{nf} fail  {nw} warn  {len(result['filtered_steps'])} filtered")
        for f in result["failures"]:
            print(f"      [FAIL] {f['code']}: {f['human']['title'][:70]}")
        for w in result["warnings"][:3]:
            print(f"      [WARN] {w['code']}: {w['human']['title'][:70]}")

    print_summary(results)

    # Write reports
    label = f"Level {args.start_level}→{target_level}"
    profile_tag = args.profile or "single"
    run_dir = out_dir / f"{profile_tag}-{args.start_level}-{target_level}"
    run_dir.mkdir(parents=True, exist_ok=True)

    if args.output in ("human", "both"):
        p = run_dir / "summary.md"
        write_human_report(results, p, f"Offline Leveling Simulation — {label}")
        print(f"Human report: {p}")

    if args.output in ("debug", "both"):
        p = run_dir / "summary.debug.md"
        write_debug_report(results, p, f"Offline Leveling Simulation — {label}")
        print(f"Debug report: {p}")

    if args.output in ("json", "both", "human"):
        p = run_dir / "summary.json"
        write_json_report(results, p)
        print(f"JSON report:  {p}")

    # Per-bot reports
    bot_dir = run_dir / "by-bot"
    write_per_bot_reports(results, bot_dir, db)
    print(f"Per-bot:      {bot_dir}/")

    # Patches
    if args.write_patches:
        patches_dir = run_dir / "patches"
        n = write_patches(results, patches_dir, db)
        print(f"Patches:      {patches_dir}/ ({n} files)")

    # Exit code for gate
    if args.fail_on_error:
        fails = sum(1 for r in results if r["result"] == "FAIL")
        if fails > 0:
            print(f"\nPRE-SOAK GATE: {fails} bot(s) FAILED offline validation. Do not start live soak.",
                  file=sys.stderr)
            sys.exit(1)


if __name__ == "__main__":
    main()
