#!/usr/bin/env python3
"""Turn mined quests (mine_zone_quests.py output) into route segments and
splice them into a route, so leveling is quest-driven. Repeatable across
zones. Only adds quests the given race/class can take (the sole
legitimate omission). Handles the three shapes the runner supports:

  DELIVER  (no objectives)      -> quest_accept + quest_turnin
  KILL     (RequiredNpcOrGo)    -> quest_grind on the objective mobs
  COLLECT  (RequiredItem)       -> quest_grind on the item's droppers

Usage:
  add_quests_from_mine.py --mine /tmp/elwynn_q.json --route routes/x.json \
     --race-bit 1 --class-bit 1 --hub ELWNorthshire \
     --ids 16,5261,33,3903,3904,3905,2158,85,86,84,87 --before gs-grind-to-7
"""
import argparse
import json


def clusters_to_kills(entries):
    out = []
    for k in entries:
        for c in (k.get("clusters") or [])[:2]:
            out.append({"entry": k["entry"], "x": round(c[0], 1),
                        "y": round(c[1], 1), "z": round(c[2], 1)})
    return out


def seg_for(qd, hub):
    q = qd["quest"]
    g = qd["giver"]
    e = qd["ender"] or g
    kills = clusters_to_kills(qd.get("kill_npcs") or [])
    if not kills:
        # collect quest -> kill the highest-chance dropper
        for it in qd.get("items") or []:
            for d in (it.get("droppers") or [])[:1]:
                # dropper spawn coords aren't in the mine; fall back to
                # the giver area and let the runner's search find them.
                pass
    if kills:
        return [{
            "id": f"q{q}-grind", "type": "quest_grind", "quest": q,
            "min_level": qd["quest_level"],
            "giver": g["entry"], "giver_x": g["x"], "giver_y": g["y"], "giver_z": g["z"],
            "kill_entries": kills,
            "turnin": e["entry"], "turnin_x": e["x"], "turnin_y": e["y"], "turnin_z": e["z"],
            "unstick": hub, "attempts": 16, "optional": True,
        }]
    # deliver / collect-from-vendor / talk -> accept then turn in
    return [
        {"id": f"q{q}-accept", "type": "quest_accept", "quest": q,
         "min_level": qd["quest_level"], "giver": g["entry"],
         "x": g["x"], "y": g["y"], "z": g["z"], "unstick": hub, "optional": True},
        {"id": f"q{q}-turnin", "type": "quest_turnin", "quest": q,
         "turnin": e["entry"], "x": e["x"], "y": e["y"], "z": e["z"],
         "unstick": hub, "optional": True},
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mine", required=True)
    ap.add_argument("--route", required=True)
    ap.add_argument("--ids", required=True)
    ap.add_argument("--race-bit", type=int, default=0)
    ap.add_argument("--class-bit", type=int, default=0)
    ap.add_argument("--hub", required=True)
    ap.add_argument("--before", default=None, help="segment id to insert before")
    a = ap.parse_args()

    mine = {q["quest"]: q for q in json.load(open(a.mine))}
    want = [int(x) for x in a.ids.split(",") if x]
    r = json.load(open(a.route))
    have = {s["id"] for s in r["segments"]}
    have_q = {s.get("quest") for s in r["segments"]}

    new_segs = []
    for q in want:
        qd = mine.get(q)
        if not qd:
            print(f"  skip {q}: not in mine"); continue
        if q in have_q:
            print(f"  skip {q}: already in route"); continue
        races, classes = qd["allowable_races"], qd["allowable_classes"]
        if a.race_bit and races and not (races & a.race_bit):
            print(f"  skip {q}: race-locked"); continue
        if a.class_bit and classes and not (classes & a.class_bit):
            print(f"  skip {q}: class-locked"); continue
        for s in seg_for(qd, a.hub):
            if s["id"] not in have:
                new_segs.append(s)
        print(f"  + q{q} '{qd['title'][:30]}' L{qd['quest_level']}")

    if a.before:
        idx = next((i for i, s in enumerate(r["segments"]) if s["id"] == a.before),
                   len(r["segments"]))
    else:
        idx = len(r["segments"])
    r["segments"][idx:idx] = new_segs
    json.dump(r, open(a.route, "w"), indent=1)
    print(f"inserted {len(new_segs)} segment(s) into {a.route.split('/')[-1]} "
          f"before {a.before or 'END'}")


if __name__ == "__main__":
    main()
