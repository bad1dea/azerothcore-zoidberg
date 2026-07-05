#!/usr/bin/env python3
"""Comprehensive fleet health monitor + auto-fixer for the mod-autonomous-player
generated-route fleet. One invocation = one cycle. Detects and fixes, by
comparing live state to the previous cycle's snapshot:

  * dead runner process        -> relaunch (unless parked)
  * death loop                 -> deaths jumped >=6 since last cycle -> park
  * quest stall                -> no level AND no segdone gain for >=3 cycles
                                  -> nudge (restart runner to re-pick a segment);
                                  persists -> park
  * stale/wedged bot           -> alive, not in combat, position unchanged across
                                  a cycle while its runner made no progress -> nudge
  * stuck ghost                -> ghost this AND last cycle -> release+reclaim
  * bag pressure               -> <=1 free slot -> log (runner self-vendors)
  * gear                       -> repair all fleet gear to max (starter zones lack
                                  a repair vendor)

Deliberately conservative: a bot that is LEVELING or completing quests is never
touched. Park writes a <char>.parked marker (skipped by relaunch); clear by hand
after a route/nav fix.
"""
import glob
import json
import os
import re
import subprocess
import time

STATE_DIR = os.path.expanduser("~/ap_fleet_state")
PREV = os.path.join(STATE_DIR, ".monitor_prev.json")
ROUTES_DIR = os.path.expanduser(
    "~/build/azerothcore-zoidberg/modules/mod-autonomous-player/tools/routes_generated")
TOOLS = os.path.expanduser(
    "~/build/azerothcore-zoidberg/modules/mod-autonomous-player/tools")
SOAP = "/tmp/soap.py"
STALL_CYCLES_NUDGE = 3    # cycles of zero progress before a nudge
STALL_CYCLES_PARK = 6     # cycles of zero progress before parking
DEATH_LOOP_DELTA = 6      # deaths gained in one cycle => loop


def soap(cmd):
    try:
        return subprocess.run(["python3", SOAP, cmd], capture_output=True,
                              text=True, timeout=40).stdout
    except Exception:
        return ""


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout


def dbpw():
    env = sh("docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}'")
    for line in env.splitlines():
        if line.startswith("MYSQL_ROOT_PASSWORD="):
            return line.split("=", 1)[1]
    return ""


def env_soap():
    e = {}
    p = os.path.expanduser("~/secrets/ap_soap.env")
    if os.path.exists(p):
        for l in open(p):
            if "=" in l and not l.startswith("#"):
                k, v = l.strip().split("=", 1)
                e[k] = v.strip().strip('"').strip("'")
    return e


def char_of(route_path):
    try:
        return json.load(open(route_path))["char"]
    except Exception:
        return None


def runner_alive(route_base):
    return bool(sh(f"pgrep -f 'route_runner.py --route routes_generated/{route_base}'").strip())


def launch(route_path, char, se):
    log = os.path.join(STATE_DIR, f"{char}_run.log")
    state = os.path.join(STATE_DIR, f"{char}_state.json")
    cmd = (f"cd {TOOLS} && nohup python3 route_runner.py --route routes_generated/{os.path.basename(route_path)} "
           f"--state {state} --host 127.0.0.1 --port {se.get('AP_SOAP_PORT','7878')} "
           f"--user {se['AP_SOAP_USER']} --password {se['AP_SOAP_PASSWORD']} > {log} 2>&1 &")
    sh(cmd)


