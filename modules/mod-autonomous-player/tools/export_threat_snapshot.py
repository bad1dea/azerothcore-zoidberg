#!/usr/bin/env python3
"""Export a hostile-spawn snapshot for the runner's threat-aware transit
planner (safe_path.py).

Threat model, deliberately conservative: every creature spawn on the fleet's
maps whose template level fits the leveling band is a potential threat UNLESS
it is provably service/friendly. "Provably friendly" = its faction is shared
with an NPC the routes interact with (givers/enders/vendors/trainers -- the
bot demonstrably stands next to those without being attacked) or it carries
any npcflag (vendor/trainer/gossip/quest -- service NPCs). Faction hostility
proper lives in FactionTemplate.dbc, which the SQL DB does not carry; this
proxy over-avoids some neutral wildlife, which only costs a slightly longer
detour. The planner's level-aware radii already zero out mobs trivially below
the bot.

Run on a host with docker access to ac-database:
    python3 export_threat_snapshot.py > threat_spawns.json
"""
import json
import subprocess
import sys

# Union of giver/turnin/vendor/trainer/repair entries across all committed
# routes (routes/ + routes_generated/) -- see the generating snippet in the
# session that introduced this file. Regenerate with the same sweep if routes
# gain new families.
FRIENDLY_ENTRIES = (
    "151,152,196,197,198,240,241,244,246,247,248,251,252,253,255,261,269,270,"
    "278,279,295,658,711,713,714,786,823,829,836,911,912,913,944,1240,1252,"
    "1374,1375,1376,1377,1378,1427,1428,1429,1431,1432,1495,1496,1498,1499,"
    "1500,1515,1518,1568,1569,1570,1652,1661,1872,1965,2115,2118,2122,2123,"
    "2129,2130,2211,2908,2947,2948,2980,2981,2985,2987,2988,2991,2993,3052,"
    "3055,3059,3060,3062,3066,3075,3076,3139,3142,3143,3145,3147,3153,3154,"
    "3156,3158,3164,3169,3186,3188,3193,3194,3208,3209,3233,3287,3304,3336,"
    "3337,3338,3429,3441,3488,5688,5888,5891,6467,6747,6774,6775,6782,6784,"
    "6786,6806,6928,9296,10176,11378,12738,15278,15280,15281,15287,15295,"
    "15296,15297,15301,15397,15401,15402,15405,15416,15417,15418,15433,15513,"
    "15920,15921,15941,15942,15945,16144,16210,16259,16924,17849,23618,180918"
)

MAPS = (0, 1, 530)
MAX_LEVEL = 14


def q(sql: str) -> str:
    dbpw = subprocess.run(
        ["docker", "inspect", "ac-database",
         "--format", "{{range .Config.Env}}{{println .}}{{end}}"],
        capture_output=True, text=True).stdout
    dbpw = [l.split("=", 1)[1] for l in dbpw.splitlines()
            if l.startswith("MYSQL_ROOT_PASSWORD=")][0]
    r = subprocess.run(
        ["docker", "exec", "ac-database", "mysql", "-uroot", f"-p{dbpw}",
         "-N", "-B", "-e", sql], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        raise SystemExit(1)
    return r.stdout


def main() -> int:
    # AGGRESSIVE factions only (FactionTemplate.dbc enemyGroup with any
    # player bit -- parsed by the sibling aggressive_factions.json build
    # step): starter zones are full of YELLOW mobs (boars, plainstriders,
    # young wolves) that never attack first, and treating them as threats
    # turned every level-1 commute into a long weave around harmless
    # wildlife. Red mobs proximity-aggro; yellow mobs cost nothing to
    # walk past. Verified against known mobs 8/8 (Defias/wolves/bears/
    # Razormane red; boars/plainstriders/ragged wolves yellow).
    import os
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "aggressive_factions.json")) as fh:
        aggressive = ",".join(str(x) for x in json.load(fh))
    out = {}
    for map_id in MAPS:
        rows = q(
            "SELECT ROUND(c.position_x,1), ROUND(c.position_y,1),"
            " ROUND(c.position_z,1), ct.maxlevel"
            " FROM acore_world.creature c"
            " JOIN acore_world.creature_template ct ON ct.entry = c.id1"
            f" WHERE c.map = {map_id} AND ct.maxlevel <= {MAX_LEVEL}"
            " AND ct.npcflag = 0"
            f" AND ct.faction IN ({aggressive});")
        spawns = []
        for line in rows.splitlines():
            parts = line.split("\t")
            if len(parts) == 4:
                spawns.append([float(parts[0]), float(parts[1]),
                               float(parts[2]), int(parts[3])])
        out[str(map_id)] = spawns
        sys.stderr.write(f"map {map_id}: {len(spawns)} threat spawns\n")
    json.dump(out, sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
