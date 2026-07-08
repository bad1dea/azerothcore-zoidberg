#!/usr/bin/env python3
"""Live route-quality metrics, per bot and per family.

A route is not "good" because the offline suite passes -- it is good if the
fleet actually behaves well running it. This reads the run logs (and the
structured ``DECISION`` records route_runner.py now emits) and reports the
behaviour that matters:

  - % of active time spent grinding (vs questing/travelling)
  - quest completions per hour
  - deaths per hour
  - travel legs per quest completion (a distance proxy; true distance needs
    position telemetry, noted below)
  - backward guide-step moves (bot worked quests out of the guide's order)
  - grind-while-eligible: grinds chosen while a quest was actually runnable
    (should be ZERO now that grind is a strict fallback -- a regression guard)
  - deaths during travel vs during combat

Usage:
    python3 route_metrics.py [--dir ~/ap_fleet_state] [--family FAM]
                             [--json] [--char NAME]

Runs where the logs live (zoidberg). Pure stdlib.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
from collections import defaultdict


TS = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\]\s*(.*)$")
SEG = re.compile(r"=== segment \[([^\]]+)\] \(([a-z_]+)\)")
DONE = re.compile(r"^\[([^\]]+)\] DONE \(level (\d+)\)")
DEATH = re.compile(r"^death #(\d+) -- starting recovery")
SAFEPATH = re.compile(r"^safe_path:")
DECISION = re.compile(r"^DECISION\s+(.*)$")


def _secs(h: str, m: str, s: str, prev: float | None) -> float:
    """Seconds-of-day, unrolled across midnight so a log is monotonic."""
    t = int(h) * 3600 + int(m) * 60 + int(s)
    if prev is not None:
        while t < prev - 60:  # wrapped past midnight
            t += 86400
    return float(t)


def _parse_decision(rest: str) -> dict:
    out: dict[str, str] = {}
    for tok in rest.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def analyze(path: str) -> dict:
    """One bot's run log -> metric dict."""
    seg_time: dict[str, float] = defaultdict(float)  # type -> seconds active
    quest_completions = 0
    deaths = 0
    travel_deaths = 0
    combat_deaths = 0
    backward_steps = 0
    grind_while_eligible = 0
    grind_decisions = 0
    grind_reasons: dict[str, int] = defaultdict(int)
    travel_legs = 0

    cur_type: str | None = None
    cur_seg_start: float | None = None
    last_t: float | None = None
    first_t: float | None = None
    walked_since_seg = False  # a safe_path leg happened in the current segment
    last_guide_step: int | None = None

    with open(path, errors="ignore") as fh:
        for raw in fh:
            mt = TS.match(raw.rstrip("\n"))
            if not mt:
                continue
            t = _secs(mt.group(1), mt.group(2), mt.group(3), last_t)
            body = mt.group(4)
            if first_t is None:
                first_t = t
            last_t = t

            mseg = SEG.search(body)
            if mseg:
                if cur_type is not None and cur_seg_start is not None:
                    seg_time[cur_type] += t - cur_seg_start
                cur_type = mseg.group(2)
                cur_seg_start = t
                walked_since_seg = False
                continue

            if SAFEPATH.match(body):
                travel_legs += 1
                walked_since_seg = True
                continue

            md = DONE.match(body)
            if md:
                if not md.group(1).startswith("grind-"):
                    quest_completions += 1
                continue

            mdeath = DEATH.match(body)
            if mdeath:
                deaths += 1
                # Heuristic: a death right after a safe_path leg (and not yet
                # back in a stationary fight) is a travel death; otherwise the
                # bot died in the segment's combat. Marked approximate.
                if walked_since_seg or cur_type is None:
                    travel_deaths += 1
                else:
                    combat_deaths += 1
                continue

            mdec = DECISION.match(body)
            if mdec:
                d = _parse_decision(mdec.group(1))
                if d.get("reason") == "run_grind":
                    grind_decisions += 1
                    grind_reasons[d.get("grind_reason", "?")] += 1
                    if d.get("guide_quest_available") == "1":
                        grind_while_eligible += 1
                elif d.get("reason") == "run_quest":
                    gs = d.get("guide_step")
                    if gs not in (None, "-"):
                        step = int(gs)
                        if last_guide_step is not None and step < last_guide_step:
                            backward_steps += 1
                        last_guide_step = step
                continue

    if cur_type is not None and cur_seg_start is not None and last_t is not None:
        seg_time[cur_type] += last_t - cur_seg_start

    active = sum(seg_time.values())
    grind_s = sum(v for k, v in seg_time.items() if k == "grind_to_level")
    hours = ((last_t - first_t) / 3600.0) if (first_t is not None
                                              and last_t is not None
                                              and last_t > first_t) else 0.0
    return {
        "hours": round(hours, 2),
        "pct_grinding": round(100 * grind_s / active, 1) if active else 0.0,
        "quests_per_hr": round(quest_completions / hours, 2) if hours else 0.0,
        "deaths_per_hr": round(deaths / hours, 2) if hours else 0.0,
        "quest_completions": quest_completions,
        "deaths": deaths,
        "travel_deaths": travel_deaths,
        "combat_deaths": combat_deaths,
        "travel_legs_per_quest": round(travel_legs / quest_completions, 1)
        if quest_completions else 0.0,
        "backward_guide_steps": backward_steps,
        "grind_decisions": grind_decisions,
        "grind_while_eligible": grind_while_eligible,
        "grind_reasons": dict(grind_reasons),
    }


