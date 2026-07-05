#!/usr/bin/env python3
"""Compile six external starter profiles into locally validated coverage.

External XML is only a lead/order benchmark. Every emitted quest, source and
coordinate comes from the supplied acore_world snapshot. Output is stable for
identical config/profile/snapshot/route inputs and is suitable for route-plan
generation; it is never executed as Honorbuddy XML.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


# "gameobject" added 2026-07-05: the GuideRuntime InteractGameObject step is
# now live-verified end to end (Deathtestbot collected 6x Scavenged Goods from
# Equipment Boxes and turned q3902 in to REWARDED after the BotLoot SendLoot
# fix). "use_item" and "exploration" remain unsupported here until their steps
# are live-driven, even though the C++ exists -- coverage must reflect what is
# proven to run, not merely what compiles.
SUPPORTED_OBJECTIVES = {"delivery", "kill", "creature_collection", "gameobject"}


def as_int(value: object, default: int = 0) -> int:
    try:
        return int(value or default)
    except (TypeError, ValueError):
        return default


def parse_profile(path: Path) -> dict:
    root = ET.parse(path).getroot()
    definitions: dict[int, dict] = {}
    for quest in root.findall(".//Quest"):
        quest_id = as_int(quest.get("Id"))
        if not quest_id:
            continue
        objectives = []
        for objective in quest.findall("./Objective"):
            item = {
                "type": objective.get("Type", "unknown"),
                "mob_id": as_int(objective.get("MobId")),
                "item_id": as_int(objective.get("ItemId")),
                "count": as_int(objective.get("KillCount") or objective.get("CollectCount")),
                "gameobjects": sorted({
                    as_int(go.get("Id")) for go in objective.findall(".//GameObject") if as_int(go.get("Id"))
                }),
                "hotspots": [
                    [float(point.get(axis, "0")) for axis in ("X", "Y", "Z")]
                    for point in objective.findall(".//Hotspot")
                ],
            }
            objectives.append(item)
        definitions[quest_id] = {
            "id": quest_id,
            "name": quest.get("Name", ""),
            "objectives": objectives,
        }

    actions = []
    order = root.find("QuestOrder")
    if order is not None:
        for element in order.iter():
            if element is order:
                continue
            quest_id = as_int(element.get("QuestId"))
            action = {"action": element.tag}
            if quest_id:
                action["quest_id"] = quest_id
            if element.tag == "GrindTo":
                action["level"] = as_int(element.get("Level"))
            if element.tag in {"RunTo", "Hotspot"}:
                action["point"] = [float(element.get(axis, "0")) for axis in ("X", "Y", "Z")]
            if element.tag == "CustomBehavior":
                for key in ("File", "ProfileName", "ItemId", "MobId", "QuestId", "X", "Y", "Z"):
                    if element.get(key) is not None:
                        action[key.lower()] = element.get(key)
            if len(action) > 1 or element.tag in {"GrindTo", "CustomBehavior", "RunTo"}:
                actions.append(action)

    vendors = []
    for vendor in root.findall(".//Vendors/Vendor"):
        vendors.append({
            "name": vendor.get("Name", ""), "entry_lead": as_int(vendor.get("Entry")),
            "type": vendor.get("Type", ""), "class": vendor.get("TrainClass", ""),
            "point_lead": [float(vendor.get(axis, "0")) for axis in ("X", "Y", "Z")],
        })

    ids = set(definitions)
    ids.update(as_int(action.get("quest_id")) for action in actions)
    ids.discard(0)
    return {
        "source_file": path.name,
        "min_level": as_int(root.findtext("MinLevel")),
        "max_level": as_int(root.findtext("MaxLevel")),
        "quest_ids": sorted(ids),
        "definitions": [definitions[key] for key in sorted(definitions)],
        "actions": actions,
        "vendors": vendors,
    }


def route_quest_ids(route: dict) -> set[int]:
    return {as_int(segment.get("quest")) for segment in route.get("segments", []) if as_int(segment.get("quest"))}


def skipped_comment_ids(route: dict) -> set[int]:
    comment = route.get("comment", "")
    ids: set[int] = set()
    # Keep the skip word and IDs in the same sentence. Route comments often
    # contain many unrelated quest IDs, dates, NPC entries and teleport IDs.
    for sentence in re.split(r"[.;\n]+", comment):
        if re.search(r"\b(skip|skipped|unsupported)\b", sentence, re.IGNORECASE):
            ids.update(int(value) for value in re.findall(r"\b\d{2,5}\b", sentence))
    return ids


def index_snapshot(snapshot: dict) -> dict:
    existing_quest_ids = set(snapshot.get("existing_quest_ids", []))
    existing_quest_ids.update(as_int(row["ID"]) for row in snapshot["quests"])
    index = {
        "quests": {as_int(row["ID"]): row for row in snapshot["quests"]},
        "existing_quest_ids": existing_quest_ids,
        "addons": {as_int(row["ID"]): row for row in snapshot["addons"]},
        "starters": defaultdict(list), "enders": defaultdict(list),
        "creatures": defaultdict(list), "gameobjects": defaultdict(list),
        "item_sources": defaultdict(list),
    }
    for row in snapshot["starters"]:
        index["starters"][as_int(row["quest"])].append(row)
    for row in snapshot["enders"]:
        index["enders"][as_int(row["quest"])].append(row)
    for row in snapshot["creatures"]:
        index["creatures"][as_int(row["entry"])].append(row)
    for row in snapshot["gameobjects"]:
        index["gameobjects"][as_int(row["entry"])].append(row)
    for row in snapshot["item_sources"]:
        index["item_sources"][as_int(row["item"])].append(row)
    return index


def local_rows(rows: list[dict], family: dict) -> list[dict]:
    xmin, xmax, ymin, ymax = family["bounds"]
    return [
        row for row in rows
        if row.get("map") is not None and as_int(row.get("map")) == family["map"]
        and xmin <= float(row.get("x") or 0) <= xmax
        and ymin <= float(row.get("y") or 0) <= ymax
    ]


def classify_objectives(quest: dict, addon: dict, index: dict, family: dict) -> tuple[list[dict], set[str], list[str]]:
    objectives = []
    behaviors: set[str] = set()
    validation: list[str] = []
    for slot in range(1, 5):
        entry = as_int(quest[f"RequiredNpcOrGo{slot}"])
        count = as_int(quest[f"RequiredNpcOrGoCount{slot}"])
        if entry > 0:
            rows = local_rows(index["creatures"].get(entry, []), family)
            spawned = any(row.get("map") is not None for row in rows)
            objectives.append({
                "slot": slot, "type": "kill", "entry": entry, "count": count,
                "spawned": spawned, "local_sources": rows,
            })
            behaviors.add("kill")
            if not spawned:
                validation.append(f"objective_creature_{entry}_has_no_spawn")
        elif entry < 0:
            go = -entry
            rows = local_rows(index["gameobjects"].get(go, []), family)
            spawned = any(row.get("map") is not None for row in rows)
            objectives.append({
                "slot": slot, "type": "gameobject", "entry": go, "count": count,
                "spawned": spawned, "local_sources": rows,
            })
            behaviors.add("gameobject")
            if not spawned:
                validation.append(f"objective_gameobject_{go}_has_no_spawn")
    for slot in range(1, 7):
        item_id = as_int(quest[f"RequiredItemId{slot}"])
        count = as_int(quest[f"RequiredItemCount{slot}"])
        if not item_id:
            continue
        sources = index["item_sources"].get(item_id, [])
        kinds = sorted({source["kind"] for source in sources})
        local_sources = []
        for source in sources:
            source = dict(source)
            source_index = index["creatures"] if source["kind"] == "creature" else index["gameobjects"]
            source["spawns"] = local_rows(source_index.get(as_int(source["entry"]), []), family)
            local_sources.append(source)
        provided_at_accept = item_id == as_int(quest.get("StartItem"))
        if provided_at_accept:
            behavior = "delivery"
        else:
            behavior = "creature_collection" if "creature" in kinds else "gameobject"
        objectives.append({
            "slot": slot, "type": "item", "item": item_id, "count": count,
            "source_kinds": kinds, "local_sources": local_sources,
            "provided_at_accept": provided_at_accept,
        })
        behaviors.add(behavior)
        if not provided_at_accept and not any(source["spawns"] for source in local_sources):
            validation.append(f"required_item_{item_id}_has_no_local_loot_source")
    if as_int(addon.get("SpecialFlags")) & 0x2:
        behaviors.add("exploration")
    if as_int(addon.get("SourceSpellID")):
        behaviors.add("use_item")
    if not behaviors:
        behaviors.add("delivery")
    return objectives, behaviors, validation


def compile_variant(family: dict, variant: dict, route: dict, profile: dict, snapshot: dict, index: dict) -> dict:
    included = route_quest_ids(route)
    contradictions = sorted(included & skipped_comment_ids(route))
    rows = []
    for quest_id in snapshot["family_quests"][family["id"]]:
        quest = index["quests"].get(quest_id)
        if not quest:
            rows.append({"quest": quest_id, "status": "omitted", "reason": "missing_local_quest_template"})
            continue
        addon = index["addons"].get(quest_id, {})
        objectives, behaviors, validation = classify_objectives(quest, addon, index, family)
        reasons = []
        races = as_int(quest.get("AllowableRaces"))
        classes = as_int(addon.get("AllowableClasses"))
        if races and not (races & variant["race_mask"]):
            reasons.append("wrong_race")
        if classes and not (classes & variant["class_mask"]):
            reasons.append("wrong_class")
        previous = abs(as_int(addon.get("PrevQuestID")))
        if previous and previous not in index["existing_quest_ids"]:
            reasons.append("invalid_chain")
            validation.append(f"previous_quest_{previous}_missing_from_snapshot")
        starters = local_rows(index["starters"].get(quest_id, []), family)
        enders = local_rows(index["enders"].get(quest_id, []), family)
        if not starters:
            reasons.append("invalid_giver")
        if not enders:
            reasons.append("invalid_ender")
        elite = any(
            objective["type"] == "kill"
            and any(as_int(creature.get("rank")) >= 1 for creature in index["creatures"].get(objective["entry"], []))
            for objective in objectives
        )
        if as_int(quest.get("SuggestedGroupNum")) > 0 or elite:
            reasons.append("unsafe_group_or_elite")
        unsupported = sorted(behaviors - SUPPORTED_OBJECTIVES)
        if unsupported:
            reasons.append("unsupported_objective_behavior")
        if validation:
            reasons.append("invalid_local_source")

        if quest_id in included:
            status, reason = "included", "route"
        elif reasons:
            status, reason = "omitted", reasons[0]
        else:
            status, reason = "omitted", "deliberate_route_quality_choice"
        rows.append({
            "quest": quest_id, "title": quest.get("LogTitle", ""),
            "quest_level": as_int(quest.get("QuestLevel")), "min_level": as_int(quest.get("MinLevel")),
            "reward_xp_difficulty": as_int(quest.get("RewardXPDifficulty")),
            "reward_money": as_int(quest.get("RewardMoney")),
            "status": status, "reason": reason, "all_reasons": reasons,
            "behaviors": sorted(behaviors), "objectives": objectives,
            "validation": sorted(set(validation)),
            "starters": starters,
            "enders": enders,
            "external_profile_lead": quest_id in set(profile["quest_ids"]),
            "chain": {"previous": previous, "next": as_int(addon.get("NextQuestID")),
                      "exclusive_group": as_int(addon.get("ExclusiveGroup"))},
        })
    local_ids = set(snapshot["family_quests"][family["id"]])
    return {
        "route": variant["route"], "race_mask": variant["race_mask"], "class_mask": variant["class_mask"],
        "quests": rows,
        "summary": {
            "local_candidates": len(rows),
            "included": sum(row["status"] == "included" for row in rows),
            "eligible_supported_omitted": sum(row["reason"] == "deliberate_route_quality_choice" for row in rows),
            "included_with_validation_failures": sum(
                row["status"] == "included" and bool(row["all_reasons"]) for row in rows
            ),
            "external_profile_ids": len(profile["quest_ids"]),
            "external_ids_not_local_candidates": sorted(set(profile["quest_ids"]) - local_ids),
            "stale_comment_contradictions": contradictions,
        },
    }


def markdown(compiled: dict) -> str:
    lines = [f"# {compiled['family']} route coverage", "",
             "External profile data is a research lead. All classifications and coordinates are local `acore_world` facts.", ""]
    for variant in compiled["variants"]:
        summary = variant["summary"]
        lines.extend([
            f"## {variant['route']}", "",
            f"Candidates: {summary['local_candidates']} · included: {summary['included']} · "
            f"supported omissions: {summary['eligible_supported_omitted']} · external leads: {summary['external_profile_ids']}", "",
            "| Quest | Level | Status | Reason | Behaviors |", "|---:|---:|---|---|---|",
        ])
        for quest in variant["quests"]:
            title = str(quest.get("title", "")).replace("|", "\\|")
            lines.append(
                f"| {quest['quest']} {title} | {quest.get('quest_level', 0)} | {quest['status']} | "
                f"{quest['reason']} | {', '.join(quest.get('behaviors', []))} |"
            )
        lines.append("")
        if summary["stale_comment_contradictions"]:
            lines.append(f"Stale route-comment contradictions: {summary['stale_comment_contradictions']}")
            lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name("coverage_families.json"))
    parser.add_argument("--profiles-root", type=Path, required=True)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--routes-dir", type=Path, default=Path(__file__).with_name("routes"))
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    config = json.loads(args.config.read_text())
    snapshot = json.loads(args.snapshot.read_text())
    index = index_snapshot(snapshot)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    manifest = {"schema_version": 1, "families": []}

    for family in config["families"]:
        profile = parse_profile(args.profiles_root / family["profile"])
        for vendor in profile["vendors"]:
            vendor["local_matches"] = local_rows(
                index["creatures"].get(vendor["entry_lead"], []), family
            )
            vendor["locally_validated"] = bool(vendor["local_matches"])
        compiled = {
            "schema_version": 1, "family": family["id"], "map": family["map"],
            "bounds": family["bounds"], "external_profile": profile,
            "variants": [],
        }
        for variant in family["variants"]:
            route = json.loads((args.routes_dir / variant["route"]).read_text())
            compiled["variants"].append(compile_variant(family, variant, route, profile, snapshot, index))
        json_path = args.output_dir / f"{family['id']}.json"
        md_path = args.output_dir / f"{family['id']}.md"
        json_path.write_text(json.dumps(compiled, indent=2, sort_keys=True) + "\n")
        md_path.write_text(markdown(compiled))
        manifest["families"].append({
            "id": family["id"], "json": json_path.name, "report": md_path.name,
            "variants": [item["summary"] | {"route": item["route"]} for item in compiled["variants"]],
        })

    (args.output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
