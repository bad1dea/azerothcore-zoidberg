#!/usr/bin/env python3
"""Audit guide quests against the live DB — find missing objectives, wrong coords.

Uses pre-fetched batch data for speed. Run the batch queries first:
  ssh khuong@10.10.30.20 "docker exec ac-database mysql ..." > scratchpad/all_quests.tsv

Usage: python3 audit_guide_quests.py [--fix] [--max-level 40]
"""

import yaml
import subprocess
import os
import sys
import math
from collections import defaultdict

DB_HOST = "10.10.30.20"
DB_CONTAINER = "ac-database"
DB_PASSWORD = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"
GUIDE_DIR = "/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/data/guides"
SCRATCHPAD = "/tmp/claude-1000/-home-khuong-azerothcore-zoidberg/a532453a-cc1f-4396-805b-cfe5f1cfb227/scratchpad"


def query_db(sql):
    cmd = ["ssh", f"khuong@{DB_HOST}",
           f"docker exec {DB_CONTAINER} mysql -u root -p{DB_PASSWORD} -N -e \"{sql}\""]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    rows = []
    if result.returncode == 0 and result.stdout.strip():
        for line in result.stdout.strip().split('\n'):
            if line:
                rows.append(line.split('\t'))
    return rows


def load_tsv(path):
    rows = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line:
                    rows.append(line.split('\t'))
    except FileNotFoundError:
        pass
    return rows


def fetch_all_data():
    """Batch-fetch all quest data from DB into TSV files."""
    print("Fetching quest data from DB...")

    quest_sql = """SELECT qt.ID, qt.LogTitle, qt.QuestLevel, qt.MinLevel,
        qt.RequiredNpcOrGo1, qt.RequiredNpcOrGoCount1,
        qt.RequiredNpcOrGo2, qt.RequiredNpcOrGoCount2,
        qt.RequiredNpcOrGo3, qt.RequiredNpcOrGoCount3,
        qt.RequiredNpcOrGo4, qt.RequiredNpcOrGoCount4,
        qt.RequiredItemId1, qt.RequiredItemCount1,
        qt.RequiredItemId2, qt.RequiredItemCount2,
        qt.StartItem,
        IFNULL(qta.SpecialFlags, 0),
        IFNULL(qta.AllowableClasses, 0)
    FROM acore_world.quest_template qt
    LEFT JOIN acore_world.quest_template_addon qta ON qta.ID = qt.ID"""
    rows = query_db(quest_sql)
    with open(f"{SCRATCHPAD}/all_quests.tsv", 'w') as f:
        for r in rows:
            f.write('\t'.join(r) + '\n')
    print(f"  {len(rows)} quests")

    starters = query_db("SELECT id, quest FROM acore_world.creature_queststarter")
    with open(f"{SCRATCHPAD}/quest_starters.tsv", 'w') as f:
        for r in starters:
            f.write('\t'.join(r) + '\n')
    print(f"  {len(starters)} quest starters")

    enders = query_db("SELECT id, quest FROM acore_world.creature_questender")
    with open(f"{SCRATCHPAD}/quest_enders.tsv", 'w') as f:
        for r in enders:
            f.write('\t'.join(r) + '\n')
    print(f"  {len(enders)} quest enders")

    spawns = query_db("""SELECT c.id1, c.map, c.position_x, c.position_y, c.position_z
        FROM acore_world.creature c
        WHERE c.id1 IN (
          SELECT id FROM acore_world.creature_queststarter
          UNION SELECT id FROM acore_world.creature_questender
          UNION SELECT RequiredNpcOrGo1 FROM acore_world.quest_template WHERE RequiredNpcOrGo1 > 0
          UNION SELECT RequiredNpcOrGo2 FROM acore_world.quest_template WHERE RequiredNpcOrGo2 > 0
          UNION SELECT RequiredNpcOrGo3 FROM acore_world.quest_template WHERE RequiredNpcOrGo3 > 0
          UNION SELECT RequiredNpcOrGo4 FROM acore_world.quest_template WHERE RequiredNpcOrGo4 > 0
        )""")
    with open(f"{SCRATCHPAD}/combined_spawns.tsv", 'w') as f:
        for r in spawns:
            f.write('\t'.join(r) + '\n')
    print(f"  {len(spawns)} NPC spawns")


