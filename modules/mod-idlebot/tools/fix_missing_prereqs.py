#!/usr/bin/env python3
"""Fix missing prerequisite quests in zone guides.

Reads the prereq audit output, fetches NPC data from the DB, and inserts
accept+turn-in steps for each missing prereq quest into the guide.

Usage: python3 fix_missing_prereqs.py
"""

import yaml
import subprocess
import os
import re
import sys

DB_HOST = "10.10.30.20"
DB_PASS = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"
GUIDE_DIR = "/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/data/guides"
PREREQ_FILE = "/tmp/claude-1000/-home-khuong-azerothcore-zoidberg/a532453a-cc1f-4396-805b-cfe5f1cfb227/scratchpad/all_prereqs.tsv"


def query_db(sql):
    cmd = f'docker exec ac-database mysql -u root -p{DB_PASS} -N -e "{sql}"'
    r = subprocess.run(["ssh", f"khuong@{DB_HOST}", cmd],
                       capture_output=True, text=True, timeout=15)
    rows = []
    for line in r.stdout.strip().split('\n'):
        if line:
            rows.append(line.split('\t'))
    return rows


def batch_query(quest_ids):
    """Fetch starter NPC, ender NPC, and their positions for a batch of quest IDs."""
    if not quest_ids:
        return {}, {}, {}

    ids_str = ",".join(str(q) for q in quest_ids)

    # Starter NPCs
    starters = {}
    for row in query_db(f"SELECT quest, id FROM acore_world.creature_queststarter WHERE quest IN ({ids_str})"):
        starters[int(row[0])] = int(row[1])

    # Ender NPCs
    enders = {}
    for row in query_db(f"SELECT quest, id FROM acore_world.creature_questender WHERE quest IN ({ids_str})"):
        enders[int(row[0])] = int(row[1])

    # NPC positions
    all_npcs = set(starters.values()) | set(enders.values())
    if not all_npcs:
        return starters, enders, {}

    npc_str = ",".join(str(n) for n in all_npcs)
    positions = {}
    for row in query_db(f"SELECT id1, map, position_x, position_y, position_z FROM acore_world.creature WHERE id1 IN ({npc_str}) GROUP BY id1"):
        positions[int(row[0])] = {
            'map': int(row[1]),
            'x': float(row[2]),
            'y': float(row[3]),
            'z': float(row[4])
        }

    return starters, enders, positions


def load_prereqs():
    """Load the prereq mapping from the cached TSV."""
    prereqs = {}
    with open(PREREQ_FILE) as f:
        for line in f:
            parts = line.strip().split('\t')
            if len(parts) >= 3:
                prereqs[int(parts[0])] = (int(parts[1]), parts[2])
    return prereqs


