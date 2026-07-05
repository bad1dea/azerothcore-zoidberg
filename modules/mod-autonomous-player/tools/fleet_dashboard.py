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
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>AP Fleet</title>
<style>
:root{--bg:#0b0e14;--panel:#141925;--panel2:#1b2233;--line:#273249;--txt:#e6ecf5;
--mut:#8b97ad;--acc:#5b9dff;--ok:#3ddc84;--warn:#ffb454;--bad:#ff5c72;--ghost:#b48cff;}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
background:linear-gradient(180deg,#0b0e14,#0e131d);color:var(--txt);
padding-bottom:env(safe-area-inset-bottom);}
header{position:sticky;top:0;z-index:5;background:rgba(11,14,20,.92);backdrop-filter:blur(10px);
border-bottom:1px solid var(--line);padding:12px 16px calc(12px + env(safe-area-inset-top))}
.hrow{display:flex;align-items:center;gap:12px;flex-wrap:wrap;max-width:960px;margin:0 auto}
h1{font-size:16px;margin:0;font-weight:650}h1 .dot{color:var(--acc)}
.chips{display:flex;gap:8px;flex-wrap:wrap;margin-left:auto}
.chip{background:var(--panel);border:1px solid var(--line);border-radius:999px;
padding:5px 11px;font-size:12px;color:var(--mut);white-space:nowrap}
.chip b{color:var(--txt);font-weight:650}.chip.good b{color:var(--ok)}.chip.bad b{color:var(--bad)}
.meta{font-size:11px;color:var(--mut);width:100%;max-width:960px;margin:6px auto 0}
main{max-width:960px;margin:0 auto;padding:12px 12px 40px}
.row{background:var(--panel);border:1px solid var(--line);border-left:3px solid var(--line);
border-radius:12px;margin-bottom:8px;overflow:hidden}
.row.hit{border-left-color:var(--ok)} .row.down{border-left-color:var(--bad)}
.head{display:flex;align-items:center;gap:10px;padding:12px 14px;cursor:pointer;
min-height:52px;-webkit-tap-highlight-color:transparent;user-select:none}
.head:hover{background:#171d2c}
.caret{color:var(--mut);font-size:12px;transition:transform .15s;flex:none;width:12px}
.row.open .caret{transform:rotate(90deg)}
.sdot{width:9px;height:9px;border-radius:50%;flex:none}
.d-ok{background:var(--ok)}.d-fight{background:var(--warn)}.d-ghost{background:var(--ghost)}
.d-dead{background:var(--bad)}.d-off{background:#556}
.nm{font-size:16px;font-weight:650;flex:none}
.cl{font-size:12px;color:var(--mut);flex:none}
.lvlpill{font-size:13px;font-weight:650;background:var(--panel2);border-radius:8px;padding:3px 9px;flex:none}
.lvlpill .t{color:var(--mut);font-weight:400}
.spacer{flex:1 1 auto;min-width:8px}
.inline{display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end}
.tag{font-size:11px;color:var(--mut);background:var(--panel2);border-radius:6px;padding:3px 7px;white-space:nowrap}
.tag b{color:var(--txt)}.tag.warn b{color:var(--warn)}.tag.bad b{color:var(--bad)}.tag.ok b{color:var(--ok)}
.body{display:none;padding:0 14px 14px;border-top:1px solid var(--line)}
.row.open .body{display:block}
.bar{height:7px;background:var(--panel2);border-radius:7px;overflow:hidden;margin:12px 0}
.bar>i{display:block;height:100%;background:linear-gradient(90deg,#3b7bd6,#5b9dff)}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.stat{background:var(--panel2);border-radius:8px;padding:8px 10px}
.stat .k{font-size:10px;color:var(--mut);text-transform:uppercase;letter-spacing:.4px}
.stat .v{font-size:16px;font-weight:650;margin-top:2px}
.stat .v.warn{color:var(--warn)}.stat .v.bad{color:var(--bad)}.stat .v.ok{color:var(--ok)}
.minibar{height:4px;background:#2a3346;border-radius:4px;margin-top:6px;overflow:hidden}
.minibar>i{display:block;height:100%}
.act{font-size:12.5px;color:var(--mut);margin-top:10px}.act b{color:#b3c1da;font-weight:600}
h4{margin:12px 0 5px;font-size:11px;color:var(--acc);text-transform:uppercase;letter-spacing:.5px}
.logbox{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:11.5px;color:#95a2ba;
background:#0c1017;border:1px solid var(--line);border-radius:8px;padding:9px;max-height:260px;
overflow:auto;white-space:pre-wrap;word-break:break-word;-webkit-overflow-scrolling:touch}
.lh{font-size:11.5px;color:var(--mut);line-height:1.8}
.stale{color:var(--warn)}
@media(max-width:640px){
 .cl{display:none}.spacer{flex-basis:100%;height:0}
 .inline{justify-content:flex-start;width:100%}
 .head{flex-wrap:wrap}.grid{grid-template-columns:repeat(2,1fr)}
}
</style></head><body>
<header>
 <div class="hrow"><h1><span class="dot">&#9679;</span> AP Fleet</h1><div class="chips" id="chips"></div></div>
 <div class="meta" id="meta"></div>
</header>
<main id="list"></main>
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
function state(r){if(!r.online)return["OFFLINE","d-off","down"];if(r.ghost)return["ghost","d-ghost","down"];
 if(r.alive===false)return["dead","d-dead","down"];if(r.combat)return["fighting","d-fight",""];return["roaming","d-ok",""];}
function row(r){
 const lvl=r.level??"?",tgt=r.target??"?",hit=Number.isInteger(lvl)&&Number.isInteger(tgt)&&lvl>=tgt;
 const [st,sd,dcl]=state(r);
 const bu=r.bags&&String(r.bags).includes("/")?String(r.bags).split("/").map(Number):null;
 const bpct=bu?Math.round(bu[0]/bu[1]*100):0, bfull=bu&&(bu[1]-bu[0])<=2;
 const lvlpct=hit?100:(Number.isInteger(lvl)&&Number.isInteger(tgt)?Math.round(lvl/tgt*100):0);
 const d15=r.deaths_15m??0, stale=(r.runner_log_mtime||0)>900, op=open.has(r.name);
 return `<div class="row ${hit?'hit':''} ${dcl} ${op?'open':''}" data-n="${esc(r.name)}">
  <div class="head">
   <span class="caret">&#9654;</span><span class="sdot ${sd}"></span>
   <span class="nm">${esc(r.name)}</span><span class="cl">${esc(cls(r.route))}</span>
   <span class="lvlpill">${lvl}<span class="t"> / ${tgt}</span>${hit?' &#10003;':''}</span>
   <span class="spacer"></span>
   <div class="inline">
    <span class="tag">ilvl <b>${r.ilvl??'?'}</b></span>
    <span class="tag ${d15>=5?'bad':d15>=3?'warn':''}">d/15m <b>${d15}</b></span>
    <span class="tag ${bfull?'bad':''}">bags <b>${esc(r.bags??'?')}</b></span>
    <span class="tag">q <b>${r.quests_done??'?'}</b></span>
   </div>
  </div>
  <div class="body">
   <div class="bar"><i style="width:${lvlpct}%"></i></div>
   <div class="grid">
    <div class="stat"><div class="k">level</div><div class="v">${lvl}/${tgt}</div></div>
    <div class="stat"><div class="k">state</div><div class="v">${st}</div></div>
    <div class="stat"><div class="k">hp</div><div class="v">${esc(r.hp??'?')}</div></div>
    <div class="stat"><div class="k">ilvl</div><div class="v">${r.ilvl??'?'}</div></div>
    <div class="stat"><div class="k">quests done</div><div class="v">${r.quests_done??'?'}</div></div>
    <div class="stat"><div class="k">deaths 15m</div><div class="v ${d15>=5?'bad':d15>=3?'warn':''}">${d15}</div></div>
    <div class="stat"><div class="k">bags</div><div class="v ${bfull?'bad':''}">${esc(r.bags??'?')}</div>
     <div class="minibar"><i style="width:${bpct}%;background:${bfull?'var(--bad)':'var(--acc)'}"></i></div></div>
    <div class="stat"><div class="k">deaths total</div><div class="v">${r.deaths??'?'}</div></div>
    <div class="stat"><div class="k">unsticks</div><div class="v">${r.unsticks??'?'}</div></div>
   </div>
   <div class="act">doing <b>${esc(r.current_segment||'?')}</b> &middot; segs ${r.segments_done??'?'}/${r.segments_total??'?'}
    &middot; last ding ${r.mins_since_level!=null?(r.mins_since_level+'m ago'):'—'} &middot; map ${r.map??'?'} ${esc(r.pos||'')}</div>
   <div class="act ${stale?'stale':''}">${stale?('[stale '+r.runner_log_mtime+'s] '):''}${esc(r.last_log||'')}</div>
   <h4>recent activity</h4>
   <div class="logbox">${(r.recent_log||[]).map(esc).join("\n")||"(no log)"}</div>
   <h4>level history</h4>
   <div class="lh">${(r.level_history||[]).map(h=>`L${h.level} @ ${esc(h.t||'')}`).join("<br>")||"—"}</div>
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
  `<span class="chip good"><b>${hit}</b>/${rows.length} target</span>`+
  `<span class="chip"><b>${al}</b>&#9679; <span style="color:var(--ghost)">${gh}</span>&#9679; <span style="color:var(--bad)">${dd}</span>&#9679;</span>`+
  `<span class="chip ${d15>=10?'bad':''}"><b>${d15}</b> d/15m</span>`+
  `<span class="chip">ilvl~<b>${avil}</b></span>`;
 document.getElementById("meta").innerHTML="updated "+esc(d.generated)+(d.soap_error?` &middot; <span style="color:var(--bad)">SOAP: ${esc(d.soap_error)}</span>`:"");
 rows.sort((a,b)=>{const da=(!a.online||a.ghost||a.alive===false),db=(!b.online||b.ghost||b.alive===false);
   if(da!==db)return da?-1:1; return (b.level||0)-(a.level||0)||String(a.name).localeCompare(String(b.name));});
 document.getElementById("list").innerHTML=rows.map(row).join("");
 document.querySelectorAll(".row .head").forEach(h=>h.onclick=()=>{
   const c=h.parentElement,n=c.dataset.n; open.has(n)?open.delete(n):open.add(n); c.classList.toggle("open");});
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
