#!/usr/bin/env python3
"""Reset the 14-bot fleet to a clean level-1 start to validate the generated
profiles end to end. Runs on the fleet host. For each route char:
  * DB: level=1, xp=0, money kept; delete its character_queststatus +
    character_queststatus_rewarded rows; set position/map to the route's first
    segment coord (racial start area) so it begins the route from the start,
    not wherever it out-levelled to.
  * runner state file: reset done/deaths/level_history/skipped/defer_fails.
Bots MUST be offline when this runs (the runner relaunch logs them back in as a
fresh L1 char at the start). Repair happens via the monitor after launch.
"""
import glob
import json
import os
import subprocess

ROUTES = os.path.expanduser(
    "~/build/azerothcore-zoidberg/modules/mod-autonomous-player/tools/routes_generated")
STATE = os.path.expanduser("~/ap_fleet_state")


def pw():
    e = subprocess.run("docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}'",
                       shell=True, capture_output=True, text=True).stdout
    return next((l.split("=", 1)[1] for l in e.splitlines() if l.startswith("MYSQL_ROOT_PASSWORD=")), "")


def q(sql, PW):
    # A default database MUST be selected: MySQL's multi-table
    # `DELETE cr FROM ... JOIN` form fails with "No database selected"
    # even when every table is fully schema-qualified. Without it the
    # DELETEs silently no-op (only the single-table UPDATE lands), which
    # is exactly how the rewarded/queststatus rows survived a "reset" and
    # deadlocked the fleet at level 1 (every quest "already rewarded").
    r = subprocess.run(
        ["docker", "exec", "ac-database", "mysql", "-uroot", f"-p{PW}",
         "acore_characters", "-e", sql],
        capture_output=True, text=True)
    err = "\n".join(l for l in r.stderr.splitlines()
                    if "Using a password" not in l).strip()
    if r.returncode != 0 or err:
        raise RuntimeError(f"SQL failed (rc={r.returncode}): {err or r.stdout}")


def start_coord(route):
    """First segment's start position: prefer giver_x/y/z, else x/y/z."""
    for s in route["segments"]:
        if "giver_x" in s:
            return s["giver_x"], s["giver_y"], s["giver_z"]
        if "x" in s:
            return s["x"], s["y"], s["z"]
    return None


def main():
    PW = pw()
    for f in sorted(glob.glob(os.path.join(ROUTES, "*.json"))):
        if f.endswith("_generation_summary.json"):
            continue
        route = json.load(open(f))
        char = route["char"]
        mapid = route.get("map", 0)
        sc = start_coord(route)
        if not sc:
            print(f"{char}: no start coord, skipped")
            continue
        x, y, z = sc
        q(f"""UPDATE acore_characters.characters SET level=1, xp=0,
               position_x={x}, position_y={y}, position_z={z}, map={mapid}
               WHERE name='{char}';
             DELETE cq FROM acore_characters.character_queststatus cq
               JOIN acore_characters.characters c ON c.guid=cq.guid WHERE c.name='{char}';
             DELETE cr FROM acore_characters.character_queststatus_rewarded cr
               JOIN acore_characters.characters c ON c.guid=cr.guid WHERE c.name='{char}';""", PW)
        sp = os.path.join(STATE, f"{char}_state.json")
        json.dump({"done": [], "level_history": [], "deaths": 0,
                   "segment_attempts": {}, "skipped": [], "defer_fails": {}},
                  open(sp, "w"), indent=1)
        print(f"reset {char} -> L1 at map {mapid} ({round(x)},{round(y)})")


if __name__ == "__main__":
    main()