def process_guide(guide_path, prereqs, starters, enders, positions):
    """Insert missing prereq quest steps into a guide."""
    with open(guide_path) as f:
        guide = yaml.safe_load(f)

    if not guide or 'steps' not in guide:
        return 0

    quests_in_guide = set()
    for s in guide['steps']:
        qid = s.get('quest_id')
        if qid:
            quests_in_guide.add(qid)

    # Find which quests need prereqs inserted
    inserts = []  # (insert_before_index, prereq_quest_id)
    for i, s in enumerate(guide['steps']):
        qid = s.get('quest_id')
        if not qid or qid not in prereqs:
            continue
        prev_id, prev_name = prereqs[qid]
        if prev_id in quests_in_guide:
            continue
        if s.get('type') == 'accept_quest':
            inserts.append((i, prev_id))

    if not inserts:
        return 0

    # Insert in reverse order so indices stay valid
    inserted = 0
    for idx, prev_id in reversed(inserts):
        starter_npc = starters.get(prev_id, 0)
        ender_npc = enders.get(prev_id, starter_npc)

        starter_pos = positions.get(starter_npc, {'map': 0, 'x': 0, 'y': 0, 'z': 0})
        ender_pos = positions.get(ender_npc, starter_pos)

        # Get the map from the surrounding step if our NPC has no position
        if starter_pos['x'] == 0 and idx < len(guide['steps']):
            nearby = guide['steps'][idx].get('coordinates', {})
            starter_pos = {
                'map': nearby.get('map_id', nearby.get('map', 0)),
                'x': nearby.get('x', 0),
                'y': nearby.get('y', 0),
                'z': nearby.get('z', 0)
            }
            ender_pos = starter_pos

        accept_step = {
            'id': f'prereq_q{prev_id}_accept',
            'name': f'Accept prerequisite quest {prev_id}',
            'type': 'accept_quest',
            'quest_id': prev_id,
            'npc_id': starter_npc if starter_npc else None,
            'coordinates': {
                'x': round(starter_pos['x'], 2),
                'y': round(starter_pos['y'], 2),
                'z': round(starter_pos['z'], 2),
                'radius': 5.0
            }
        }
        if starter_pos.get('map'):
            accept_step['map_id'] = starter_pos['map']

        turnin_step = {
            'id': f'prereq_q{prev_id}_turnin',
            'name': f'Turn in prerequisite quest {prev_id}',
            'type': 'turn_in_quest',
            'quest_id': prev_id,
            'npc_id': ender_npc if ender_npc else None,
            'coordinates': {
                'x': round(ender_pos['x'], 2),
                'y': round(ender_pos['y'], 2),
                'z': round(ender_pos['z'], 2),
                'radius': 5.0
            }
        }
        if ender_pos.get('map'):
            turnin_step['map_id'] = ender_pos['map']

        # Remove None npc_ids
        if accept_step.get('npc_id') is None:
            del accept_step['npc_id']
        if turnin_step.get('npc_id') is None:
            del turnin_step['npc_id']

        guide['steps'].insert(idx, turnin_step)
        guide['steps'].insert(idx, accept_step)
        quests_in_guide.add(prev_id)
        inserted += 1

    if inserted > 0:
        with open(guide_path, 'w') as f:
            yaml.dump(guide, f, default_flow_style=False, sort_keys=False,
                      allow_unicode=True, width=120)

    return inserted


def main():
    print("Loading prereq data...")
    prereqs = load_prereqs()
    print(f"  {len(prereqs)} quest prereqs loaded.")

    # Collect all unique missing prereq quest IDs across all guides
    all_missing = set()
    for root, dirs, files in os.walk(GUIDE_DIR):
        for fn in sorted(files):
            if not fn.endswith('.yaml') or not fn[0:2].isdigit():
                continue
            path = os.path.join(root, fn)
            with open(path) as f:
                g = yaml.safe_load(f)
            if not g or 'steps' not in g:
                continue
            quests = set(s.get('quest_id') for s in g['steps'] if s.get('quest_id'))
            for qid in quests:
                if qid in prereqs:
                    prev_id = prereqs[qid][0]
                    if prev_id not in quests:
                        all_missing.add(prev_id)

    print(f"  {len(all_missing)} unique missing prereq quests to fix.")

    # Batch-fetch NPC data
    print("Fetching NPC data from DB...")
    # Process in chunks to avoid shell argument limits
    missing_list = list(all_missing)
    starters, enders, positions = {}, {}, {}
    chunk_size = 50
    for i in range(0, len(missing_list), chunk_size):
        chunk = missing_list[i:i+chunk_size]
        s, e, p = batch_query(chunk)
        starters.update(s)
        enders.update(e)
        positions.update(p)
        print(f"  Chunk {i//chunk_size + 1}: {len(chunk)} quests queried")

    print(f"  Starters: {len(starters)}, Enders: {len(enders)}, Positions: {len(positions)}")

    # Process each guide
    total_inserted = 0
    for root, dirs, files in os.walk(GUIDE_DIR):
        for fn in sorted(files):
            if not fn.endswith('.yaml') or not fn[0:2].isdigit():
                continue
            path = os.path.join(root, fn)
            n = process_guide(path, prereqs, starters, enders, positions)
            if n > 0:
                rel = os.path.relpath(path, GUIDE_DIR)
                print(f"  {rel}: +{n*2} steps ({n} prereqs inserted)")
                total_inserted += n

    print(f"\nDone. Inserted {total_inserted} missing prereq quests ({total_inserted*2} steps total).")


if __name__ == '__main__':
    main()
