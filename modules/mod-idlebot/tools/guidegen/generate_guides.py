#!/usr/bin/env python3
"""Generate clean leveling guides from AzerothCore DB data.

Builds quest chains from the DB, resolves NPC coordinates, generates
objective steps, and outputs IdleBot-format YAML guides.

Usage: python3 generate_guides.py [--range 1-12] [--race human]
"""

import json, os, sys, math, yaml, argparse
from collections import defaultdict

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../data/generated")
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../data/guides/generated")

# Race IDs
RACES = {
    "human": 1, "orc": 2, "dwarf": 3, "nightelf": 4, "undead": 5,
    "tauren": 6, "gnome": 7, "troll": 8, "bloodelf": 10, "draenei": 11
}
# Class IDs
CLASSES = {
    "warrior": 1, "paladin": 2, "hunter": 3, "rogue": 4, "priest": 5,
    "deathknight": 6, "shaman": 7, "mage": 8, "warlock": 9, "druid": 11
}
RACE_NAMES = {v: k for k, v in RACES.items()}
CLASS_NAMES = {v: k for k, v in CLASSES.items()}

# Faction: 0=Alliance, 1=Horde
RACE_FACTION = {1: 0, 3: 0, 4: 0, 7: 0, 11: 0,  # Alliance
                2: 1, 5: 1, 6: 1, 8: 1, 10: 1}   # Horde

# Alliance races mask, Horde races mask (from SharedDefines.h RACEMASK_*)
ALLIANCE_RACES = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 6) | (1 << 10)  # human, dwarf, nelf, gnome, draenei
HORDE_RACES = (1 << 1) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9)     # orc, undead, tauren, troll, belf

# Starting zones per race (approximate zone center, radius for quest search)
RACE_ZONES = {
    "human":    [{"name": "Northshire", "map": 0, "x": -8949, "y": -132, "z": 83, "radius": 400, "level_min": 1, "level_max": 6},
                 {"name": "Goldshire", "map": 0, "x": -9465, "y": 74, "z": 56, "radius": 2000, "level_min": 6, "level_max": 12}],
    "dwarf":    [{"name": "Coldridge", "map": 0, "x": -6240, "y": 331, "z": 383, "radius": 600, "level_min": 1, "level_max": 6},
                 {"name": "Kharanos", "map": 0, "x": -5582, "y": -524, "z": 403, "radius": 2000, "level_min": 6, "level_max": 12}],
    "gnome":    [{"name": "Gnomeregan", "map": 0, "x": -6240, "y": 331, "z": 383, "radius": 600, "level_min": 1, "level_max": 6},
                 {"name": "Kharanos", "map": 0, "x": -5582, "y": -524, "z": 403, "radius": 2000, "level_min": 6, "level_max": 12}],
    "nightelf": [{"name": "Shadowglen", "map": 1, "x": 10311, "y": 832, "z": 1326, "radius": 500, "level_min": 1, "level_max": 6},
                 {"name": "Dolanaar", "map": 1, "x": 9860, "y": 590, "z": 1300, "radius": 2000, "level_min": 6, "level_max": 12}],
    "draenei":  [{"name": "Ammen Vale", "map": 530, "x": -3963, "y": -13871, "z": 100, "radius": 600, "level_min": 1, "level_max": 6},
                 {"name": "Azure Watch", "map": 530, "x": -4154, "y": -12444, "z": 32, "radius": 2000, "level_min": 6, "level_max": 12}],
    "undead":   [{"name": "Deathknell", "map": 0, "x": 1676, "y": 1678, "z": 121, "radius": 600, "level_min": 1, "level_max": 6},
                 {"name": "Brill", "map": 0, "x": 2274, "y": 337, "z": 34, "radius": 2000, "level_min": 6, "level_max": 12}],
    "orc":      [{"name": "Valley of Trials", "map": 1, "x": -618, "y": -4251, "z": 38, "radius": 600, "level_min": 1, "level_max": 6},
                 {"name": "Razor Hill", "map": 1, "x": 307, "y": -4717, "z": 9, "radius": 2000, "level_min": 6, "level_max": 12}],
    "troll":    [{"name": "Valley of Trials", "map": 1, "x": -618, "y": -4251, "z": 38, "radius": 600, "level_min": 1, "level_max": 6},
                 {"name": "Razor Hill", "map": 1, "x": 307, "y": -4717, "z": 9, "radius": 2000, "level_min": 6, "level_max": 12}],
    "tauren":   [{"name": "Camp Narache", "map": 1, "x": -2917, "y": -258, "z": 53, "radius": 600, "level_min": 1, "level_max": 6},
                 {"name": "Bloodhoof", "map": 1, "x": -2326, "y": -383, "z": 57, "radius": 2000, "level_min": 6, "level_max": 12}],
    "bloodelf": [{"name": "Sunstrider Isle", "map": 530, "x": 10324, "y": -6370, "z": 22, "radius": 500, "level_min": 1, "level_max": 6},
                 {"name": "Falconwing", "map": 530, "x": 9406, "y": -6851, "z": 14, "radius": 2000, "level_min": 6, "level_max": 12}],
}


