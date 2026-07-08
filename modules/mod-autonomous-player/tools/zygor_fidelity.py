#!/usr/bin/env python3
"""Zygor fidelity: parse a Zygor leveling guide into its FULL ordered step
flow (not just quest ids) and measure how much of that flow the generated
route actually preserves.

This is the yardstick for "Zygor-routed" (vs merely "Zygor-quest-ordered").
The guide is the route SKELETON: travel/goto, road/path/gate/cave hints,
flight paths, hearth/set-home, accept, turn-in, kill/collect, gameobject
use, use-item, ding gates, and class/race gates -- each with its Zygor step
number. Today's pipeline keeps only the quest step numbers (guide_priority)
and drops every travel/hearth/road/cave instruction; this report quantifies
exactly that gap, per family, so we can (a) not call a route "Zygor-routed"
until the gap is closed and (b) drive the generator changes off real numbers.

It reports two SEPARATE things (kept independent on purpose):
  - FIDELITY: is the guide's step flow preserved in the generated route
    (quest order, and travel/hearth/road/cave steps represented at all)?
  - (DB correctness is validated elsewhere, in the coverage pipeline; this
    tool is about guide-flow preservation, and flags where the two must
    later reconcile -- e.g. a Zygor coord vs the DB spawn.)

Usage:
    python3 zygor_fidelity.py [--family durotar] [--verbose] [--json]

Guide files: ~/research/ZygorGuidesRemaster-3.3.5a_WOTLK/.../ZygorLeveling{Horde,Alliance}CLASSIC.lua
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
GUIDES = os.path.expanduser(
    "~/research/ZygorGuidesRemaster-3.3.5a_WOTLK/ZygorGuidesViewerRM/"
    "Guides/Retail/Leveling")
HORDE = os.path.join(GUIDES, "ZygorLevelingHordeCLASSIC.lua")
ALLIANCE = os.path.join(GUIDES, "ZygorLevelingAllianceCLASSIC.lua")

# family -> (guide file, RegisterGuide section-name substring)
FAMILY_GUIDE = {
    "durotar":   (HORDE, "Durotar (1-12)"),
    "mulgore":   (HORDE, "Mulgore (1-12)"),
    "tirisfal":  (HORDE, "Tirisfal Glades (1-12)"),
    "eversong":  (HORDE, "Eversong Woods (1-13)"),
    "elwynn":    (ALLIANCE, "Human Starter (1-11)"),
    "dunmorogh": (ALLIANCE, "Dwarf & Gnome Starter (1-11)"),
    "teldrassil": (ALLIANCE, "Night Elf Starter (1-11)"),
    "ammenvale": (ALLIANCE, "Draenei Starter (1-11)"),
}

# One representative generated route per family (clones share segments).
FAMILY_ROUTE = {
    "durotar": "durotar_orc_warrior_1_12.json",
    "mulgore": "mulgore_tauren_shaman_1_10.json",
    "tirisfal": "tirisfal_undead_rogue_1_10.json",
    "eversong": "eversong_belf_paladin_1_10.json",
    "elwynn": "elwynn_human_mage_1_10.json",
    "dunmorogh": "dunmorogh_dwarf_warrior_1_10.json",
    "teldrassil": "teldrassil_nightelf_hunter_1_10.json",
    "ammenvale": "ammenvale_draenei_shaman_1_10.json",
}

GOTO = re.compile(r"\|goto\s+(?:([A-Za-z '\-]+?)\s+)?([\d.]+),([\d.]+)(?:\s*<\s*([\d.]+))?")
IDREF = re.compile(r"##(\d+)")
QREF = re.compile(r"\|q\s+(\d+)")

# hint keywords that mark a real travel/route instruction we must preserve
TRAVEL_HINT = re.compile(
    r"\b(road|path|gate|cave|bridge|tunnel|ramp|stairs|enter|leave|exit|"
    r"inside|outside|follow|go (?:up|down|east|west|north|south|through|around|"
    r"back|to|inside|outside|across)|cross|climb|head)\b", re.I)


def extract_section(text: str, name_substr: str) -> str:
    """Return the [[ ... ]] body of the RegisterGuide whose name contains
    name_substr."""
    i = text.find(name_substr)
    if i < 0:
        return ""
    start = text.find("[[", i)
    end = text.find("]])", start)
    return text[start + 2:end] if start > 0 and end > 0 else ""


def parse_guide(family: str) -> list[dict]:
    """Parse a family's guide section into an ordered list of step records.
    Each record: {step, kind, quest?, entry?, item?, zone?, x?, y?, radius?,
    hint?, walk?, race?}. kind in: travel, accept, turnin, talk, kill,
    collect, gameobject, use, fpath, hearth, sethome, ding, buy, train,
    gate, other."""
    path, name = FAMILY_GUIDE[family]
    body = extract_section(open(path, errors="ignore").read(), name)
    steps: list[dict] = []
    step_no = 0
    zone = None
    cur_race = None
    for raw in body.splitlines():
        ln = raw.strip()
        if not ln or ln.startswith("|tip") or ln.startswith("label ") \
                or ln.startswith("sticky") or ln.startswith("'"):
            continue
        low = ln.lower()
        if low == "step" or low.startswith("step "):
            step_no += 1
            continue
        if low.startswith("defaultfor "):
            cur_race = ln.split(None, 1)[1].strip()
            continue

        g = GOTO.search(ln)
        if g and g.group(1):
            zone = g.group(1).strip()
        coord = None
        if g:
            coord = (zone, float(g.group(2)), float(g.group(3)),
                     float(g.group(4)) if g.group(4) else None)

        def rec(kind, **kw):
            r = {"step": step_no, "kind": kind, "race": cur_race}
            if coord:
                r.update(zone=coord[0], x=coord[1], y=coord[2], radius=coord[3])
            if "walk" in low or "only if walking" in low:
                r["walk"] = True
            r.update(kw)
            steps.append(r)

        idm = IDREF.search(ln)
        qid = int(idm.group(1)) if idm else None
        qref = QREF.search(ln)
        qquest = int(qref.group(1)) if qref else None

        if low.startswith("accept "):
            rec("accept", quest=qid, name=ln[7:].split("##")[0].strip())
        elif low.startswith("turnin "):
            rec("turnin", quest=qid, name=ln[7:].split("##")[0].strip())
        elif low.startswith("talk "):
            rec("talk", entry=qid, name=ln[5:].split("##")[0].strip())
        elif low.startswith("kill "):
            rec("kill", entry=qid, quest=qquest, name=ln[5:].split("##")[0].strip())
        elif low.startswith("collect "):
            rec("collect", item=qid, quest=qquest, name=ln[8:].split("##")[0].strip())
        elif low.startswith("click "):
            rec("gameobject", quest=qquest, name=ln[6:].split("|")[0].strip())
        elif low.startswith("use "):
            rec("use", item=qid, quest=qquest, name=ln[4:].split("##")[0].strip())
        elif low.startswith("fpath "):
            rec("fpath", name=ln[6:].split("|")[0].strip())
        elif low.startswith("hearth "):
            rec("hearth", name=ln[7:].split("|")[0].strip())
        elif "set hearth" in low or low.startswith("sethome"):
            rec("sethome", name=ln.split("|")[0].strip())
        elif low.startswith("ding "):
            rec("ding", name=ln[5:].split("|")[0].strip())
        elif low.startswith("buy "):
            rec("buy", name=ln[4:].split("|")[0].strip())
        elif low.startswith("train"):
            rec("train", name=ln.split("|")[0].strip())
        elif g:
            # a bare goto line with leading prose = a TRAVEL instruction.
            hint = ln.split("|goto")[0].strip()
            kind = "travel" if (hint and TRAVEL_HINT.search(hint)) else "travel"
            rec(kind, hint=hint or "(move)")
        # else: unrecognized meta line -- ignore
    return steps


def load_route(family: str) -> dict:
    p = os.path.join(HERE, "routes_generated", FAMILY_ROUTE[family])
    return json.loads(open(p).read())


def fidelity(family: str) -> dict:
    steps = parse_guide(family)
    route = load_route(family)
    segs = route["segments"]
    seg_quests = {s["quest"] for s in segs if s.get("quest")}
    has_travel_segs = any(s["type"] in ("walk", "travel", "hearth") for s in segs)

    by_kind = Counter(s["kind"] for s in steps)
    # Quest actions in the guide (accept/turnin/kill/collect/use/gameobject
    # that carry a quest id) -> is that quest represented in the route?
    guide_quests = [s for s in steps
                    if s["kind"] in ("accept", "turnin", "kill", "collect",
                                     "use", "gameobject")
                    and (s.get("quest") or 0)]
    guide_quest_ids = {s.get("quest") for s in guide_quests if s.get("quest")}
    quests_kept = guide_quest_ids & seg_quests
    quests_dropped = guide_quest_ids - seg_quests

    # Non-quest FLOW steps the skeleton must preserve, currently unrepresented
    travel = [s for s in steps if s["kind"] == "travel"]
    road_travel = [s for s in travel
                   if s.get("hint") and TRAVEL_HINT.search(s["hint"])]
    fpath = [s for s in steps if s["kind"] == "fpath"]
    hearth = [s for s in steps if s["kind"] in ("hearth", "sethome")]
    dings = [s for s in steps if s["kind"] == "ding"]

    # order preservation: are the guide's quests, in guide order, in the same
    # relative order in the generated route? (count order inversions)
    guide_order = [q for q in
                   [s.get("quest") for s in guide_quests] if q in quests_kept]
    seen = []
    for q in guide_order:
        if q not in seen:
            seen.append(q)
    route_pos = {s["quest"]: i for i, s in enumerate(segs) if s.get("quest")}
    seq = [route_pos[q] for q in seen if q in route_pos]
    inversions = sum(1 for i in range(len(seq)) for j in range(i + 1, len(seq))
                     if seq[i] > seq[j])

    return {
        "family": family,
        "guide_steps": len(steps),
        "by_kind": dict(by_kind),
        "guide_quests": len(guide_quest_ids),
        "quests_kept": len(quests_kept),
        "quests_dropped": sorted(quests_dropped),
        "travel_steps": len(travel),
        "road_path_steps": len(road_travel),
        "fpath_steps": len(fpath),
        "hearth_steps": len(hearth),
        "ding_gates": len(dings),
        "route_has_travel_segments": has_travel_segs,
        # everything non-quest is currently dropped unless the route has
        # walk/hearth segments -- which today it does not.
        "flow_steps_dropped": len(travel) + len(fpath) + len(hearth),
        "order_inversions": inversions,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--family", default=None)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    fams = [args.family] if args.family else list(FAMILY_GUIDE)
    reports = [fidelity(f) for f in fams]
    if args.json:
        print(json.dumps(reports, indent=2))
        return 0

    print("ZYGOR FIDELITY -- how much of the guide's step flow the route keeps")
    print("=" * 78)
    hdr = (f"{'family':<11}{'gsteps':>7}{'quests':>7}{'kept':>5}{'travel':>7}"
           f"{'road':>5}{'fpath':>6}{'hearth':>7}{'FLOW-DROP':>10}{'ord.inv':>8}")
    print(hdr)
    print("-" * 78)
    for r in reports:
        print(f"{r['family']:<11}{r['guide_steps']:>7}{r['guide_quests']:>7}"
              f"{r['quests_kept']:>5}{r['travel_steps']:>7}{r['road_path_steps']:>5}"
              f"{r['fpath_steps']:>6}{r['hearth_steps']:>7}"
              f"{r['flow_steps_dropped']:>10}{r['order_inversions']:>8}")
    tot_flow = sum(r["flow_steps_dropped"] for r in reports)
    tot_travel = sum(r["travel_steps"] for r in reports)
    any_travel_seg = any(r["route_has_travel_segments"] for r in reports)
    print("-" * 78)
    print(f"TOTAL guide flow steps dropped (travel+fpath+hearth): {tot_flow}"
          f"  (of which {tot_travel} are travel/road/gate/cave moves)")
    print(f"routes currently emit travel/walk/hearth segments: "
          f"{'YES' if any_travel_seg else 'NO -- every travel instruction is dropped'}")
    print()
    print("VERDICT: a route is 'Zygor-routed' only when FLOW-DROP -> 0 and the")
    print("guide's travel/road/hearth/cave steps become real route segments.")
    if args.verbose and args.family:
        print("\n--- parsed guide flow (first 60 steps) ---")
        for s in parse_guide(args.family)[:60]:
            loc = (f" @{s.get('zone','')} {s.get('x','')},{s.get('y','')}"
                   if "x" in s else "")
            extra = s.get("hint") or s.get("name") or ""
            q = f" q{s['quest']}" if s.get("quest") else ""
            print(f"  step{s['step']:>3} {s['kind']:<10}{q:<7} {extra}{loc}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
