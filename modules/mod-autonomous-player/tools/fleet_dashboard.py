#!/usr/bin/env python3
"""Live fleet dashboard for route_runner bots.

Aggregates three real sources into one auto-refreshing page:
  - the worldserver's live `.autonomousplayer status` over SOAP
    (level, position, hp, alive/ghost -- ground truth),
  - each runner's state JSON (deaths, unsticks, segments done,
    level history),
  - each runner's log tail (what it's doing right now).

Stdlib only, same SOAP mechanism as live_regression_suite. Serves
plain HTML + a JSON endpoint; no external assets.

Usage:
    python3 fleet_dashboard.py --state-dir /path/to/scratchpad \
        --host 10.10.30.20 --user SOAPADMIN --password ... [--port 8899]
"""

from __future__ import annotations

import argparse
import glob
import html
import json
import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live_regression_suite import Config, soap_command  # noqa: E402

ROUTES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "routes")

CACHE = {"ts": 0.0, "fleet": []}
CACHE_LOCK = threading.Lock()
CFG = None
STATE_DIR = None


def route_index():
    idx = {}
    for path in glob.glob(os.path.join(ROUTES_DIR, "*.json")):
        try:
            with open(path) as f:
                r = json.load(f)
        except Exception:
            continue
        char = r.get("char", "")
        if char and char != "CHANGEME":
            target = 0
            for s in r.get("segments", []):
                if s.get("type") == "grind_to_level":
                    target = max(target, s.get("level", 0))
            idx[char] = {"route": os.path.basename(path), "target": target,
                         "segments": len(r.get("segments", []))}
    return idx


def collect():
    fleet = {}
    routes = route_index()
    # 1. live SOAP status
    try:
        out = soap_command(CFG, ".autonomousplayer status", timeout=15.0)
    except Exception as exc:
        out = ""
        fleet["_error"] = str(exc)
    for m in re.finditer(
            r"(\w+) lvl (\d+) map (\d+) pos \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)"
            r" hp (\d+)/(\d+) alive=(\w+) combat=(\w+) ghost=(\w+)"
            r"(?: bags (\d+)/(\d+) ilvl (\d+) quests (\d+))?", out):
        fleet[m.group(1)] = {
            "name": m.group(1), "level": int(m.group(2)), "map": int(m.group(3)),
            "pos": f"({float(m.group(4)):.0f}, {float(m.group(5)):.0f}, {float(m.group(6)):.0f})",
            "hp": f"{m.group(7)}/{m.group(8)}",
            "alive": m.group(9) == "true", "combat": m.group(10) == "true",
            "ghost": m.group(11) == "true", "online": True,
            "bags": f"{m.group(12)}/{m.group(13)}" if m.group(12) else "?",
            "bag_free": (int(m.group(13)) - int(m.group(12))) if m.group(12) else None,
            "ilvl": m.group(14) or "?",
            "quests_done": m.group(15) or "?",
        }
    # 2. runner state + 3. log tail
    for spath in glob.glob(os.path.join(STATE_DIR, "*twelve_state.json")):
        base = os.path.basename(spath).replace("_state.json", "")
        char = None
        try:
            with open(spath) as f:
                st = json.load(f)
        except Exception:
            continue
        for name in list(fleet) + [base.capitalize()]:
            if isinstance(name, str) and name.lower() == base:
                char = name
                break
        char = char or base.capitalize()
        entry = fleet.setdefault(char, {"name": char, "online": False})
        entry["deaths"] = st.get("deaths", 0)
        entry["unsticks"] = st.get("unsticks", 0)
        entry["segments_done"] = len(st.get("done", []))
        hist = st.get("level_history", [])
        if hist:
            entry["last_level_at"] = hist[-1].get("t", "")
            entry.setdefault("level", hist[-1].get("level"))
        lpath = os.path.join(STATE_DIR, base + "_run.log")
        try:
            with open(lpath, "rb") as f:
                f.seek(0, 2)
                f.seek(max(0, f.tell() - 4000))
                lines = f.read().decode(errors="replace").strip().splitlines()
            entry["last_log"] = lines[-1][-120:] if lines else ""
            entry["runner_log_mtime"] = int(time.time() - os.path.getmtime(lpath))
        except Exception:
            entry["last_log"] = "(no log)"
        meta = routes.get(char, {})
        entry["route"] = meta.get("route", "?")
        entry["target"] = meta.get("target", "?")
        entry["segments_total"] = meta.get("segments", "?")
    err = fleet.pop("_error", None)
    rows = sorted((v for v in fleet.values() if isinstance(v, dict)),
                  key=lambda r: str(r.get("route", "")))
    return {"rows": rows, "soap_error": err, "generated": time.strftime("%F %T")}


