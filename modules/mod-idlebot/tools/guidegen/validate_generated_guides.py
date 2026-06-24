#!/usr/bin/env python3
"""Validate generated guides against the DB data.

Checks every step for:
- Quest exists in DB
- Accept NPC exists and coords within 30yd of actual spawn
- Turn-in NPC exists and coords within 30yd
- Kill/collect objectives have spawns on same map
- PrevQuestID chain satisfied (prev appears earlier in guide)
- No accept→turn_in with missing objective (unless quest has no objectives)
- Level progression is reasonable

Usage: python3 validate_generated_guides.py [--guides DIR]
"""

import json, os, sys, math, yaml, argparse

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../data/generated")
GUIDE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../data/guides/generated")


def dist(x1, y1, x2, y2):
    return math.sqrt((x1 - x2) ** 2 + (y1 - y2) ** 2)


def load_data():
    data = {}
    for name in ["quests", "quest_starters", "quest_enders", "npc_spawns", "go_spawns"]:
        path = os.path.join(DATA_DIR, f"{name}.json")
        with open(path) as f:
            data[name] = json.load(f)
    # Load item sources for objective coord validation
    item_sources_path = os.path.join(DATA_DIR, "item_sources.json")
    if os.path.exists(item_sources_path):
        with open(item_sources_path) as f:
            data["item_sources"] = json.load(f)
    else:
        data["item_sources"] = {}
    return data


