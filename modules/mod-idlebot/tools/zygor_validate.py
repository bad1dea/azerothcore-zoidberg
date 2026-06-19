#!/usr/bin/env python3
"""
zygor_validate.py — filter parsed Zygor routes to what is valid on this
AzerothCore (3.3.5a) world DB, and emit clean per-guide route files.

The route keeps only the quest ORDER + ids + light objective hints. It keeps NO
coordinates: the runtime quest engine resolves quest-giver / quest-ender / mob /
object positions from the DB by id. This tool just drops content that does not
exist on the server (e.g. Cataclysm-revamped 1-60 quests) so the bot never
chases a quest that isn't there.

Inputs:
  routes.json        output of zygor_parse.py
  --valid-quests F   newline list of quest_template.ID present on the server
                     (dump: SELECT ID FROM acore_world.quest_template)
  --min-coverage X   keep a guide only if >=X of its quests are valid (def 0.8)
  --out DIR          write <faction>/<id>.json route files here

Emits a coverage report to stderr.

  zygor_validate.py routes.json --valid-quests valid_quests.txt --out ../data/routes
"""
import argparse
import json
import os
import sys


def load_ids(path):
    return set(int(x) for x in open(path) if x.strip().isdigit())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("routes")
    ap.add_argument("--valid-quests", required=True)
    ap.add_argument("--min-coverage", type=float, default=0.8)
    ap.add_argument("--out")
    args = ap.parse_args()

    valid = load_ids(args.valid_quests)
    guides = json.load(open(args.routes))

    kept, dropped = [], []
    for g in guides:
        qids = {s["quest"] for s in g["steps"]
                if s["action"] in ("accept", "turnin") and s.get("quest")}
        if not qids:
            dropped.append((0.0, g["id"], "no quests"))
            continue
        cov = sum(1 for q in qids if q in valid) / len(qids)
        if cov < args.min_coverage:
            dropped.append((cov, g["id"], "low coverage"))
            continue

        # Drop steps that reference an invalid quest; keep non-quest steps.
        clean_steps = []
        for s in g["steps"]:
            q = s.get("quest")
            if q is not None and q not in valid:
                continue
            clean_steps.append(s)
        out = dict(g)
        out["steps"] = clean_steps
        out["coverage"] = round(cov, 3)
        out["quest_count"] = len([s for s in clean_steps
                                  if s["action"] == "accept"])
        kept.append(out)

    kept.sort(key=lambda g: (g["faction"], g.get("startlevel") or 0))

    if args.out:
        for g in kept:
            d = os.path.join(args.out, g["faction"].lower())
            os.makedirs(d, exist_ok=True)
            json.dump(g, open(os.path.join(d, g["id"] + ".json"), "w"), indent=1)

    sys.stderr.write(
        "kept {} guides ({} quests), dropped {}\n".format(
            len(kept), sum(g["quest_count"] for g in kept), len(dropped)))
    by_fac = {}
    for g in kept:
        by_fac.setdefault(g["faction"], []).append(g)
    for fac, gs in sorted(by_fac.items()):
        lv = sorted({int(g.get("startlevel") or 0) for g in gs})
        sys.stderr.write("  {}: {} guides, start-levels {}\n".format(
            fac, len(gs), lv))
    return 0


if __name__ == "__main__":
    sys.exit(main())
