#!/usr/bin/env python3
"""Extract all quest/NPC/GO/spawn data from AzerothCore DB into JSON files.

Uses batch queries (one per table) for speed. Saves to data/generated/.
This is the foundation for the guide generator — all other tools read these files.

Usage: python3 extract_db_quests.py [--db-host HOST]
"""

import subprocess, json, sys, os, argparse

DB_HOST = "10.10.30.20"
DB_PASS = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../data/generated")


def q(sql, timeout=60):
    r = subprocess.run(
        ["ssh", f"khuong@{DB_HOST}",
         f"docker exec ac-database mysql -u root -p{DB_PASS} -N -e \"{sql}\""],
        capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        print(f"  WARN: query failed: {r.stderr[:200]}", file=sys.stderr)
        return ""
    return r.stdout.strip()


def parse_rows(raw, col_names):
    """Parse tab-separated output into list of dicts."""
    rows = []
    for line in raw.split("\n"):
        if not line.strip():
            continue
        parts = line.split("\t")
        row = {}
        for i, name in enumerate(col_names):
            val = parts[i] if i < len(parts) else ""
            # Auto-convert numbers
            if val == "NULL" or val == "":
                row[name] = None
            else:
                try:
                    row[name] = int(val)
                except ValueError:
                    try:
                        row[name] = float(val)
                    except ValueError:
                        row[name] = val
        rows.append(row)
    return rows


def extract_quests():
    """Extract quest_template + quest_template_addon."""
    print("Extracting quests...")
    cols = ["ID", "QuestType", "QuestLevel", "MinLevel", "AllowableRaces",
            "Flags", "StartItem", "RewardNextQuest",
            "RequiredNpcOrGo1", "RequiredNpcOrGo2", "RequiredNpcOrGo3", "RequiredNpcOrGo4",
            "RequiredNpcOrGoCount1", "RequiredNpcOrGoCount2", "RequiredNpcOrGoCount3", "RequiredNpcOrGoCount4",
            "RequiredItemId1", "RequiredItemId2", "RequiredItemId3", "RequiredItemId4",
            "RequiredItemId5", "RequiredItemId6",
            "RequiredItemCount1", "RequiredItemCount2", "RequiredItemCount3", "RequiredItemCount4",
            "RequiredItemCount5", "RequiredItemCount6",
            "LogTitle"]
    raw = q(f"SELECT {','.join(cols)} FROM acore_world.quest_template", timeout=120)
    quests = parse_rows(raw, cols)
    print(f"  {len(quests)} quests extracted")

    # Addon data
    addon_cols = ["ID", "MaxLevel", "AllowableClasses", "PrevQuestID", "NextQuestID",
                  "ExclusiveGroup", "BreadcrumbForQuestId", "SpecialFlags"]
    raw = q(f"SELECT {','.join(addon_cols)} FROM acore_world.quest_template_addon")
    addons = {r["ID"]: r for r in parse_rows(raw, addon_cols)}
    print(f"  {len(addons)} addon records")

    # Merge addon into quests
    for quest in quests:
        addon = addons.get(quest["ID"], {})
        quest["AllowableClasses"] = addon.get("AllowableClasses", 0)
        quest["PrevQuestID"] = addon.get("PrevQuestID", 0)
        quest["NextQuestID"] = addon.get("NextQuestID", 0)
        quest["ExclusiveGroup"] = addon.get("ExclusiveGroup", 0)
        quest["BreadcrumbForQuestId"] = addon.get("BreadcrumbForQuestId", 0)
        quest["SpecialFlags"] = addon.get("SpecialFlags", 0)

    return {q["ID"]: q for q in quests}


def extract_quest_relations():
    """Extract quest starter/ender NPC/GO relations."""
    print("Extracting quest relations...")

    starters = {}
    raw = q("SELECT id, quest FROM acore_world.creature_queststarter")
    for line in raw.split("\n"):
        if not line.strip():
            continue
        parts = line.split("\t")
        npc, quest = int(parts[0]), int(parts[1])
        starters.setdefault(quest, []).append({"type": "creature", "entry": npc})

    raw = q("SELECT id, quest FROM acore_world.gameobject_queststarter")
    for line in raw.split("\n"):
        if not line.strip():
            continue
        parts = line.split("\t")
        go, quest = int(parts[0]), int(parts[1])
        starters.setdefault(quest, []).append({"type": "gameobject", "entry": go})
    print(f"  {sum(len(v) for v in starters.values())} quest starters")

    enders = {}
    raw = q("SELECT id, quest FROM acore_world.creature_questender")
    for line in raw.split("\n"):
        if not line.strip():
            continue
        parts = line.split("\t")
        npc, quest = int(parts[0]), int(parts[1])
        enders.setdefault(quest, []).append({"type": "creature", "entry": npc})

    raw = q("SELECT id, quest FROM acore_world.gameobject_questender")
    for line in raw.split("\n"):
        if not line.strip():
            continue
        parts = line.split("\t")
        go, quest = int(parts[0]), int(parts[1])
        enders.setdefault(quest, []).append({"type": "gameobject", "entry": go})
    print(f"  {sum(len(v) for v in enders.values())} quest enders")

    return starters, enders


def extract_npc_spawns():
    """Extract creature spawns (position + template name)."""
    print("Extracting NPC spawns...")
    raw = q("SELECT c.id1, ct.name, c.map, c.position_x, c.position_y, c.position_z, ct.minlevel, ct.maxlevel, ct.npcflag "
            "FROM acore_world.creature c "
            "JOIN acore_world.creature_template ct ON ct.entry = c.id1", timeout=180)
    spawns = {}
    count = 0
    for line in raw.split("\n"):
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < 6:
            continue
        entry = int(parts[0])
        spawn = {
            "entry": entry,
            "name": parts[1],
            "map": int(parts[2]),
            "x": float(parts[3]),
            "y": float(parts[4]),
            "z": float(parts[5]),
            "minlevel": int(parts[6]) if len(parts) > 6 else 0,
            "maxlevel": int(parts[7]) if len(parts) > 7 else 0,
            "npcflag": int(parts[8]) if len(parts) > 8 else 0,
        }
        spawns.setdefault(entry, []).append(spawn)
        count += 1
    print(f"  {count} spawns for {len(spawns)} unique NPCs")
    return spawns


def extract_playercreateinfo():
    """Extract starting positions for each race/class."""
    print("Extracting player create info...")
    raw = q("SELECT race, class, map, position_x, position_y, position_z, orientation "
            "FROM acore_world.playercreateinfo")
    info = []
    for line in raw.split("\n"):
        if not line.strip():
            continue
        parts = line.split("\t")
        info.append({
            "race": int(parts[0]),
            "class": int(parts[1]),
            "map": int(parts[2]),
            "x": float(parts[3]),
            "y": float(parts[4]),
            "z": float(parts[5]),
        })
    print(f"  {len(info)} race/class combos")
    return info


def extract_go_spawns():
    """Extract gameobject spawns for quest-related GOs."""
    print("Extracting GO spawns...")
    raw = q("SELECT g.id, gt.name, g.map, g.position_x, g.position_y, g.position_z "
            "FROM acore_world.gameobject g "
            "JOIN acore_world.gameobject_template gt ON gt.entry = g.id", timeout=120)
    spawns = {}
    count = 0
    for line in raw.split("\n"):
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < 6:
            continue
        entry = int(parts[0])
        spawn = {
            "entry": entry,
            "name": parts[1],
            "map": int(parts[2]),
            "x": float(parts[3]),
            "y": float(parts[4]),
            "z": float(parts[5]),
        }
        spawns.setdefault(entry, []).append(spawn)
        count += 1
    print(f"  {count} GO spawns for {len(spawns)} unique GOs")
    return spawns


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    quests = extract_quests()
    starters, enders = extract_quest_relations()
    npc_spawns = extract_npc_spawns()
    create_info = extract_playercreateinfo()
    go_spawns = extract_go_spawns()

    # Save all to JSON
    def save(name, data):
        path = os.path.join(OUT_DIR, name)
        with open(path, "w") as f:
            json.dump(data, f, indent=1, ensure_ascii=False)
        print(f"  Saved {path} ({os.path.getsize(path) // 1024}KB)")

    save("quests.json", quests)
    save("quest_starters.json", starters)
    save("quest_enders.json", enders)
    save("npc_spawns.json", npc_spawns)
    save("go_spawns.json", go_spawns)
    save("playercreateinfo.json", create_info)

    print(f"\nDone. All data in {OUT_DIR}/")


if __name__ == "__main__":
    main()
