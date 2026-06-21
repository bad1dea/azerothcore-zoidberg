#!/usr/bin/env python3
"""
gen_dbguide.py — generate an idlebot strict guide (YAML) directly from the
AzerothCore world DB for a faction/race over a set of zones (QuestSortIDs).

Zygor's retail product has no usable classic 1-60 routes (Cata-revamped), so for
the WotLK classic starting zones we derive the leveling guide straight from the
server's own quest data: every solo, race-eligible quest in the target zones, in
quest-level order, each emitted as accept -> kill/collect objective(s) -> turn-in
with real world coordinates resolved from `creature` spawns.

Runs ON the DB host; shells out to `docker exec ac-database mysql`. The bot's
executor auto-skips already-completed quests, so the guide can safely be (re)run
from step 0.

Usage (on zoidberg):
  python3 gen_dbguide.py --faction Alliance --race-bit 4 \
      --zones 132,1,38 --min 1 --max 20 \
      --id alliance-dwarf-1-20 --name "Alliance Dwarf 1-20" \
      --race dwarf --out /home/khuong/azerothcore-wotlk/modules/mod-idlebot/data/guides/alliance/dwarf/dwarf_1_20.yaml
"""
import argparse
import subprocess
import sys
import math

DB = "acore_world"


def q(sql):
    """Run a -N (no-header) tab-separated query; SQL is piped via stdin to
    `docker exec -i` so there is zero shell/quoting interaction with the SQL."""
    cmd = (
        'PW=$(docker inspect ac-database --format '
        '"{{range .Config.Env}}{{println .}}{{end}}" | grep -m1 '
        '"MYSQL_ROOT_PASSWORD=" | cut -d= -f2); '
        'docker exec -i ac-database mysql -uroot -p"$PW" -N 2>/dev/null'
    )
    out = subprocess.run(["bash", "-c", cmd], input=sql,
                         capture_output=True, text=True)
    rows = []
    for line in out.stdout.splitlines():
        if line.strip() == "":
            continue
        rows.append(line.split("\t"))
    return rows


def in_list(xs):
    return ",".join(str(x) for x in xs) if xs else "0"


