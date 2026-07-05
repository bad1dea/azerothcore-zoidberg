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
            entry["xp_at_last_level"] = hist[-1].get("xp", 0)
            # minutes since the last ding (from the recorded timestamp)
            try:
                t = time.mktime(time.strptime(hist[-1]["t"], "%Y-%m-%d %H:%M:%S"))
                entry["mins_since_level"] = int((time.time() - t) / 60)
            except Exception:
                pass
        entry["level_history"] = hist[-8:]
        lpath = os.path.join(STATE_DIR, base + "_run.log")
        try:
            with open(lpath, "rb") as f:
                f.seek(0, 2)
                f.seek(max(0, f.tell() - 24000))
                lines = f.read().decode(errors="replace").strip().splitlines()
            entry["last_log"] = lines[-1][-140:] if lines else ""
            entry["recent_log"] = [ln[-160:] for ln in lines[-24:]]
            entry["runner_log_mtime"] = int(time.time() - os.path.getmtime(lpath))
            # deaths in the last 15 min (real timestamp parse)
            cut = time.strftime("%H:%M:%S", time.localtime(time.time() - 900))
            now = time.strftime("%H:%M:%S")
            d15 = 0
            cur_seg = None
            for ln in lines:
                m = re.match(r"\[(\d\d:\d\d:\d\d)\]", ln)
                if not m:
                    continue
                if "death #" in ln and cut <= m.group(1) <= now:
                    d15 += 1
                sm = re.search(r"=== segment \[([^\]]+)\]", ln)
                if sm:
                    cur_seg = sm.group(1)
            entry["deaths_15m"] = d15
            entry["current_segment"] = cur_seg or "?"
        except Exception:
            entry["last_log"] = "(no log)"
            entry["recent_log"] = []
        meta = routes.get(char, {})
        entry["route"] = meta.get("route", "?")
        entry["target"] = meta.get("target", "?")
        entry["segments_total"] = meta.get("segments", "?")
    err = fleet.pop("_error", None)
    # Only the managed fleet (has a route or a runner state file); drops
    # stray SOAP-registered bots like Deathtestbot / test fixtures.
    fleet_names = set(routes) | {
        os.path.basename(p).replace("_state.json", "").capitalize()
        for p in glob.glob(os.path.join(STATE_DIR, "*twelve_state.json"))}
    rows = sorted((v for v in fleet.values()
                   if isinstance(v, dict) and v.get("name") in fleet_names),
                  key=lambda r: str(r.get("route", "")))
    return {"rows": rows, "soap_error": err, "generated": time.strftime("%F %T")}


def get_fleet():
    with CACHE_LOCK:
        if time.time() - CACHE["ts"] > 8.0:
            CACHE["fleet"] = collect()
            CACHE["ts"] = time.time()
        return CACHE["fleet"]


