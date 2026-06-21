#!/usr/bin/env python3
"""
zygor_route_to_guide.py — convert a Zygor route JSON (from zygor_parse.py) into an
idlebot strict-mode guide YAML, resolving all coordinates from the AzerothCore world
DB (the same resolution path proven in gen_dbguide.py).

Zygor route steps carry the correct quest ORDER and the npc/item/go ENTRY ids, but no
coords ("runtime resolves from DB by id"). This tool walks the steps in order, maps
each to a guide step type, and resolves coords:
  accept  -> accept_quest        (npc spawn; null npc -> creature_queststarter)
  turnin  -> turn_in_quest       (npc spawn; null npc -> creature_questender)
  kill    -> kill_mobs           (npc spawn; completion quest_objective_complete:q/obj)
  collect -> kill_mobs on dropper(from_npc, else item->creature_loot_template,
             else quest RequiredItemId->dropper); completion quest/obj
  use     -> interact_gameobject IF `go` exists in the gameobject table; else SKIP
             (Zygor "use" is often a bag item, not a world object)
  fpath/hearth -> SKIP (executor has no flight/hearth step; MoveTo covers travel)

Every step is reported on stderr as KEEP/SKIP with the reason so we can confirm we
captured (or deliberately dropped) all of them.

Runs ON the DB host (zoidberg) — shells out to `docker exec ac-database mysql`.

Usage (on zoidberg):
  python3 zygor_route_to_guide.py \
      --route .../data/routes/alliance/zygor_..._hellfire_peninsula.json \
      --id alliance-outland-hellfire --name "Alliance Outland: Hellfire" \
      --level-min 58 --level-max 64 --max-steps 30 \
      --out .../data/guides/alliance/outland/hellfire.yaml
"""
import argparse
import json
import subprocess
import sys
import math

DB = "acore_world"


def cluster_centroid(pts, rad_lo, rad_hi):
    """Densest spawn cluster, not the global average (which lands in dead space or a
    neighbouring camp with a huge radius). Bucket into coarse cells, take the densest,
    expand to spawns within WINDOW, return tight (cx, cy, cz, radius)."""
    CELL = 50.0
    WINDOW = 100.0
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
    return cx, cy, cz, min(max(rad, rad_lo), rad_hi)


def q(sql):
    """Run a -N (no-header) tab-separated query; SQL piped via stdin to docker exec."""
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


def resolve_creature_coords(entries):
    """creature entry -> (map, cx, cy, cz, radius). `creature` stores id1/id2/id3."""
    if not entries:
        return {}
    lst = in_list(entries)
    rows = q(
        "SELECT id1,id2,id3,map,position_x,position_y,position_z FROM "
        f"{DB}.creature WHERE id1 IN ({lst}) OR id2 IN ({lst}) OR id3 IN ({lst})"
    )
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
        mp = max(maps, key=lambda m: len(maps[m]))
        cx, cy, cz, rad = cluster_centroid(maps[mp], 40.0, 130.0)
        out[e] = (mp, cx, cy, cz, rad)
    return out


