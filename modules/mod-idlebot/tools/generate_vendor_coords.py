#!/usr/bin/env python3
"""Generate nearest vendor/repair NPC coords for guide steps.

For each zone section in a guide, finds the nearest vendor+repair NPC
from the acore_world DB and adds vendor_npc metadata to the guide YAML.
This gives the bot a known vendor to walk to instead of searching dynamically.

Usage: python3 generate_vendor_coords.py
"""

import yaml
import subprocess
import os
import math

DB_HOST = "10.10.30.20"
DB_CONTAINER = "ac-database"
DB_PASSWORD = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"
GUIDE_DIR = "/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/data/guides"

# UNIT_NPC_FLAG_VENDOR = 0x80 (128), UNIT_NPC_FLAG_REPAIR = 0x1000 (4096)
VENDOR_FLAG = 128
REPAIR_FLAG = 4096


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


def get_vendors_by_map():
    """Get all vendor+repair NPCs grouped by map."""
    rows = query_db(f"""
        SELECT c.map, c.position_x, c.position_y, c.position_z,
               ct.entry, ct.name, ct.npcflag
        FROM acore_world.creature c
        JOIN acore_world.creature_template ct ON ct.entry = c.id1
        WHERE (ct.npcflag & {VENDOR_FLAG}) != 0
        AND ct.npcflag & 2 = 0
        ORDER BY c.map, ct.entry
    """)

    vendors = {}
    for row in rows:
        map_id = int(row[0])
        if map_id not in vendors:
            vendors[map_id] = []
        vendors[map_id].append({
            'map': map_id,
            'x': float(row[1]),
            'y': float(row[2]),
            'z': float(row[3]),
            'entry': int(row[4]),
            'name': row[5],
            'can_repair': (int(row[6]) & REPAIR_FLAG) != 0
        })
    return vendors


def find_nearest_vendor(vendors_on_map, x, y):
    """Find the nearest vendor to (x, y) on this map."""
    best = None
    best_dist = float('inf')
    for v in vendors_on_map:
        dx = v['x'] - x
        dy = v['y'] - y
        d = dx * dx + dy * dy
        if d < best_dist:
            best_dist = d
            best = v
    return best, math.sqrt(best_dist) if best else 0


def process_guide(guide_path, all_vendors):
    """Add nearest_vendor metadata to guide steps."""
    with open(guide_path) as f:
        guide = yaml.safe_load(f)

    if not guide or 'steps' not in guide:
        return 0

    # For every 10th step (zone sections), find the nearest vendor
    modified = 0
    last_vendor = None
    for i, step in enumerate(guide['steps']):
        coords = step.get('coordinates', {})
        map_id = coords.get('map_id', coords.get('map', 0))
        x = coords.get('x', 0)
        y = coords.get('y', 0)

        if x == 0 and y == 0:
            continue

        # Only update every 10 steps to avoid redundancy
        if i % 10 != 0:
            continue

        vendors_on_map = all_vendors.get(map_id, [])
        if not vendors_on_map:
            continue

        vendor, dist = find_nearest_vendor(vendors_on_map, x, y)
        if not vendor or dist > 2000:
            continue

        # Skip if same vendor as last time
        if last_vendor and vendor['entry'] == last_vendor['entry']:
            continue

        last_vendor = vendor

        # Add vendor info to the step
        step['nearest_vendor'] = {
            'entry': vendor['entry'],
            'name': vendor['name'],
            'x': round(vendor['x'], 1),
            'y': round(vendor['y'], 1),
            'z': round(vendor['z'], 1),
            'can_repair': vendor['can_repair']
        }
        modified += 1

    if modified > 0:
        with open(guide_path, 'w') as f:
            yaml.dump(guide, f, default_flow_style=False, sort_keys=False,
                      allow_unicode=True, width=120)

    return modified


def main():
    print("Loading vendor data from DB...")
    all_vendors = get_vendors_by_map()
    total_vendors = sum(len(v) for v in all_vendors.values())
    print(f"Loaded {total_vendors} vendor NPCs across {len(all_vendors)} maps.")

    guide_files = []
    for root, dirs, files in os.walk(GUIDE_DIR):
        for fn in files:
            if fn.endswith('.yaml') or fn.endswith('.yml'):
                path = os.path.join(root, fn)
                if 'zygor' in fn:
                    guide_files.append(path)

    print(f"Found {len(guide_files)} zygor guide files.")

    total = 0
    for gf in sorted(guide_files):
        name = os.path.basename(gf)
        n = process_guide(gf, all_vendors)
        if n > 0:
            print(f"  {name}: {n} vendor waypoints added")
            total += n

    print(f"\nDone. Added {total} vendor waypoints total.")


if __name__ == '__main__':
    main()