def load_data():
    """Load all extracted DB data + HB profile hints + event quest filter."""
    data = {}
    for name in ["quests", "quest_starters", "quest_enders", "npc_spawns", "go_spawns", "playercreateinfo"]:
        path = os.path.join(DATA_DIR, f"{name}.json")
        if not os.path.exists(path):
            print(f"MISSING: {path} — run extract_db_quests.py first")
            sys.exit(1)
        with open(path) as f:
            data[name] = json.load(f)

    # Load comprehensive item sources (creature + gameobject drops with lootid indirection)
    item_sources_path = os.path.join(DATA_DIR, "item_sources.json")
    if os.path.exists(item_sources_path):
        with open(item_sources_path) as f:
            data["item_sources"] = json.load(f)
        print(f"  Loaded {len(data['item_sources'])} item source entries")
    else:
        data["item_sources"] = {}

    # Load HB profile hints if available
    hb_path = os.path.join(DATA_DIR, "profile_hints_honorbuddy.json")
    if os.path.exists(hb_path):
        with open(hb_path) as f:
            data["hb_hints"] = json.load(f)
        print(f"  Loaded HB profile hints ({len(data['hb_hints'])} race/faction combos)")
    else:
        data["hb_hints"] = {}

    # Load event/seasonal quest IDs to filter
    event_path = os.path.join(DATA_DIR, "event_quests.txt")
    data["event_quests"] = set()
    if os.path.exists(event_path):
        with open(event_path) as f:
            for line in f:
                line = line.strip()
                if line.isdigit():
                    data["event_quests"].add(int(line))
        print(f"  Loaded {len(data['event_quests'])} event/seasonal quest IDs to filter")

    return data


def dist(x1, y1, x2, y2):
    return math.sqrt((x1 - x2) ** 2 + (y1 - y2) ** 2)


def find_nearest_spawn(spawns_dict, entry, target_map, target_x, target_y):
    """Find the spawn of `entry` closest to (target_x, target_y) on target_map."""
    key = str(entry)
    if key not in spawns_dict:
        return None
    best = None
    best_dist = float("inf")
    for s in spawns_dict[key]:
        if s["map"] != target_map:
            continue
        d = dist(s["x"], s["y"], target_x, target_y)
        if d < best_dist:
            best_dist = d
            best = s
    # If nothing on same map, take any spawn
    if best is None:
        for s in spawns_dict[key]:
            d = dist(s["x"], s["y"], target_x, target_y)
            if d < best_dist:
                best_dist = d
                best = s
    return best


def can_race_accept(quest, race_id):
    """Check if the race can accept this quest."""
    races = quest.get("AllowableRaces", 0)
    if races is None or races == 0:
        return True
    return (races & (1 << (race_id - 1))) != 0


def is_event_quest(data, qid, quest):
    """Check if a quest is seasonal/event/dungeon — should be filtered."""
    if qid in data.get("event_quests", set()):
        return True
    flags = quest.get("Flags", 0) or 0
    if flags & 4096:  # QUEST_FLAGS_SEASONAL
        return True
    qtype = quest.get("QuestType", 0) or 0
    if qtype in (4, 62, 81, 82):  # dungeon, life, raid, pvp
        return True
    title = (quest.get("LogTitle", "") or "").lower()
    if any(kw in title for kw in ["candy bucket", "egg hunt", "brewfest", "hallow", "winter veil",
                                   "midsummer", "love is in", "lunar festival", "noblegarden",
                                   "children's week", "pilgrim", "day of the dead"]):
        return True
    return False


