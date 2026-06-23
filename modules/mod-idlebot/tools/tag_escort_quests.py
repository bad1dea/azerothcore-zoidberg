#!/usr/bin/env python3
"""Tag escort quests in idlebot guide YAMLs.

Queries the world DB for quests with SpecialFlags & 2 (escort flag), then
scans all guide YAML files and changes kill_mobs steps for those quests
to escort_quest type. Also sets the npc_id if missing.

Usage: python3 tag_escort_quests.py [ssh_host]
"""

import yaml
import subprocess
import sys
import os

HOST = sys.argv[1] if len(sys.argv) > 1 else "10.10.30.20"
PASS = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"
GUIDE_DIR = "/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/data/guides"


def query(sql):
    cmd = f'ssh khuong@{HOST} "docker exec ac-database mysql -u root -p{PASS} -N -e \\"{sql}\\""'
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=30)
    rows = []
    for line in result.stdout.strip().split('\n'):
        if line:
            rows.append(line.split('\t'))
    return rows


def main():
    # Get escort quest IDs
    escort_rows = query(
        "SELECT qt.ID FROM acore_world.quest_template qt "
        "JOIN acore_world.quest_template_addon qta ON qta.ID = qt.ID "
        "WHERE qta.SpecialFlags & 2 AND qt.QuestLevel > 0"
    )
    escort_ids = set(int(r[0]) for r in escort_rows if r)
    print(f"Found {len(escort_ids)} escort quests in DB.")

    # Get quest givers (creature_queststarter)
    giver_rows = query("SELECT quest, id FROM acore_world.creature_queststarter")
    quest_givers = {}
    for r in giver_rows:
        if r and len(r) >= 2:
            qid = int(r[0])
            npc = int(r[1])
            if qid not in quest_givers:
                quest_givers[qid] = npc

    total_tagged = 0
    for root, dirs, files in os.walk(GUIDE_DIR):
        for fn in files:
            if not (fn.endswith('.yaml') or fn.endswith('.yml')):
                continue
            path = os.path.join(root, fn)
            with open(path) as f:
                guide = yaml.safe_load(f)
            if not guide or 'steps' not in guide:
                continue

            modified = 0
            for step in guide['steps']:
                qid = step.get('quest_id')
                if not qid or qid not in escort_ids:
                    continue
                if step.get('type') != 'kill_mobs':
                    continue
                step['type'] = 'escort_quest'
                if 'npc_id' not in step and qid in quest_givers:
                    step['npc_id'] = quest_givers[qid]
                modified += 1

            if modified > 0:
                with open(path, 'w') as f:
                    yaml.dump(guide, f, default_flow_style=False, sort_keys=False,
                              allow_unicode=True, width=120)
                print(f"  {fn}: tagged {modified} escort steps")
                total_tagged += modified

    print(f"\nDone. Tagged {total_tagged} steps as escort_quest across all guides.")


if __name__ == '__main__':
    main()
