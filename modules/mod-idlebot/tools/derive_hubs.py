#!/usr/bin/env python3
"""
derive_hubs.py — derive race-neutral leveling hubs straight from where the
WotLK quests actually are, for the 1-58 band the Cata Zygor guides don't cover.

For each faction and each hub level, it gathers the quest givers of quests that
faction can do around that level, buckets their positions on a coarse grid, and
takes the densest bucket's centroid as the hub. That lands on the real main
quest town for that level — the natural leveling progression, grounded in the DB
(no hand-typed coords). Merge the output into IdleBotZoneRoute.cpp; the 58-80
hubs stay Zygor-derived (zygor_*.py).

Input: questgivers.tsv with columns
  QuestLevel  MinLevel  AllowableRaces  map  x  y  z
(dump: SELECT qt.QuestLevel, qt.MinLevel, qt.AllowableRaces, c.map,
 ROUND(c.position_x), ROUND(c.position_y), ROUND(c.position_z)
 FROM quest_template qt JOIN creature_queststarter qs ON qs.quest=qt.ID
 JOIN creature c ON c.id1=qs.id WHERE c.map IN (0,1) ...)

Usage: derive_hubs.py questgivers.tsv > hubs_derived.txt
"""
import sys
from collections import defaultdict

# WotLK race bitmasks.
HORDE = 2 | 16 | 32 | 128 | 512        # Orc Undead Tauren Troll BloodElf = 690
ALLIANCE = 1 | 4 | 8 | 64 | 1024       # Human Dwarf NightElf Gnome Draenei = 1101

# Hub levels and the QuestLevel window gathered for each.
HUB_LEVELS = [12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56]
GRID = 200.0   # bucket size (yards); a quest town clusters well within this.


def faction_can_do(allowable, mask, strict):
    # strict: only quests EXCLUSIVE to this faction's races (guaranteed to sit in
    # that faction's safe territory). non-strict also counts any-race quests.
    other = ALLIANCE if mask == HORDE else HORDE
    if strict:
        return (allowable & mask) != 0 and (allowable & other) == 0
    return allowable == 0 or (allowable & mask) != 0


def derive(rows, mask, strict):
    hubs = []
    for hub in HUB_LEVELS:
        lo, hi = hub - 1, hub + 4
        buckets = defaultdict(list)
        for ql, ml, allow, mp, x, y, z in rows:
            if ql < lo or ql >= hi:
                continue
            if not faction_can_do(allow, mask, strict):
                continue
            key = (mp, round(x / GRID), round(y / GRID))
            buckets[key].append((x, y, z))
        if not buckets:
            hubs.append((hub, None))
            continue
        # densest bucket
        best = max(buckets.values(), key=len)
        n = len(best)
        cx = round(sum(p[0] for p in best) / n, 1)
        cy = round(sum(p[1] for p in best) / n, 1)
        cz = round(sum(p[2] for p in best) / n, 1)
        mp = max(buckets, key=lambda k: len(buckets[k]))[0]
        hubs.append((hub, (mp, cx, cy, cz, n)))
    return hubs


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    rows = []
    for line in open(argv[1]):
        p = line.split()
        if len(p) < 7:
            continue
        try:
            rows.append((int(p[0]), int(p[1]), int(p[2]), int(p[3]),
                         float(p[4]), float(p[5]), float(p[6])))
        except ValueError:
            continue

    strict = "--strict" in argv
    for fac_name, fac_id, mask in (("Alliance", 0, ALLIANCE), ("Horde", 1, HORDE)):
        sys.stderr.write("\n=== {}{} ===\n".format(fac_name, " (strict)" if strict else ""))
        for hub, data in derive(rows, mask, strict):
            if not data:
                sys.stderr.write("  L{:<3} (no quests found)\n".format(hub))
                continue
            mp, cx, cy, cz, n = data
            # emit a C++ LevelHub row (zone left blank — fill from coords if wanted)
            print('            {{ {}, {:>3}, {:>4}, {:>9.1f}f, {:>9.1f}f, {:>7.1f}f, '
                  '"L{} hub", true }},  // {} givers'
                  .format(fac_id, hub, mp, cx, cy, cz, hub, n))
            sys.stderr.write("  L{:<3} map{:<4} ({:.0f},{:.0f})  {} givers\n"
                             .format(hub, mp, cx, cy, n))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