def resolve_go_coords(entries):
    """gameobject entry -> (map, cx, cy, cz, radius). `gameobject` uses plain `id`."""
    if not entries:
        return {}
    rows = q(
        "SELECT id,map,position_x,position_y,position_z FROM "
        f"{DB}.gameobject WHERE id IN ({in_list(entries)})"
    )
    bymap = {}
    for r in rows:
        try:
            e = int(r[0]); mp = int(r[1])
            x = float(r[2]); y = float(r[3]); z = float(r[4])
        except (ValueError, IndexError):
            continue
        bymap.setdefault(e, {}).setdefault(mp, []).append((x, y, z))
    out = {}
    for e, maps in bymap.items():
        mp = max(maps, key=lambda m: len(maps[m]))
        cx, cy, cz, rad = cluster_centroid(maps[mp], 8.0, 80.0)
        out[e] = (mp, cx, cy, cz, rad)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--route", required=True, help="path to a zygor route JSON")
    ap.add_argument("--id", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--faction", default=None, help="override; else from route")
    ap.add_argument("--level-min", type=int, default=0)
    ap.add_argument("--level-max", type=int, default=0)
    ap.add_argument("--max-steps", type=int, default=0, help="cap route steps (0=all)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    route = json.load(open(a.route))
    rsteps = route.get("steps", [])
    if a.max_steps > 0:
        rsteps = rsteps[:a.max_steps]
    faction = (a.faction or route.get("faction", "")).lower()

    # ---- pass 1: gather every entry/item/quest we must resolve ----
    npc_entries = set()       # creature entries (accept/turnin/kill givers + mobs)
    go_entries = set()        # gameobject entries (use)
    quests_need_starter = set()
    quests_need_ender = set()
    items_need_dropper = set()
    quests_need_reqitem = set()  # collect with item=null -> look up quest RequiredItemId

    for s in rsteps:
        act = s.get("action")
        if act == "accept":
            if s.get("npc"):
                npc_entries.add(int(s["npc"]))
            elif s.get("quest"):
                quests_need_starter.add(int(s["quest"]))
        elif act == "turnin":
            if s.get("npc"):
                npc_entries.add(int(s["npc"]))
            elif s.get("quest"):
                quests_need_ender.add(int(s["quest"]))
        elif act == "kill":
            if s.get("npc"):
                npc_entries.add(int(s["npc"]))
        elif act == "collect":
            if s.get("from_npc"):
                npc_entries.add(int(s["from_npc"]))
            elif s.get("item"):
                items_need_dropper.add(int(s["item"]))
            elif s.get("quest"):
                quests_need_reqitem.add(int(s["quest"]))
        elif act == "use":
            if s.get("go"):
                go_entries.add(int(s["go"]))

    # ---- starters / enders ----
    starter, ender = {}, {}
    if quests_need_starter:
        for r in q(f"SELECT quest,id FROM {DB}.creature_queststarter WHERE quest IN ({in_list(quests_need_starter)})"):
            starter.setdefault(int(r[0]), int(r[1]))
    if quests_need_ender:
        for r in q(f"SELECT quest,id FROM {DB}.creature_questender WHERE quest IN ({in_list(quests_need_ender)})"):
            ender.setdefault(int(r[0]), int(r[1]))
    npc_entries.update(starter.values())
    npc_entries.update(ender.values())

    # ---- quest RequiredItemId for item-less collects ----
    reqitem = {}  # quest -> [itemId,...]
    if quests_need_reqitem:
        rows = q(
            "SELECT ID,RequiredItemId1,RequiredItemId2,RequiredItemId3,"
            "RequiredItemId4,RequiredItemId5,RequiredItemId6 "
            f"FROM {DB}.quest_template WHERE ID IN ({in_list(quests_need_reqitem)})"
        )
        for r in rows:
            try:
                qid = int(r[0])
                items = [int(x) for x in r[1:7] if x and int(x) > 0]
            except (ValueError, IndexError):
                continue
            reqitem[qid] = items
            items_need_dropper.update(items)

    # ---- item -> droppers (top 2 by chance) ----
    droppers = {}
    if items_need_dropper:
        for r in q(
            f"SELECT Item,Entry,Chance FROM {DB}.creature_loot_template "
            f"WHERE Item IN ({in_list(items_need_dropper)}) ORDER BY Chance DESC"
        ):
            it, e = int(r[0]), int(r[1])
            droppers.setdefault(it, [])
            if e not in droppers[it] and len(droppers[it]) < 2:
                droppers[it].append(e)
        for es in droppers.values():
            npc_entries.update(es)

    # ---- resolve all coords ----
    ccoords = resolve_creature_coords(list(npc_entries))
    gcoords = resolve_go_coords(list(go_entries))

    # ---- pass 2: build guide steps in route order ----
    out_steps = []   # list of dicts ready to emit
    kept = skipped = 0
    accept_attempted = set()  # quests we saw an accept step for
    accepted_ok = set()       # quests whose accept step was kept (resolvable giver)

    def report(tag, s, reason=""):
        sys.stderr.write(f"  [{tag}] {s.get('action')} q={s.get('quest')} "
                         f"{s.get('name','')[:40]} {reason}\n")

    for s in rsteps:
        act = s.get("action")
        qid = s.get("quest")
        if act == "accept":
            if qid:
                accept_attempted.add(qid)
            npc = s.get("npc") or starter.get(qid)
            if not npc or int(npc) not in ccoords:
                report("SKIP", s, "accept: no resolvable giver"); skipped += 1; continue
            mp, x, y, z, _ = ccoords[int(npc)]
            out_steps.append(dict(kind="accept_quest", qid=qid, npc=int(npc),
                                  mp=mp, x=x, y=y, z=z, rad=5.0))
            if qid:
                accepted_ok.add(qid)
            report("KEEP", s); kept += 1
        elif act == "turnin":
            npc = s.get("npc") or ender.get(qid)
            if not npc or int(npc) not in ccoords:
                report("SKIP", s, "turnin: no resolvable ender"); skipped += 1; continue
            mp, x, y, z, _ = ccoords[int(npc)]
            out_steps.append(dict(kind="turn_in_quest", qid=qid, npc=int(npc),
                                  mp=mp, x=x, y=y, z=z, rad=5.0))
            report("KEEP", s); kept += 1
        elif act == "kill":
            npc = s.get("npc")
            obj = s.get("obj") or 1
            if not qid:
                # No quest -> no objective to complete against; a quest_objective
                # step with id None is degenerate (the bot loops on it). Skip.
                report("SKIP", s, "kill: no quest (intermediate)"); skipped += 1; continue
            if not npc or int(npc) not in ccoords:
                report("SKIP", s, "kill: mob has no 3.3.5a spawn"); skipped += 1; continue
            mp, x, y, z, rad = ccoords[int(npc)]
            out_steps.append(dict(kind="kill_mobs", qid=qid, ids=[int(npc)],
                                  obj=int(obj), mp=mp, x=x, y=y, z=z,
                                  rad=max(rad, 60.0)))
            report("KEEP", s); kept += 1
        elif act == "collect":
            obj = s.get("obj") or 1
            if not qid:
                report("SKIP", s, "collect: no quest (intermediate item)"); skipped += 1; continue
            ents = []
            if s.get("from_npc") and int(s["from_npc"]) in ccoords:
                ents = [int(s["from_npc"])]
            elif s.get("item"):
                ents = [e for e in droppers.get(int(s["item"]), []) if e in ccoords]
            elif qid and qid in reqitem:
                for it in reqitem[qid]:
                    ents = [e for e in droppers.get(it, []) if e in ccoords]
                    if ents:
                        break
            if not ents:
                report("SKIP", s, "collect: no resolvable dropper"); skipped += 1; continue
            mp, x, y, z, rad = ccoords[ents[0]]
            out_steps.append(dict(kind="kill_mobs", qid=qid, ids=ents,
                                  obj=int(obj), mp=mp, x=x, y=y, z=z,
                                  rad=max(rad, 60.0)))
            report("KEEP", s, f"(droppers {ents})"); kept += 1
        elif act == "use":
            go = s.get("go")
            if not go or int(go) not in gcoords:
                report("SKIP", s, "use: no world gameobject (likely a bag item)"); skipped += 1; continue
            mp, x, y, z, rad = gcoords[int(go)]
            out_steps.append(dict(kind="interact_gameobject", qid=qid, go=int(go),
                                  mp=mp, x=x, y=y, z=z, rad=max(rad, 5.0)))
            report("KEEP", s); kept += 1
        else:  # fpath / hearth / anything else
            report("SKIP", s, f"unhandled action '{act}'"); skipped += 1

    # Coherence: if a quest's accept step was attempted but skipped (giver not
    # resolvable here), drop its objective/turn-in steps too — the bot can't progress
    # or turn in a quest it never accepted; those steps would just stall the watchdog.
    orphan = accept_attempted - accepted_ok
    if orphan:
        keep = []
        for st in out_steps:
            if st["kind"] in ("turn_in_quest", "kill_mobs") and st.get("qid") in orphan:
                sys.stderr.write(f"  [DROP] {st['kind']} q{st.get('qid')} — accept was skipped\n")
                skipped += 1
                kept -= 1
                continue
            keep.append(st)
        out_steps = keep

    if not out_steps:
        sys.exit("no resolvable steps produced")

    # ---- emit YAML ----
    L = []
    L.append("# Auto-generated from a Zygor route by zygor_route_to_guide.py")
    L.append(f"# route={route.get('title','')!r}")
    L.append(f"# steps kept={kept} skipped={skipped} (of {len(rsteps)})")
    L.append(f"id: {a.id}")
    L.append(f'name: "{a.name}"')
    L.append(f"faction: {faction}")
    L.append("race: any")
    L.append("class: any")
    L.append(f"level_min: {a.level_min}")
    L.append(f"level_max: {a.level_max}")
    L.append("steps:")
    for st in out_steps:
        k = st["kind"]
        if k == "accept_quest":
            L.append(f"  - id: q{st['qid']}_accept")
            L.append(f'    name: "Accept quest {st["qid"]}"')
            L.append("    type: accept_quest")
            L.append(f"    quest_id: {st['qid']}")
            L.append(f"    npc_id: {st['npc']}")
            L.append(f"    map_id: {st['mp']}")
            L.append(f"    coordinates: {{ x: {st['x']:.2f}, y: {st['y']:.2f}, z: {st['z']:.2f}, radius: {st['rad']:.1f} }}")
            L.append("    timeout_seconds: 300")
        elif k == "turn_in_quest":
            L.append(f"  - id: q{st['qid']}_turnin")
            L.append(f'    name: "Turn in quest {st["qid"]}"')
            L.append("    type: turn_in_quest")
            L.append(f"    quest_id: {st['qid']}")
            L.append(f"    npc_id: {st['npc']}")
            L.append(f"    map_id: {st['mp']}")
            L.append(f"    coordinates: {{ x: {st['x']:.2f}, y: {st['y']:.2f}, z: {st['z']:.2f}, radius: {st['rad']:.1f} }}")
            L.append("    timeout_seconds: 300")
        elif k == "kill_mobs":
            ids = ",".join(str(e) for e in st["ids"])
            L.append(f"  - id: q{st['qid']}_obj{st['obj']}")
            L.append(f'    name: "Quest {st["qid"]} objective {st["obj"]}"')
            L.append("    type: kill_mobs")
            L.append(f"    quest_id: {st['qid']}")
            L.append(f"    creature_ids: [{ids}]")
            L.append(f"    map_id: {st['mp']}")
            L.append(f"    coordinates: {{ x: {st['x']:.2f}, y: {st['y']:.2f}, z: {st['z']:.2f}, radius: {st['rad']:.1f} }}")
            L.append(f'    completion_condition: "quest_objective_complete:{st["qid"]}/{st["obj"]}"')
            L.append("    timeout_seconds: 900")
            L.append("    retry_count: 3")
        elif k == "interact_gameobject":
            L.append(f"  - id: q{st['qid']}_use{st['go']}")
            L.append(f'    name: "Use gameobject {st["go"]} for quest {st["qid"]}"')
            L.append("    type: interact_gameobject")
            L.append(f"    quest_id: {st['qid']}")
            L.append(f"    gameobject_id: {st['go']}")
            L.append(f"    map_id: {st['mp']}")
            L.append(f"    coordinates: {{ x: {st['x']:.2f}, y: {st['y']:.2f}, z: {st['z']:.2f}, radius: {st['rad']:.1f} }}")
            L.append("    timeout_seconds: 300")
    text = "\n".join(L) + "\n"
    with open(a.out, "w") as f:
        f.write(text)
    sys.stderr.write(f"\nwrote {a.out}: kept={kept} skipped={skipped} "
                     f"(of {len(rsteps)} route steps), {len(out_steps)} guide steps\n")


if __name__ == "__main__":
    main()
