#!/usr/bin/env python3
"""Fix item-use quests in guide YAMLs by inserting missing objective steps.

For quests that give a StartItem and have no objective step between accept
and turn-in, inserts a use_item_at_location or kill_mobs step.

Usage: python3 fix_item_use_quests.py
"""

import yaml
import subprocess
import os
import math

DB_HOST = "10.10.30.20"
DB_CONTAINER = "ac-database"
DB_PASSWORD = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"
GUIDE_DIR = "/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/data/guides"


def query_db(sql):
    cmd = [
        "ssh", f"khuong@{DB_HOST}",
        f"docker exec {DB_CONTAINER} mysql -u root -p{DB_PASSWORD} "
        f"acore_world -N -e \"{sql}\""
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        return []
    rows = []
    for line in result.stdout.strip().split('\n'):
        if line:
            rows.append(line.split('\t'))
    return rows


def get_quest_info(quest_id):
    rows = query_db(
        f"SELECT StartItem, RequiredNpcOrGo1, RequiredNpcOrGo2, "
        f"RequiredNpcOrGoCount1, RequiredNpcOrGoCount2, "
        f"RequiredItemId1, RequiredItemId2, RequiredItemCount1, RequiredItemCount2 "
        f"FROM quest_template WHERE ID = {quest_id}"
    )
    if not rows:
        return None
    r = rows[0]
    return {
        'start_item': int(r[0]),
        'req_npc1': int(r[1]),
        'req_npc2': int(r[2]),
        'req_count1': int(r[3]),
        'req_count2': int(r[4]),
        'req_item1': int(r[5]),
        'req_item2': int(r[6]),
        'req_item_count1': int(r[7]),
        'req_item_count2': int(r[8]),
    }


def get_creature_centroid(entry):
    rows = query_db(
        f"SELECT AVG(position_x), AVG(position_y), AVG(position_z), map "
        f"FROM creature WHERE id1 = {entry} GROUP BY map ORDER BY COUNT(*) DESC LIMIT 1"
    )
    if not rows:
        return None
    return {
        'x': round(float(rows[0][0]), 1),
        'y': round(float(rows[0][1]), 1),
        'z': round(float(rows[0][2]), 1),
        'map': int(rows[0][3])
    }


def process_guide(guide_path):
    with open(guide_path) as f:
        guide = yaml.safe_load(f)

    if not guide or 'steps' not in guide:
        return 0

    new_steps = []
    inserted = 0

    i = 0
    while i < len(guide['steps']):
        step = guide['steps'][i]
        new_steps.append(step)

        # Check: is this an accept_quest followed by a turn_in_quest
        # with no objective step in between?
        if step.get('type') == 'accept_quest' and step.get('quest_id'):
            qid = step['quest_id']
            # Look ahead for the turn-in
            j = i + 1
            has_objective = False
            turn_in_idx = None
            while j < len(guide['steps']):
                ns = guide['steps'][j]
                nq = ns.get('quest_id')
                nt = ns.get('type', '')
                if nq == qid:
                    if nt in ('kill_mobs', 'use_item_on_npc', 'use_item_at_location',
                              'interact_gameobject', 'escort_quest'):
                        has_objective = True
                        break
                    if nt == 'turn_in_quest':
                        turn_in_idx = j
                        break
                elif nq and nq != qid:
                    break
                j += 1

            if not has_objective and turn_in_idx is not None:
                info = get_quest_info(qid)
                if info and (info['req_npc1'] > 0 or info['start_item'] > 0):
                    # Need to insert an objective step
                    coords = step.get('coordinates', {})
                    turn_in_coords = guide['steps'][turn_in_idx].get('coordinates', {})

                    if info['req_npc1'] > 0:
                        # Kill objective — find creature location
                        creature_ids = [info['req_npc1']]
                        if info['req_npc2'] > 0:
                            creature_ids.append(info['req_npc2'])

                        centroid = get_creature_centroid(info['req_npc1'])
                        if centroid:
                            obj_step = {
                                'id': f'q{qid}_objective',
                                'name': f'Quest {qid} objective',
                                'type': 'kill_mobs',
                                'quest_id': qid,
                                'creature_ids': creature_ids,
                                'coordinates': {
                                    'x': centroid['x'],
                                    'y': centroid['y'],
                                    'z': centroid['z'],
                                    'radius': 60.0,
                                    'map_id': centroid['map']
                                },
                                'completion_condition': f'quest_objective_complete:{qid}/1'
                            }
                            if info['start_item'] > 0:
                                obj_step['item_id'] = info['start_item']
                                obj_step['type'] = 'use_item_at_location'
                                obj_step['name'] = f'Use item for quest {qid}'
                            new_steps.append(obj_step)
                            inserted += 1
                            print(f"    Inserted {obj_step['type']} for q{qid} at ({centroid['x']},{centroid['y']})")

                    elif info['start_item'] > 0:
                        # Item-use only quest (no kill target)
                        # Use the turn-in coords as a rough location
                        obj_step = {
                            'id': f'q{qid}_use_item',
                            'name': f'Use item for quest {qid}',
                            'type': 'use_item_at_location',
                            'quest_id': qid,
                            'item_id': info['start_item'],
                            'coordinates': turn_in_coords.copy() if turn_in_coords else coords.copy()
                        }
                        new_steps.append(obj_step)
                        inserted += 1
                        print(f"    Inserted use_item_at_location for q{qid}")

        i += 1

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
            if fn.endswith('.yaml') or fn.endswith('.yml'):
                guide_files.append(os.path.join(root, fn))

    print(f"Found {len(guide_files)} guide files.")
    total = 0
    for gf in sorted(guide_files):
        name = os.path.relpath(gf, GUIDE_DIR)
        n = process_guide(gf)
        if n > 0:
            print(f"  {name}: {n} objective steps inserted")
            total += n

    print(f"\nDone. Inserted {total} missing objective steps.")


if __name__ == '__main__':
    main()
