#!/usr/bin/env python3
"""Generate hotspots for kill objective steps in idlebot YAML guides.

Queries creature spawn positions from the acore_world DB (via SSH to zoidberg)
and clusters them into 3-5 patrol waypoints per kill step. Writes the hotspots
back into the YAML guide files.
"""

import yaml
import subprocess
import sys
import os
import math
import json
from collections import defaultdict

DB_HOST = "10.10.30.20"
DB_CONTAINER = "ac-database"
DB_PASSWORD = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"

GUIDE_DIR = "/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/data/guides"
MAX_HOTSPOTS = 5
MIN_HOTSPOTS = 3
MIN_SPAWNS_FOR_HOTSPOTS = 3  # Need at least 3 spawns to make hotspots


def query_db(sql):
    """Run a SQL query on the zoidberg DB and return rows."""
    cmd = [
        "ssh", f"khuong@{DB_HOST}",
        f"docker exec {DB_CONTAINER} mysql -u root -p{DB_PASSWORD} "
        f"-N -e \"{sql}\""
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        return []
    rows = []
    for line in result.stdout.strip().split('\n'):
        if line:
            rows.append(line.split('\t'))
    return rows


def get_creature_spawns(creature_ids):
    """Get spawn positions for a list of creature entries."""
    if not creature_ids:
        return {}
    ids_str = ",".join(str(c) for c in creature_ids)
    rows = query_db(
        f"SELECT id1, map, position_x, position_y, position_z "
        f"FROM acore_world.creature WHERE id1 IN ({ids_str})"
    )
    spawns = defaultdict(list)
    for row in rows:
        entry = int(row[0])
        spawns[entry].append({
            'map': int(row[1]),
            'x': float(row[2]),
            'y': float(row[3]),
            'z': float(row[4])
        })
    return spawns


def cluster_spawns(spawns, step_map, step_x, step_y, step_radius, n_clusters):
    """Simple k-means-ish clustering of spawn points into hotspots."""
    # Filter to same map and within reasonable range of step center
    max_range = max(step_radius * 2, 200.0)
    pts = []
    for s in spawns:
        if s['map'] != step_map:
            continue
        dx = s['x'] - step_x
        dy = s['y'] - step_y
        if (dx*dx + dy*dy) <= max_range * max_range:
            pts.append(s)

    if len(pts) < MIN_SPAWNS_FOR_HOTSPOTS:
        return []

    n = min(n_clusters, len(pts))
    if n < MIN_HOTSPOTS:
        n = min(MIN_HOTSPOTS, len(pts))

    # Simple k-means: pick initial centers spread across the points
    step = max(1, len(pts) // n)
    centers = [(pts[i * step]['x'], pts[i * step]['y'], pts[i * step]['z'])
               for i in range(n)]

    for _ in range(10):  # 10 iterations of k-means
        clusters = [[] for _ in range(n)]
        for p in pts:
            best_i = 0
            best_d = float('inf')
            for i, (cx, cy, cz) in enumerate(centers):
                d = (p['x']-cx)**2 + (p['y']-cy)**2
                if d < best_d:
                    best_d = d
                    best_i = i
            clusters[best_i].append(p)

        new_centers = []
        for i, cluster in enumerate(clusters):
            if cluster:
                cx = sum(p['x'] for p in cluster) / len(cluster)
                cy = sum(p['y'] for p in cluster) / len(cluster)
                cz = sum(p['z'] for p in cluster) / len(cluster)
                new_centers.append((cx, cy, cz))
            else:
                new_centers.append(centers[i])
        centers = new_centers

    # Filter out empty clusters and return
    hotspots = []
    for i, cluster in enumerate(clusters):
        if not cluster:
            continue
        cx, cy, cz = centers[i]
        hotspots.append({
            'x': round(cx, 1),
            'y': round(cy, 1),
            'z': round(cz, 1)
        })

    return hotspots


def process_guide(guide_path):
    """Add hotspots to kill steps in a guide YAML file."""
    with open(guide_path) as f:
        content = f.read()

    guide = yaml.safe_load(content)
    if not guide or 'steps' not in guide:
        return 0

    # Collect all creature IDs needed
    all_creature_ids = set()
    kill_steps = []
    for i, step in enumerate(guide['steps']):
        if step.get('type') != 'kill_mobs':
            continue
        cids = step.get('creature_ids', [])
        cid = step.get('creature_id')
        if cid:
            cids = [cid]
        if not cids:
            continue
        all_creature_ids.update(cids)
        kill_steps.append((i, step, cids))

    if not kill_steps:
        return 0

    # Batch query all creature spawns
    spawns = get_creature_spawns(list(all_creature_ids))

    modified = 0
    for idx, step, cids in kill_steps:
        # Skip if already has hotspots
        if step.get('hotspots'):
            continue

        coords = step.get('coordinates', {})
        step_map = coords.get('map_id', coords.get('map', 0))
        step_x = coords.get('x', 0)
        step_y = coords.get('y', 0)
        step_z = coords.get('z', 0)
        step_radius = coords.get('radius', 80)

        # Gather all spawns for this step's creatures
        all_spawns = []
        for cid in cids:
            all_spawns.extend(spawns.get(cid, []))

        hotspots = cluster_spawns(all_spawns, step_map, step_x, step_y,
                                  step_radius, MAX_HOTSPOTS)
        if hotspots:
            step['hotspots'] = hotspots
            modified += 1

    if modified > 0:
        with open(guide_path, 'w') as f:
            yaml.dump(guide, f, default_flow_style=False, sort_keys=False,
                      allow_unicode=True, width=120)

    return modified


def main():
    guide_files = []
    for root, dirs, files in os.walk(GUIDE_DIR):
        for fn in files:
            if fn.endswith('.yaml') or fn.endswith('.yml'):
                path = os.path.join(root, fn)
                # Only process the zygor 1-80 guides (the ones the bots use)
                if 'zygor' in fn or 'zygor' in root:
                    guide_files.append(path)

    if not guide_files:
        print("No zygor guide files found.")
        return

    print(f"Found {len(guide_files)} zygor guide files.")

    total_modified = 0
    for gf in sorted(guide_files):
        name = os.path.basename(gf)
        print(f"Processing {name}...", end=" ", flush=True)
        n = process_guide(gf)
        print(f"{n} kill steps got hotspots.")
        total_modified += n

    print(f"\nDone. Added hotspots to {total_modified} kill steps total.")


if __name__ == '__main__':
    main()
