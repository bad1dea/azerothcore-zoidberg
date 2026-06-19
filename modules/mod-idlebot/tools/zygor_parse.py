#!/usr/bin/env python3
"""
zygor_parse.py — turn Zygor `ZygorLeveling*.lua` guides into idlebot quest
routes (JSON).

Zygor guides are an ordered script. We do NOT keep Zygor's zone-percent
coordinates: idlebot derives real world coords from the AzerothCore DB by id.
What we DO keep is the *route* — the order quests are accepted / turned in, the
NPC to interact with, and lightweight objective hints (kill/collect/use). The
DB-validation pass (zygor_validate.py) then confirms each id exists in 3.3.5a
and drops Cata-only content.

Output: one JSON object per RegisterGuide block:
  {
    "id": "horde__tirisfal_glades_1_11__undead_1_11",
    "title": "...\\Tirisfal Glades (1-11)\\Undead (1-11)",
    "faction": "Horde", "next": "<title or null>",
    "startlevel": 1.0, "endlevel": null,
    "conditions": ["raceclass('BloodElf') and level<=5.39", ...],
    "steps": [
      {"action":"accept","quest":8325,"npc":15278,"only":null},
      {"action":"turnin","quest":8325,"npc":15278,"only":null},
      {"action":"kill","quest":8325,"obj":1,"npc":15274,"count":8,"name":"Mana Wyrm"},
      {"action":"use","quest":8330,"obj":2,"go":220,"name":"Scroll of Scourge Magic"},
      {"action":"fpath","name":"...","only":null},
      {"action":"hearth","name":"...","only":null},
      ...
    ]
  }

Usage:
  zygor_parse.py <ZygorLevelingHordeCATA.lua> [more.lua ...] > routes.json
"""
import json
import re
import sys

# "Name##12345" / "##12345" / "12345"  -> 12345 (first numeric id)
_ID_RE = re.compile(r"##\s*(\d+)|(?<![\d#])(\d{2,})")


def first_id(s):
    """First entity id in a token like 'Mana Wyrm##15274+' or '15278'."""
    m = re.search(r"##\s*(\d+)", s)
    if m:
        return int(m.group(1))
    m = re.match(r"\s*(\d+)", s)
    return int(m.group(1)) if m else None


def name_of(s):
    """Human name from 'Mana Wyrm##15274+' or 'Thick Fluid |q 28608/1'."""
    s = s.split("|", 1)[0]                       # drop |q/|tip/... modifiers
    s = re.split(r"##", s, maxsplit=1)[0]        # drop ##id suffix
    return s.strip().lstrip("0123456789 ").strip() or None


# |q 8325/1   or  |q 8325   -> (quest, objective|None)
_Q_RE = re.compile(r"\|q\s+(\d+)(?:/(\d+))?")
# |only <filter...>  (until next |modifier or eol)
_ONLY_INLINE_RE = re.compile(r"\|only\s+([^|]+)")
# leading count: ".kill 8 Mana Wyrm##.." / ".get 6 Arcane Sliver"
_COUNT_RE = re.compile(r"^(\d+)\s+(.*)$")


def slug(title):
    tail = title.split("\\")
    parts = [p for p in tail if p and "Leveling Guides" not in p]
    s = "__".join(parts[-3:]) if len(parts) >= 3 else "__".join(parts)
    s = s.lower()
    s = re.sub(r"[^a-z0-9]+", "_", s).strip("_")
    return s


def parse_qbind(rest):
    qm = _Q_RE.search(rest)
    if qm:
        return int(qm.group(1)), (int(qm.group(2)) if qm.group(2) else None)
    return None, None


def parse_only(rest):
    m = _ONLY_INLINE_RE.search(rest)
    return m.group(1).strip() if m else None