def family_of(char: str, state_dir: str) -> str:
    """Best-effort family label: the running route's family field if the
    process command line is unavailable, falling back to '?'."""
    # The state file doesn't carry the family; the DECISION lines do. Cheapest
    # reliable source is the first DECISION line's family= token.
    log = os.path.join(state_dir, f"{char}_run.log")
    try:
        with open(log, errors="ignore") as fh:
            for line in fh:
                m = re.search(r"DECISION .*family=(\S+)", line)
                if m:
                    return m.group(1)
    except OSError:
        pass
    return "?"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.expanduser("~/ap_fleet_state"))
    ap.add_argument("--family", default=None, help="filter to one family")
    ap.add_argument("--char", default=None, help="filter to one bot")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    rows = []
    for log in sorted(glob.glob(os.path.join(args.dir, "*_run.log"))):
        char = os.path.basename(log)[:-len("_run.log")]
        if args.char and char != args.char:
            continue
        fam = family_of(char, args.dir)
        if args.family and fam != args.family:
            continue
        m = analyze(log)
        m["char"] = char
        m["family"] = fam
        rows.append(m)

    if args.json:
        print(json.dumps(rows, indent=2))
        return 0

    rows.sort(key=lambda r: (r["family"], -r["pct_grinding"]))
    hdr = (f"{'bot':<14}{'family':<26}{'hrs':>5}{'grind%':>7}{'q/hr':>6}"
           f"{'d/hr':>6}{'bkwd':>5}{'g!elig':>7}{'tD/cD':>7}")
    print(hdr)
    print("-" * len(hdr))
    fam_agg: dict[str, list] = defaultdict(list)
    for r in rows:
        print(f"{r['char']:<14}{r['family']:<26}{r['hours']:>5}"
              f"{r['pct_grinding']:>7}{r['quests_per_hr']:>6}"
              f"{r['deaths_per_hr']:>6}{r['backward_guide_steps']:>5}"
              f"{r['grind_while_eligible']:>7}"
              f"{str(r['travel_deaths']) + '/' + str(r['combat_deaths']):>7}")
        fam_agg[r["family"]].append(r)

    print()
    print("per-family averages:")
    print(f"{'family':<26}{'bots':>5}{'grind%':>8}{'q/hr':>6}{'d/hr':>6}"
          f"{'g!elig':>8}")
    for fam in sorted(fam_agg):
        rs = fam_agg[fam]
        n = len(rs)
        print(f"{fam:<26}{n:>5}"
              f"{sum(r['pct_grinding'] for r in rs) / n:>8.1f}"
              f"{sum(r['quests_per_hr'] for r in rs) / n:>6.2f}"
              f"{sum(r['deaths_per_hr'] for r in rs) / n:>6.2f}"
              f"{sum(r['grind_while_eligible'] for r in rs):>8}")

    tot_gwe = sum(r["grind_while_eligible"] for r in rows)
    print()
    print(f"grind-while-eligible total: {tot_gwe} "
          f"({'OK -- grind is a true fallback' if tot_gwe == 0 else 'REGRESSION: grind ran while a quest was runnable'})")
    print("note: travel_legs distance is a safe_path-count proxy; true "
          "distance-between-quest-actions needs position telemetry (follow-up).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