def find_zone_quests(data, zone, race_id):
    """Find all quests whose starter NPC is in this zone's radius."""
    quests = data["quests"]
    starters = data["quest_starters"]
    npc_spawns = data["npc_spawns"]
    go_spawns = data["go_spawns"]

    zone_quests = []
    for qid_str, quest in quests.items():
        qid = int(qid_str)
        qlevel = quest.get("QuestLevel", 0)
        min_level = quest.get("MinLevel", 0)

        # Filter event/seasonal quests
        if is_event_quest(data, qid, quest):
            continue

        # Level filter
        if qlevel == -1:
            # Class quest — use MinLevel
            if min_level > zone["level_max"]:
                continue
        elif qlevel > 0:
            if qlevel > zone["level_max"] + 2:
                continue
            if qlevel < zone["level_min"] - 2:
                continue

        # Race filter
        if not can_race_accept(quest, race_id):
            continue

        # Find starter NPC/GO in this zone
        quest_starters = starters.get(str(qid), [])
        if not quest_starters:
            continue

        starter_in_zone = False
        starter_pos = None
        for starter in quest_starters:
            if starter["type"] == "creature":
                spawn = find_nearest_spawn(npc_spawns, starter["entry"], zone["map"], zone["x"], zone["y"])
            else:
                spawn = find_nearest_spawn(go_spawns, starter["entry"], zone["map"], zone["x"], zone["y"])
            if spawn and spawn["map"] == zone["map"]:
                d = dist(spawn["x"], spawn["y"], zone["x"], zone["y"])
                if d <= zone["radius"]:
                    starter_in_zone = True
                    starter_pos = spawn
                    break

        if not starter_in_zone:
            continue

        # Verify objective NPCs exist in the DB (filter Cata/event quests)
        objectives_valid = True
        for j in range(1, 5):
            target = quest.get(f"RequiredNpcOrGo{j}", 0)
            if target and target > 0:  # Creature
                if str(target) not in npc_spawns:
                    objectives_valid = False
                    break
            elif target and target < 0:  # Gameobject
                if str(abs(target)) not in go_spawns:
                    objectives_valid = False
                    break
        if not objectives_valid:
            continue

        zone_quests.append({
            "id": qid,
            "quest": quest,
            "starter": quest_starters[0],
            "starter_pos": starter_pos,
        })

    return zone_quests


def topological_sort_quests(zone_quests, all_quests):
    """Sort quests respecting PrevQuestID dependencies."""
    by_id = {q["id"]: q for q in zone_quests}
    in_zone = set(by_id.keys())

    # Build dependency graph
    deps = {}
    for q in zone_quests:
        prev = q["quest"].get("PrevQuestID", 0)
        if prev and prev > 0 and prev in in_zone:
            deps[q["id"]] = prev
        else:
            deps[q["id"]] = None

    # Topological sort
    sorted_ids = []
    visited = set()

    def visit(qid):
        if qid in visited:
            return
        visited.add(qid)
        dep = deps.get(qid)
        if dep and dep in by_id:
            visit(dep)
        sorted_ids.append(qid)

    # Process quests by level for stable ordering
    level_sorted = sorted(zone_quests, key=lambda q: (q["quest"].get("MinLevel", 0), q["quest"].get("QuestLevel", 0), q["id"]))
    for q in level_sorted:
        visit(q["id"])

    return [by_id[qid] for qid in sorted_ids if qid in by_id]


