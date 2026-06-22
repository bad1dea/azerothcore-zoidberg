#!/usr/bin/env python3
"""
chain_route.py — chain a parsed Zygor leveling guide into ONE combined route.

The remaster's leveling Lua parses (zygor_parse.py) into a LIST of ~26 route
blocks (race starts "X (1-13)", "Main Guide (13-20)", "Levels (20-25..55-60)",
"Outland (60-70)", "Northrend (70-80)"), each chained to the next via its `next`
field. A per-race leveling route is the concatenation of every block reachable by
following `next` from that race's starting block.

This was previously done with an ad-hoc scratchpad loop; folded into a tool so the
guide pipeline is reproducible (see docs/ZYGOR_WOTLK_HANDOFF.md step 3).

Usage:
  python3 chain_route.py --parse data/routes/zygor_wotlk_horde_leveling.json \\
      --start "Undead (1-13)" --out tools/route_undead_full.json
"""
import argparse
import json
import sys


def seg(title):
    """Last path segment of a Zygor block title/next, robust to the parse quirk
    of mixed single/double backslashes and a stray leading quote."""
    return title.replace("\\\\", "\\").split("\\")[-1].strip().strip('"').strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parse", required=True, help="parsed leveling JSON (list of blocks)")
    ap.add_argument("--start", required=True, help="starting block segment, e.g. 'Undead (1-13)'")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    blocks = json.load(open(a.parse))

    # segment -> block; on a duplicate segment keep the one with MORE steps
    # (the remaster has an empty duplicate "Main Guide (13-20)").
    by_seg = {}
    for b in blocks:
        s = seg(b.get("title", ""))
        if s not in by_seg or len(b.get("steps", [])) > len(by_seg[s].get("steps", [])):
            by_seg[s] = b

    start = a.start.strip()
    if start not in by_seg:
        sys.exit(f"start block {start!r} not found. Available: {sorted(by_seg)}")

    chain = []
    steps = []
    cur = start
    visited = set()
    while cur and cur in by_seg and cur not in visited:
        visited.add(cur)
        b = by_seg[cur]
        chain.append(cur)
        steps.extend(b.get("steps", []))
        nxt = b.get("next")
        cur = seg(nxt) if nxt else None

    route = {"title": f"chained:{start}", "chain": chain, "steps": steps}
    with open(a.out, "w") as f:
        json.dump(route, f)
    sys.stderr.write(f"wrote {a.out}: {len(chain)} blocks -> {len(steps)} steps\n")
    sys.stderr.write("  " + " -> ".join(chain) + "\n")


if __name__ == "__main__":
    main()