def load_all_data():
    """Load batch data into lookup dicts."""
    quests = {}
    for r in load_tsv(f"{SCRATCHPAD}/all_quests.tsv"):
        if len(r) < 19:
            continue
        qid = int(r[0])
        quests[qid] = {
            'id': qid, 'title': r[1], 'level': int(r[2]), 'minlevel': int(r[3]),
            'npc_or_go': [(int(r[4]), int(r[5])), (int(r[6]), int(r[7])),
                          (int(r[8]), int(r[9])), (int(r[10]), int(r[11]))],
            'items': [(int(r[12]), int(r[13])), (int(r[14]), int(r[15]))],
            'start_item': int(r[16]),
            'special_flags': int(r[17]),
            'classes': int(r[18])
        }

    starters = defaultdict(list)  # quest_id -> [npc_entries]
    for r in load_tsv(f"{SCRATCHPAD}/quest_starters.tsv"):
        starters[int(r[1])].append(int(r[0]))

    enders = defaultdict(list)
    for r in load_tsv(f"{SCRATCHPAD}/quest_enders.tsv"):
        enders[int(r[1])].append(int(r[0]))

    spawns = defaultdict(list)  # npc_entry -> [(map, x, y, z)]
    for r in load_tsv(f"{SCRATCHPAD}/combined_spawns.tsv"):
        spawns[int(r[0])].append({
            'map': int(r[1]), 'x': float(r[2]), 'y': float(r[3]), 'z': float(r[4])
        })

    return quests, starters, enders, spawns


def distance(x1, y1, x2, y2):
    return math.sqrt((x1-x2)**2 + (y1-y2)**2)


def find_nearest_spawn(spawn_list, map_id, x, y):
    best = None
    best_dist = float('inf')
    for s in spawn_list:
        if s['map'] != map_id:
            continue
        d = distance(s['x'], s['y'], x, y)
        if d < best_dist:
            best_dist = d
            best = s
    if best is None:
        for s in spawn_list:
            d = distance(s['x'], s['y'], x, y)
            if d < best_dist:
                best_dist = d
                best = s
    return best, best_dist


def audit_guide(guide_path, quests, starters, enders, spawns, max_level=40, fix=False):
    with open(guide_path) as f:
        guide = yaml.safe_load(f)
    if not guide or 'steps' not in guide:
        return [], 0

    steps = guide['steps']
    issues = []
    fixes = 0
    insert_steps = []

    # Build quest step map
    quest_steps = {}
    for i, step in enumerate(steps):
        qid = step.get('quest_id')
        if not qid:
            continue
        if qid not in quest_steps:
            quest_steps[qid] = {'accept': None, 'turnin': None, 'objectives': []}
        stype = step.get('type', '')
        if stype == 'accept_quest' and quest_steps[qid]['accept'] is None:
            quest_steps[qid]['accept'] = i
        elif stype == 'turn_in_quest' and quest_steps[qid]['turnin'] is None:
            quest_steps[qid]['turnin'] = i
        elif stype in ('kill_mobs', 'use_item_on_npc', 'interact_gameobject', 'escort_quest'):
            quest_steps[qid]['objectives'].append(i)

    for qid, pos in quest_steps.items():
        info = quests.get(qid)
        if not info:
            issues.append(f"  MISSING: Quest {qid} not in DB")
            continue
        if max_level > 0 and info['minlevel'] > max_level and info['level'] > max_level:
            continue

        has_obj_steps = len(pos['objectives']) > 0
        has_kill_objectives = any(e > 0 and c > 0 for e, c in info['npc_or_go'])
        has_item_objectives = any(e > 0 and c > 0 for e, c in info['items'])
        has_start_item = info['start_item'] > 0
        is_auto = info['special_flags'] & 4

        # Check 1: Missing objective steps
        if has_kill_objectives and not has_obj_steps and pos['accept'] is not None and pos['turnin'] is not None:
            kill_entries = [e for e, c in info['npc_or_go'] if e > 0]
            issues.append(f"  MISSING KILL: q{qid} '{info['title']}' (L{info['level']}) "
                         f"needs kill {kill_entries} but no objective step")

            if fix:
                all_mob_spawns = []
                for entry in kill_entries:
                    all_mob_spawns.extend(spawns.get(entry, []))
                if all_mob_spawns:
                    accept_step = steps[pos['accept']]
                    map_id = accept_step.get('coordinates', {}).get('map_id',
                        accept_step.get('coordinates', {}).get('map', 0))
                    map_s = [s for s in all_mob_spawns if s['map'] == map_id] or all_mob_spawns
                    cx = sum(s['x'] for s in map_s) / len(map_s)
                    cy = sum(s['y'] for s in map_s) / len(map_s)
                    cz = sum(s['z'] for s in map_s) / len(map_s)
                    spread = max(distance(s['x'], s['y'], cx, cy) for s in map_s) if len(map_s) > 1 else 30
                    new_step = {
                        'id': f'q{qid}_obj_fix', 'name': f'Quest {qid} objective 1',
                        'type': 'kill_mobs', 'quest_id': qid, 'creature_ids': kill_entries,
                        'coordinates': {'x': round(cx, 2), 'y': round(cy, 2), 'z': round(cz, 2),
                                       'radius': round(max(min(spread * 0.7, 100), 30), 1),
                                       'map_id': map_s[0]['map']},
                        'completion_condition': f'quest_objective_complete:{qid}/1'
                    }
                    insert_steps.append((pos['turnin'], new_step))
                    fixes += 1
                    issues[-1] += " → FIXED"

        elif has_item_objectives and not has_obj_steps and pos['accept'] is not None and pos['turnin'] is not None:
            item_entries = [e for e, c in info['items'] if e > 0]
            issues.append(f"  MISSING COLLECT: q{qid} '{info['title']}' (L{info['level']}) "
                         f"needs items {item_entries} but no objective step")

        elif has_start_item and not is_auto and not has_obj_steps and pos['accept'] is not None and pos['turnin'] is not None:
            issues.append(f"  ITEM-USE: q{qid} '{info['title']}' (L{info['level']}) "
                         f"StartItem={info['start_item']} but no objective step")

        # Check 2: Bad accept coords
        if pos['accept'] is not None:
            ac = steps[pos['accept']].get('coordinates', {})
            ax, ay = ac.get('x', 0), ac.get('y', 0)
            amap = ac.get('map_id', ac.get('map', 0))
            if ax != 0 or ay != 0:
                for npc in starters.get(qid, []):
                    sp = spawns.get(npc, [])
                    if sp:
                        nearest, dist = find_nearest_spawn(sp, amap, ax, ay)
                        if dist > 500 and nearest:
                            issues.append(f"  BAD ACCEPT: q{qid} '{info['title']}' "
                                        f"step {pos['accept']} ({ax:.0f},{ay:.0f}) "
                                        f"is {dist:.0f}yd from NPC {npc}")
                            if fix:
                                ac['x'] = round(nearest['x'], 2)
                                ac['y'] = round(nearest['y'], 2)
                                ac['z'] = round(nearest['z'], 2)
                                fixes += 1
                                issues[-1] += " → FIXED"

        # Check 3: Bad turn-in coords
        if pos['turnin'] is not None:
            tc = steps[pos['turnin']].get('coordinates', {})
            tx, ty = tc.get('x', 0), tc.get('y', 0)
            tmap = tc.get('map_id', tc.get('map', 0))
            if tx != 0 or ty != 0:
                for npc in enders.get(qid, []):
                    sp = spawns.get(npc, [])
                    if sp:
                        nearest, dist = find_nearest_spawn(sp, tmap, tx, ty)
                        if dist > 500 and nearest:
                            issues.append(f"  BAD TURNIN: q{qid} '{info['title']}' "
                                        f"step {pos['turnin']} ({tx:.0f},{ty:.0f}) "
                                        f"is {dist:.0f}yd from NPC {npc}")
                            if fix:
                                tc['x'] = round(nearest['x'], 2)
                                tc['y'] = round(nearest['y'], 2)
                                tc['z'] = round(nearest['z'], 2)
                                fixes += 1
                                issues[-1] += " → FIXED"

    # Apply insertions
    if fix and insert_steps:
        for idx, new_step in sorted(insert_steps, key=lambda x: x[0], reverse=True):
            steps.insert(idx, new_step)
        guide['steps'] = steps
        with open(guide_path, 'w') as f:
            yaml.dump(guide, f, default_flow_style=False, sort_keys=False,
                      allow_unicode=True, width=120)

    return issues, fixes