def generate_quest_steps(quest_info, data, zone):
    """Generate accept → objectives → turn-in steps for a quest."""
    steps = []
    qid = quest_info["id"]
    quest = quest_info["quest"]
    starters = data["quest_starters"]
    enders = data["quest_enders"]
    npc_spawns = data["npc_spawns"]
    go_spawns = data["go_spawns"]

    title = quest.get("LogTitle", f"Quest {qid}")
    class_mask = quest.get("AllowableClasses", 0)

    # --- Accept step ---
    starter_list = starters.get(str(qid), [])
    accept_coords = None
    accept_npc = None
    if starter_list:
        s = starter_list[0]
        if s["type"] == "creature":
            spawn = find_nearest_spawn(npc_spawns, s["entry"], zone["map"], zone["x"], zone["y"])
            accept_npc = s["entry"]
        else:
            spawn = find_nearest_spawn(go_spawns, s["entry"], zone["map"], zone["x"], zone["y"])
        if spawn:
            accept_coords = {"x": round(spawn["x"], 2), "y": round(spawn["y"], 2),
                            "z": round(spawn["z"], 2), "radius": 5.0}
            if "map" in spawn:
                accept_coords["map_id"] = spawn["map"]

    step = {
        "id": f"q{qid}_accept",
        "name": f"Accept {title}",
        "type": "accept_quest",
        "quest_id": qid,
    }
    if accept_npc:
        step["npc_id"] = accept_npc
    if accept_coords:
        step["coordinates"] = accept_coords
    if class_mask and class_mask > 0:
        step["restrictions"] = {"class_mask": class_mask}
    steps.append(step)

    # --- Objective steps ---
    # Kill and item objectives are independent — generate ALL of them.
    item_sources = data.get("item_sources", {})
    start_item = quest.get("StartItem", 0) or 0
    kill_creature_ids = set()  # Track creatures from kill objectives to detect overlap with item drops
    has_any_objective = False

    # 1) Kill/interact objectives (RequiredNpcOrGo1-4)
    for i in range(1, 5):
        target = quest.get(f"RequiredNpcOrGo{i}", 0)
        count = quest.get(f"RequiredNpcOrGoCount{i}", 0)
        if not target or target == 0 or not count:
            continue

        has_any_objective = True
        if target > 0:
            kill_creature_ids.add(target)
            spawn = find_nearest_spawn(npc_spawns, target, zone["map"], zone["x"], zone["y"])
            obj_step = {
                "id": f"q{qid}_kill{i}",
                "name": f"Quest {qid} objective {i}",
                "type": "kill_mobs",
                "quest_id": qid,
                "creature_ids": [target],
                "completion_condition": f"quest_objective_complete:{qid}/{i}",
            }
            if spawn:
                obj_step["coordinates"] = {
                    "x": round(spawn["x"], 2), "y": round(spawn["y"], 2),
                    "z": round(spawn["z"], 2), "radius": 80.0,
                    "map_id": spawn["map"],
                }
        else:
            go_entry = abs(target)
            spawn = find_nearest_spawn(go_spawns, go_entry, zone["map"], zone["x"], zone["y"])
            obj_step = {
                "id": f"q{qid}_go{i}",
                "name": f"Quest {qid} objective {i}",
                "type": "interact_gameobject",
                "quest_id": qid,
                "gameobject_id": go_entry,
                "completion_condition": f"quest_objective_complete:{qid}/{i}",
            }
            if spawn:
                obj_step["coordinates"] = {
                    "x": round(spawn["x"], 2), "y": round(spawn["y"], 2),
                    "z": round(spawn["z"], 2), "radius": 30.0,
                    "map_id": spawn["map"],
                }

        if class_mask and class_mask > 0:
            obj_step["restrictions"] = {"class_mask": class_mask}
        steps.append(obj_step)

    # 2) Item collect objectives (RequiredItemId1-6) — independent of kill objectives
    #    Collect all item objectives first, then merge those from the same creature.
    item_objectives = []  # list of (i, item_id, item_count, source)
    for i in range(1, 7):
        item_id = quest.get(f"RequiredItemId{i}", 0)
        item_count = quest.get(f"RequiredItemCount{i}", 0)
        if not item_id or item_id == 0 or not item_count:
            continue
        if item_id == start_item:
            continue
        source = item_sources.get(str(item_id), {})
        item_objectives.append((i, item_id, item_count, source))

    # Merge item objectives that drop from the same primary creature
    emitted_creature_sets = set()  # frozenset of creature IDs already emitted
    for (i, item_id, item_count, source) in item_objectives:
        drop_creatures = source.get("creatures", [])
        drop_gameobjects = source.get("gameobjects", [])

        # If the item drops from the same creature(s) as a kill objective, skip
        if drop_creatures and all(c in kill_creature_ids for c in drop_creatures):
            continue

        # If another item objective already emitted a step for the same creatures, skip
        if drop_creatures:
            key = frozenset(drop_creatures[:3])
            if key in emitted_creature_sets:
                continue
            emitted_creature_sets.add(key)

        has_any_objective = True

        if drop_creatures:
            # Item drops from creatures — generate kill_mobs step at mob spawn
            obj_step = {
                "id": f"q{qid}_collect{i}",
                "name": f"Quest {qid} objective {i}",
                "type": "kill_mobs",
                "quest_id": qid,
                "creature_ids": drop_creatures[:5],
                "completion_condition": f"quest_objective_complete:{qid}/{i}",
            }
            spawn = find_nearest_spawn(npc_spawns, drop_creatures[0], zone["map"], zone["x"], zone["y"])
            if spawn:
                obj_step["coordinates"] = {
                    "x": round(spawn["x"], 2), "y": round(spawn["y"], 2),
                    "z": round(spawn["z"], 2), "radius": 80.0,
                    "map_id": spawn["map"],
                }
            else:
                # Fallback: use accept coords but mark as unsafe
                if accept_coords:
                    obj_step["coordinates"] = dict(accept_coords)
                    obj_step["coordinates"]["radius"] = 80.0
                obj_step["_unsafe"] = f"no spawn found for creature {drop_creatures[0]}"
        elif drop_gameobjects:
            # Item comes from a gameobject — generate interact_gameobject step
            obj_step = {
                "id": f"q{qid}_collect{i}",
                "name": f"Quest {qid} objective {i}",
                "type": "interact_gameobject",
                "quest_id": qid,
                "gameobject_id": drop_gameobjects[0],
                "completion_condition": f"quest_objective_complete:{qid}/{i}",
            }
            spawn = find_nearest_spawn(go_spawns, drop_gameobjects[0], zone["map"], zone["x"], zone["y"])
            if spawn:
                obj_step["coordinates"] = {
                    "x": round(spawn["x"], 2), "y": round(spawn["y"], 2),
                    "z": round(spawn["z"], 2), "radius": 30.0,
                    "map_id": spawn["map"],
                }
            else:
                if accept_coords:
                    obj_step["coordinates"] = dict(accept_coords)
                    obj_step["coordinates"]["radius"] = 30.0
                obj_step["_unsafe"] = f"no spawn found for gameobject {drop_gameobjects[0]}"
        else:
            # No known source — generate step with accept coords + unsafe marker
            obj_step = {
                "id": f"q{qid}_collect{i}",
                "name": f"Quest {qid} objective {i}",
                "type": "kill_mobs",
                "quest_id": qid,
                "completion_condition": f"quest_objective_complete:{qid}/{i}",
                "_unsafe": f"item {item_id} has no known drop source in DB",
            }
            if accept_coords:
                obj_step["coordinates"] = dict(accept_coords)
                obj_step["coordinates"]["radius"] = 80.0

        if class_mask and class_mask > 0:
            obj_step["restrictions"] = {"class_mask": class_mask}
        steps.append(obj_step)

    # 3) Use-item quests (StartItem > 0 with no other objectives)
    if start_item > 0 and not has_any_objective:
        step = {
            "id": f"q{qid}_use_item",
            "name": f"Use item for quest {qid}",
            "type": "use_item_at_location",
            "quest_id": qid,
            "item_id": start_item,
        }
        if accept_coords:
            step["coordinates"] = dict(accept_coords)
            step["coordinates"]["radius"] = 30.0
        if class_mask and class_mask > 0:
            step["restrictions"] = {"class_mask": class_mask}
        steps.append(step)

    # --- Turn-in step ---
    ender_list = enders.get(str(qid), [])
    turnin_coords = None
    turnin_npc = None
    if ender_list:
        e = ender_list[0]
        if e["type"] == "creature":
            spawn = find_nearest_spawn(npc_spawns, e["entry"], zone["map"], zone["x"], zone["y"])
            turnin_npc = e["entry"]
        else:
            spawn = find_nearest_spawn(go_spawns, e["entry"], zone["map"], zone["x"], zone["y"])
        if spawn:
            turnin_coords = {"x": round(spawn["x"], 2), "y": round(spawn["y"], 2),
                            "z": round(spawn["z"], 2), "radius": 5.0}
            if "map" in spawn:
                turnin_coords["map_id"] = spawn["map"]

    step = {
        "id": f"q{qid}_turnin",
        "name": f"Turn in {title}",
        "type": "turn_in_quest",
        "quest_id": qid,
    }
    if turnin_npc:
        step["npc_id"] = turnin_npc
    if turnin_coords:
        step["coordinates"] = turnin_coords
    if class_mask and class_mask > 0:
        step["restrictions"] = {"class_mask": class_mask}
    steps.append(step)

    return steps


