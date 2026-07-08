#!/usr/bin/env python3
"""Export the local acore_world facts used by the route coverage compiler.

The output is a deterministic JSON value (no timestamp or credentials). DB
access follows the existing dev-stack SSH + Docker convention and reads the
running database container's password only inside the remote shell.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


QUEST_COLUMNS = [
    "ID", "QuestLevel", "MinLevel", "SuggestedGroupNum", "RewardXPDifficulty",
    "RewardMoney", "StartItem", "Flags", "AllowableRaces", "LogTitle",
    "RequiredNpcOrGo1", "RequiredNpcOrGo2", "RequiredNpcOrGo3", "RequiredNpcOrGo4",
    "RequiredNpcOrGoCount1", "RequiredNpcOrGoCount2", "RequiredNpcOrGoCount3", "RequiredNpcOrGoCount4",
    "RequiredItemId1", "RequiredItemId2", "RequiredItemId3", "RequiredItemId4", "RequiredItemId5", "RequiredItemId6",
    "RequiredItemCount1", "RequiredItemCount2", "RequiredItemCount3", "RequiredItemCount4", "RequiredItemCount5", "RequiredItemCount6",
]

ADDON_COLUMNS = [
    "ID", "AllowableClasses", "SourceSpellID", "PrevQuestID", "NextQuestID",
    "ExclusiveGroup", "BreadcrumbForQuestId", "SpecialFlags",
]


class WorldDb:
    def __init__(self, ssh_host: str):
        self.ssh_host = ssh_host

    def query(self, sql: str) -> list[list[str]]:
        remote = (
            "PW=$(docker inspect ac-database --format "
            "'{{range .Config.Env}}{{println .}}{{end}}' "
            "| sed -n 's/^MYSQL_ROOT_PASSWORD=//p' | head -1); "
            "docker exec ac-database mysql -uroot -p\"$PW\" "
            "--batch --skip-column-names acore_world -e " + json.dumps(sql)
        )
        result = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", self.ssh_host, remote],
            check=True, capture_output=True, text=True,
        )
        return [line.split("\t") for line in result.stdout.splitlines() if line.strip()]


def numeric(value: str) -> int | float | str | None:
    if value == "NULL":
        return None
    try:
        return int(value)
    except ValueError:
        try:
            return float(value)
        except ValueError:
            return value


def records(rows: list[list[str]], columns: list[str]) -> list[dict]:
    return [{key: numeric(value) for key, value in zip(columns, row)} for row in rows]


def in_list(values: set[int]) -> str:
    return ",".join(str(value) for value in sorted(values)) or "0"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name("coverage_families.json"))
    parser.add_argument("--ssh-host", default="khuong@10.10.30.20")
    parser.add_argument("--profiles-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    config = json.loads(args.config.read_text())
    db = WorldDb(args.ssh_host)
    family_quests: dict[str, list[int]] = {}
    all_quests: set[int] = set()

    for family in config["families"]:
        xmin, xmax, ymin, ymax = family["bounds"]
        where = (
            f"map={family['map']} AND position_x BETWEEN {xmin} AND {xmax} "
            f"AND position_y BETWEEN {ymin} AND {ymax}"
        )
        sql = (
            "SELECT DISTINCT relation.quest FROM ("
            f"SELECT qs.quest FROM creature_queststarter qs JOIN creature spawn ON spawn.id1=qs.id WHERE {where} "
            "UNION "
            f"SELECT qs.quest FROM gameobject_queststarter qs JOIN gameobject spawn ON spawn.id=qs.id WHERE {where}"
            ") relation JOIN quest_template qt ON qt.ID=relation.quest "
            "WHERE qt.MinLevel BETWEEN 1 AND 15 AND qt.QuestLevel BETWEEN 1 AND 18 ORDER BY relation.quest"
        )
        ids = {int(row[0]) for row in db.query(sql)}
        # Explicit supplement (Zygor Guides / other order-benchmark research,
        # never trusted for coords/objectives -- only the quest ID itself):
        # the bounding-box scan above only finds quests whose GIVER spawns
        # inside the family's box, missing real, in-chain quests whose giver
        # sits just outside it (or whose MinLevel/QuestLevel falls outside
        # the 1-15/1-18 window here). Every id added this way still goes
        # through every real validation below (quest_template existence,
        # giver/ender local spawns, objective sources) -- an id that turns
        # out invalid locally is simply omitted downstream, same as any
        # bounding-box-discovered id.
        ids.update(int(q) for q in family.get("extra_quest_ids", []))
        ids = sorted(ids)
        family_quests[family["id"]] = ids
        all_quests.update(ids)

    quest_ids = in_list(all_quests)
    quests = records(db.query(
        f"SELECT {','.join(QUEST_COLUMNS)} FROM quest_template WHERE ID IN ({quest_ids}) ORDER BY ID"
    ), QUEST_COLUMNS)
    addons = records(db.query(
        f"SELECT {','.join(ADDON_COLUMNS)} FROM quest_template_addon WHERE ID IN ({quest_ids}) ORDER BY ID"
    ), ADDON_COLUMNS)
    existing_quest_ids = [int(row[0]) for row in db.query("SELECT ID FROM quest_template ORDER BY ID")]

    relation_columns = ["quest", "kind", "entry", "map", "x", "y", "z"]
    starters = records(db.query(
        "SELECT rel.quest,rel.kind,rel.entry,spawn.map,ROUND(spawn.x,3),ROUND(spawn.y,3),ROUND(spawn.z,3) FROM ("
        f"SELECT qs.quest,'creature' kind,qs.id entry FROM creature_queststarter qs WHERE qs.quest IN ({quest_ids}) "
        "UNION ALL "
        f"SELECT qs.quest,'gameobject' kind,qs.id entry FROM gameobject_queststarter qs WHERE qs.quest IN ({quest_ids})"
        ") rel JOIN ("
        "SELECT id1 entry,map,position_x x,position_y y,position_z z,'creature' kind FROM creature "
        "UNION ALL SELECT id entry,map,position_x x,position_y y,position_z z,'gameobject' kind FROM gameobject"
        ") spawn ON spawn.entry=rel.entry AND spawn.kind=rel.kind ORDER BY rel.quest,rel.kind,rel.entry,spawn.map,spawn.x,spawn.y,spawn.z"
    ), relation_columns)
    enders = records(db.query(
        "SELECT rel.quest,rel.kind,rel.entry,spawn.map,ROUND(spawn.x,3),ROUND(spawn.y,3),ROUND(spawn.z,3) FROM ("
        f"SELECT qe.quest,'creature' kind,qe.id entry FROM creature_questender qe WHERE qe.quest IN ({quest_ids}) "
        "UNION ALL "
        f"SELECT qe.quest,'gameobject' kind,qe.id entry FROM gameobject_questender qe WHERE qe.quest IN ({quest_ids})"
        ") rel JOIN ("
        "SELECT id1 entry,map,position_x x,position_y y,position_z z,'creature' kind FROM creature "
        "UNION ALL SELECT id entry,map,position_x x,position_y y,position_z z,'gameobject' kind FROM gameobject"
        ") spawn ON spawn.entry=rel.entry AND spawn.kind=rel.kind ORDER BY rel.quest,rel.kind,rel.entry,spawn.map,spawn.x,spawn.y,spawn.z"
    ), relation_columns)

    creature_entries: set[int] = set()
    gameobject_entries: set[int] = set()
    item_entries: set[int] = set()
    external_vendor_entries: set[int] = set()
    if args.profiles_root:
        for family in config["families"]:
            root = ET.parse(args.profiles_root / family["profile"]).getroot()
            external_vendor_entries.update(
                int(vendor.get("Entry")) for vendor in root.findall(".//Vendors/Vendor")
                if vendor.get("Entry") and vendor.get("Entry").isdigit()
            )
    creature_entries.update(external_vendor_entries)
    for quest in quests:
        for index in range(1, 5):
            entry = int(quest[f"RequiredNpcOrGo{index}"] or 0)
            if entry > 0:
                creature_entries.add(entry)
            elif entry < 0:
                gameobject_entries.add(-entry)
        for index in range(1, 7):
            item = int(quest[f"RequiredItemId{index}"] or 0)
            if item:
                item_entries.add(item)

    item_sources: list[dict] = []
    if item_entries:
        items = in_list(item_entries)
        source_columns = ["item", "kind", "entry", "chance"]
        item_sources = records(db.query(
            f"SELECT Item,'creature',Entry,Chance FROM creature_loot_template WHERE Item IN ({items}) "
            "UNION ALL "
            f"SELECT gl.Item,'gameobject',gt.entry,gl.Chance FROM gameobject_loot_template gl "
            f"JOIN gameobject_template gt ON gt.Data1=gl.Entry WHERE gl.Item IN ({items}) "
            "ORDER BY 1,2,3"
        ), source_columns)
        for source in item_sources:
            if source["kind"] == "creature":
                creature_entries.add(int(source["entry"]))
            else:
                gameobject_entries.add(int(source["entry"]))

    creature_columns = ["entry", "name", "minlevel", "maxlevel", "rank", "map", "x", "y", "z"]
    creatures = records(db.query(
        "SELECT ct.entry,ct.name,ct.minlevel,ct.maxlevel,ct.rank,c.map,"
        "ROUND(c.position_x,3),ROUND(c.position_y,3),ROUND(c.position_z,3) "
        f"FROM creature_template ct LEFT JOIN creature c ON c.id1=ct.entry WHERE ct.entry IN ({in_list(creature_entries)}) "
        "ORDER BY ct.entry,c.map,c.position_x,c.position_y,c.position_z"
    ), creature_columns)
    gameobject_columns = ["entry", "name", "type", "map", "x", "y", "z"]
    gameobjects = records(db.query(
        "SELECT gt.entry,gt.name,gt.type,g.map,ROUND(g.position_x,3),ROUND(g.position_y,3),ROUND(g.position_z,3) "
        f"FROM gameobject_template gt LEFT JOIN gameobject g ON g.id=gt.entry WHERE gt.entry IN ({in_list(gameobject_entries)}) "
        "ORDER BY gt.entry,g.map,g.position_x,g.position_y,g.position_z"
    ), gameobject_columns)

    snapshot = {
        "schema_version": 1,
        "family_quests": family_quests,
        "existing_quest_ids": existing_quest_ids,
        "quests": quests,
        "addons": addons,
        "starters": starters,
        "enders": enders,
        "creatures": creatures,
        "gameobjects": gameobjects,
        "item_sources": item_sources,
        "external_vendor_entries": sorted(external_vendor_entries),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(snapshot, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
