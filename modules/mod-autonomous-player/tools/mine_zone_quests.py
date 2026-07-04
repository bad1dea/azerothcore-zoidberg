#!/usr/bin/env python3
"""Mine all quests startable within a map-coordinate bounding box, with
everything the route author needs: giver/ender NPCs + real spawn coords,
objectives (kill NPCs / collect items), the real spawn clusters of kill
targets, and the prerequisite chain. Repeatable substitute for the
hand-run ad-hoc SQL the first routes were built from.

DB access is via the same ssh+docker path the rest of the toolchain uses
(no direct MySQL client on this host). Usage:

    python3 mine_zone_quests.py --map 1 \
        --xmin -600 --xmax 600 --ymin -5300 --ymax -4000 \
        --lmin 1 --lmax 14 > /tmp/durotar_quests.json
"""
import argparse
import json
import subprocess
import sys

SSH_HOST = "khuong@10.10.30.20"
# Fetch the DB root password from the running container's env at call
# time -- never hardcode the secret into a committed file.
PW_CMD = (
    "docker inspect ac-database --format "
    "'{{range .Config.Env}}{{println .}}{{end}}' "
    "| grep MYSQL_ROOT_PASSWORD | cut -d= -f2"
)


def q(sql: str) -> list[list[str]]:
    """Run one SQL against acore_world, return rows as lists of strings."""
    remote = (
        f'PW=$({PW_CMD}); '
        f'docker exec ac-database mysql -uroot -p"$PW" acore_world -N -e '
        f'"{sql}"'
    )
    out = subprocess.run(
        ["ssh", SSH_HOST, remote],
        capture_output=True, text=True, check=True,
    ).stdout
    rows = []
    for ln in out.splitlines():
        if ln.startswith("mysql:") or not ln.strip():
            continue
        rows.append(ln.split("\t"))
    return rows


def spawn_of(entry: int, map_id: int) -> tuple | None:
    """Nearest-to-median real spawn of a creature entry on a map."""
    rows = q(
        f"SELECT ROUND(position_x,1),ROUND(position_y,1),ROUND(position_z,1) "
        f"FROM creature WHERE id1={entry} AND map={map_id} "
        f"ORDER BY guid LIMIT 1"
    )
    if not rows:
        return None
    return tuple(float(v) for v in rows[0])


def clusters_of(entry: int, map_id: int, n: int = 3) -> list:
    """A few spread-out real spawn points for a kill target."""
    rows = q(
        f"SELECT ROUND(position_x,1),ROUND(position_y,1),ROUND(position_z,1) "
        f"FROM creature WHERE id1={entry} AND map={map_id} ORDER BY position_y"
    )
    if not rows:
        return []
    pts = [[float(v) for v in r] for r in rows]
    # sample evenly across the sorted list to spread the anchors out
    step = max(1, len(pts) // n)
    return pts[::step][:n]


def item_droppers(item_id: int, map_id: int) -> list:
    rows = q(
        f"SELECT clt.Entry, ct.name, clt.Chance "
        f"FROM creature_loot_template clt "
        f"JOIN creature_template ct ON ct.entry=clt.Entry "
        f"WHERE clt.Item={item_id} ORDER BY clt.Chance DESC LIMIT 4"
    )
    return [[int(r[0]), r[1], float(r[2])] for r in rows]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", type=int, required=True)
    ap.add_argument("--xmin", type=float, required=True)
    ap.add_argument("--xmax", type=float, required=True)
    ap.add_argument("--ymin", type=float, required=True)
    ap.add_argument("--ymax", type=float, required=True)
    ap.add_argument("--lmin", type=int, default=1)
    ap.add_argument("--lmax", type=int, default=15)
    a = ap.parse_args()

    # Quests startable by a creature spawned inside the bbox.
    rows = q(
        f"SELECT DISTINCT qs.quest, qt.LogTitle, qt.QuestLevel, qt.MinLevel, "
        f"qs.id, ROUND(c.position_x,1), ROUND(c.position_y,1), ROUND(c.position_z,1) "
        f"FROM creature_queststarter qs "
        f"JOIN creature c ON c.id1=qs.id "
        f"JOIN quest_template qt ON qt.ID=qs.quest "
        f"WHERE c.map={a.map} "
        f"AND c.position_x BETWEEN {a.xmin} AND {a.xmax} "
        f"AND c.position_y BETWEEN {a.ymin} AND {a.ymax} "
        f"AND qt.QuestLevel BETWEEN {a.lmin} AND {a.lmax} "
        f"ORDER BY qt.QuestLevel, qs.quest"
    )

    quests = []
    for r in rows:
        qid = int(r[0])
        giver = int(r[4])
        obj = q(
            f"SELECT RequiredNpcOrGo1,RequiredNpcOrGoCount1,"
            f"RequiredNpcOrGo2,RequiredNpcOrGoCount2,"
            f"RequiredNpcOrGo3,RequiredNpcOrGoCount3,"
            f"RequiredNpcOrGo4,RequiredNpcOrGoCount4,"
            f"RequiredItemId1,RequiredItemCount1,"
            f"RequiredItemId2,RequiredItemCount2 "
            f"FROM quest_template WHERE ID={qid}"
        )[0]
        addon = q(
            f"SELECT PrevQuestID,NextQuestID,ExclusiveGroup "
            f"FROM quest_template_addon WHERE ID={qid}"
        )
        ender = q(
            f"SELECT ce.id, ROUND(c.position_x,1), ROUND(c.position_y,1), "
            f"ROUND(c.position_z,1) FROM creature_questender ce "
            f"JOIN creature c ON c.id1=ce.id WHERE ce.quest={qid} "
            f"AND c.map={a.map} LIMIT 1"
        )
        kill_npcs = []
        for i in range(4):
            npc = int(obj[i * 2]) if obj[i * 2] not in ("0", "") else 0
            cnt = int(obj[i * 2 + 1]) if obj[i * 2 + 1] not in ("0", "") else 0
            if npc > 0:
                kill_npcs.append({
                    "entry": npc, "count": cnt,
                    "clusters": clusters_of(npc, a.map),
                })
        items = []
        for i in range(2):
            it = int(obj[8 + i * 2]) if obj[8 + i * 2] not in ("0", "") else 0
            cnt = int(obj[9 + i * 2]) if obj[9 + i * 2] not in ("0", "") else 0
            if it > 0:
                items.append({
                    "item": it, "count": cnt,
                    "droppers": item_droppers(it, a.map),
                })
        quests.append({
            "quest": qid,
            "title": r[1],
            "quest_level": int(r[2]),
            "min_level": int(r[3]),
            "giver": {"entry": giver, "x": float(r[5]),
                      "y": float(r[6]), "z": float(r[7])},
            "ender": ({"entry": int(ender[0][0]), "x": float(ender[0][1]),
                       "y": float(ender[0][2]), "z": float(ender[0][3])}
                      if ender else None),
            "kill_npcs": kill_npcs,
            "items": items,
            "prev_quest": int(addon[0][0]) if addon else 0,
            "next_quest": int(addon[0][1]) if addon else 0,
            "exclusive_group": int(addon[0][2]) if addon else 0,
        })
    json.dump(quests, sys.stdout, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