def get_hb_quest_order(data, faction, race_name, level_min, level_max):
    """Get HB profile quest ordering for this race/level range."""
    hb = data.get("hb_hints", {})
    key = f"{faction}/{race_name}"
    if key not in hb:
        return []

    all_qids = []
    for profile in hb[key]:
        pmin = profile.get("level_min", 1)
        pmax = profile.get("level_max", 80)
        # Check overlap with requested range
        if pmin > level_max or pmax < level_min:
            continue
        for qid in profile.get("quest_order", []):
            if qid not in all_qids:
                all_qids.append(qid)
    return all_qids


def generate_race_guides(race_name, data, level_range=(1, 12)):
    """Generate zone guides for a race."""
    race_id = RACES[race_name]
    zones = RACE_ZONES.get(race_name, [])
    faction = "alliance" if RACE_FACTION[race_id] == 0 else "horde"

    guides = []
    for zone in zones:
        if zone["level_max"] < level_range[0] or zone["level_min"] > level_range[1]:
            continue

        print(f"  Zone: {zone['name']} ({zone['level_min']}-{zone['level_max']})...")

        # Try HB ordering first
        hb_order = get_hb_quest_order(data, faction, race_name, zone["level_min"], zone["level_max"])
        if hb_order:
            print(f"    Using HB profile ordering ({len(hb_order)} quests)")

        # Find zone quests from DB
        zone_quests = find_zone_quests(data, zone, race_id)
        zone_quest_by_id = {q["id"]: q for q in zone_quests}

        if hb_order:
            # Use HB order but only for quests that exist in our DB AND zone
            sorted_quests = []
            seen = set()
            for qid in hb_order:
                if qid in zone_quest_by_id and qid not in seen:
                    sorted_quests.append(zone_quest_by_id[qid])
                    seen.add(qid)
            # Append any DB quests not in HB order
            for q in zone_quests:
                if q["id"] not in seen:
                    sorted_quests.append(q)
                    seen.add(q["id"])
        else:
            sorted_quests = topological_sort_quests(zone_quests, data["quests"])

        print(f"    Found {len(sorted_quests)} quests")

        # Generate steps
        all_steps = []
        for q in sorted_quests:
            steps = generate_quest_steps(q, data, zone)
            all_steps.extend(steps)

        # Build guide
        zone_slug = zone["name"].lower().replace(" ", "_").replace("'", "")
        guide_id = f"{race_name}-{zone_slug}-{zone['level_min']}-{zone['level_max']}"

        guide = {
            "id": guide_id,
            "name": f"{zone['name']} ({zone['level_min']}-{zone['level_max']})",
            "faction": faction,
            "race": race_name,
            "level_min": zone["level_min"],
            "level_max": zone["level_max"],
            "steps": all_steps,
        }

        guides.append(guide)

    # Chain guides together
    for i in range(len(guides) - 1):
        guides[i]["next_guide"] = guides[i + 1]["id"]

    return guides


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--range", default="1-12", help="Level range (e.g. 1-12)")
    parser.add_argument("--race", default=None, help="Single race to generate (e.g. human)")
    parser.add_argument("--out", default=OUT_DIR, help="Output directory")
    args = parser.parse_args()

    level_min, level_max = map(int, args.range.split("-"))
    os.makedirs(args.out, exist_ok=True)

    print("Loading DB data...")
    data = load_data()

    races = [args.race] if args.race else list(RACE_ZONES.keys())

    total_guides = 0
    total_steps = 0
    for race in races:
        print(f"\n=== {race.upper()} ===")
        guides = generate_race_guides(race, data, (level_min, level_max))

        faction = "alliance" if RACE_FACTION[RACES[race]] == 0 else "horde"
        race_dir = os.path.join(args.out, faction, race)
        os.makedirs(race_dir, exist_ok=True)

        for i, guide in enumerate(guides):
            filename = f"{i:02d}_{guide['id'].split('-', 1)[1]}.yaml"
            filepath = os.path.join(race_dir, filename)
            with open(filepath, "w") as f:
                yaml.dump(guide, f, default_flow_style=False, sort_keys=False,
                          allow_unicode=True, width=120)
            print(f"  Wrote {filepath} ({len(guide['steps'])} steps)")
            total_guides += 1
            total_steps += len(guide["steps"])

    print(f"\nDone. Generated {total_guides} guides with {total_steps} total steps in {args.out}/")


if __name__ == "__main__":
    main()