def main():
    fix = '--fix' in sys.argv
    max_level = 40
    for arg in sys.argv:
        if arg.startswith('--max-level='):
            max_level = int(arg.split('=')[1])

    os.makedirs(SCRATCHPAD, exist_ok=True)

    # Check if batch data exists, fetch if not
    if not os.path.exists(f"{SCRATCHPAD}/all_quests.tsv"):
        fetch_all_data()
    else:
        print("Using cached batch data.\n")

    quests, starters, enders, spawns_data = load_all_data()
    print(f"Loaded {len(quests)} quests, {sum(len(v) for v in starters.values())} starters, "
          f"{sum(len(v) for v in enders.values())} enders, "
          f"{sum(len(v) for v in spawns_data.values())} spawns\n")

    if fix:
        print("FIX MODE — will modify guides.\n")
    else:
        print("AUDIT MODE — use --fix to apply fixes.\n")

    guide_files = sorted([
        os.path.join(root, fn)
        for root, dirs, files in os.walk(GUIDE_DIR)
        for fn in files
        if fn.endswith('.yaml') and 'zygor' in fn
    ])

    total_issues = 0
    total_fixes = 0
    for gf in guide_files:
        name = os.path.basename(gf)
        issues, fx = audit_guide(gf, quests, starters, enders, spawns_data, max_level, fix)
        if issues:
            print(f"=== {name} ===")
            for issue in issues:
                print(issue)
            print()
        total_issues += len(issues)
        total_fixes += fx

    print(f"Total: {total_issues} issues, {total_fixes} fixed.")


if __name__ == '__main__':
    main()
