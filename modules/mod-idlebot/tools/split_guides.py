#!/usr/bin/env python3
"""Split mega-guide YAMLs into chained zone files.

Reads each *_zygor_1_80.yaml and splits into zone-level files with
next_guide chaining. Class-specific quests are extracted to separate files.

Usage: python3 split_guides.py
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

# Zone definitions per race — (zone_name, level_min, level_max)
ZONE_BANDS = {
    'tauren': [
        ('camp_narache', 1, 6),
        ('bloodhoof', 6, 12),
        ('barrens', 12, 20),
        ('stonetalon', 20, 25),
        ('ashenvale', 25, 30),
        ('thousand_needles', 30, 35),
        ('desolace', 35, 40),
    ],
    'undead': [
        ('deathknell', 1, 6),
        ('brill', 6, 12),
        ('silverpine', 12, 20),
        ('hillsbrad', 20, 25),
        ('arathi', 25, 30),
        ('alterac', 30, 35),
        ('hinterlands', 35, 40),
    ],
    'orc': [
        ('valley_of_trials', 1, 6),
        ('razor_hill', 6, 12),
        ('barrens', 12, 20),
        ('stonetalon', 20, 25),
        ('ashenvale', 25, 30),
        ('thousand_needles', 30, 35),
        ('desolace', 35, 40),
    ],
    'troll': [
        ('echo_isles', 1, 6),
        ('razor_hill', 6, 12),
        ('barrens', 12, 20),
        ('stonetalon', 20, 25),
        ('ashenvale', 25, 30),
        ('thousand_needles', 30, 35),
        ('desolace', 35, 40),
    ],
    'bloodelf': [
        ('sunstrider_isle', 1, 6),
        ('falconwing', 6, 12),
        ('ghostlands', 12, 20),
        ('hillsbrad', 20, 25),
        ('arathi', 25, 30),
        ('hinterlands', 30, 35),
        ('western_plaguelands', 35, 40),
    ],
    'dwarf': [
        ('coldridge', 1, 6),
        ('kharanos', 6, 12),
        ('loch_modan', 12, 20),
        ('wetlands', 20, 25),
        ('duskwood', 25, 30),
        ('stranglethorn', 30, 35),
        ('badlands', 35, 40),
    ],
    'gnome': [
        ('gnomeregan', 1, 6),
        ('kharanos', 6, 12),
        ('loch_modan', 12, 20),
        ('wetlands', 20, 25),
        ('duskwood', 25, 30),
        ('stranglethorn', 30, 35),
        ('badlands', 35, 40),
    ],
    'human': [
        ('northshire', 1, 6),
        ('goldshire', 6, 12),
        ('westfall', 12, 20),
        ('redridge', 20, 25),
        ('duskwood', 25, 30),
        ('stranglethorn', 30, 35),
        ('badlands', 35, 40),
    ],
    'nightelf': [
        ('shadowglen', 1, 6),
        ('dolanaar', 6, 12),
        ('darkshore', 12, 20),
        ('ashenvale', 20, 25),
        ('stonetalon', 25, 30),
        ('desolace', 30, 35),
        ('feralas', 35, 40),
    ],
    'draenei': [
        ('ammen_vale', 1, 6),
        ('azure_watch', 6, 12),
        ('bloodmyst', 12, 20),
        ('darkshore', 20, 25),
        ('ashenvale', 25, 30),
        ('stonetalon', 30, 35),
        ('desolace', 35, 40),
    ],
}

FACTIONS = {
    'tauren': 'horde', 'undead': 'horde', 'orc': 'horde',
    'troll': 'horde', 'bloodelf': 'horde',
    'dwarf': 'alliance', 'gnome': 'alliance', 'human': 'alliance',
    'nightelf': 'alliance', 'draenei': 'alliance',
}

# WoW class IDs
CLASS_NAMES = {
    1: 'warrior', 2: 'paladin', 3: 'hunter', 4: 'rogue',
    5: 'priest', 6: 'death_knight', 7: 'shaman', 8: 'mage',
    9: 'warlock', 11: 'druid'
}


def query_db(sql):
    cmd = [
        "ssh", f"khuong@{DB_HOST}",
        f"docker exec {DB_CONTAINER} mysql -u root -p{DB_PASSWORD} -N -e \"{sql}\""
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        return []
    return [line.split('\t') for line in result.stdout.strip().split('\n') if line]


def load_quest_data():
    """Load quest level and class data from DB."""
    rows = query_db(
        "SELECT qt.ID, qt.QuestLevel, qt.MinLevel, IFNULL(qta.AllowableClasses, 0) "
        "FROM acore_world.quest_template qt "
        "LEFT JOIN acore_world.quest_template_addon qta ON qta.ID = qt.ID "
        "WHERE qt.QuestLevel <= 45 OR qt.QuestLevel = -1"
    )
    quests = {}
    for r in rows:
        qid = int(r[0])
        qlevel = int(r[1])
        minlevel = int(r[2])
        classes = int(r[3])
        # QuestLevel -1 means scales with player — use MinLevel
        if qlevel == -1:
            qlevel = minlevel
        quests[qid] = {'level': qlevel, 'minlevel': minlevel, 'classes': classes}
    return quests


def get_quest_level(quest_data, quest_id):
    """Get the effective level of a quest."""
    if quest_id in quest_data:
        return quest_data[quest_id]['level']
    return 0


def is_class_quest(quest_data, quest_id):
    """Check if quest is class-specific."""
    if quest_id in quest_data:
        classes = quest_data[quest_id]['classes']
        if classes != 0:
            # Count bits set — if only 1 class allowed, it's class-specific
            bits = bin(classes).count('1')
            return bits <= 3  # up to 3 classes = specific enough
    return False


def get_class_name(quest_data, quest_id):
    """Get the class name for a class-specific quest."""
    if quest_id not in quest_data:
        return None
    classes = quest_data[quest_id]['classes']
    for class_id, name in CLASS_NAMES.items():
        if classes & (1 << (class_id - 1)):
            return name
    return None


def find_zone_band(race, quest_level):
    """Find which zone band a quest level falls into."""
    bands = ZONE_BANDS.get(race, [])
    for i, (zone, lmin, lmax) in enumerate(bands):
        if lmin <= quest_level < lmax:
            return i, zone, lmin, lmax
    # Beyond our defined bands — put in overflow
    return len(bands), 'overflow', 40, 80


def split_guide(guide_path, race, quest_data):
    """Split a mega-guide into zone files."""
    with open(guide_path) as f:
        guide = yaml.safe_load(f)

    if not guide or 'steps' not in guide:
        return []

    faction = FACTIONS.get(race, 'horde')
    bands = ZONE_BANDS.get(race, [])

    # Buckets: zone_index → list of steps
    zone_steps = defaultdict(list)
    class_steps = defaultdict(list)  # class_name → list of steps

    # Track the current quest's zone bucket so non-quest steps
    # go with their surrounding quests
    current_zone_idx = 0

    for step in guide['steps']:
        qid = step.get('quest_id')

        if qid and qid in quest_data:
            qlevel = get_quest_level(quest_data, qid)

            # Class quest extraction
            if is_class_quest(quest_data, qid):
                cname = get_class_name(quest_data, qid)
                if cname:
                    class_steps[cname].append(step)
                    continue

            zone_idx, _, _, _ = find_zone_band(race, qlevel)
            current_zone_idx = zone_idx

        zone_steps[current_zone_idx].append(step)

    # Generate zone files
    output_files = []
    guide_dir = os.path.dirname(guide_path)

    sorted_zones = sorted(zone_steps.keys())

    for i, zone_idx in enumerate(sorted_zones):
        if zone_idx < len(bands):
            zone_name, lmin, lmax = bands[zone_idx]
        else:
            zone_name, lmin, lmax = 'overflow', 40, 80

        guide_id = f"{race}-{zone_name}-{lmin}-{lmax}"
        filename = f"{zone_idx:02d}_{zone_name}_{lmin}_{lmax}.yaml"

        # Determine next_guide
        next_idx = i + 1
        if next_idx < len(sorted_zones):
            next_zone_idx = sorted_zones[next_idx]
            if next_zone_idx < len(bands):
                nz, nl, nh = bands[next_zone_idx]
            else:
                nz, nl, nh = 'overflow', 40, 80
            next_guide_id = f"{race}-{nz}-{nl}-{nh}"
        else:
            next_guide_id = ""

        zone_guide = {
            'id': guide_id,
            'name': f"{zone_name.replace('_', ' ').title()} ({lmin}-{lmax})",
            'faction': faction,
            'race': race,
            'level_min': lmin,
            'level_max': lmax,
            'next_guide': next_guide_id,
            'steps': zone_steps[zone_idx],
        }

        filepath = os.path.join(guide_dir, filename)
        with open(filepath, 'w') as f:
            yaml.dump(zone_guide, f, default_flow_style=False, sort_keys=False,
                      allow_unicode=True, width=120)

        output_files.append((filepath, len(zone_steps[zone_idx])))
        print(f"    {filename}: {len(zone_steps[zone_idx])} steps (L{lmin}-{lmax})")

    # Generate class quest files
    if class_steps:
        class_dir = os.path.join(GUIDE_DIR, 'class')
        os.makedirs(class_dir, exist_ok=True)

        for cname, steps in class_steps.items():
            class_guide = {
                'id': f"class-{cname}-{race}",
                'name': f"{cname.title()} Class Quests ({race.title()})",
                'faction': faction,
                'race': race,
                'class': cname,
                'level_min': 1,
                'level_max': 80,
                'steps': steps,
            }

            cdir = os.path.join(class_dir, cname)
            os.makedirs(cdir, exist_ok=True)
            cpath = os.path.join(cdir, f"{race}_{cname}.yaml")
            with open(cpath, 'w') as f:
                yaml.dump(class_guide, f, default_flow_style=False, sort_keys=False,
                          allow_unicode=True, width=120)
            print(f"    class/{cname}/{race}_{cname}.yaml: {len(steps)} class quest steps")

    return output_files


def main():
    print("Loading quest data from DB...")
    quest_data = load_quest_data()
    print(f"Loaded {len(quest_data)} quests.\n")

    # Find all zygor mega-guides
    mega_guides = []
    for root, dirs, files in os.walk(GUIDE_DIR):
        for fn in files:
            if fn.endswith('_zygor_1_80.yaml'):
                mega_guides.append(os.path.join(root, fn))

    if not mega_guides:
        print("No zygor mega-guides found.")
        return

    print(f"Found {len(mega_guides)} mega-guides to split.\n")

    for gpath in sorted(mega_guides):
        # Extract race from filename
        fname = os.path.basename(gpath)
        race = fname.replace('_zygor_1_80.yaml', '')

        if race not in ZONE_BANDS:
            print(f"Skipping {fname} — no zone bands defined for race '{race}'")
            continue

        print(f"Splitting {fname} (race={race}):")
        split_guide(gpath, race, quest_data)

        # Remove the mega-guide
        os.remove(gpath)
        print(f"  Removed {fname}")
        print()

    print("Done. Run the guide loader to verify the new zone files load correctly.")


if __name__ == '__main__':
    main()
