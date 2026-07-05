#!/usr/bin/env python3
"""Repair kill/grind coordinates in the generated routes to the TRUE densest
spawn camp of each mob/object, from the live DB. Runs on the fleet host (needs
docker access to ac-database).

Root cause it fixes: the coverage snapshot under-samples spawns, so the
generator's density pick often landed on a lone straggler near the giver while
the real camp sat 100+yd away -> guide selection found candidates=0 -> the kill
quest/grind stalled -> (with skip-after-1 + cascade) it took whole quest chains
down. Snapping every kill_entries[]/go_entries[]/grind_to_level coord to the DB's
densest same-entry cluster makes candidates>0, so quests and grinds actually
complete instead of stalling.

Givers and turn-in coords are NOT touched (they're the authored NPC positions).
Only mob/object hotspots move. Idempotent.
"""
import glob
import json
import os
import re
import subprocess

ROUTES = os.path.expanduser(
    "~/build/azerothcore-zoidberg/modules/mod-autonomous-player/tools/routes_generated")


def dbpw():
    env = subprocess.run("docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}'",
                         shell=True, capture_output=True, text=True).stdout
    for line in env.splitlines():
        if line.startswith("MYSQL_ROOT_PASSWORD="):
            return line.split("=", 1)[1]
    return ""


PW = dbpw()


def q(sql):
    out = subprocess.run(["docker", "exec", "ac-database", "mysql", "-uroot",
                          f"-p{PW}", "-N", "-e", sql], capture_output=True, text=True).stdout
    return out.strip()


_cache = {}


def densest(table, id_col, entry, mapid):
    """Return (x,y,z) of the densest same-entry cluster, or None."""
    key = (table, entry, mapid)
    if key in _cache:
        return _cache[key]
    sql = (f"SELECT position_x,position_y,position_z FROM acore_world.{table} t "
           f"WHERE t.{id_col}={entry} AND t.map={mapid} "
           f"ORDER BY (SELECT COUNT(*) FROM acore_world.{table} c2 WHERE c2.{id_col}={entry} "
           f"AND c2.map={mapid} AND SQRT(POW(c2.position_x-t.position_x,2)+"
           f"POW(c2.position_y-t.position_y,2))<40) DESC LIMIT 1;")
    row = q(sql)
    res = None
    if row:
        parts = row.split("\t")
        if len(parts) == 3:
            res = tuple(round(float(v), 1) for v in parts)
    _cache[key] = res
    return res


def repair(route_path):
    d = json.load(open(route_path))
    mapid = d.get("map", 0)
    changed = 0
    for s in d.get("segments", []):
        t = s.get("type")
        # gameobject collections: go_entries are objects
        for e in s.get("go_entries", []):
            pt = densest("gameobject", "id", e["entry"], mapid)
            if pt and (abs(pt[0] - e["x"]) + abs(pt[1] - e["y"])) > 5:
                e["x"], e["y"], e["z"] = pt
                changed += 1
        # kill/collection: kill_entries are creatures
        for e in s.get("kill_entries", []):
            pt = densest("creature", "id1", e["entry"], mapid)
            if pt and (abs(pt[0] - e["x"]) + abs(pt[1] - e["y"])) > 5:
                e["x"], e["y"], e["z"] = pt
                changed += 1
        # keep the segment's primary x/y/z aligned with its first kill/go entry
        first = (s.get("kill_entries") or s.get("go_entries") or [None])[0]
        if first and s.get("type") in ("quest_grind", "quest_gameobject"):
            s["x"], s["y"], s["z"] = first["x"], first["y"], first["z"]
            if "kill_entry" in s:
                s["kill_entry"] = first["entry"]
        # grind_to_level: snap the grind mob to its densest camp
        if t == "grind_to_level" and "entry" in s:
            pt = densest("creature", "id1", s["entry"], mapid)
            if pt and (abs(pt[0] - s["x"]) + abs(pt[1] - s["y"])) > 5:
                s["x"], s["y"], s["z"] = pt
                changed += 1
    json.dump(d, open(route_path, "w"), indent=2)
    return changed


def main():
    total = 0
    for f in sorted(glob.glob(os.path.join(ROUTES, "*.json"))):
        if f.endswith("_generation_summary.json"):
            continue
        c = repair(f)
        total += c
        print(f"{os.path.basename(f):42s} coords repaired: {c}")
    print(f"TOTAL coords repaired: {total}")


if __name__ == "__main__":
    main()