def get_fleet():
    with CACHE_LOCK:
        if time.time() - CACHE["ts"] > 8.0:
            CACHE["fleet"] = collect()
            CACHE["ts"] = time.time()
        return CACHE["fleet"]


PAGE = """<!doctype html><html><head><meta charset="utf-8">
<title>AP bot fleet</title>
<meta http-equiv="refresh" content="10">
<style>
 body {{ font-family: ui-monospace, monospace; background:#111; color:#ddd; margin:2em; }}
 h1 {{ font-size:1.2em; }} small {{ color:#888; }}
 table {{ border-collapse: collapse; width:100%; }}
 th, td {{ text-align:left; padding:4px 10px; border-bottom:1px solid #333; font-size:0.92em; }}
 th {{ color:#9cf; }}
 .dead {{ color:#f66; font-weight:bold; }} .combat {{ color:#fc6; }} .ok {{ color:#6f6; }}
 .target-hit {{ background:#132; }}
 .log {{ color:#999; font-size:0.85em; }}
 .stale {{ color:#f96; }}
</style></head><body>
<h1>mod-autonomous-player fleet <small>{generated} &middot; auto-refresh 10s {err}</small></h1>
<table>
<tr><th>bot</th><th>route</th><th>lvl</th><th>target</th><th>hp</th><th>state</th>
<th>bags</th><th>ilvl</th><th>quests</th>
<th>segs</th><th>deaths</th><th>unsticks</th><th>pos</th><th>runner last line</th></tr>
{rows}
</table></body></html>"""


def render():
    data = get_fleet()
    rows = []
    for r in data["rows"]:
        lvl, tgt = r.get("level", "?"), r.get("target", "?")
        hit = isinstance(lvl, int) and isinstance(tgt, int) and lvl >= tgt
        if not r.get("online"):
            state, cls = "OFFLINE", "dead"
        elif r.get("ghost"):
            state, cls = "ghost", "dead"
        elif not r.get("alive", True):
            state, cls = "dead", "dead"
        elif r.get("combat"):
            state, cls = "fighting", "combat"
        else:
            state, cls = "roaming", "ok"
        stale = r.get("runner_log_mtime", 0) > 900
        log_cls = "log stale" if stale else "log"
        log = html.escape(str(r.get("last_log", "")))
        if stale:
            log = f"[stale {r['runner_log_mtime']}s] " + log
        rows.append(
            f"<tr class='{'target-hit' if hit else ''}'>"
            f"<td>{html.escape(str(r.get('name')))}</td>"
            f"<td>{html.escape(str(r.get('route','?')).replace('.json',''))}</td>"
            f"<td>{lvl}{' &#10003;' if hit else ''}</td><td>{tgt}</td>"
            f"<td>{r.get('hp','?')}</td><td class='{cls}'>{state}</td>"
            f"<td class='{'dead' if r.get('bag_free')==0 else ''}'>{r.get('bags','?')}</td>"
            f"<td>{r.get('ilvl','?')}</td><td>{r.get('quests_done','?')}</td>"
            f"<td>{r.get('segments_done','?')}/{r.get('segments_total','?')}</td>"
            f"<td>{r.get('deaths','?')}</td><td>{r.get('unsticks','?')}</td>"
            f"<td>{r.get('pos','?')}</td><td class='{log_cls}'>{log}</td></tr>")
    err = f"&middot; <span class='dead'>SOAP: {html.escape(data['soap_error'])}</span>" \
        if data.get("soap_error") else ""
    return PAGE.format(generated=data["generated"], err=err, rows="\n".join(rows))


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/api"):
            body = json.dumps(get_fleet(), indent=1).encode()
            ctype = "application/json"
        else:
            body = render().encode()
            ctype = "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    global CFG, STATE_DIR
    p = argparse.ArgumentParser()
    p.add_argument("--state-dir", required=True)
    p.add_argument("--port", type=int, default=8899)
    p.add_argument("--host", default=os.environ.get("AP_SOAP_HOST", "127.0.0.1"))
    p.add_argument("--soap-port", type=int, default=int(os.environ.get("AP_SOAP_PORT", "7878")))
    p.add_argument("--user", default=os.environ.get("AP_SOAP_USER", ""))
    p.add_argument("--password", default=os.environ.get("AP_SOAP_PASSWORD", ""))
    a = p.parse_args()
    CFG = Config(host=a.host, port=a.soap_port, user=a.user, password=a.password,
                 bot_account="", bot_char="", creature_entry=0)
    STATE_DIR = a.state_dir
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
    print(f"fleet dashboard on http://0.0.0.0:{a.port}/ (JSON at /api)")
    srv.serve_forever()


if __name__ == "__main__":
    main()