PAGE = r"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AP Fleet</title>
<style>
:root{--bg:#0b0e14;--panel:#141925;--panel2:#1b2233;--line:#263149;--txt:#dfe6f2;
--mut:#8b97ad;--acc:#5b9dff;--ok:#3ddc84;--warn:#ffb454;--bad:#ff5c72;--ghost:#b48cff;}
*{box-sizing:border-box}
body{margin:0;font:14px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
background:linear-gradient(180deg,#0b0e14,#0e131d);color:var(--txt);}
header{position:sticky;top:0;z-index:5;background:rgba(11,14,20,.9);backdrop-filter:blur(8px);
border-bottom:1px solid var(--line);padding:14px 22px;display:flex;align-items:center;gap:18px;flex-wrap:wrap}
h1{font-size:16px;margin:0;font-weight:650;letter-spacing:.2px}
h1 .dot{color:var(--acc)}
.chips{display:flex;gap:10px;flex-wrap:wrap;margin-left:auto}
.chip{background:var(--panel);border:1px solid var(--line);border-radius:999px;
padding:5px 12px;font-size:12px;color:var(--mut);white-space:nowrap}
.chip b{color:var(--txt);font-weight:650}
.chip.good b{color:var(--ok)} .chip.bad b{color:var(--bad)}
.meta{font-size:11px;color:var(--mut);width:100%}
main{padding:18px 22px;display:grid;gap:14px;grid-template-columns:repeat(auto-fill,minmax(340px,1fr))}
.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:14px 16px;
transition:border-color .15s, transform .05s;cursor:pointer}
.card:hover{border-color:#39496b}
.card.hit{border-color:#245c3f;background:linear-gradient(180deg,#122117,#141925)}
.card.down{border-color:#5a2733}
.top{display:flex;align-items:baseline;gap:8px}
.name{font-size:15px;font-weight:650}
.cls{font-size:11px;color:var(--mut)}
.badge{margin-left:auto;font-size:11px;font-weight:650;padding:3px 9px;border-radius:999px}
.b-ok{background:#123023;color:var(--ok)} .b-fight{background:#3a2c12;color:var(--warn)}
.b-ghost{background:#2a2140;color:var(--ghost)} .b-dead{background:#3a1620;color:var(--bad)}
.b-off{background:#222;color:var(--mut)}
.lvl{display:flex;align-items:baseline;gap:8px;margin:10px 0 4px}
.lvl .big{font-size:26px;font-weight:700;line-height:1}
.lvl .tgt{font-size:12px;color:var(--mut)}
.lvl .check{color:var(--ok)}
.bar{height:6px;background:var(--panel2);border-radius:6px;overflow:hidden;margin:4px 0 12px}
.bar>i{display:block;height:100%;background:linear-gradient(90deg,#3b7bd6,#5b9dff)}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:10px}
.stat{background:var(--panel2);border-radius:8px;padding:7px 9px}
.stat .k{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.4px}
.stat .v{font-size:15px;font-weight:650;margin-top:2px}
.stat .v.warn{color:var(--warn)} .stat .v.bad{color:var(--bad)} .stat .v.ok{color:var(--ok)}
.minibar{height:4px;background:#2a3346;border-radius:4px;margin-top:5px;overflow:hidden}
.minibar>i{display:block;height:100%}
.act{font-size:12px;color:var(--mut);margin-top:2px}
.act b{color:#aebdd6;font-weight:600}
.log{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:11px;color:#7f8ba3;
margin-top:8px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.detail{display:none;margin-top:12px;border-top:1px solid var(--line);padding-top:10px}
.card.open .detail{display:block}
.detail h4{margin:8px 0 4px;font-size:11px;color:var(--acc);text-transform:uppercase;letter-spacing:.5px}
.logbox{font-family:ui-monospace,monospace;font-size:11px;color:#93a0b8;background:#0c1017;
border:1px solid var(--line);border-radius:8px;padding:8px;max-height:230px;overflow:auto;white-space:pre-wrap}
.lh{font-size:11px;color:var(--mut)}
.stale{color:var(--warn)}
</style></head><body>
<header>
 <h1><span class="dot">&#9679;</span> mod-autonomous-player &mdash; Fleet</h1>
 <div class="chips" id="chips"></div>
 <div class="meta" id="meta"></div>
</header>
<main id="grid"></main>
<script>
const cls=n=>({durotar_orc_warrior_1_12:"Orc Warrior",durotar_troll_hunter_1_12:"Troll Hunter",
durotar_orc_warlock_1_12:"Orc Warlock",mulgore_tauren_shaman_1_10:"Tauren Shaman",
mulgore_tauren_druid_1_10:"Tauren Druid",mulgore_tauren_warrior_1_10:"Tauren Warrior",
tirisfal_undead_rogue_1_10:"Undead Rogue",tirisfal_undead_priest_1_10:"Undead Priest",
eversong_belf_paladin_1_8:"BElf Paladin",eversong_belf_hunter_1_8:"BElf Hunter",
elwynn_human_warrior_1_8:"Human Warrior",elwynn_human_mage_1_8:"Human Mage",
dunmorogh_dwarf_warrior_1_8:"Dwarf Warrior",dunmorogh_gnome_mage_1_8:"Gnome Mage"}[(n||"").replace(".json","")]||n);
const esc=s=>(s==null?"":String(s)).replace(/[&<>]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;"}[c]));
const open=new Set();
function state(r){if(!r.online)return["OFFLINE","b-off","down"];if(r.ghost)return["ghost","b-ghost","down"];
 if(r.alive===false)return["dead","b-dead","down"];if(r.combat)return["fighting","b-fight",""];return["roaming","b-ok",""];}
function card(r){
 const lvl=r.level??"?",tgt=r.target??"?",hit=Number.isInteger(lvl)&&Number.isInteger(tgt)&&lvl>=tgt;
 const [st,bcl,dcl]=state(r);
 const bu=r.bags&&r.bags.includes("/")?r.bags.split("/").map(Number):null;
 const bpct=bu?Math.round(bu[0]/bu[1]*100):0, bfull=bu&&(bu[1]-bu[0])<=2;
 const lvlpct=hit?100:(Number.isInteger(lvl)&&Number.isInteger(tgt)?Math.round(lvl/tgt*100):0);
 const d15=r.deaths_15m??0, stale=(r.runner_log_mtime||0)>900;
 const id="c_"+r.name;
 return `<div class="card ${hit?'hit':''} ${dcl} ${open.has(r.name)?'open':''}" id="${id}" data-n="${r.name}">
  <div class="top"><span class="name">${esc(r.name)}</span><span class="cls">${esc(cls(r.route))}</span>
   <span class="badge ${bcl}">${st}</span></div>
  <div class="lvl"><span class="big">${lvl}</span><span class="tgt">/ ${tgt}${hit?' <span class="check">&#10003;</span>':''}</span>
   <span class="tgt" style="margin-left:auto">${r.mins_since_level!=null?('&#9650; '+r.mins_since_level+'m ago'):''}</span></div>
  <div class="bar"><i style="width:${lvlpct}%"></i></div>
  <div class="grid">
   <div class="stat"><div class="k">ilvl</div><div class="v">${r.ilvl??'?'}</div></div>
   <div class="stat"><div class="k">quests</div><div class="v">${r.quests_done??'?'}</div></div>
   <div class="stat"><div class="k">deaths 15m</div><div class="v ${d15>=5?'bad':d15>=3?'warn':''}">${d15}</div></div>
   <div class="stat"><div class="k">bags</div><div class="v ${bfull?'bad':''}">${r.bags??'?'}</div>
    <div class="minibar"><i style="width:${bpct}%;background:${bfull?'var(--bad)':'var(--acc)'}"></i></div></div>
   <div class="stat"><div class="k">deaths tot</div><div class="v">${r.deaths??'?'}</div></div>
   <div class="stat"><div class="k">hp</div><div class="v">${esc(r.hp??'?')}</div></div>
  </div>
  <div class="act">doing: <b>${esc(r.current_segment||'?')}</b> &middot; segs ${r.segments_done??'?'}/${r.segments_total??'?'} &middot; unstk ${r.unsticks??0} &middot; map ${r.map??'?'} ${esc(r.pos||'')}</div>
  <div class="log ${stale?'stale':''}">${stale?('[stale '+r.runner_log_mtime+'s] '):''}${esc(r.last_log||'')}</div>
  <div class="detail">
   <h4>recent activity</h4>
   <div class="logbox">${(r.recent_log||[]).map(esc).join("\n")||"(no log)"}</div>
   <h4>level history</h4>
   <div class="lh">${(r.level_history||[]).map(h=>`L${h.level} @ ${esc(h.t||'')}`).join(" &middot; ")||"—"}</div>
  </div></div>`;
}
async function tick(){
 let d; try{d=await (await fetch("/api")).json();}catch(e){document.getElementById("meta").textContent="fetch error: "+e;return;}
 const rows=d.rows||[];
 const hit=rows.filter(r=>Number.isInteger(r.level)&&Number.isInteger(r.target)&&r.level>=r.target).length;
 const gh=rows.filter(r=>r.ghost).length, dd=rows.filter(r=>r.online&&r.alive===false&&!r.ghost).length;
 const al=rows.filter(r=>r.online&&r.alive!==false&&!r.ghost).length;
 const d15=rows.reduce((s,r)=>s+(r.deaths_15m||0),0);
 const il=rows.filter(r=>+r.ilvl>0); const avil=il.length?Math.round(il.reduce((s,r)=>s+ +r.ilvl,0)/il.length):0;
 document.getElementById("chips").innerHTML=
  `<span class="chip good"><b>${hit}</b>/${rows.length} at target</span>`+
  `<span class="chip"><b>${al}</b> alive &middot; <span style="color:var(--ghost)">${gh}</span> ghost &middot; <span style="color:var(--bad)">${dd}</span> dead</span>`+
  `<span class="chip ${d15>=10?'bad':''}"><b>${d15}</b> deaths/15m</span>`+
  `<span class="chip">avg ilvl <b>${avil}</b></span>`;
 document.getElementById("meta").innerHTML="updated "+esc(d.generated)+(d.soap_error?` &middot; <span style="color:var(--bad)">SOAP: ${esc(d.soap_error)}</span>`:"");
 rows.sort((a,b)=>(b.level||0)-(a.level||0)||String(a.route).localeCompare(String(b.route)));
 document.getElementById("grid").innerHTML=rows.map(card).join("");
 document.querySelectorAll(".card").forEach(c=>c.onclick=()=>{
   const n=c.dataset.n; open.has(n)?open.delete(n):open.add(n); c.classList.toggle("open");});
}
tick(); setInterval(tick,8000);
</script></body></html>"""


def render():
    return PAGE


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
