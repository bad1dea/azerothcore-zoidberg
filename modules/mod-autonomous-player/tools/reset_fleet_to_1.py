#!/usr/bin/env python3
"""Reset the fleet to a TRUE factory state: each route character is
logged out, DELETED, and re-created (same name/account/race/class/gender)
through the real Player::Create path via `.autonomousplayer recreate` --
the only mechanism that correctly produces starting items, spells,
skills, racial-start position, and full health together.

History of why SQL surgery was abandoned: the old UPDATE/DELETE approach
kept old inventory/spells/money, missed the corpse table (a bot dead at
reset time ghost-walked 700yd to yesterday's corpse and was two-tapped
at level 1), and needed hand-maintained racial-start coordinates.
Re-creation gets all of it right by construction.

Runs on the fleet host with SOAP creds in the environment (source
~/secrets/ap_soap.env first). Stop the runners before running this; the
relaunch afterward logs everyone back in as fresh level-1 characters.
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
STATE = os.path.expanduser("~/ap_fleet_state")


def soap(cmd: str) -> str:
    r = subprocess.run(
        ["python3", os.path.join(TOOLS, "soap.py"), cmd],
        capture_output=True, text=True, timeout=40)
    return r.stdout


def _dbpw() -> str:
    e = subprocess.run(
        ["docker", "inspect", "ac-database",
         "--format", "{{range .Config.Env}}{{println .}}{{end}}"],
        capture_output=True, text=True).stdout
    return next(l.split("=", 1)[1] for l in e.splitlines()
                if l.startswith("MYSQL_ROOT_PASSWORD="))


def char_exists(char: str) -> bool:
    # DB truth: SOAP status prints NOTHING for an offline character, so
    # the first factory run reported 30 false failures while every
    # recreation had actually succeeded.
    r = subprocess.run(
        ["docker", "exec", "ac-database", "mysql", "-uroot", f"-p{_dbpw()}",
         "acore_characters", "-N", "-e",
         f"SELECT 1 FROM characters WHERE name='{char}' LIMIT 1;"],
        capture_output=True, text=True)
    return "1" in r.stdout


def main() -> int:
    ok, failed = [], []
    for f in sorted(glob.glob(os.path.join(ROUTES, "*.json"))):
        if f.endswith("_generation_summary.json"):
            continue
        char = json.load(open(f))["char"]

        soap(f"autonomousplayer logout {char}")
        time.sleep(2.0)

        out = soap(f"autonomousplayer recreate {char}")
        if "Recreating" not in out:
            print(f"{char}: recreate REFUSED: {out.strip()[-120:]}")
            failed.append(char)
            continue

        # Async creation: poll until the character resolves again.
        recreated = False
        for _ in range(20):
            time.sleep(3.0)
            if char_exists(char):
                recreated = True
                break
        if not recreated:
            print(f"{char}: recreation never resolved -- investigate")
            failed.append(char)
            continue

        sp = os.path.join(STATE, f"{char}_state.json")
        json.dump({"done": [], "level_history": [], "deaths": 0,
                   "segment_attempts": {}, "skipped": [], "defer_fails": {}},
                  open(sp, "w"), indent=1)
        parked = os.path.join(STATE, f"{char}.parked")
        if os.path.exists(parked):
            os.remove(parked)
        print(f"reset {char} -> factory level 1 (recreated)")
        ok.append(char)

    print(f"\nfactory reset: {len(ok)} ok, {len(failed)} failed"
          + (f" ({failed})" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
