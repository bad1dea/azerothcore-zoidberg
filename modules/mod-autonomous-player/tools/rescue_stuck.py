#!/usr/bin/env python3
"""Fleet dead-bot rescue: one cycle, called by fleet_monitor.sh (cron */10).

A bot found dead/ghost on two CONSECUTIVE cycles (>= ~10 min) has a wedged
recovery -- found live 2026-07-05: corpses stranded inside the Razormane belt
put level-2 Locktwelve/Trolltwelve into silent reclaim->insta-death loops for
30+ minutes while their runners hung. Runner-side recovery handles the normal
cases; this is the last-resort ops guarantee that no bot stays dead longer
than two monitor cycles: revive + teleport to the family's racial-start tele
and let the runner's next ensure_online/segment pass resume.

State: ~/ap_fleet_state/.rescue/<char> marker files (created on first dead
sighting, cleared when alive; rescue fires when a marker older than
MIN_DEAD_SECONDS is seen while still dead).
"""
import glob
import json
import os
import re
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROUTES = os.path.join(TOOLS, "routes_generated")
MARKERS = os.path.expanduser("~/ap_fleet_state/.rescue")
MIN_DEAD_SECONDS = 540

TELE_BY_FAMILY = {
    "durotar": "ValleyOfTrials",
    "mulgore": "CampNarache",
    "elwynn": "NorthshireValley",
    "tirisfal": "Deathknell",
    "dunmorogh": "ColdridgeValley",
    "eversong": "SunstriderIsle",
}


def soap(cmd: str) -> str:
    r = subprocess.run(
        ["python3", os.path.join(TOOLS, "soap.py"), cmd],
        capture_output=True, text=True, timeout=40)
    return r.stdout


def main() -> int:
    os.makedirs(MARKERS, exist_ok=True)
    now = time.time()
    for f in sorted(glob.glob(os.path.join(ROUTES, "*.json"))):
        if f.endswith("_generation_summary.json"):
            continue
        route = json.load(open(f))
        char = route["char"]
        family = os.path.basename(f).split("_")[0]
        try:
            out = soap(f"autonomousplayer status {char}")
        except Exception:
            continue
        m = re.search(rf"{char} lvl \d+ .*alive=(\w+) .*ghost=(\w+)", out)
        marker = os.path.join(MARKERS, char)
        if not m:
            continue  # offline: the runner/monitor relaunch owns that case
        dead = m.group(1) != "true" or m.group(2) == "true"
        if not dead:
            if os.path.exists(marker):
                os.remove(marker)
            continue
        if not os.path.exists(marker):
            open(marker, "w").write(str(now))
            continue
        if now - float(open(marker).read().strip() or 0) < MIN_DEAD_SECONDS:
            continue
        tele = TELE_BY_FAMILY.get(family)
        if not tele:
            continue
        soap(f"revive {char}")
        soap(f"tele name {char} {tele}")
        os.remove(marker)
        print(f"RESCUED {char}: dead >={MIN_DEAD_SECONDS}s -> revived + tele {tele}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
