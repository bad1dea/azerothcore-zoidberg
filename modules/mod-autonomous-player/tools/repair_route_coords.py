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


def all_spawns(table, id_col, entry, mapid):
    key = (table, entry, mapid)
    if key in _cache:
        return _cache[key]
    rows = q(f"SELECT position_x,position_y,position_z FROM acore_world.{table} "
             f"WHERE {id_col}={entry} AND map={mapid};")
    pts = []
    for r in rows.splitlines():
        p = r.split("\t")
        if len(p) == 3:
            pts.append(tuple(round(float(v), 1) for v in p))
    _cache[key] = pts
    return pts


def _cluster_size(pts, c):
    return sum(1 for p in pts if (p[0] - c[0]) ** 2 + (p[1] - c[1]) ** 2 <= 40 * 40)


def densest(table, id_col, entry, mapid):
    """Densest cluster centre, or None."""
    pts = all_spawns(table, id_col, entry, mapid)
    if not pts:
        return None
    return max(pts, key=lambda c: _cluster_size(pts, c))


_camp_cache = {}


def best_camp(mapid, lvl_lo, lvl_hi, bounds):
    """Densest rank-0 loot-dropping creature camp WITHIN the route's zone bounds
    (map 0 holds multiple zones -- an unbounded search would drag a Tirisfal bot
    to a denser Elwynn camp across the continent) whose maxlevel is in
    [lvl_lo, lvl_hi]. Loot-dropping keeps it to real killable mobs. Returns
    (entry,x,y,z) or None."""
    xmin, xmax, ymin, ymax = bounds
    key = (mapid, lvl_lo, lvl_hi, round(xmin), round(xmax), round(ymin), round(ymax))
    if key in _camp_cache:
        return _camp_cache[key]
    sql = (
        "SELECT c.id1, c.position_x, c.position_y, c.position_z, "
        "(SELECT COUNT(*) FROM acore_world.creature c2 WHERE c2.id1=c.id1 AND c2.map=c.map "
        " AND SQRT(POW(c2.position_x-c.position_x,2)+POW(c2.position_y-c.position_y,2))<40) dens "
        "FROM acore_world.creature c JOIN acore_world.creature_template ct ON ct.entry=c.id1 "
        f"WHERE c.map={mapid} AND ct.rank=0 AND ct.maxlevel BETWEEN {lvl_lo} AND {lvl_hi} "
        f"AND c.position_x BETWEEN {xmin} AND {xmax} AND c.position_y BETWEEN {ymin} AND {ymax} "
        "AND EXISTS (SELECT 1 FROM acore_world.creature_loot_template lt WHERE lt.Entry=c.id1) "
        "AND ct.unit_flags & 0x2 = 0 "  # not non-attackable
        "ORDER BY dens DESC LIMIT 1;")
    row = q(sql)
    res = None
    if row:
        p = row.split("\t")
        if len(p) >= 4:
            res = (int(p[0]), round(float(p[1]), 1), round(float(p[2]), 1), round(float(p[3]), 1))
    _camp_cache[key] = res
    return res


def roam_points(table, id_col, entry, mapid, want=4):
    """For a spread-thin mob (densest cluster < 4), return up to `want`
    well-separated spawn points forming a roam circuit, so the grind can move
    between scattered spawns instead of camping one killed-out spot. For a real
    camp (densest cluster >= 4) just return the single densest centre."""
    pts = all_spawns(table, id_col, entry, mapid)
    if not pts:
        return []
    best = max(pts, key=lambda c: _cluster_size(pts, c))
    if _cluster_size(pts, best) >= 4:
        return [best]
    # greedy farthest-point spread starting from the densest
    chosen = [best]
    while len(chosen) < want and len(chosen) < len(pts):
        far = max(pts, key=lambda p: min((p[0]-c[0])**2 + (p[1]-c[1])**2 for c in chosen))
        if min((far[0]-c[0])**2 + (far[1]-c[1])**2 for c in chosen) < 30*30:
            break
        chosen.append(far)
    return chosen


