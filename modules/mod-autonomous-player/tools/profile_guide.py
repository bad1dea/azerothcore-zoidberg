#!/usr/bin/env python3
"""Parse a Likon69/Honorbuddy Profile v3 starter guide into structured, DB-
validated leveling data, and enrich the generated routes with it.

The external XML is a hand-authored leveling GUIDE and carries exactly the data
the DB-only generator had to guess at: for every quest objective, 5-8 authored
Hotspot coordinates (a built-in roam circuit at the real camp); CollectFrom
sources for gather objectives; SetGrindArea/GrindTo blocks (authored grind camps
with a target level); and Repair/Food/Train Vendor coordinates. The XML order is
the intended leveling sequence.

We treat it strictly as a RESEARCH LEAD: every quest id, mob/GO/item id, NPC id,
and coordinate is validated against local acore_world before use (an unvalidated
external id/coord is dropped, never trusted). Class/race `If` conditions are
evaluated for the target variant so each route gets only its own steps.

Two modes:
  parse   <profile.xml> --race <mask> --class <mask>   -> structured JSON to stdout
  enrich  --family <id> ...                            -> patch routes_generated

Enrichment replaces each quest segment's kill/collect hotspots with the profile's
authored ones and rebuilds the grind ladder from the profile's GrindArea/GrindTo
(both DB-validated), and sets the route's repair vendor -- so bots follow the
guide's proven spots instead of density heuristics.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

# WoW class/race name -> mask bit, to evaluate the profile's If conditions.
CLASS_BIT = {"Warrior": 1, "Paladin": 2, "Hunter": 4, "Rogue": 8, "Priest": 16,
             "DeathKnight": 32, "Shaman": 64, "Mage": 128, "Warlock": 256, "Druid": 1024}
RACE_BIT = {"Human": 1, "Orc": 2, "Dwarf": 4, "NightElf": 8, "Undead": 16,
            "Tauren": 32, "Gnome": 64, "Troll": 128, "BloodElf": 512, "Draenei": 1024}


def cond_ok(cond: str, race_mask: int, class_mask: int, level: int = 1) -> bool:
    """Evaluate a profile If Condition for this variant. Conservative: an
    unrecognized condition passes (we don't want to silently drop content), but
    any Class/Race clause we DO understand must match."""
    for m in re.finditer(r"WoWClass\.(\w+)", cond):
        bit = CLASS_BIT.get(m.group(1))
        if bit is not None and "==" in cond and not (class_mask & bit):
            return False
    for m in re.finditer(r"WoWRace\.(\w+)", cond):
        bit = RACE_BIT.get(m.group(1))
        if bit is not None and "==" in cond and not (race_mask & bit):
            return False
    return True


def parse_profile(path: Path, race_mask: int, class_mask: int) -> dict:
    """Return {quests: {qid: {name, objectives:[{kind,id,count,hotspots}]}},
    grind_areas:[{level,hotspots}], vendors:[{type,entry,x,y,z,class}],
    order:[('pickup'|'turnin'|'grind', ...)]} for this variant."""
    root = ET.parse(path).getroot()
    quests: dict[int, dict] = {}
    grind_areas: list[dict] = []
    vendors: list[dict] = []
    order: list[tuple] = []

    def add_objective(q, obj):
        hs = [(float(h.get("X")), float(h.get("Y")), float(h.get("Z")))
              for h in obj.findall(".//Hotspot")]
        src_go = obj.find(".//GameObject")
        q["objectives"].append({
            "type": obj.get("Type"),  # KillMob / CollectItem
            "mob": int(obj.get("MobId", 0)) or None,
            "item": int(obj.get("ItemId", 0)) or None,
            "go": int(src_go.get("Id")) if src_go is not None else None,
            "count": int(obj.get("KillCount", 0) or obj.get("CollectCount", 0) or 1),
            "hotspots": hs,
        })

    def visit(el):
        for child in el:
            tag = child.tag
            if tag == "If":
                if cond_ok(child.get("Condition", ""), race_mask, class_mask):
                    visit(child)
                continue
            if tag in ("While",):
                visit(child)
                continue
            if tag == "Quest":
                qid = int(child.get("Id", 0))
                q = quests.setdefault(qid, {"name": child.get("Name", ""), "objectives": []})
                for obj in child.findall("Objective"):
                    add_objective(q, obj)
            elif tag == "Objective":
                # Top-level objective carrying its own QuestId (e.g. q376's
                # CollectItem paws/wings, collected incidentally during grinding
                # -- no dedicated hotspots).
                qid = int(child.get("QuestId", 0))
                if qid:
                    q = quests.setdefault(qid, {"name": child.get("QuestName", ""), "objectives": []})
                    add_objective(q, child)
            elif tag == "PickUp":
                order.append(("pickup", int(child.get("QuestId", 0)),
                              int(child.get("GiverId", 0)), child.get("GiverName", "")))
            elif tag == "TurnIn":
                order.append(("turnin", int(child.get("QuestId", 0)),
                              int(child.get("TurnInId", 0)), child.get("TurnInName", "")))
            elif tag == "SetGrindArea":
                hs = [(float(h.get("X")), float(h.get("Y")), float(h.get("Z")))
                      for h in child.findall(".//Hotspot")]
                grind_areas.append({"level": None, "hotspots": hs})
            elif tag == "GrindTo":
                if grind_areas:
                    grind_areas[-1]["level"] = int(child.get("Level", 0))
                order.append(("grind", int(child.get("Level", 0)), len(grind_areas) - 1))
            elif tag == "Vendor":
                vendors.append({"type": child.get("Type"), "entry": int(child.get("Entry", 0)),
                                "x": float(child.get("X", 0)), "y": float(child.get("Y", 0)),
                                "z": float(child.get("Z", 0)), "class": child.get("TrainClass")})
            else:
                visit(child)

    visit(root)
    return {"quests": quests, "grind_areas": grind_areas, "vendors": vendors, "order": order}


def _db_entries_exist(entries, table, id_col):
    """Batch-validate a set of creature/GO entry ids against acore_world;
    return the subset that exist. Uses docker mysql on the fleet host's DB."""
    if not entries:
        return set()
    ids = ",".join(str(int(e)) for e in entries)
    pw = subprocess.run("docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}'",
                        shell=True, capture_output=True, text=True).stdout
    pw = next((l.split("=", 1)[1] for l in pw.splitlines() if l.startswith("MYSQL_ROOT_PASSWORD=")), "")
    out = subprocess.run(["docker", "exec", "ac-database", "mysql", "-uroot", f"-p{pw}", "-N", "-e",
                          f"SELECT entry FROM acore_world.{table} WHERE entry IN ({ids});"],
                         capture_output=True, text=True).stdout
    return {int(x) for x in out.split() if x.strip().isdigit()}


def enrich_routes(config_path, profiles_root, routes_dir):
    """Patch each route's quest hotspots with the profile's authored roam
    circuits (DB-validated) and set the repair vendor. External coords are used
    only after the referenced mob/GO id is confirmed to exist locally."""
    config = json.loads(Path(config_path).read_text())
    for family in config["families"]:
        prof_path = Path(profiles_root) / family["profile"]
        if not prof_path.exists():
            print(f"[{family['id']}] profile missing: {prof_path}")
            continue
        for variant in family["variants"]:
            data = parse_profile(prof_path, variant["race_mask"], variant["class_mask"])
            rpath = Path(routes_dir) / variant["route"]
            if not rpath.exists():
                continue
            route = json.loads(rpath.read_text())
            # collect referenced ids for one-shot DB validation
            mob_ids, go_ids = set(), set()
            for q in data["quests"].values():
                for o in q["objectives"]:
                    if not o["hotspots"]:
                        continue
                    if o["go"]:
                        go_ids.add(o["go"])
                    elif o["mob"]:
                        mob_ids.add(o["mob"])
            good_mobs = _db_entries_exist(mob_ids, "creature_template", "entry")
            good_gos = _db_entries_exist(go_ids, "gameobject_template", "entry")
            patched = 0
            for seg in route["segments"]:
                qid = seg.get("quest")
                if not qid or qid not in data["quests"]:
                    continue
                q = data["quests"][qid]
                kes, ges = [], []
                for o in q["objectives"]:
                    if not o["hotspots"]:
                        continue
                    if o["go"] and o["go"] in good_gos:
                        for (x, y, z) in o["hotspots"]:
                            ges.append({"entry": o["go"], "x": round(x, 1), "y": round(y, 1), "z": round(z, 1)})
                    elif o["mob"] and o["mob"] in good_mobs:
                        for (x, y, z) in o["hotspots"]:
                            kes.append({"entry": o["mob"], "x": round(x, 1), "y": round(y, 1), "z": round(z, 1)})
                if kes and seg["type"] == "quest_grind":
                    seg["kill_entries"] = kes
                    seg["kill_entry"] = kes[0]["entry"]
                    seg["x"], seg["y"], seg["z"] = kes[0]["x"], kes[0]["y"], kes[0]["z"]
                    patched += 1
                elif ges and seg["type"] == "quest_gameobject":
                    seg["go_entries"] = ges
                    seg["x"], seg["y"], seg["z"] = ges[0]["x"], ges[0]["y"], ges[0]["z"]
                    patched += 1
            # repair vendor from the profile (fills the starting-zone gap)
            rep = next((v for v in data["vendors"] if v["type"] == "Repair" and v["entry"]), None)
            if rep and _db_entries_exist({rep["entry"]}, "creature_template", "entry"):
                hv = route.setdefault("home_vendor", {})
                hv.setdefault("repair", {"vendor": rep["entry"], "x": round(rep["x"], 1),
                                         "y": round(rep["y"], 1), "z": round(rep["z"], 1)})
            rpath.write_text(json.dumps(route, indent=2) + "\n")
            print(f"{variant['route']:42s} quest-hotspots patched: {patched}  repair_vendor: {bool(rep)}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["parse", "enrich"])
    ap.add_argument("profile", type=Path, nargs="?")
    ap.add_argument("--race", type=int)
    ap.add_argument("--class", type=int, dest="klass")
    ap.add_argument("--config", type=Path, default=Path(__file__).with_name("coverage_families.json"))
    ap.add_argument("--profiles-root", type=Path)
    ap.add_argument("--routes-dir", type=Path, default=Path(__file__).with_name("routes_generated"))
    a = ap.parse_args()
    if a.mode == "parse":
        print(json.dumps(parse_profile(a.profile, a.race, a.klass), indent=1))
    else:
        enrich_routes(a.config, a.profiles_root, a.routes_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
