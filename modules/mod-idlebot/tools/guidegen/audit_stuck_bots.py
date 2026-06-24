#!/usr/bin/env python3
"""Diagnose stuck bots by checking their current step against the DB.

For each registered bot, reads current guide/step/position and checks:
- Does the quest exist?
- Is the NPC at the right coords?
- Is the quest acceptable (level, prereqs)?
- What's blocking?

Usage: python3 audit_stuck_bots.py
"""

import subprocess, json, os, sys, math, yaml

DB_HOST = "10.10.30.20"
DB_PASS = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"
DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../data/generated")
GUIDE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../data/guides")


def q(sql):
    r = subprocess.run(
        ["ssh", f"khuong@{DB_HOST}",
         f"docker exec ac-database mysql -u root -p{DB_PASS} -N -e \"{sql}\""],
        capture_output=True, text=True, timeout=30)
    return r.stdout.strip()


def dist(x1, y1, x2, y2):
    return math.sqrt((x1 - x2) ** 2 + (y1 - y2) ** 2)


def main():
    # Load DB data
    quests = json.load(open(os.path.join(DATA_DIR, "quests.json")))
    npc_spawns = json.load(open(os.path.join(DATA_DIR, "npc_spawns.json")))

    # Get bot state from DB
    raw = q("SELECT b.bot_name, c.name, c.level, c.race, c.class, c.map, c.position_x, c.position_y, c.position_z, "
            "b.guide_id, b.step_index, b.death_count_total "
            "FROM acore_characters.idlebot_bots b "
            "JOIN acore_characters.characters c ON c.name = b.bot_name "
            "ORDER BY b.bot_name")

    print("# Stuck Bot Audit Report\n")

    for line in raw.split("\n"):
        if not line.strip():
            continue
        parts = line.split("\t")
        bot_name = parts[0]
        level = int(parts[2])
        race = int(parts[3])
        cls = int(parts[4])
        bot_map = int(parts[5])
        bot_x = float(parts[6])
        bot_y = float(parts[7])
        bot_z = float(parts[8])
        guide_id = parts[9] if parts[9] != "NULL" else ""
        step_idx = int(parts[10])
        deaths = int(parts[11])

        print(f"## {bot_name}")
        print(f"  Level: {level}, Race: {race}, Class: {cls}")
        print(f"  Position: map={bot_map} ({bot_x:.0f}, {bot_y:.0f}, {bot_z:.0f})")
        print(f"  Guide: {guide_id}, Step: {step_idx}, Deaths: {deaths}")

        if not guide_id:
            print(f"  **NO GUIDE ASSIGNED**\n")
            continue

        # Find and load the guide file
        guide_file = None
        for root, dirs, files in os.walk(GUIDE_DIR):
            for fn in files:
                if fn.endswith(".yaml"):
                    path = os.path.join(root, fn)
                    try:
                        with open(path) as f:
                            g = yaml.safe_load(f)
                        if g and g.get("id") == guide_id:
                            guide_file = path
                            break
                    except:
                        pass
            if guide_file:
                break

        if not guide_file:
            print(f"  **GUIDE FILE NOT FOUND for '{guide_id}'**\n")
            continue

        with open(guide_file) as f:
            guide = yaml.safe_load(f)

        if step_idx >= len(guide.get("steps", [])):
            print(f"  **STEP {step_idx} OUT OF RANGE (guide has {len(guide['steps'])} steps)**")
            print(f"  next_guide: {guide.get('next_guide', 'NONE')}\n")
            continue

        step = guide["steps"][step_idx]
        qid = step.get("quest_id")
        stype = step.get("type", "?")
        npc_id = step.get("npc_id")
        coords = step.get("coordinates", {})
        step_x = coords.get("x", 0)
        step_y = coords.get("y", 0)
        step_map = coords.get("map_id", coords.get("map", 0))

        print(f"  Current step: {stype} — {step.get('name', '?')}")
        if qid:
            print(f"  Quest: {qid}")

        # Distance from bot to step target
        if step_x and step_y:
            d = dist(bot_x, bot_y, step_x, step_y)
            print(f"  Distance to target: {d:.0f}yd (map {step_map})")
            if bot_map != step_map:
                print(f"  **WRONG MAP**: bot on map {bot_map}, step on map {step_map}")

        # Check NPC
        if npc_id:
            spawns = npc_spawns.get(str(npc_id), [])
            if not spawns:
                print(f"  **NPC {npc_id} NOT FOUND IN DB**")
            else:
                same_map = [s for s in spawns if s["map"] == bot_map]
                if same_map:
                    nearest = min(same_map, key=lambda s: dist(s["x"], s["y"], bot_x, bot_y))
                    nd = dist(nearest["x"], nearest["y"], bot_x, bot_y)
                    print(f"  NPC {npc_id} ({nearest.get('name','?')}): nearest spawn {nd:.0f}yd away at ({nearest['x']:.0f}, {nearest['y']:.0f})")
                    if step_x:
                        sd = dist(nearest["x"], nearest["y"], step_x, step_y)
                        if sd > 30:
                            print(f"  **BAD STEP COORDS**: NPC is {sd:.0f}yd from step coords ({step_x:.0f}, {step_y:.0f})")
                else:
                    print(f"  **NPC {npc_id} NOT ON BOT'S MAP {bot_map}** — spawns on maps: {list(set(s['map'] for s in spawns))}")

        # Check quest
        if qid:
            quest = quests.get(str(qid))
            if not quest:
                print(f"  **QUEST {qid} NOT IN DB**")
            else:
                min_level = quest.get("MinLevel", 0)
                if min_level and min_level > level:
                    print(f"  **UNDER-LEVELED**: quest needs L{min_level}, bot is L{level}")
                prev = quest.get("PrevQuestID", 0)
                if prev and prev > 0:
                    # Check if prev was completed
                    done = q(f"SELECT COUNT(*) FROM acore_characters.character_queststatus_rewarded "
                            f"WHERE guid = (SELECT guid FROM acore_characters.characters WHERE name = '{bot_name}') "
                            f"AND quest = {prev}")
                    if done.strip() == "0":
                        print(f"  **PREREQ NOT DONE**: needs quest {prev} completed first")

        # Diagnosis
        issues = []
        if bot_map != step_map and step_map:
            issues.append("wrong_map")
        if npc_id and not npc_spawns.get(str(npc_id)):
            issues.append("npc_not_found")
        if qid and not quests.get(str(qid)):
            issues.append("quest_not_in_db")

        if issues:
            print(f"  **Issues**: {', '.join(issues)}")
        else:
            print(f"  No obvious DB issues — may be terrain/pathing/combat")

        print()


if __name__ == "__main__":
    main()
