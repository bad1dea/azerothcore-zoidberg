#!/usr/bin/env python3
"""Reachability report: which quest targets the bots physically cannot get to.

Consumes the structured ``REACH`` records route_runner.py emits whenever a
quest accept / turn-in / objective fails, and turns anecdote into data:

  - a ranked list of unreachable givers/objectives by frequency (across the
    whole fleet), each with target entry, coords, how many bots/quests hit it,
    and the median distance the bot ended up stranded at;
  - the class split -- cant_reach (navigation), bag_full (loot), mechanic
    (phased/scripted/wrong-object) -- so a pathing problem is never confused
    with a quest-data or interaction problem.

Every ``cant_reach`` entry is a target whose coords are DB-real (the route
uses DB spawn coords) but which the bot could not walk to -- the q786 class.
A tight cluster of these is a hand-authored-approach-waypoint job; a broad
spread is a systemic navigation fix.

Usage: python3 reach_report.py [--dir ~/ap_fleet_state] [--kind accept|turnin|objective]
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import statistics
from collections import defaultdict


REACH = re.compile(
    r"REACH kind=(\w+) class=(\w+) quest=(\S+) entry=(\S+) family=(\S+) "
    r"target=\(([-\d]+),([-\d]+),([-\d]+)\) bot=\(([^)]*)\) dist=([-\d]+) dz=([-\d]+)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.expanduser("~/ap_fleet_state"))
    ap.add_argument("--kind", default=None)
    args = ap.parse_args()

    # keyed by (entry, target) -> record aggregation
    agg: dict[tuple, dict] = {}
    class_counts: dict[str, int] = defaultdict(int)
    kind_counts: dict[str, int] = defaultdict(int)
    total = 0

    for log in glob.glob(os.path.join(args.dir, "*_run.log")):
        char = os.path.basename(log)[:-len("_run.log")]
        for line in open(log, errors="ignore"):
            m = REACH.search(line)
            if not m:
                continue
            kind, cls, quest, entry, family, tx, ty, tz, bot, dist, dz = m.groups()
            if args.kind and kind != args.kind:
                continue
            total += 1
            class_counts[cls] += 1
            kind_counts[kind] += 1
            key = (entry, tx, ty)
            a = agg.setdefault(key, {
                "entry": entry, "target": f"({tx},{ty},{tz})", "kind": kind,
                "class": cls, "quests": set(), "chars": set(),
                "dists": [], "dzs": [], "families": set(), "hits": 0})
            a["hits"] += 1
            a["quests"].add(quest)
            a["chars"].add(char)
            a["families"].add(family)
            if dist != "-1":
                a["dists"].append(int(dist))
            a["dzs"].append(int(dz))
            # a target that is ever reached-but-failed vs never-reached: keep the
            # worst (cant_reach dominates the label for ranking navigation work)
            if cls == "cant_reach":
                a["class"] = "cant_reach"

    if not total:
        print("no REACH records yet (fleet needs to run on the instrumented "
              "runner and hit some accept/turnin/objective failures).")
        return 0

    print(f"REACH records: {total}   by class: {dict(class_counts)}   "
          f"by kind: {dict(kind_counts)}")
    print()
    print("UNREACHABLE / FAILED TARGETS (ranked by hits):")
    hdr = (f"{'entry':>7} {'kind':<10}{'class':<11}{'hits':>5}{'bots':>5}"
           f"{'medDist':>8}{'medDz':>7}  target / quests")
    print(hdr)
    print("-" * 92)
    rows = sorted(agg.values(), key=lambda a: (-a["hits"], a["entry"]))
    for a in rows:
        md = int(statistics.median(a["dists"])) if a["dists"] else -1
        mz = int(statistics.median(a["dzs"])) if a["dzs"] else 0
        quests = ",".join(sorted(a["quests"])[:4])
        print(f"{a['entry']:>7} {a['kind']:<10}{a['class']:<11}{a['hits']:>5}"
              f"{len(a['chars']):>5}{md:>8}{mz:>7}  {a['target']} {quests}")

    cant = [a for a in rows if a["class"] == "cant_reach"]
    print()
    print(f"cant_reach targets: {len(cant)} distinct  "
          f"(these are DB-real NPCs/GOs the bot could not walk to -- "
          f"navigation fixes / approach waypoints)")
    print("note: median distance is how far from the target the bot ended up; "
          "a large value with a nonzero Z gap = terrain/mesh dead-end.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
