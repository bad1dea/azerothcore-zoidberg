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
import json
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


def cluster_centroid(pts):
    """Pick the DENSEST spawn cluster, not the global average. A mob spread across
    a whole zone has a global centroid that lands in dead space (or in a different
    camp's territory), giving a huge radius the bot wanders inside — straying into
    unrelated hostiles and rarely finding the target. Instead: bucket spawns into
    coarse grid cells, take the densest cell, expand to spawns within WINDOW of it,
    and return that tight centroid + radius. Returns (cx, cy, cz, radius)."""
    CELL = 50.0      # grid bucket size (yards)
    WINDOW = 100.0   # gather spawns within this of the densest cell center
    cells = {}
    for (x, y, z) in pts:
        cells.setdefault((int(x // CELL), int(y // CELL)), []).append((x, y, z))
    dense = max(cells.values(), key=len)
    bx = sum(p[0] for p in dense) / len(dense)
    by = sum(p[1] for p in dense) / len(dense)
    near = [p for p in pts if math.hypot(p[0] - bx, p[1] - by) <= WINDOW] or dense
    cx = sum(p[0] for p in near) / len(near)
    cy = sum(p[1] for p in near) / len(near)
    cz = sum(p[2] for p in near) / len(near)
    rad = max((math.hypot(p[0] - cx, p[1] - cy) for p in near), default=0.0)
    # Tight radius: enough to roam the cluster, not so wide it reaches other camps.
    return cx, cy, cz, min(max(rad, 40.0), 130.0)


def resolve_coords(entries):
    """entry -> { mapid: (count, mapid, cx, cy, cz, radius) } for every map the entry
    spawns on. Keeping ALL maps (not just the busiest) lets the caller pin an objective
    to the quest GIVER's continent — critical because many creatures spawn on both
    continents, and a global "busiest map" pick produces cross-continent steps the bot
    can't path to (MoveTo can't cross maps). `creature` stores id1/id2/id3 per row."""
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
        out[e] = {}
        for mp, pts in maps.items():
            cx, cy, cz, rad = cluster_centroid(pts)
            out[e][mp] = (len(pts), mp, cx, cy, cz, rad)
    return out


def resolve_go_coords(entries):
    """Like resolve_coords() but for GAMEOBJECTS. A quest's RequiredNpcOrGo is
    NEGATIVE when the objective target is a gameobject (abs value = GO entry) —
    e.g. "use Marla's Grave" (q6395). Spawns live in the `gameobject` table keyed
    by `id` (the GO entry). Returns the same { entry: { mapid: (count,mapid,cx,cy,cz,radius) } }
    shape so the build loop treats GO and creature objectives identically."""
    if not entries:
        return {}
    lst = in_list(entries)
    rows = q(
        "SELECT id,map,position_x,position_y,position_z FROM "
        f"{DB}.gameobject WHERE id IN ({lst})"
    )
    bymap = {}
    eset = set(entries)
    for r in rows:
        try:
            e = int(r[0]); mp = int(r[1])
            x = float(r[2]); y = float(r[3]); z = float(r[4])
        except (ValueError, IndexError):
            continue
        if e in eset:
            bymap.setdefault(e, {}).setdefault(mp, []).append((x, y, z))
    out = {}
    for e, maps in bymap.items():
        out[e] = {}
        for mp, pts in maps.items():
            cx, cy, cz, rad = cluster_centroid(pts)
            out[e][mp] = (len(pts), mp, cx, cy, cz, rad)
    return out


def busiest_map(coords, entry):
    """The map where `entry` has the most spawns (for resolving a quest GIVER)."""
    if entry not in coords or not coords[entry]:
        return None
    return max(coords[entry], key=lambda m: coords[entry][m][0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--faction", required=True)
    ap.add_argument("--race", required=True)
    ap.add_argument("--race-bit", type=int, required=True)
    ap.add_argument("--zones", default="", help="comma QuestSortIDs (DB mode)")
    ap.add_argument("--zygor-route", default="",
                    help="path to a parsed Zygor route JSON: take the quest LIST + ORDER "
                         "from Zygor (leveling sequence) and resolve all details from the "
                         "DB. Overrides --zones/--min/--max ordering.")
    ap.add_argument("--min", type=int, default=1)
    ap.add_argument("--max", type=int, default=20)
    ap.add_argument("--id", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--map", type=int, default=-1,
                    help="lock the guide to this continent map (0=EK, 1=Kalimdor); "
                         "skip quests whose giver doesn't spawn there")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    rb = a.race_bit

    # Zygor mode: the quest list + order come from a parsed Zygor leveling route
    # (the curated levelling sequence). DB mode: all quests in the given zones, by level.
    zygor_order = []   # quest ids in Zygor first-accept order
    if a.zygor_route:
        route = json.load(open(a.zygor_route))
        seen = set()
        for s in route.get("steps", []):
            if s.get("action") == "accept" and s.get("quest"):
                qid = int(s["quest"])
                if qid not in seen:
                    seen.add(qid)
                    zygor_order.append(qid)
        if not zygor_order:
            sys.exit("zygor route has no accept steps")
        where = (f"ID IN ({in_list(zygor_order)}) "
                 f"AND (AllowableRaces=0 OR (AllowableRaces & {rb}))")
    else:
        zones = [int(z) for z in a.zones.split(",") if z]
        where = (f"QuestSortID IN ({in_list(zones)}) "
                 f"AND QuestLevel BETWEEN {a.min} AND {a.max} "
                 f"AND (AllowableRaces=0 OR (AllowableRaces & {rb}))")

    # --- Q1: quest objective data ---
    cols = ("ID,QuestLevel,MinLevel,AllowableRaces,"
            "RequiredNpcOrGo1,RequiredNpcOrGoCount1,RequiredNpcOrGo2,RequiredNpcOrGoCount2,"
            "RequiredNpcOrGo3,RequiredNpcOrGoCount3,RequiredNpcOrGo4,RequiredNpcOrGoCount4,"
            "RequiredItemId1,RequiredItemCount1,RequiredItemId2,RequiredItemCount2,"
            "RequiredItemId3,RequiredItemCount3,RequiredItemId4,RequiredItemCount4,"
            "LogTitle")
    rows = q(f"SELECT {cols} FROM {DB}.quest_template WHERE {where} ORDER BY QuestLevel, ID")

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
                "prev": 0,   # filled from quest_template_addon below
            }
        except (ValueError, IndexError):
            continue
        quests.append(d)

    if not quests:
        sys.exit("no eligible quests found")

    # PrevQuestID lives in quest_template_addon in this fork (not quest_template).
    allids = [d["id"] for d in quests]
    prevmap = {}
    for r in q(f"SELECT ID,PrevQuestID FROM {DB}.quest_template_addon WHERE ID IN ({in_list(allids)})"):
        try:
            prevmap[int(r[0])] = abs(int(r[1]))
        except (ValueError, IndexError):
            continue
    for d in quests:
        d["prev"] = prevmap.get(d["id"], 0)

    byid = {d["id"]: d for d in quests}
    if a.zygor_route:
        # Zygor's curated leveling order is authoritative (it already weaves prereq
        # chains, zone hops and level pacing). Just follow it.
        quests = [byid[qid] for qid in zygor_order if qid in byid]
    else:
        # --- Prereq-aware order (topological) ---
        # A pure QuestLevel sort breaks quest chains: a follow-up can land before its
        # prerequisite, so the bot "cannot accept (unmet prereq)", skips it, never
        # returns (lost quest + XP). Order so a quest with a PrevQuestID in THIS guide
        # comes after it (Kahn's algorithm, (level, id) tiebreak to stay ~level-ordered).
        import heapq
        qset = {d["id"] for d in quests}
        indeg = {d["id"]: (1 if (d["prev"] and d["prev"] in qset) else 0) for d in quests}
        children = {}
        for d in quests:
            if d["prev"] and d["prev"] in qset:
                children.setdefault(d["prev"], []).append(d["id"])
        ready = [(d["level"], d["id"]) for d in quests if indeg[d["id"]] == 0]
        heapq.heapify(ready)
        ordered = []
        while ready:
            lvl, qid = heapq.heappop(ready)
            ordered.append(byid[qid])
            for c in children.get(qid, []):
                indeg[c] -= 1
                if indeg[c] == 0:
                    heapq.heappush(ready, (byid[c]["level"], c))
        if len(ordered) < len(quests):   # cycles / orphaned prereqs -> append by level
            done = {d["id"] for d in ordered}
            for d in sorted(quests, key=lambda x: (x["level"], x["id"])):
                if d["id"] not in done:
                    ordered.append(d)
        quests = ordered

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
    # Creatures (givers, enders, kill targets, item droppers) and gameobjects
    # (negative RequiredNpcOrGo objectives) are resolved separately: a GO entry
    # can numerically collide with a creature entry, so they get distinct maps.
    entries = set()
    go_entries = set()
    for d in quests:
        if d["id"] in starter:
            entries.add(starter[d["id"]])
        if d["id"] in ender:
            entries.add(ender[d["id"]])
        for (e, c) in d["npcgo"]:
            if e > 0:
                entries.add(e)
            elif e < 0:
                go_entries.add(-e)              # negative => gameobject objective
        for (it, c) in d["items"]:
            for e in droppers.get(it, []):
                entries.add(e)
    coords = resolve_coords(list(entries))
    go_coords = resolve_go_coords(list(go_entries))

    # --- build steps (giver-map constrained) ---
    steps = []
    kept = 0
    skipped_xmap = 0
    for d in quests:
        qid = d["id"]
        s_npc = starter.get(qid)
        e_npc = ender.get(qid)
        if not s_npc or not e_npc:
            continue
        # The quest's continent is where its GIVER spawns. Everything else (objectives,
        # turn-in) must be on that same map, or the bot — which can only MoveTo within a
        # map — wedges trying to cross. Resolve all coords on `qmap` and skip the quest if
        # any required part can't be placed there (it's a dungeon/cross-continent quest
        # the bot can't complete solo on foot).
        # Continent: locked by --map if given (skip quests whose giver isn't there),
        # else the giver's busiest map.
        if a.map >= 0:
            qmap = a.map if s_npc in coords and a.map in coords[s_npc] else None
        else:
            qmap = busiest_map(coords, s_npc)
        if qmap is None or e_npc not in coords or qmap not in coords[e_npc]:
            if a.map >= 0:
                skipped_xmap += 1
            continue

        # Does the quest require objectives at all? (kill creature, use gameobject,
        # or collect item). A negative RequiredNpcOrGo is a gameobject objective.
        has_obj = (any(e != 0 for (e, c) in d["npcgo"]) or
                   any(it > 0 for (it, c) in d["items"]))

        obj_steps = []
        cross = False
        for i in range(4):
            entry, cnt = d["npcgo"][i]
            if entry > 0:                                # kill creatures
                if entry in coords and qmap in coords[entry]:
                    _, mp, cx, cy, cz, rad = coords[entry][qmap]
                    obj_steps.append(("kill", [entry], cnt, i + 1, mp, cx, cy, cz, max(rad, 60.0)))
                else:
                    cross = True                         # objective mob not on the giver's map
            elif entry < 0:                              # use a gameobject (q6395-style)
                goid = -entry
                if goid in go_coords and qmap in go_coords[goid]:
                    _, mp, cx, cy, cz, rad = go_coords[goid][qmap]
                    # GO spawns are usually a single exact point; keep the search
                    # radius modest but findable on arrival.
                    obj_steps.append(("useobject", goid, cnt, i + 1, mp, cx, cy, cz,
                                      min(max(rad, 20.0), 60.0)))
                else:
                    cross = True                         # GO not on the giver's map
            it, icnt = d["items"][i]
            if it > 0:                                   # collect item -> kill droppers on qmap
                ents = [e for e in droppers.get(it, []) if e in coords and qmap in coords[e]]
                if ents:
                    _, mp, cx, cy, cz, rad = coords[ents[0]][qmap]
                    obj_steps.append(("kill", ents, icnt, i + 1, mp, cx, cy, cz, max(rad, 60.0)))
                else:
                    cross = True                         # no dropper on the giver's map

        # A quest that needs objectives but none resolve on the giver's continent is
        # undoable on foot (dungeon / other continent) -> drop the whole quest.
        if has_obj and not obj_steps:
            skipped_xmap += 1
            continue
        if cross and not obj_steps:
            skipped_xmap += 1
            continue

        _, smap, sx, sy, sz, _ = coords[s_npc][qmap]
        _, emap, ex, ey, ez, _ = coords[e_npc][qmap]
        title = d["title"].replace('"', "'")

        steps.append(("accept", qid, s_npc, smap, sx, sy, sz, title))
        for o in obj_steps:
            if o[0] == "useobject":
                steps.append(("useobject", qid) + tuple(o))
            else:
                steps.append(("kill", qid) + tuple(o))
        steps.append(("turnin", qid, e_npc, emap, ex, ey, ez, title))
        kept += 1

    sys.stderr.write(f"skipped {skipped_xmap} cross-continent/instance quests\n")

    # --- emit YAML ---
    L = []
    L.append(f"# Auto-generated from acore_world by gen_dbguide.py")
    src = f"zygor-route {a.zygor_route!r}" if a.zygor_route else f"zones {a.zones}"
    L.append(f"# source={src} race={a.race} quests={kept} (DB-validated + coord-resolved)")
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
        elif st[0] == "useobject":
            _, qid, _kind, goid, cnt, obj, mp, x, y, z, rad = st
            L.append(f"  - id: q{qid}_obj{obj}")
            L.append(f'    name: "Quest {qid} use gameobject {goid}"')
            L.append("    type: interact_gameobject")
            L.append(f"    quest_id: {qid}")
            L.append(f"    gameobject_id: {goid}")
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