def parse_body(title, faction, body):
    guide = {
        "id": slug(title),
        "title": title,
        "faction": faction,
        "next": None,
        "startlevel": None,
        "endlevel": None,
        "conditions": [],
        "steps": [],
    }
    cur_npc = None       # active .talk/.clicknpc within the current step
    pending_only = None  # a bare "only <x>" line applies to the current step

    for raw in body.splitlines():
        line = raw.rstrip()
        if not line.strip():
            continue
        stripped = line.strip()

        # step-level headers (no leading dot)
        head = stripped.split(None, 1)
        kw = head[0]
        arg = head[1].strip() if len(head) > 1 else ""

        if kw == "step":
            cur_npc = None
            pending_only = None
            continue
        if kw == "next":
            guide["next"] = arg
            continue
        if kw == "startlevel":
            try:
                guide["startlevel"] = float(arg)
            except ValueError:
                pass
            continue
        if kw == "endlevel":
            try:
                guide["endlevel"] = float(arg)
            except ValueError:
                pass
            continue
        if kw == "condition" and arg.startswith("suggested"):
            guide["conditions"].append(arg[len("suggested"):].strip())
            continue
        if kw == "only":
            pending_only = arg
            continue
        if kw in ("author", "image", "dynamic", "leechsteps", "map", "label",
                  "achieveid", "ding", "goto", "if", "local", "condition"):
            continue

        # action lines (.x / ..x)
        if not stripped.startswith("."):
            continue
        action = stripped.lstrip(".")
        averb = action.split(None, 1)
        if not averb:
            continue
        verb = averb[0]
        rest = averb[1].strip() if len(averb) > 1 else ""
        only = parse_only(rest) or pending_only

        if verb in ("talk", "clicknpc"):
            cur_npc = first_id(rest)
            continue
        if verb in ("accept", "turnin"):
            q = first_id(rest)
            if q:
                guide["steps"].append({
                    "action": verb, "quest": q, "npc": cur_npc, "only": only})
            continue
        if verb == "kill":
            m = _COUNT_RE.match(rest)
            count, target = (int(m.group(1)), m.group(2)) if m else (1, rest)
            q, obj = parse_qbind(rest)
            guide["steps"].append({
                "action": "kill", "quest": q, "obj": obj,
                "npc": first_id(target), "count": count, "name": name_of(target),
                "only": only})
            continue
        if verb in ("get", "collect"):
            m = _COUNT_RE.match(rest)
            count, target = (int(m.group(1)), m.group(2)) if m else (1, rest)
            q, obj = parse_qbind(rest)
            guide["steps"].append({
                "action": "collect", "quest": q, "obj": obj,
                "item": first_id(target), "count": count, "name": name_of(target),
                "only": only})
            continue
        if verb == "from":
            # source mob(s) for the following/last collect — attach as hint
            if guide["steps"] and guide["steps"][-1]["action"] == "collect":
                guide["steps"][-1]["from_npc"] = first_id(rest)
                guide["steps"][-1]["from_name"] = name_of(rest)
            continue
        if verb == "click":
            q, obj = parse_qbind(rest)
            guide["steps"].append({
                "action": "use", "quest": q, "obj": obj,
                "go": first_id(rest), "name": name_of(rest), "only": only})
            continue
        if verb == "fpath":
            guide["steps"].append({
                "action": "fpath", "name": rest.split("|")[0].strip() or None,
                "only": only})
            continue
        if verb == "home":
            guide["steps"].append({
                "action": "hearth", "npc": first_id(rest),
                "name": name_of(rest), "only": only})
            continue
        # buy / use / goal / tip etc. -> ignored for the route backbone
    return guide


_REG_RE = re.compile(
    r'RegisterGuide\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*\[\[(.*?)\]\]', re.DOTALL)


def parse_file(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    faction = "Horde" if "Horde" in path else (
        "Alliance" if "Alliance" in path else "Neutral")
    out = []
    for m in _REG_RE.finditer(text):
        title = m.group(1).replace('\\\\', '\\')
        out.append(parse_body(title, faction, m.group(2)))
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    guides = []
    for path in argv[1:]:
        guides.extend(parse_file(path))
    json.dump(guides, sys.stdout, indent=1)
    sys.stdout.write("\n")
    sys.stderr.write(
        "parsed {} guide(s); {} total route steps\n".format(
            len(guides), sum(len(g["steps"]) for g in guides)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
