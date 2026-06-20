#!/usr/bin/env python3
"""
Convert Zygor leveling guides into generated IdleBot guide files.

This first pass is intentionally conservative:

- reads either a zip archive or an extracted folder
- detects `ZygorLeveling*.lua`
- parses recognized route/objective commands
- preserves source guide metadata and zone-percent `goto` hints
- records unknown or skipped commands for later review
- writes generated guides only; does not overwrite existing files unless --force

Generated `.yaml` files are emitted in a JSON-compatible YAML subset so the
script can run with Python stdlib only on this host.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import zipfile
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Tuple


GUIDE_RE = re.compile(
    r'RegisterGuide\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*\[\[(.*?)\]\]', re.DOTALL
)
Q_RE = re.compile(r"\|q\s+(\d+)(?:/(\d+))?")
ONLY_INLINE_RE = re.compile(r"\|only\s+([^|]+)")
COUNT_RE = re.compile(r"^(\d+)\s+(.*)$")
GOTO_RE = re.compile(r"^(?:(.*?)\s+)?([0-9]+(?:\.[0-9]+)?)\s*,\s*([0-9]+(?:\.[0-9]+)?)")
ID_HASH_RE = re.compile(r"##\s*(\d+)")
LEADING_ID_RE = re.compile(r"^\s*(\d+)")
LEVEL_RANGE_RE = re.compile(r"\((\d+)(?:-(\d+))?\)")

RACES = {
    "human",
    "dwarf",
    "gnome",
    "night elf",
    "draenei",
    "orc",
    "undead",
    "tauren",
    "troll",
    "blood elf",
    "death knight",
}

CLASSES = {
    "warrior",
    "paladin",
    "hunter",
    "rogue",
    "priest",
    "death knight",
    "shaman",
    "mage",
    "warlock",
    "druid",
    "monk",
}

IGNORED_HEADERS = {
    "author",
    "image",
    "dynamic",
    "leechsteps",
    "map",
    "label",
    "achieveid",
    "ding",
    "if",
    "local",
}

IGNORED_DOT_VERBS = {
    "goal",
    "tip",
    "modelnpc",
    "homeport",
    "use",
    "buy",
    "collectcap",
    "|tip",
    "|confirm",
}


@dataclass
class SourceHint:
    zone: Optional[str] = None
    x: Optional[float] = None
    y: Optional[float] = None

    def to_dict(self) -> Dict[str, object]:
        out: Dict[str, object] = {}
        if self.zone:
            out["zone"] = self.zone
        if self.x is not None:
            out["x"] = self.x
        if self.y is not None:
            out["y"] = self.y
        return out


@dataclass
class UnknownLine:
    line: int
    raw: str
    reason: str


@dataclass
class ParsedAction:
    verb: str
    line: int
    data: Dict[str, object] = field(default_factory=dict)
    source_hint: Optional[SourceHint] = None
    restrictions: Dict[str, List[str]] = field(default_factory=dict)


@dataclass
class ParsedGuide:
    title: str
    file_name: str
    faction: str
    startlevel: Optional[float] = None
    endlevel: Optional[float] = None
    next_title: Optional[str] = None
    guide_races: List[str] = field(default_factory=list)
    guide_classes: List[str] = field(default_factory=list)
    actions: List[ParsedAction] = field(default_factory=list)
    unknown: List[UnknownLine] = field(default_factory=list)

    @property
    def recognized_count(self) -> int:
        return len(self.actions)


def first_id(text: str) -> Optional[int]:
    match = ID_HASH_RE.search(text)
    if match:
        return int(match.group(1))
    match = LEADING_ID_RE.match(text)
    return int(match.group(1)) if match else None


def clean_name(text: str) -> Optional[str]:
    text = text.split("|", 1)[0]
    text = re.split(r"##", text, maxsplit=1)[0]
    text = text.strip().lstrip("0123456789 ").strip()
    return text or None


def parse_qbind(text: str) -> Tuple[Optional[int], Optional[int]]:
    match = Q_RE.search(text)
    if not match:
        return None, None
    quest_id = int(match.group(1))
    objective = int(match.group(2)) if match.group(2) else None
    return quest_id, objective


def parse_only_inline(text: str) -> Optional[str]:
    match = ONLY_INLINE_RE.search(text)
    return match.group(1).strip() if match else None


def title_slug(title: str) -> str:
    value = title.lower().replace("\\", "_")
    value = re.sub(r"[^a-z0-9]+", "_", value).strip("_")
    return value


def normalize_word(word: str) -> str:
    return re.sub(r"[^a-z]", "", word.lower())


def title_level_range(title: str) -> Tuple[Optional[int], Optional[int]]:
    matches = LEVEL_RANGE_RE.findall(title)
    if not matches:
        return None, None
    start, end = matches[-1]
    return int(start), int(end or start)


def parse_restrictions(text: Optional[str]) -> Dict[str, List[str]]:
    if not text:
        return {}

    norm = text.replace("'", " ").replace('"', " ")
    words = [w for w in re.split(r"[^A-Za-z]+", norm) if w]
    lowered = [w.lower() for w in words]

    races: List[str] = []
    classes: List[str] = []

    joined = " ".join(lowered)
    for race in sorted(RACES, key=len, reverse=True):
        if race in joined:
            races.append(race)
    for klass in sorted(CLASSES, key=len, reverse=True):
        if klass in joined:
            classes.append(klass)

    out: Dict[str, List[str]] = {}
    if races:
        out["races"] = sorted(set(races))
    if classes:
        out["classes"] = sorted(set(classes))
    return out


def merge_restrictions(base: Dict[str, List[str]], extra: Dict[str, List[str]]) -> Dict[str, List[str]]:
    merged: Dict[str, List[str]] = {}
    for key in ("races", "classes"):
        values = set(base.get(key, []))
        values.update(extra.get(key, []))
        if values:
            merged[key] = sorted(values)
    return merged


def derive_title_restrictions(title: str) -> Tuple[List[str], List[str]]:
    tail = title.split("\\")[-1]
    label = re.sub(r"\(\d+(?:-\d+)?\)", "", tail).strip()
    restrictions = parse_restrictions(label)
    return restrictions.get("races", []), restrictions.get("classes", [])


def parse_goto(arg: str) -> Optional[SourceHint]:
    match = GOTO_RE.match(arg.strip())
    if not match:
        return None
    zone = match.group(1).strip() if match.group(1) else None
    return SourceHint(zone=zone, x=float(match.group(2)), y=float(match.group(3)))


def detect_faction(title: str, file_name: str) -> str:
    text = f"{title} {file_name}".lower()
    if "alliance" in text:
        return "alliance"
    if "horde" in text:
        return "horde"
    return "neutral"


def iter_guide_files_from_zip(path: pathlib.Path) -> Iterable[Tuple[str, str]]:
    with zipfile.ZipFile(path) as zf:
        for name in sorted(zf.namelist()):
            if "/Guides/Leveling/" not in name or not name.endswith(".lua"):
                continue
            if "ZygorLeveling" not in pathlib.PurePosixPath(name).name:
                continue
            with zf.open(name) as fh:
                yield pathlib.PurePosixPath(name).name, fh.read().decode("utf-8", "replace")


def iter_guide_files_from_dir(path: pathlib.Path) -> Iterable[Tuple[str, str]]:
    for file_path in sorted(path.rglob("ZygorLeveling*.lua")):
        yield file_path.name, file_path.read_text(encoding="utf-8", errors="replace")


def iter_guide_files(path: pathlib.Path) -> Iterable[Tuple[str, str]]:
    if path.is_file() and path.suffix.lower() == ".zip":
        yield from iter_guide_files_from_zip(path)
        return
    if path.is_dir():
        yield from iter_guide_files_from_dir(path)
        return
    raise FileNotFoundError(f"input path not found or unsupported: {path}")


def parse_body(title: str, file_name: str, body: str) -> ParsedGuide:
    guide_races, guide_classes = derive_title_restrictions(title)
    faction = detect_faction(title, file_name)
    guide = ParsedGuide(
        title=title,
        file_name=file_name,
        faction=faction,
        guide_races=guide_races,
        guide_classes=guide_classes,
    )

    current_npc: Optional[int] = None
    current_goto: Optional[SourceHint] = None
    pending_only: Optional[str] = None

    for line_no, raw in enumerate(body.splitlines(), 1):
        line = raw.rstrip()
        stripped = line.strip()
        if not stripped:
            continue

        parts = stripped.split(None, 1)
        head = parts[0]
        arg = parts[1].strip() if len(parts) > 1 else ""

        if head == "step":
            current_npc = None
            current_goto = None
            pending_only = None
            continue
        if head == "next":
            guide.next_title = arg
            continue
        if head == "startlevel":
            try:
                guide.startlevel = float(arg)
            except ValueError:
                pass
            continue
        if head == "endlevel":
            try:
                guide.endlevel = float(arg)
            except ValueError:
                pass
            continue
        if head == "condition" and arg.startswith("suggested"):
            restrictions = parse_restrictions(arg[len("suggested"):].strip())
            guide.guide_races = sorted(set(guide.guide_races + restrictions.get("races", [])))
            guide.guide_classes = sorted(set(guide.guide_classes + restrictions.get("classes", [])))
            continue
        if head == "only":
            pending_only = arg
            continue
        if head == "goto":
            current_goto = parse_goto(arg)
            if current_goto is None:
                guide.unknown.append(UnknownLine(line_no, stripped, "unparsed goto"))
            continue
        if head in IGNORED_HEADERS:
            continue

        if stripped.startswith("|"):
            continue

        if stripped.startswith(".'") or stripped.startswith("'"):
            guide.unknown.append(UnknownLine(line_no, stripped, "instructional text"))
            continue

        if not stripped.startswith("."):
            guide.unknown.append(UnknownLine(line_no, stripped, "unrecognized header"))
            continue

        action = stripped.lstrip(".")
        action_parts = action.split(None, 1)
        if not action_parts or not action_parts[0]:
            guide.unknown.append(UnknownLine(line_no, stripped, "empty dot command"))
            continue
        verb = action_parts[0]
        rest = action_parts[1].strip() if len(action_parts) > 1 else ""
        only = parse_only_inline(rest) or pending_only
        restrictions = parse_restrictions(only)

        if verb in ("talk", "clicknpc"):
            current_npc = first_id(rest)
            if current_npc is None:
                guide.unknown.append(UnknownLine(line_no, stripped, "talk/clicknpc missing npc id"))
            continue

        if verb in ("accept", "turnin"):
            quest_id = first_id(rest)
            if quest_id is None:
                guide.unknown.append(UnknownLine(line_no, stripped, f"{verb} missing quest id"))
                continue
            guide.actions.append(
                ParsedAction(
                    verb=verb,
                    line=line_no,
                    data={"quest_id": quest_id, "npc_id": current_npc},
                    source_hint=current_goto,
                    restrictions=restrictions,
                )
            )
            continue

        if verb == "kill":
            match = COUNT_RE.match(rest)
            count, target = (int(match.group(1)), match.group(2)) if match else (1, rest)
            quest_id, objective = parse_qbind(rest)
            guide.actions.append(
                ParsedAction(
                    verb="kill",
                    line=line_no,
                    data={
                        "quest_id": quest_id,
                        "objective_index": objective,
                        "creature_id": first_id(target),
                        "required_count": count,
                        "target_name": clean_name(target),
                    },
                    source_hint=current_goto,
                    restrictions=restrictions,
                )
            )
            continue

        if verb in ("get", "collect"):
            match = COUNT_RE.match(rest)
            count, target = (int(match.group(1)), match.group(2)) if match else (1, rest)
            quest_id, objective = parse_qbind(rest)
            guide.actions.append(
                ParsedAction(
                    verb="collect",
                    line=line_no,
                    data={
                        "quest_id": quest_id,
                        "objective_index": objective,
                        "item_id": first_id(target),
                        "required_count": count,
                        "item_name": clean_name(target),
                    },
                    source_hint=current_goto,
                    restrictions=restrictions,
                )
            )
            continue

        if verb == "from":
            if guide.actions and guide.actions[-1].verb == "collect":
                guide.actions[-1].data["from_creature_id"] = first_id(rest)
                guide.actions[-1].data["from_creature_name"] = clean_name(rest)
            else:
                guide.unknown.append(UnknownLine(line_no, stripped, "from without preceding collect"))
            continue

        if verb == "click":
            quest_id, objective = parse_qbind(rest)
            guide.actions.append(
                ParsedAction(
                    verb="click",
                    line=line_no,
                    data={
                        "quest_id": quest_id,
                        "objective_index": objective,
                        "gameobject_id": first_id(rest),
                        "object_name": clean_name(rest),
                    },
                    source_hint=current_goto,
                    restrictions=restrictions,
                )
            )
            continue

        if verb == "home":
            guide.actions.append(
                ParsedAction(
                    verb="home",
                    line=line_no,
                    data={"npc_id": first_id(rest), "target_name": clean_name(rest)},
                    source_hint=current_goto,
                    restrictions=restrictions,
                )
            )
            continue

        if verb == "fpath":
            guide.actions.append(
                ParsedAction(
                    verb="fpath",
                    line=line_no,
                    data={"target_name": rest.split("|", 1)[0].strip() or None},
                    source_hint=current_goto,
                    restrictions=restrictions,
                )
            )
            continue

        if verb in IGNORED_DOT_VERBS:
            continue

        guide.unknown.append(UnknownLine(line_no, stripped, f"unknown verb {verb}"))

    return guide


def parse_guides_from_content(file_name: str, text: str) -> List[ParsedGuide]:
    guides: List[ParsedGuide] = []
    for match in GUIDE_RE.finditer(text):
        title = match.group(1).replace("\\\\", "\\")
        guides.append(parse_body(title, file_name, match.group(2)))
    return guides


def build_step_name(action: ParsedAction) -> str:
    data = action.data
    if action.verb == "accept":
        return f"Accept quest {data['quest_id']}"
    if action.verb == "turnin":
        return f"Turn in quest {data['quest_id']}"
    if action.verb == "kill":
        target = data.get("target_name") or data.get("creature_id") or "target"
        return f"Kill {data.get('required_count', 1)} {target}"
    if action.verb == "collect":
        item = data.get("item_name") or data.get("item_id") or "item"
        return f"Collect {data.get('required_count', 1)} {item}"
    if action.verb == "click":
        target = data.get("object_name") or data.get("gameobject_id") or "object"
        return f"Interact with {target}"
    if action.verb == "home":
        target = data.get("target_name") or "innkeeper"
        return f"Set hearthstone with {target}"
    if action.verb == "fpath":
        target = data.get("target_name") or "flight master"
        return f"Discover flight path at {target}"
    return f"{action.verb} step"


def normalized_step_type(action: ParsedAction) -> str:
    return {
        "accept": "accept_quest",
        "turnin": "turn_in_quest",
        "kill": "kill_mobs",
        "collect": "collect_items",
        "click": "interact_gameobject",
        "home": "set_hearthstone",
        "fpath": "discover_flight_path",
    }[action.verb]


def to_generated_guide(guide: ParsedGuide) -> Dict[str, object]:
    start_level, end_level = title_level_range(guide.title)
    level_min = int(guide.startlevel) if guide.startlevel is not None else (start_level or 1)
    level_max = end_level or (int(guide.endlevel) if guide.endlevel is not None else level_min)

    title_parts = [p for p in guide.title.split("\\") if p and "Leveling Guides" not in p]
    zone_name = title_parts[-2] if len(title_parts) >= 2 else None
    zone_label = re.sub(r"\s*\(\d+(?:-\d+)?\)", "", zone_name).strip() if zone_name else None
    leaf_name = re.sub(r"\s*\(\d+(?:-\d+)?\)", "", title_parts[-1]).strip() if title_parts else guide.title
    display_name = " ".join(
        part for part in [guide.faction.title(), leaf_name, zone_label, f"{level_min}-{level_max}"] if part
    )

    output: Dict[str, object] = {
        "id": title_slug(guide.title),
        "name": display_name,
        "source": {
            "kind": "zygor_reference",
            "title": guide.title,
            "file": guide.file_name,
        },
        "faction": guide.faction,
        "races": guide.guide_races,
        "classes": guide.guide_classes,
        "level_min": level_min,
        "level_max": level_max,
        "zones": [zone_name] if zone_name else [],
        "runtime": {
            "mode": "strict_questing",
            "use_playerbot_combat": True,
            "use_playerbot_loot": True,
            "use_playerbot_quest_actions": True,
        },
        "steps": [],
    }

    for idx, action in enumerate(guide.actions, 1):
        step: Dict[str, object] = {
            "id": f"{normalized_step_type(action)}_{idx:03d}",
            "type": normalized_step_type(action),
            "name": build_step_name(action),
        }
        step.update({k: v for k, v in action.data.items() if v is not None})

        if action.restrictions:
            step["restrictions"] = action.restrictions
        if action.source_hint:
            hint = action.source_hint.to_dict()
            if hint:
                step["source_hint"] = hint

        quest_id = action.data.get("quest_id")
        objective_index = action.data.get("objective_index")
        if quest_id and objective_index and action.verb in {"kill", "collect", "click"}:
            step["completion_condition"] = f"quest_objective_complete:{quest_id}/{objective_index}"

        output["steps"].append(step)

    return output


def write_generated_yaml(path: pathlib.Path, payload: Dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, ensure_ascii=False)
        fh.write("\n")


def guide_output_path(base_dir: pathlib.Path, guide: ParsedGuide) -> pathlib.Path:
    race_dir = guide.guide_races[0].replace(" ", "_") if guide.guide_races else "generic"
    return base_dir / guide.faction / race_dir / f"{title_slug(guide.title)}.yaml"


def write_report(path: pathlib.Path, source_input: pathlib.Path, selected: List[ParsedGuide], written: List[pathlib.Path], skipped_existing: List[pathlib.Path]) -> None:
    total_steps = sum(g.recognized_count for g in selected)
    unknown_total = sum(len(g.unknown) for g in selected)
    lines = [
        "# Zygor Conversion Report",
        "",
        f"- input: `{source_input}`",
        f"- guides converted: {len(written)}",
        f"- guides selected: {len(selected)}",
        f"- recognized steps: {total_steps}",
        f"- unknown/skipped lines recorded: {unknown_total}",
        f"- skipped existing outputs: {len(skipped_existing)}",
        "",
        "## Guides",
        "",
    ]
    for guide, out_path in zip(selected, written):
        start_level, end_level = title_level_range(guide.title)
        report_min = int(guide.startlevel) if guide.startlevel is not None else start_level
        report_max = end_level or (int(guide.endlevel) if guide.endlevel is not None else report_min)
        lines.extend(
            [
                f"### {guide.title}",
                "",
                f"- output: `{out_path}`",
                f"- faction: `{guide.faction}`",
                f"- levels: `{report_min}` to `{report_max}`",
                f"- recognized steps: {guide.recognized_count}",
                f"- unknown/skipped lines: {len(guide.unknown)}",
                "",
            ]
        )
    if skipped_existing:
        lines.extend(["## Existing Outputs Skipped", ""])
        for path_item in skipped_existing:
            lines.append(f"- `{path_item}`")
        lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def write_unknown_report(path: pathlib.Path, selected: List[ParsedGuide]) -> None:
    lines = [
        "# Zygor Unknown Commands",
        "",
        "These are source lines the first-pass converter did not map into generated IdleBot guide steps.",
        "",
    ]
    for guide in selected:
        lines.extend([f"## {guide.title}", ""])
        if not guide.unknown:
            lines.extend(["- none", ""])
            continue
        for entry in guide.unknown:
            lines.append(f"- line {entry.line}: `{entry.raw}` ({entry.reason})")
        lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def select_guides(all_guides: List[ParsedGuide], faction: Optional[str], min_level: Optional[int], max_level: Optional[int], limit: Optional[int]) -> List[ParsedGuide]:
    selected: List[ParsedGuide] = []
    for guide in all_guides:
        gl_min, gl_max = title_level_range(guide.title)
        start_level = int(guide.startlevel) if guide.startlevel is not None else (gl_min or 0)
        end_level = gl_max or (int(guide.endlevel) if guide.endlevel is not None else start_level)

        if faction and guide.faction != faction:
            continue
        if min_level is not None and end_level < min_level:
            continue
        if max_level is not None and start_level > max_level:
            continue
        selected.append(guide)
        if limit is not None and len(selected) >= limit:
            break
    return selected


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, help="zip archive or extracted Zygor folder")
    parser.add_argument("--output", required=True, help="generated guide output directory")
    parser.add_argument("--limit", type=int, default=None)
    parser.add_argument("--faction", choices=["alliance", "horde", "neutral"], default=None)
    parser.add_argument("--min-level", type=int, default=None)
    parser.add_argument("--max-level", type=int, default=None)
    parser.add_argument("--report-file", default=None)
    parser.add_argument("--unknown-file", default=None)
    parser.add_argument("--force", action="store_true")
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    input_path = pathlib.Path(args.input).expanduser()
    output_dir = pathlib.Path(args.output)

    parsed_guides: List[ParsedGuide] = []
    for file_name, text in iter_guide_files(input_path):
        parsed_guides.extend(parse_guides_from_content(file_name, text))

    selected = select_guides(parsed_guides, args.faction, args.min_level, args.max_level, args.limit)
    if not selected:
        print("no guides matched the provided filters", file=sys.stderr)
        return 1

    written: List[pathlib.Path] = []
    skipped_existing: List[pathlib.Path] = []
    for guide in selected:
        out_path = guide_output_path(output_dir, guide)
        if out_path.exists() and not args.force:
            skipped_existing.append(out_path)
            continue
        write_generated_yaml(out_path, to_generated_guide(guide))
        written.append(out_path)

    if args.report_file:
        write_report(pathlib.Path(args.report_file), input_path, selected, written, skipped_existing)
    if args.unknown_file:
        write_unknown_report(pathlib.Path(args.unknown_file), selected)

    summary = {
        "input": str(input_path),
        "selected_guides": len(selected),
        "written_guides": len(written),
        "recognized_steps": sum(g.recognized_count for g in selected),
        "unknown_lines": sum(len(g.unknown) for g in selected),
        "outputs": [str(p) for p in written],
    }
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
