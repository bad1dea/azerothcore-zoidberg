#!/usr/bin/env python3
"""Merge class quest steps INTO zone guides with class restrictions.

For each zone guide, finds matching class quest files and inserts their
steps at the right position (based on quest MinLevel). Each inserted step
gets a class_mask restriction so only the right class executes it.

Usage: python3 merge_class_quests.py
"""

import yaml
import subprocess
import os
import math

DB_HOST = "10.10.30.20"
DB_CONTAINER = "ac-database"
DB_PASSWORD = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"
GUIDE_DIR = "/home/khuong/azerothcore-zoidberg/modules/mod-idlebot/data/guides"

CLASS_NAMES = {
    'warrior': 1, 'paladin': 2, 'hunter': 3, 'rogue': 4,
    'priest': 5, 'shaman': 7, 'mage': 8, 'warlock': 9, 'druid': 11
}


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


def get_quest_level(quest_id):
    rows = query_db(f"SELECT MinLevel, QuestLevel FROM acore_world.quest_template WHERE ID = {quest_id}")
    if rows:
        return int(rows[0][0]), int(rows[0][1])
    return 0, 0


def process_race(race_dir, race_name):
    """Merge class quests into zone guides for a race."""
    # Find all zone guides for this race
    zone_files = sorted([
        f for f in os.listdir(race_dir)
        if f.endswith('.yaml') and f[0:2].isdigit()
    ])

    if not zone_files:
        return 0

    # Find matching class quest files
    class_dir = os.path.join(GUIDE_DIR, "class")
    class_steps_by_level = {}  # level -> list of (class_id, steps)

    for class_name, class_id in CLASS_NAMES.items():
        class_file = os.path.join(class_dir, class_name, f"{race_name}_{class_name}.yaml")
        if not os.path.exists(class_file):
            continue

        with open(class_file) as f:
            cguide = yaml.safe_load(f)

        if not cguide or 'steps' not in cguide:
            continue

        # Determine what level these class quests should be inserted at
        first_quest = None
        for s in cguide['steps']:
            if s.get('quest_id'):
                first_quest = s['quest_id']
                break

        if first_quest:
            min_level, quest_level = get_quest_level(first_quest)
            insert_level = max(min_level, 1)
        else:
            insert_level = 1

        # Add class restriction to each step
        class_mask = 1 << (class_id - 1)
        for s in cguide['steps']:
            if 'restrictions' not in s:
                s['restrictions'] = {}
            s['restrictions']['class_mask'] = class_mask

        if insert_level not in class_steps_by_level:
            class_steps_by_level[insert_level] = []
        class_steps_by_level[insert_level].append((class_name, cguide['steps']))

    if not class_steps_by_level:
        return 0

    # Insert class quest steps into the appropriate zone guide
    total_inserted = 0
    for zone_file in zone_files:
        zone_path = os.path.join(race_dir, zone_file)
        with open(zone_path) as f:
            guide = yaml.safe_load(f)

        if not guide or 'steps' not in guide:
            continue

        level_min = guide.get('level_min', 1)
        level_max = guide.get('level_max', 80)

        # Find class quests that belong in this zone's level range
        steps_to_insert = []
        for level, class_groups in class_steps_by_level.items():
            if level_min <= level <= level_max:
                for class_name, steps in class_groups:
                    steps_to_insert.extend(steps)

        if not steps_to_insert:
            continue

        # Find insertion point — after the last accept_quest in the first
        # few steps (so class quests come after the bot picks up zone quests)
        insert_idx = len(guide['steps'])
        for i, s in enumerate(guide['steps']):
            qid = s.get('quest_id', 0)
            if qid:
                min_level, _ = get_quest_level(qid)
                # Insert class quests before the first quest that requires
                # a higher level than the class quest
                for cl, cgroups in class_steps_by_level.items():
                    if level_min <= cl <= level_max and min_level >= cl:
                        insert_idx = min(insert_idx, i)
                        break

        # Insert at the found position
        for s in reversed(steps_to_insert):
            guide['steps'].insert(insert_idx, s)
            total_inserted += 1

        with open(zone_path, 'w') as f:
            yaml.dump(guide, f, default_flow_style=False, sort_keys=False,
                      allow_unicode=True, width=120)

        print(f"  {zone_file}: +{len(steps_to_insert)} class quest steps")

    return total_inserted


def main():
    total = 0

    # Process each race directory
    for faction in ['horde', 'alliance']:
        faction_dir = os.path.join(GUIDE_DIR, faction)
        if not os.path.isdir(faction_dir):
            continue
        for race in os.listdir(faction_dir):
            race_dir = os.path.join(faction_dir, race)
            if not os.path.isdir(race_dir):
                continue
            print(f"{faction}/{race}:")
            n = process_race(race_dir, race)
            total += n

    print(f"\nDone. Inserted {total} class quest steps total.")


if __name__ == '__main__':
    main()