def resolve_coords(entries):
    """entry -> (map, cx, cy, cz, radius) using the creature spawn table.
    `creature` stores up to 3 template ids per row (id1/id2/id3)."""
    if not entries:
        return {}
    lst = in_list(entries)
    rows = q(
        "SELECT id1,id2,id3,map,position_x,position_y,position_z FROM "
        f"{DB}.creature WHERE id1 IN ({lst}) OR id2 IN ({lst}) OR id3 IN ({lst})"
    )
    # entry -> map -> list[(x,y,z)]
    bymap = {}
    eset = set(entries)
    for r in rows:
        try:
            ids = {int(r[0]), int(r[1]), int(r[2])}
            mp = int(r[3]); x = float(r[4]); y = float(r[5]); z = float(r[6])
        except (ValueError, IndexError):
            continue
        for e in ids & eset:
            bymap.setdefault(e, {}).setdefault(mp, []).append((x, y, z))
    out = {}
    for e, maps in bymap.items():
        # pick the map with the most spawns of this entry
        mp = max(maps, key=lambda m: len(maps[m]))
        pts = maps[mp]
        cx = sum(p[0] for p in pts) / len(pts)
        cy = sum(p[1] for p in pts) / len(pts)
        cz = sum(p[2] for p in pts) / len(pts)
        rad = max((math.hypot(p[0] - cx, p[1] - cy) for p in pts), default=0.0)
        out[e] = (mp, cx, cy, cz, min(max(rad, 8.0), 200.0))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--faction", required=True)
    ap.add_argument("--race", required=True)
    ap.add_argument("--race-bit", type=int, required=True)
    ap.add_argument("--zones", required=True, help="comma QuestSortIDs")
    ap.add_argument("--min", type=int, default=1)
    ap.add_argument("--max", type=int, default=20)
    ap.add_argument("--id", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    zones = [int(z) for z in a.zones.split(",")]
    rb = a.race_bit

    # --- Q1: eligible solo quests in the target zones, by level ---
    cols = ("ID,QuestLevel,MinLevel,AllowableRaces,"
            "RequiredNpcOrGo1,RequiredNpcOrGoCount1,RequiredNpcOrGo2,RequiredNpcOrGoCount2,"
            "RequiredNpcOrGo3,RequiredNpcOrGoCount3,RequiredNpcOrGo4,RequiredNpcOrGoCount4,"
            "RequiredItemId1,RequiredItemCount1,RequiredItemId2,RequiredItemCount2,"
            "RequiredItemId3,RequiredItemCount3,RequiredItemId4,RequiredItemCount4,"
            "LogTitle")
    rows = q(
        f"SELECT {cols} FROM {DB}.quest_template "
        f"WHERE QuestSortID IN ({in_list(zones)}) "
        f"AND QuestLevel BETWEEN {a.min} AND {a.max} "
        f"AND (AllowableRaces=0 OR (AllowableRaces & {rb})) "
        f"ORDER BY QuestLevel, ID"
    )

    quests = []
    for r in rows:
        try:
            d = {
                "id": int(r[0]), "level": int(r[1]),
                "npcgo": [(int(r[4]), int(r[5])), (int(r[6]), int(r[7])),
                          (int(r[8]), int(r[9])), (int(r[10]), int(r[11]))],
                "items": [(int(r[12]), int(r[13])), (int(r[14]), int(r[15])),
                          (int(r[16]), int(r[17])), (int(r[18]), int(r[19]))],
                "title": r[20] if len(r) > 20 else "",
            }
        except (ValueError, IndexError):
            continue
        quests.append(d)

    if not quests:
        sys.exit("no eligible quests found")

    qids = [d["id"] for d in quests]

    # --- Q2: starters / enders ---
    starter, ender = {}, {}
    for r in q(f"SELECT quest,id FROM {DB}.creature_queststarter WHERE quest IN ({in_list(qids)})"):
        starter.setdefault(int(r[0]), int(r[1]))
    for r in q(f"SELECT quest,id FROM {DB}.creature_questender WHERE quest IN ({in_list(qids)})"):
        ender.setdefault(int(r[0]), int(r[1]))

    # --- Q3: item -> droppers (top 2 by chance) ---
    need_items = sorted({it for d in quests for (it, c) in d["items"] if it > 0})
    droppers = {}
    if need_items:
        for r in q(
            f"SELECT Item,Entry,Chance FROM {DB}.creature_loot_template "
            f"WHERE Item IN ({in_list(need_items)}) ORDER BY Chance DESC"
        ):
            it, e = int(r[0]), int(r[1])
            droppers.setdefault(it, [])
            if e not in droppers[it] and len(droppers[it]) < 2:
                droppers[it].append(e)

    # --- Q4: resolve coords for every entry we reference ---
    entries = set()
    for d in quests:
        if d["id"] in starter:
            entries.add(starter[d["id"]])
        if d["id"] in ender:
            entries.add(ender[d["id"]])
        for (e, c) in d["npcgo"]:
            if e > 0:
                entries.add(e)
        for (it, c) in d["items"]:
            for e in droppers.get(it, []):
                entries.add(e)
    coords = resolve_coords(list(entries))

    # --- build steps ---
    steps = []
    kept = 0
    for d in quests:
        qid = d["id"]
        s_npc = starter.get(qid)
        e_npc = ender.get(qid)
        if not s_npc or not e_npc:
            continue
        if s_npc not in coords or e_npc not in coords:
            continue  # giver/ender doesn't spawn in 3.3.5a -> skip quest

        obj_steps = []
        for i in range(4):
            entry, cnt = d["npcgo"][i]
            if entry > 0 and entry in coords:           # kill creatures
                mp, cx, cy, cz, rad = coords[entry]
                obj_steps.append(("kill", [entry], cnt, i + 1, mp, cx, cy, cz, max(rad, 60.0)))
            it, icnt = d["items"][i]
            if it > 0:                                   # collect item -> kill droppers
                ents = [e for e in droppers.get(it, []) if e in coords]
                if ents:
                    mp, cx, cy, cz, rad = coords[ents[0]]
                    obj_steps.append(("kill", ents, icnt, i + 1, mp, cx, cy, cz, max(rad, 60.0)))

        smap, sx, sy, sz, _ = coords[s_npc]
        emap, ex, ey, ez, _ = coords[e_npc]
        title = d["title"].replace('"', "'")

        steps.append(("accept", qid, s_npc, smap, sx, sy, sz, title))
        for o in obj_steps:
            steps.append(("kill", qid) + tuple(o))
        steps.append(("turnin", qid, e_npc, emap, ex, ey, ez, title))
        kept += 1

    # --- emit YAML ---
    L = []
    L.append(f"# Auto-generated from acore_world by gen_dbguide.py")
    L.append(f"# zones(QuestSortID)={zones} race={a.race} levels {a.min}-{a.max} quests={kept}")
    L.append(f"id: {a.id}")
    L.append(f'name: "{a.name}"')
    L.append(f"faction: {a.faction.lower()}")
    L.append(f"race: {a.race}")
    L.append("class: any")
    L.append(f"level_min: {a.min}")
    L.append(f"level_max: {a.max}")
    L.append("steps:")
    for st in steps:
        if st[0] == "accept":
            _, qid, npc, mp, x, y, z, title = st
            L.append(f"  - id: q{qid}_accept")
            L.append(f'    name: "Accept {title}"')
            L.append("    type: accept_quest")
            L.append(f"    quest_id: {qid}")
            L.append(f"    npc_id: {npc}")
            L.append(f"    map_id: {mp}")
            L.append(f"    coordinates: {{ x: {x:.2f}, y: {y:.2f}, z: {z:.2f}, radius: 5.0 }}")
            L.append("    timeout_seconds: 300")
        elif st[0] == "kill":
            _, qid, _kind, ents, cnt, obj, mp, x, y, z, rad = st
            ids = ",".join(str(e) for e in ents)
            L.append(f"  - id: q{qid}_obj{obj}")
            L.append(f'    name: "Quest {qid} objective {obj}"')
            L.append("    type: kill_mobs")
            L.append(f"    quest_id: {qid}")
            L.append(f"    creature_ids: [{ids}]")
            L.append(f"    map_id: {mp}")
            L.append(f"    coordinates: {{ x: {x:.2f}, y: {y:.2f}, z: {z:.2f}, radius: {rad:.1f} }}")
            L.append(f'    completion_condition: "quest_objective_complete:{qid}/{obj}"')
            L.append("    timeout_seconds: 900")
            L.append("    retry_count: 3")
        elif st[0] == "turnin":
            _, qid, npc, mp, x, y, z, title = st
            L.append(f"  - id: q{qid}_turnin")
            L.append(f'    name: "Turn in {title}"')
            L.append("    type: turn_in_quest")
            L.append(f"    quest_id: {qid}")
            L.append(f"    npc_id: {npc}")
            L.append(f"    map_id: {mp}")
            L.append(f"    coordinates: {{ x: {x:.2f}, y: {y:.2f}, z: {z:.2f}, radius: 5.0 }}")
            L.append("    timeout_seconds: 300")
    text = "\n".join(L) + "\n"
    with open(a.out, "w") as f:
        f.write(text)
    sys.stderr.write(f"wrote {a.out}: {kept} quests, {len(steps)} steps\n")


if __name__ == "__main__":
    main()
