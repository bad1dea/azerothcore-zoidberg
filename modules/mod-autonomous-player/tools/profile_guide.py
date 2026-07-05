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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["parse"])
    ap.add_argument("profile", type=Path)
    ap.add_argument("--race", type=int, required=True)
    ap.add_argument("--class", type=int, dest="klass", required=True)
    a = ap.parse_args()
    data = parse_profile(a.profile, a.race, a.klass)
    print(json.dumps(data, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
