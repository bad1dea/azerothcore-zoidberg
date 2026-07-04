#!/usr/bin/env python3
"""Generate a detailed per-zone/route/quest report from the live route
files, enriched with DB quest titles and creature names/levels
(pre-fetched to /tmp/rep_quests.tsv and /tmp/rep_creatures.tsv). Output
is a single markdown doc for deep-dive review -- it reflects exactly what
the runner will execute, in order.
"""
import glob
import json


def load_tsv(path):
    out = {}
    for ln in open(path, errors="ignore"):
        ln = ln.rstrip("\n")
        if "\t" not in ln:
            continue
        k, v = ln.split("\t", 1)
        out[k] = v
    return out


QUESTS = load_tsv("/tmp/rep_quests.tsv")      # id -> "Title|QuestLevel"
CREAT = load_tsv("/tmp/rep_creatures.tsv")    # entry -> "name|min-max|rank"


def q_title(qid):
    v = QUESTS.get(str(qid), "?|?")
    t, lvl = v.split("|", 1)
    return f'"{t}" (QL{lvl})'


def mob(entry):
    v = CREAT.get(str(entry))
    if not v:
        return f"{entry}"
    name, lvl, rank = v.split("|", 2)
    tag = " ELITE" if rank not in ("0", "") else ""
    return f"{name} [{entry}] L{lvl}{tag}"


# route file -> (zone label, class/race)
ZONE = {
    "durotar_orc_warrior_1_12": ("Durotar + Barrens", "Orc Warrior"),
    "durotar_troll_hunter_1_12": ("Durotar + Barrens", "Troll Hunter"),
    "durotar_orc_warlock_1_12": ("Durotar + Barrens", "Orc Warlock"),
    "mulgore_tauren_shaman_1_10": ("Mulgore", "Tauren Shaman"),
    "mulgore_tauren_druid_1_10": ("Mulgore", "Tauren Druid"),
    "mulgore_tauren_warrior_1_10": ("Mulgore", "Tauren Warrior"),
    "tirisfal_undead_rogue_1_10": ("Tirisfal Glades", "Undead Rogue"),
    "tirisfal_undead_priest_1_10": ("Tirisfal Glades", "Undead Priest"),
    "eversong_belf_paladin_1_8": ("Eversong Woods", "Blood Elf Paladin"),
    "eversong_belf_hunter_1_8": ("Eversong Woods", "Blood Elf Hunter"),
    "elwynn_human_warrior_1_8": ("Elwynn Forest", "Human Warrior"),
    "elwynn_human_mage_1_8": ("Elwynn Forest", "Human Mage"),
    "dunmorogh_dwarf_warrior_1_8": ("Dun Morogh", "Dwarf Warrior"),
    "dunmorogh_gnome_mage_1_8": ("Dun Morogh", "Gnome Mage"),
}


def seg_line(s):
    t = s["type"]
    sid = s["id"]
    gate = []
    if s.get("min_level"):
        gate.append(f"min L{s['min_level']}")
    if s.get("requires_quest"):
        gate.append(f"needs q{s['requires_quest']}")
    if s.get("optional"):
        gate.append("optional")
    g = f"  ({', '.join(gate)})" if gate else ""

    if t == "quest_accept":
        return f"- **accept** q{s['quest']} {q_title(s['quest'])} from {mob(s.get('giver'))}{g}"
    if t == "quest_turnin":
        return f"- **turn-in** q{s['quest']} {q_title(s['quest'])} to {mob(s.get('turnin'))}{g}"
    if t == "quest_grind":
        kills = s.get("kill_entries") or (
            [{"entry": s["kill_entry"]}] if s.get("kill_entry") else [])
        ke = ", ".join(sorted({mob(k["entry"]) for k in kills})) or "?"
        return (f"- **quest-grind** q{s['quest']} {q_title(s['quest'])}{g}\n"
                f"    - kill/collect from: {ke}\n"
                f"    - giver/turn-in: {mob(s.get('giver'))} / {mob(s.get('turnin'))}")
    if t == "grind_to_level":
        return (f"- **grind → L{s.get('level')}** on {mob(s.get('entry'))}"
                f" ({len(s.get('points', []) or [1])} anchor(s)){g}")
    if t == "walk":
        hops = s.get("hops", [])
        return f"- **walk** [{sid}] {len(hops)} hop(s) → {hops[-1][:2] if hops else '?'}{g}"
    if t == "sell":
        return f"- **vendor/sell+repair** at {mob(s.get('vendor'))}{g}"
    if t == "train":
        return f"- **train** (learn available spells){g}"
    return f"- {t} [{sid}]{g}"


def main():
    out = ["# mod-autonomous-player — Zones, Quests & Routing (deep-dive report)",
           "",
           "Generated from the live route JSONs (`tools/routes/*.json`) — this is"
           " exactly what the orchestrator executes, in order. Quest titles and"
           " creature levels are from `acore_world`. Segments are DEFERRED (not"
           " skipped) when a gate (min-level / prereq) isn't met, and re-tried on"
           " later passes; grind_to_level segments no-op when the bot is already"
           " at/above the target level.",
           ""]

    # group routes by zone
    routes = sorted(glob.glob("routes/*.json"))
    byzone = {}
    for p in routes:
        stem = p.split("/")[-1].replace(".json", "")
        if stem not in ZONE:
            continue
        r = json.load(open(p))
        if r.get("char") == "CHANGEME":
            byzone.setdefault(ZONE[stem][0], []).append((stem, r, True))
        else:
            byzone.setdefault(ZONE[stem][0], []).append((stem, r, False))

    # summary table
    out.append("## Fleet summary\n")
    out.append("| Route | Class | Char | Target | Map | Segments | Quests | Grinds |")
    out.append("|---|---|---|---|---|---|---|---|")
    for stem in ZONE:
        p = f"routes/{stem}.json"
        try:
            r = json.load(open(p))
        except Exception:
            continue
        segs = r["segments"]
        nq = len([s for s in segs if s["type"] in ("quest_accept", "quest_turnin", "quest_grind")])
        ng = len([s for s in segs if s["type"] == "grind_to_level"])
        target = stem.rsplit("_", 1)[-1]
        out.append(f"| {stem} | {ZONE[stem][1]} | {r.get('char')} | {target} | "
                   f"{r.get('map','?')} | {len(segs)} | {nq} | {ng} |")
    out.append("")

    # per-route detail
    for stem in ZONE:
        p = f"routes/{stem}.json"
        try:
            r = json.load(open(p))
        except Exception:
            continue
        zone, cls = ZONE[stem]
        out.append(f"\n## {zone} — {cls}  (`{stem}`)\n")
        out.append(f"- **Character:** {r.get('char')}  ·  **Map:** {r.get('map','?')}"
                   f"  ·  **Opportunistic spell:** {r.get('opportunistic_spell')}"
                   f"  ·  **Heal spell:** {r.get('heal_spell', '—')}")
        if r.get("comment"):
            out.append(f"- **Author notes:** {r['comment'][:400]}")
        out.append(f"- **Segments ({len(r['segments'])}), in execution order:**\n")
        for s in r["segments"]:
            out.append(seg_line(s))
        out.append("")

    open("../../../docs/autonomous-player/ROUTES_REPORT.md", "w").write("\n".join(out))
    print("wrote docs/autonomous-player/ROUTES_REPORT.md",
          f"({len(out)} lines)")


if __name__ == "__main__":
    main()
