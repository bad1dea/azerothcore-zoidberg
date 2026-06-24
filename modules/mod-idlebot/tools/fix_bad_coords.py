#!/usr/bin/env python3
"""Find and fix guide steps with wrong NPC coordinates.

For accept_quest and turn_in_quest steps, checks if the step's coordinates
match the nearest spawn of the quest's NPC. If the guide uses a distant
spawn when a closer one exists, fixes the coordinates.

Usage: python3 fix_bad_coords.py [--dry-run]
"""

import yaml
import subprocess
import os
import sys
import math

DB_HOST = "10.10.30.20"
DB_CONTAINER = "ac-database"
DB_PASSWORD = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"
GUIDE_DIR = "/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/data/guides"
MAX_ACCEPTABLE_DISTANCE = 500  # yards — flag if NPC is further than this


def query_db(sql):
    cmd = [
        "ssh", f"khuong@{DB_HOST}",
        f"docker exec {DB_CONTAINER} mysql -u root -p{DB_PASSWORD} -N -e \"{sql}\""
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        return []
    rows = []
    for line in result.stdout.strip().split('\n'):
        if line:
            rows.append(line.split('\t'))
    return rows


def get_npc_spawns(npc_entry):
    """Get all spawn positions for an NPC entry."""
    rows = query_db(
        f"SELECT map, position_x, position_y, position_z "
        f"FROM acore_world.creature WHERE id1 = {npc_entry}"
    )
    return [{'map': int(r[0]), 'x': float(r[1]), 'y': float(r[2]), 'z': float(r[3])} for r in rows]


def get_quest_npcs(quest_id, turn_in=False):
    """Get NPC entries that give/complete a quest."""
    table = "creature_involvedrelation" if turn_in else "creature_questrelation"
    rows = query_db(f"SELECT id FROM acore_world.{table} WHERE quest = {quest_id}")
    return [int(r[0]) for r in rows]


def distance(x1, y1, x2, y2):
    return math.sqrt((x1-x2)**2 + (y1-y2)**2)


def find_nearest_spawn(spawns, step_map, step_x, step_y):
    """Find the nearest spawn on the same map."""
    best = None
    best_dist = float('inf')
    for s in spawns:
        if s['map'] != step_map:
            continue
        d = distance(s['x'], s['y'], step_x, step_y)
        if d < best_dist:
            best_dist = d
            best = s
    # If no spawn on same map, find nearest on any map
    if best is None:
        for s in spawns:
            d = distance(s['x'], s['y'], step_x, step_y)
            if d < best_dist:
                best_dist = d
                best = s
    return best, best_dist


def process_guide(guide_path, dry_run=True):
    with open(guide_path) as f:
        guide = yaml.safe_load(f)

    if not guide or 'steps' not in guide:
        return 0, 0

    fixes = 0
    checked = 0

    for i, step in enumerate(guide['steps']):
        stype = step.get('type', '')
        if stype not in ('accept_quest', 'turn_in_quest'):
            continue

        qid = step.get('quest_id')
        npc_id = step.get('npc_id')
        coords = step.get('coordinates', {})
        step_x = coords.get('x', 0)
        step_y = coords.get('y', 0)
        step_map = coords.get('map_id', coords.get('map', 0))

        if not qid or (step_x == 0 and step_y == 0):
            continue

        checked += 1

        # Get the NPC entries for this quest
        turn_in = (stype == 'turn_in_quest')
        npc_entries = get_quest_npcs(qid, turn_in)
        if npc_id and npc_id not in npc_entries:
            npc_entries.append(npc_id)

        if not npc_entries:
            continue

        # Get all spawns for all possible NPCs
        all_spawns = []
        for entry in npc_entries:
            all_spawns.extend(get_npc_spawns(entry))

        if not all_spawns:
            continue

        # Find nearest spawn to the step's coordinates
        nearest, nearest_dist = find_nearest_spawn(all_spawns, step_map, step_x, step_y)

        # Find the closest spawn overall (might be on a different map or closer)
        closest_overall = None
        closest_dist = float('inf')
        for s in all_spawns:
            d = distance(s['x'], s['y'], step_x, step_y)
            if d < closest_dist:
                closest_dist = d
                closest_overall = s

        # Check if the step coords are far from the nearest spawn
        if nearest_dist > MAX_ACCEPTABLE_DISTANCE and closest_overall:
            # Is there a closer spawn?
            better_spawns = [s for s in all_spawns if s['map'] == step_map and
                           distance(s['x'], s['y'], step_x, step_y) < nearest_dist * 0.5]
            if not better_spawns:
                better_spawns = [s for s in all_spawns if
                               distance(s['x'], s['y'], step_x, step_y) < nearest_dist * 0.5]

            if better_spawns:
                best = min(better_spawns, key=lambda s: distance(s['x'], s['y'], step_x, step_y))
                best_d = distance(best['x'], best['y'], step_x, step_y)

                print(f"  Step {i}: {step.get('name')} (q{qid})")
                print(f"    Guide coords: ({step_x:.0f}, {step_y:.0f}) map {step_map}")
                print(f"    Nearest NPC:  ({nearest['x']:.0f}, {nearest['y']:.0f}) = {nearest_dist:.0f}yd")
                print(f"    Better spawn: ({best['x']:.0f}, {best['y']:.0f}) = {best_d:.0f}yd")

                if not dry_run:
                    step['coordinates']['x'] = round(best['x'], 2)
                    step['coordinates']['y'] = round(best['y'], 2)
                    step['coordinates']['z'] = round(best['z'], 2)
                    if best['map'] != step_map:
                        step['coordinates']['map_id'] = best['map']
                    print(f"    FIXED → ({best['x']:.0f}, {best['y']:.0f})")

                fixes += 1

    if fixes > 0 and not dry_run:
        with open(guide_path, 'w') as f:
            yaml.dump(guide, f, default_flow_style=False, sort_keys=False,
                      allow_unicode=True, width=120)

    return checked, fixes


def main():
    dry_run = '--dry-run' in sys.argv or '-n' in sys.argv

    if dry_run:
        print("DRY RUN — showing bad coords without fixing.\n")
    else:
        print("FIXING bad coords in guides.\n")

    guide_files = []
    for root, dirs, files in os.walk(GUIDE_DIR):
        for fn in files:
            if fn.endswith('.yaml') and 'zygor' in fn:
                guide_files.append(os.path.join(root, fn))

    total_checked = 0
    total_fixes = 0
    for gf in sorted(guide_files):
        name = os.path.basename(gf)
        print(f"Checking {name}...")
        checked, fixes = process_guide(gf, dry_run)
        total_checked += checked
        total_fixes += fixes

    print(f"\nDone. Checked {total_checked} steps, found {total_fixes} bad coords.")
    if dry_run and total_fixes > 0:
        print("Run without --dry-run to fix them.")


if __name__ == '__main__':
    main()