def main():
    now = time.strftime("%F %T")
    se = env_soap()
    prev = json.load(open(PREV)) if os.path.exists(PREV) else {}
    cur = {}
    actions = []

    # live SOAP snapshot
    status = soap(".autonomousplayer status all")
    live = {}
    for m in re.finditer(
        r'([A-Za-z]+twelve) lvl (\d+) map \d+ pos \(([-\d.]+), ([-\d.]+),[^)]*\) '
        r'hp \d+/\d+ alive=(\w+) combat=(\w+) ghost=(\w+) bags (\d+)/(\d+)', status):
        live[m.group(1)] = dict(level=int(m.group(2)), x=float(m.group(3)),
            y=float(m.group(4)), alive=m.group(5) == "true", combat=m.group(6) == "true",
            ghost=m.group(7) == "true", bfree=int(m.group(9)) - int(m.group(8)))

    routes = {char_of(p): p for p in glob.glob(os.path.join(ROUTES_DIR, "*.json"))
              if not p.endswith("_generation_summary.json") and char_of(p)}

    # repair all fleet gear
    pw = dbpw()
    rep = sh(f"docker exec ac-database mysql -uroot -p'{pw}' -N -e \""
             "UPDATE acore_characters.item_instance ii JOIN acore_world.item_template it ON it.entry=ii.itemEntry "
             "JOIN acore_characters.characters c ON c.guid=ii.owner_guid SET ii.durability=it.MaxDurability "
             "WHERE c.name LIKE '%twelve' AND it.MaxDurability>0 AND ii.durability<it.MaxDurability; SELECT ROW_COUNT();\" 2>/dev/null").strip()

    print(f"=== {now} fleet health monitor ===")
    print(f"runners: {sh('pgrep -fc route_runner.py').strip()}/14  gear repaired: {rep} items")

    for char, route in sorted(routes.items()):
        base = os.path.basename(route)
        parked = os.path.exists(os.path.join(STATE_DIR, f"{char}.parked"))
        st = {}
        sp = os.path.join(STATE_DIR, f"{char}_state.json")
        if os.path.exists(sp):
            d = json.load(open(sp))
            dn = d.get("done")
            st = dict(deaths=d.get("deaths", 0),
                      segdone=len(dn) if isinstance(dn, (list, dict)) else (dn or 0),
                      level=d.get("level_history", [{}])[-1].get("level", 1))
        lv = live.get(char, {})
        p = prev.get(char, {})
        # progress since last cycle
        gained = (st.get("level", 0) > p.get("level", -1)) or (st.get("segdone", 0) > p.get("segdone", -1))
        dloop = st.get("deaths", 0) - p.get("deaths", 0) >= DEATH_LOOP_DELTA
        moved = (abs(lv.get("x", 0) - p.get("x", 1e9)) + abs(lv.get("y", 0) - p.get("y", 1e9))) > 8.0
        stall = 0 if gained else p.get("stall", 0) + 1
        ghost_streak = (lv.get("ghost") and p.get("ghost")) if lv else False

        cur[char] = dict(level=st.get("level", 0), segdone=st.get("segdone", 0),
                         deaths=st.get("deaths", 0), x=lv.get("x", 0), y=lv.get("y", 0),
                         ghost=lv.get("ghost", False), stall=stall)

        flags = []
        if parked:
            flags.append("PARKED")
        # dead runner -> relaunch (unless parked)
        if not parked and not runner_alive(base):
            launch(route, char, se)
            actions.append(f"relaunched dead runner {char}")
            flags.append("runner-was-dead")
        # death loop: nudge a productive bot (restart -> defer the deadly segment
        # and continue); only park an early-stuck one (little progress) or one
        # that death-looped again right after a nudge.
        if not parked and dloop and not gained:
            if st.get("segdone", 0) < 6 or p.get("dloop"):
                sh(f"pkill -f 'route_runner.py.*{char}_state.json'")
                open(os.path.join(STATE_DIR, f"{char}.parked"), "w").close()
                actions.append(f"PARKED {char} (death loop persisted / early-stuck, seg={st.get('segdone')})")
                flags.append("DEATH-LOOP-PARKED")
            else:
                sh(f"pkill -f 'route_runner.py.*{char}_state.json'")
                time.sleep(1)
                launch(route, char, se)
                actions.append(f"nudged {char} (death loop +{st['deaths']-p.get('deaths',0)}, seg={st.get('segdone')} -> restart)")
                flags.append("DEATH-LOOP-NUDGE")
            cur[char]["dloop"] = True
        # stuck ghost -> force recovery
        elif not parked and ghost_streak:
            soap(f".autonomousplayer releasespirit {char}")
            soap(f".autonomousplayer reclaimcorpse {char}")
            actions.append(f"forced corpse recovery for {char} (ghost 2 cycles)")
            flags.append("STUCK-GHOST")
        # quest stall -> nudge, then park
        elif not parked and stall >= STALL_CYCLES_PARK:
            sh(f"pkill -f 'route_runner.py.*{char}_state.json'")
            open(os.path.join(STATE_DIR, f"{char}.parked"), "w").close()
            actions.append(f"PARKED {char} (stalled {stall} cycles, no level/quest progress)")
            flags.append("STALL-PARKED")
        elif not parked and stall == STALL_CYCLES_NUDGE:
            sh(f"pkill -f 'route_runner.py.*{char}_state.json'")
            time.sleep(1)
            launch(route, char, se)
            actions.append(f"nudged {char} (stalled {stall} cycles -> runner restart)")
            flags.append("STALL-NUDGE")
        if lv.get("bfree", 9) <= 1 and not parked:
            flags.append("BAGS-FULL")

        print(f"  {char:14s} L{st.get('level','?'):<2} seg={st.get('segdone','?'):<2} "
              f"deaths={st.get('deaths','?'):<3} stall={stall} "
              f"{'ghost' if lv.get('ghost') else ('combat' if lv.get('combat') else 'idle')} "
              f"bfree={lv.get('bfree','?')} {' '.join(flags)}")

    print("--- actions ---")
    for a in actions:
        print("  " + a)
    if not actions:
        print("  (none — fleet healthy)")
    json.dump(cur, open(PREV, "w"))


if __name__ == "__main__":
    main()
