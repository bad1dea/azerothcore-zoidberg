#!/usr/bin/env python3
"""Parse the Likon69 Profile v3 Transport guides into structured, DB-validated
transport-route data for the bot transport-boarding behavior.

Each Transport_<From>_to_<To>.xml describes one zeppelin/boat leg via a
`CustomBehavior File="UseTransport"`:
  TransportId    the MO_TRANSPORT gameobject (e.g. 164871 = zeppelin Thundercaller)
  WaitAt*        dock coord to stand at while waiting for the transport to arrive
  TransportStart* transport GO position when docked at departure
  StandOn*       coord to stand ON the transport (boarding point)
  TransportEnd*  transport GO position at the destination dock
  GetOff*        coord to step off to at the destination
plus RunTo steps to/from the docks.

The module's board behavior will: walk to WaitAt, wait until the transport GO is
near TransportStart, move onto StandOn, attach as a passenger (SetTransport +
AddPassenger -- a bot has no client to trigger this the normal way, which is why
bots currently get displaced by moving transports), ride until the GO reaches
TransportEnd, then detach and move to GetOff.

Every TransportId is validated against acore_world: it must exist and be a
MO_TRANSPORT (gameobject_template.type=15). External coords are used only for a
validated transport. Emits a normalized JSON table (transport-routes.json).
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path


def dbpw():
    env = subprocess.run("docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}'",
                         shell=True, capture_output=True, text=True).stdout
    return next((l.split("=", 1)[1] for l in env.splitlines() if l.startswith("MYSQL_ROOT_PASSWORD=")), "")


def db_transports(entries, pw):
    if not entries:
        return {}
    ids = ",".join(str(int(e)) for e in entries)
    out = subprocess.run(["docker", "exec", "ac-database", "mysql", "-uroot", f"-p{pw}", "-N", "-e",
                          f"SELECT entry,type,name FROM acore_world.gameobject_template WHERE entry IN ({ids});"],
                         capture_output=True, text=True).stdout
    res = {}
    for line in out.splitlines():
        p = line.split("\t")
        if len(p) == 3:
            res[int(p[0])] = {"type": int(p[1]), "name": p[2]}
    return res


def parse_transport(path: Path) -> dict | None:
    root = ET.parse(path).getroot()
    ut = root.find(".//CustomBehavior[@File='UseTransport']")
    if ut is None:
        return None

    def f(attr):
        v = ut.get(attr)
        return round(float(v), 3) if v is not None else None
    runtos = [(round(float(r.get("X")), 3), round(float(r.get("Y")), 3), round(float(r.get("Z")), 3))
              for r in root.findall(".//RunTo")]
    name_el = root.find("Name")
    return {
        "name": name_el.text if name_el is not None else path.stem,
        "transport_id": int(ut.get("TransportId")),
        "wait_at": [f("WaitAtX"), f("WaitAtY"), f("WaitAtZ")],
        "transport_start": [f("TransportStartX"), f("TransportStartY"), f("TransportStartZ")],
        "stand_on": [f("StandOnX"), f("StandOnY"), f("StandOnZ")],
        "transport_end": [f("TransportEndX"), f("TransportEndY"), f("TransportEndZ")],
        "get_off": [f("GetOffX"), f("GetOffY"), f("GetOffZ")],
        "run_to_before": runtos[0] if runtos else None,
        "run_to_after": runtos[-1] if len(runtos) > 1 else None,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profiles-root", type=Path, required=True)
    ap.add_argument("--output", type=Path, default=Path(__file__).with_name("transport_routes.json"))
    a = ap.parse_args()

    files = glob.glob(os.path.join(str(a.profiles_root), "Profile v3", "Transport", "*", "*.xml"))
    routes, ids = [], set()
    for fp in sorted(files):
        r = parse_transport(Path(fp))
        if r:
            routes.append(r)
            ids.add(r["transport_id"])

    pw = dbpw()
    tmpl = db_transports(ids, pw)
    validated, dropped = [], []
    for r in routes:
        t = tmpl.get(r["transport_id"])
        if t and t["type"] == 15:  # GAMEOBJECT_TYPE_MO_TRANSPORT
            r["transport_name"] = t["name"]
            validated.append(r)
        else:
            dropped.append((r["name"], r["transport_id"], t["type"] if t else "missing"))

    a.output.write_text(json.dumps({"schema_version": 1, "routes": validated}, indent=2) + "\n")
    print(f"parsed {len(routes)} transport profiles; {len(validated)} DB-validated MO_TRANSPORTs, "
          f"{len(dropped)} dropped")
    for r in validated:
        print(f"  {r['name']:44s} id={r['transport_id']:<7} ({r['transport_name']})")
    for name, tid, why in dropped:
        print(f"  DROPPED {name} id={tid} (type={why})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