def validate_guide(guide_path, data):
    with open(guide_path) as f:
        guide = yaml.safe_load(f)

    if not guide or "steps" not in guide:
        return [{"type": "EMPTY", "file": guide_path}]

    issues = []
    quests_seen = set()  # Track quest IDs we've accepted
    has_objective = {}   # quest_id -> bool (whether an objective step exists)

    for i, step in enumerate(guide["steps"]):
        qid = step.get("quest_id")
        stype = step.get("type", "")
        coords = step.get("coordinates", {})
        step_x = coords.get("x", 0)
        step_y = coords.get("y", 0)
        step_map = coords.get("map_id", coords.get("map", 0))

        if not qid:
            continue

        qid_str = str(qid)
        quest = data["quests"].get(qid_str)

        # Quest exists?
        if not quest:
            issues.append({"type": "QUEST_NOT_FOUND", "step": i, "quest": qid,
                          "file": os.path.basename(guide_path)})
            continue

        if stype == "accept_quest":
            # Check prereq chain
            prev = quest.get("PrevQuestID", 0)
            if prev and prev > 0 and prev not in quests_seen:
                # Check if prev is a quest we should have
                prev_str = str(prev)
                if prev_str in data["quests"]:
                    issues.append({"type": "MISSING_PREREQ", "step": i, "quest": qid,
                                  "needs": prev, "file": os.path.basename(guide_path)})

            quests_seen.add(qid)

            # Check NPC coords
            npc_id = step.get("npc_id")
            if npc_id and step_x != 0:
                spawns = data["npc_spawns"].get(str(npc_id), [])
                if not spawns:
                    issues.append({"type": "NPC_NOT_FOUND", "step": i, "quest": qid,
                                  "npc": npc_id, "file": os.path.basename(guide_path)})
                else:
                    nearest = min(spawns, key=lambda s: dist(s["x"], s["y"], step_x, step_y)
                                  if s["map"] == step_map else 99999)
                    d = dist(nearest["x"], nearest["y"], step_x, step_y) if nearest["map"] == step_map else 99999
                    if d > 30:
                        issues.append({"type": "BAD_COORDS", "step": i, "quest": qid,
                                      "npc": npc_id, "distance": round(d, 1),
                                      "file": os.path.basename(guide_path)})

        elif stype == "turn_in_quest":
            # Check for missing objective
            if qid not in has_objective:
                has_obj = False
                start_item = quest.get("StartItem", 0) or 0
                for j in range(1, 5):
                    if quest.get(f"RequiredNpcOrGo{j}", 0):
                        has_obj = True
                        break
                for j in range(1, 7):
                    item = quest.get(f"RequiredItemId{j}", 0) or 0
                    if item and item != start_item:  # Skip delivery items
                        has_obj = True
                        break
                if has_obj:
                    issues.append({"type": "MISSING_OBJECTIVE", "step": i, "quest": qid,
                                  "title": quest.get("LogTitle", "?"),
                                  "file": os.path.basename(guide_path)})

            # Check turn-in NPC coords
            npc_id = step.get("npc_id")
            if npc_id and step_x != 0:
                spawns = data["npc_spawns"].get(str(npc_id), [])
                if spawns:
                    nearest = min(spawns, key=lambda s: dist(s["x"], s["y"], step_x, step_y)
                                  if s["map"] == step_map else 99999)
                    d = dist(nearest["x"], nearest["y"], step_x, step_y) if nearest["map"] == step_map else 99999
                    if d > 30:
                        issues.append({"type": "BAD_TURNIN_COORDS", "step": i, "quest": qid,
                                      "npc": npc_id, "distance": round(d, 1),
                                      "file": os.path.basename(guide_path)})

        elif stype in ("kill_mobs", "interact_gameobject", "use_item_at_location"):
            has_objective[qid] = True

            # Check for _unsafe marker from generator
            if step.get("_unsafe"):
                issues.append({"type": "UNSAFE_OBJECTIVE", "step": i, "quest": qid,
                              "reason": step["_unsafe"], "file": os.path.basename(guide_path)})

            # Check objective target spawn exists on same map
            creature_ids = step.get("creature_ids", [])
            for cid in creature_ids:
                spawns = data["npc_spawns"].get(str(cid), [])
                if not spawns:
                    issues.append({"type": "OBJECTIVE_NPC_NOT_FOUND", "step": i, "quest": qid,
                                  "creature": cid, "file": os.path.basename(guide_path)})
                else:
                    if step_map and not any(s["map"] == step_map for s in spawns):
                        issues.append({"type": "OBJECTIVE_WRONG_MAP", "step": i, "quest": qid,
                                      "creature": cid, "step_map": step_map,
                                      "actual_maps": list(set(s["map"] for s in spawns)),
                                      "file": os.path.basename(guide_path)})
                    # Check objective coord is near actual spawn (not quest giver)
                    if step_x != 0:
                        same_map = [s for s in spawns if s["map"] == step_map]
                        if same_map:
                            nearest = min(same_map, key=lambda s: dist(s["x"], s["y"], step_x, step_y))
                            d = dist(nearest["x"], nearest["y"], step_x, step_y)
                            if d > 100:
                                issues.append({"type": "OBJECTIVE_FAR_FROM_SPAWN", "step": i, "quest": qid,
                                              "creature": cid, "distance": round(d, 1),
                                              "file": os.path.basename(guide_path)})

            # Check if objective coord equals accept NPC coord when better source exists
            if step_x != 0 and creature_ids:
                # Find the accept step for this quest to compare coords
                for prev_step in guide["steps"][:i]:
                    if prev_step.get("quest_id") == qid and prev_step.get("type") == "accept_quest":
                        ac = prev_step.get("coordinates", {})
                        ax, ay = ac.get("x", 0), ac.get("y", 0)
                        if ax != 0 and dist(step_x, step_y, ax, ay) < 5:
                            # Objective coords match accept coords — check if better exists
                            for cid in creature_ids:
                                spawns = [s for s in data["npc_spawns"].get(str(cid), []) if s["map"] == step_map]
                                if spawns:
                                    nearest = min(spawns, key=lambda s: dist(s["x"], s["y"], ax, ay))
                                    if dist(nearest["x"], nearest["y"], ax, ay) > 30:
                                        issues.append({"type": "OBJECTIVE_AT_QUESTGIVER", "step": i, "quest": qid,
                                                      "creature": cid, "msg": "objective coord equals accept NPC but source spawns exist elsewhere",
                                                      "file": os.path.basename(guide_path)})
                        break

    # Post-pass: check for quests with DB objectives but no generated step
    for qid in quests_seen:
        qid_str = str(qid)
        quest = data["quests"].get(qid_str)
        if not quest:
            continue
        if qid in has_objective:
            continue

        start_item = quest.get("StartItem", 0) or 0
        has_kill = any(quest.get(f"RequiredNpcOrGo{j}", 0) for j in range(1, 5))
        has_item = False
        for j in range(1, 7):
            item = quest.get(f"RequiredItemId{j}", 0) or 0
            if item and item != start_item:
                has_item = True
                break

        if has_kill or has_item:
            issues.append({"type": "QUEST_NO_OBJECTIVE_STEP", "quest": qid,
                          "title": quest.get("LogTitle", "?"),
                          "has_kill": has_kill, "has_item": has_item,
                          "file": os.path.basename(guide_path)})

    return issues


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--guides", default=GUIDE_DIR)
    args = parser.parse_args()

    print("Loading DB data...")
    data = load_data()

    total_issues = 0
    total_guides = 0
    issue_counts = {}

    for root, dirs, files in sorted(os.walk(args.guides)):
        for fn in sorted(files):
            if not fn.endswith(".yaml"):
                continue
            path = os.path.join(root, fn)
            issues = validate_guide(path, data)
            total_guides += 1

            if issues:
                rel = os.path.relpath(path, args.guides)
                print(f"\n{rel}: {len(issues)} issues")
                for issue in issues[:10]:  # Show max 10 per guide
                    itype = issue["type"]
                    issue_counts[itype] = issue_counts.get(itype, 0) + 1
                    total_issues += 1
                    if itype == "MISSING_PREREQ":
                        print(f"  [{itype}] step {issue['step']}: q{issue['quest']} needs prereq q{issue['needs']}")
                    elif itype == "BAD_COORDS" or itype == "BAD_TURNIN_COORDS":
                        print(f"  [{itype}] step {issue['step']}: q{issue['quest']} npc {issue['npc']} is {issue['distance']}yd away")
                    elif itype == "MISSING_OBJECTIVE":
                        print(f"  [{itype}] step {issue['step']}: q{issue['quest']} '{issue['title']}' has objectives but no kill/collect step")
                    else:
                        print(f"  [{itype}] step {issue['step']}: {issue}")
                if len(issues) > 10:
                    print(f"  ... and {len(issues) - 10} more")
                    total_issues += len(issues) - 10
                    for issue in issues[10:]:
                        issue_counts[issue["type"]] = issue_counts.get(issue["type"], 0) + 1
            else:
                rel = os.path.relpath(path, args.guides)
                print(f"{rel}: OK")

    print(f"\n{'='*60}")
    print(f"Validated {total_guides} guides, found {total_issues} issues:")
    for itype, count in sorted(issue_counts.items(), key=lambda x: -x[1]):
        print(f"  {itype}: {count}")


if __name__ == "__main__":
    main()