def repair(route_path):
    d = json.load(open(route_path))
    mapid = d.get("map", 0)
    changed = 0
    # Zone bounding box from RELIABLE in-zone anchors (givers/turn-ins/kill
    # hotspots) -- NOT grind_to_level coords, which a prior unbounded run may
    # have corrupted with a cross-zone camp. Pad 300yd. Constrains grind-camp
    # search to this route's zone (map 0 holds Tirisfal AND Elwynn, etc.).
    xs, ys = [], []
    for s in d.get("segments", []):
        for k in ("giver_x", "turnin_x", "x"):
            if s.get("type") != "grind_to_level" and k in s:
                xs.append(s[k])
        for k in ("giver_y", "turnin_y", "y"):
            if s.get("type") != "grind_to_level" and k in s:
                ys.append(s[k])
        for e in (s.get("kill_entries") or []) + (s.get("go_entries") or []):
            xs.append(e["x"]); ys.append(e["y"])
    bounds = (min(xs) - 300, max(xs) + 300, min(ys) - 300, max(ys) + 300) if xs else (-99999, 99999, -99999, 99999)
    for s in d.get("segments", []):
        t = s.get("type")
        # gameobject collections: go_entries are objects
        for e in s.get("go_entries", []):
            pt = densest("gameobject", "id", e["entry"], mapid)
            if pt and (abs(pt[0] - e["x"]) + abs(pt[1] - e["y"])) > 5:
                e["x"], e["y"], e["z"] = pt
                changed += 1
        # kill/collection: expand each creature objective into its dense camp OR,
        # for a spread-thin mob, a multi-point roam circuit (so the grind moves
        # between scattered spawns instead of camping one killed-out spot).
        if s.get("kill_entries"):
            # unique entries (idempotent: re-running won't multiply roam points)
            entries = []
            for e in s["kill_entries"]:
                if e["entry"] not in entries:
                    entries.append(e["entry"])
            orig = {e["entry"]: e for e in s["kill_entries"]}
            new_kes = []
            for entry in entries:
                pts = roam_points("creature", "id1", entry, mapid)
                if not pts:
                    new_kes.append(orig[entry])
                    continue
                for pt in pts:
                    new_kes.append({"entry": entry, "x": pt[0], "y": pt[1], "z": pt[2]})
                changed += 1
            s["kill_entries"] = new_kes
        # keep the segment's primary x/y/z aligned with its first kill/go entry
        first = (s.get("kill_entries") or s.get("go_entries") or [None])[0]
        if first and s.get("type") in ("quest_grind", "quest_gameobject"):
            s["x"], s["y"], s["z"] = first["x"], first["y"], first["z"]
            if "kill_entry" in s:
                s["kill_entry"] = first["entry"]
        # grind_to_level: the tier MUST use a genuinely dense camp at the right
        # level -- grinding is the backstop when quests are sparse, so it can't
        # itself sit on a spread-thin mob (found live: Tirisfal's grind-to-6 mob
        # Ragged Scavenger has only ~3 in its densest cluster -> candidates=0 ->
        # bots stuck at L4 unable to quest OR grind). Pick the densest rank-0
        # loot-dropping creature whose maxlevel sits within [tier-3, tier+1];
        # fall back to snapping the chosen mob if the query finds nothing.
        if t == "grind_to_level" and "entry" in s:
            tier = s.get("level", 6)
            cam = best_camp(mapid, max(1, tier - 3), tier + 1, bounds)
            if cam:
                e2, x2, y2, z2 = cam
                if e2 != s["entry"] or (abs(x2 - s["x"]) + abs(y2 - s["y"])) > 5:
                    s["entry"], s["x"], s["y"], s["z"] = e2, x2, y2, z2
                    changed += 1
            else:
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
