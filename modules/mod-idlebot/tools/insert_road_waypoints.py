#!/usr/bin/env python3
"""Insert move_to road waypoints between distant guide steps.

When two consecutive steps are >300yd apart on the same map, inserts
intermediate move_to steps along the road (using known road waypoints
from the DB's creature/NPC spawn positions along paths).

For now, inserts simple interpolated waypoints at ~150yd intervals.
Future: use actual road graph data.

Usage: python3 insert_road_waypoints.py
"""

import yaml
import os
import math

GUIDE_DIR = "/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/data/guides"
MAX_STEP_DISTANCE = 300  # yards before inserting waypoints
WAYPOINT_SPACING = 150   # yards between inserted waypoints


def distance(x1, y1, x2, y2):
    return math.sqrt((x1-x2)**2 + (y1-y2)**2)


def process_guide(guide_path):
    with open(guide_path) as f:
        guide = yaml.safe_load(f)

    if not guide or 'steps' not in guide:
        return 0

    new_steps = []
    inserted = 0

    for i, step in enumerate(guide['steps']):
        new_steps.append(step)

        # Check distance to next step
        if i + 1 >= len(guide['steps']):
            continue

        next_step = guide['steps'][i + 1]
        coords = step.get('coordinates', {})
        next_coords = next_step.get('coordinates', {})

        x1 = coords.get('x', 0)
        y1 = coords.get('y', 0)
        z1 = coords.get('z', 0)
        map1 = coords.get('map_id', coords.get('map', 0))

        x2 = next_coords.get('x', 0)
        y2 = next_coords.get('y', 0)
        z2 = next_coords.get('z', 0)
        map2 = next_coords.get('map_id', next_coords.get('map', 0))

        if map1 != map2:
            continue
        if x1 == 0 and y1 == 0:
            continue
        if x2 == 0 and y2 == 0:
            continue

        dist = distance(x1, y1, x2, y2)
        if dist <= MAX_STEP_DISTANCE:
            continue

        # Insert intermediate waypoints
        num_waypoints = int(dist / WAYPOINT_SPACING)
        for j in range(1, num_waypoints + 1):
            t = j / (num_waypoints + 1)
            wx = x1 + (x2 - x1) * t
            wy = y1 + (y2 - y1) * t
            wz = z1 + (z2 - z1) * t

            waypoint = {
                'id': f'travel_{i}_{j}',
                'name': f'Travel waypoint {j}/{num_waypoints}',
                'type': 'move_to',
                'coordinates': {
                    'x': round(wx, 1),
                    'y': round(wy, 1),
                    'z': round(wz, 1),
                    'radius': 15.0,
                    'map_id': map1
                }
            }
            new_steps.append(waypoint)
            inserted += 1

    if inserted > 0:
        guide['steps'] = new_steps
        with open(guide_path, 'w') as f:
            yaml.dump(guide, f, default_flow_style=False, sort_keys=False,
                      allow_unicode=True, width=120)

    return inserted


def main():
    guide_files = []
    for root, dirs, files in os.walk(GUIDE_DIR):
        for fn in files:
            if fn.endswith('.yaml') and 'zygor' in fn:
                guide_files.append(os.path.join(root, fn))

    total = 0
    for gf in sorted(guide_files):
        name = os.path.basename(gf)
        n = process_guide(gf)
        if n > 0:
            print(f"  {name}: {n} travel waypoints inserted")
            total += n

    print(f"\nDone. Inserted {total} travel waypoints total.")


if __name__ == '__main__':
    main()
