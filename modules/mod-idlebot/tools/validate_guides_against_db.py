#!/usr/bin/env python3
"""Validate all idlebot guide YAML files against DB data.

Covers every guide under data/guides/ (alliance/, horde/, class/),
skipping generated/ and generated_backup/.

Checks:
  QUEST_NOT_FOUND            BLOCKER  quest_id not in quests.json
  NPC_NO_SPAWN               BLOCKER  accept/turnin NPC has zero spawns
  NPC_WRONG_MAP              BLOCKER  NPC only spawns on different map than step map_id
  BAD_COORDS                 BLOCKER  accept NPC nearest spawn > 30 yd from step coords
  BAD_TURNIN_COORDS          BLOCKER  turnin NPC nearest spawn > 30 yd from step coords
  CROSS_MAP_KILL             BLOCKER  kill_mobs step map_id != any creature spawn map
  OBJECTIVE_WRONG_MAP        BLOCKER  same as CROSS_MAP_KILL (alias for kill/collect)
  OBJECTIVE_FAR_FROM_SPAWN   BLOCKER/WARNING  nearest spawn distance from kill coords:
                                    >= 1500 yd = BLOCKER (wrong zone), 500-1499 yd = WARNING
  COLLECT_GO_NO_SPAWNS       BLOCKER  collect_items gameobject has zero spawns
  QUEST_CROSS_MAP_OBJECTIVE  BLOCKER  kill step map_id differs from quest's accept map_id
  LEVEL_MIN_MISMATCH         BLOCKER/WARNING  first quest MinLevel > guide level_min:
                                    gap > 3 = BLOCKER (bot can't accept), gap 2-3 = WARNING
  NEXT_GUIDE_MISSING         BLOCKER  next_guide references a guide ID not in the set
  NEXT_GUIDE_FACTION_MISMATCH BLOCKER  next_guide is a different faction than this guide
  WRONG_RACE_QUEST           BLOCKER  quest AllowableRaces excludes guide's race
  ITEM_NO_LOOT               WARNING  collect item not found in item_sources.json
  MISSING_PREREQ             WARNING  accepted quest requires prev not yet accepted
  MISSING_OBJECTIVE          WARNING  turn_in for quest with objectives but no obj step
  QUEST_NO_OBJECTIVE_STEP    WARNING  quest has kill/item objectives, no step in guide
  EVENT_QUEST                WARNING  quest_id found in event_quests.txt
  POTENTIAL_PATHFINDING      WARNING  accept/turnin NPC has z >= 60 (elevated platform)
  UNSAFE_OBJECTIVE           WARNING  generator marked step as _unsafe

Usage:
  python3 validate_guides_against_db.py [--guides DIR] [--data DIR]
      [--json-out FILE] [--md-out FILE] [--severity {BLOCKER,WARNING,INFO,ALL}]

Defaults:
  --guides   <module>/data/guides
  --data     <module>/data/generated
  --json-out <module>/reports/guide_validation_latest.json
  --md-out   <module>/reports/guide_validation_latest.md
  --severity ALL
"""

import argparse
import json
import math
import os
import sys

import yaml

MODULE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_GUIDES = os.path.join(MODULE_DIR, "data", "guides")
DEFAULT_DATA   = os.path.join(MODULE_DIR, "data", "generated")
DEFAULT_REPORTS = os.path.join(MODULE_DIR, "reports")


# ---------------------------------------------------------------------------
# Geometry
# ---------------------------------------------------------------------------

def dist(x1, y1, x2, y2):
    return math.sqrt((x1 - x2) ** 2 + (y1 - y2) ** 2)


# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------

def load_data(data_dir):
    d = {}
    for name in ["quests", "quest_starters", "quest_enders",
                 "npc_spawns", "go_spawns", "item_sources"]:
        path = os.path.join(data_dir, f"{name}.json")
        if os.path.exists(path):
            with open(path) as f:
                d[name] = json.load(f)
        else:
            d[name] = {}

    event_path = os.path.join(data_dir, "event_quests.txt")
    event_ids = set()
    if os.path.exists(event_path):
        with open(event_path) as f:
            for line in f:
                line = line.strip()
                if line.isdigit():
                    event_ids.add(int(line))
    d["event_quests"] = event_ids
    return d


# ---------------------------------------------------------------------------
# Per-guide validation
# ---------------------------------------------------------------------------

def _nearest_spawn(spawns, step_x, step_y, step_map):
    """Return (distance, spawn) for the closest spawn on the right map, or (inf, None)."""
    same_map = [s for s in spawns if s.get("map") == step_map]
    if not same_map:
        return float("inf"), None
    best = min(same_map, key=lambda s: dist(s["x"], s["y"], step_x, step_y))
    return dist(best["x"], best["y"], step_x, step_y), best


def _issue(guide_rel, step_index, step, issue_type, severity, message,
           db_evidence="", suggested_fix=""):
    return {
        "guide":         guide_rel,
        "step_index":    step_index,
        "step_id":       step.get("id", ""),
        "quest_id":      step.get("quest_id"),
        "npc_id":        step.get("npc_id"),
        "go_id":         step.get("gameobject_id") or step.get("go_id"),
        "item_id":       step.get("item_id"),
        "issue_type":    issue_type,
        "severity":      severity,
        "message":       message,
        "db_evidence":   db_evidence,
        "suggested_fix": suggested_fix,
    }


def validate_guide(guide_path, guide_rel, data, all_guide_ids=None):
    with open(guide_path) as f:
        guide = yaml.safe_load(f)

    if not guide or "steps" not in guide:
        return []

    issues = []
    quests_seen   = set()
    has_objective = {}
    quest_accept_map = {}   # qid -> map_id of its accept step

    guide_level_min  = guide.get("level_min", 0) or 0
    guide_race       = (guide.get("race") or "").lower()
    guide_faction    = (guide.get("faction") or "").lower()
    guide_id         = guide.get("id", guide_rel)
    guide_next       = guide.get("next_guide", "")

    # Race bitmasks (WoW 3.3.5a).
    _RACE_BIT = {
        "human": 1, "orc": 2, "dwarf": 4, "nightelf": 8, "undead": 16,
        "tauren": 32, "gnome": 64, "troll": 128, "bloodelf": 512, "draenei": 1024,
    }
    guide_race_bit = _RACE_BIT.get(guide_race, 0)

    # ----------------------------------------------------------------
    # LEVEL_MIN_MISMATCH — guide claims level_min but first quest
    # requires a higher level. Bots assigned at level_min will fail
    # every accept attempt and waste ticks grinding up before any quest.
    # ----------------------------------------------------------------
    first_accept_checked = False
    for step in guide.get("steps", []):
        if step.get("type") != "accept_quest" or not step.get("quest_id"):
            continue
        qid_str = str(step["quest_id"])
        q = data["quests"].get(qid_str)
        if not q:
            break  # let QUEST_NOT_FOUND handle it
        q_min = q.get("MinLevel", 0) or 0
        gap   = q_min - guide_level_min
        if gap > 1:
            sev = "BLOCKER" if gap > 3 else "WARNING"
            issues.append(_issue(
                guide_rel, None, step,
                "LEVEL_MIN_MISMATCH", sev,
                f"guide level_min={guide_level_min} but first quest {step['quest_id']} "
                f"\"{q.get('LogTitle','?')}\" requires MinLevel={q_min} (gap={gap})",
                db_evidence=f"quest MinLevel={q_min}",
                suggested_fix=f"Set guide level_min={q_min} or assign this guide only after bot reaches level {q_min}",
            ))
        first_accept_checked = True
        break

    # ----------------------------------------------------------------
    # NEXT_GUIDE_MISSING — next_guide references a guide that doesn't
    # exist in the current guide set. Bots chain into a missing guide
    # and silently sit idle when the guide can't be found.
    # ----------------------------------------------------------------
    if guide_next and all_guide_ids is not None and guide_next not in all_guide_ids:
        issues.append({
            "guide":         guide_rel,
            "step_index":    None,
            "step_id":       None,
            "quest_id":      None,
            "npc_id":        None,
            "go_id":         None,
            "item_id":       None,
            "issue_type":    "NEXT_GUIDE_MISSING",
            "severity":      "BLOCKER",
            "message":       f"next_guide '{guide_next}' not found in loaded guide set",
            "db_evidence":   f"guide id '{guide_next}' unknown",
            "suggested_fix": f"Create guide '{guide_next}' or remove next_guide field",
        })

    # ----------------------------------------------------------------
    # NEXT_GUIDE_FACTION_MISMATCH — next_guide has a different faction.
    # An alliance guide chaining to a horde guide will send alliance bots
    # into horde starting zones.
    # ----------------------------------------------------------------
    # (Evaluated below after all_guide_ids are resolved — needs the next
    #  guide's metadata, which requires it to be loaded. Handled in the
    #  cross-guide post-pass in main() instead.)

    # ----------------------------------------------------------------
    # WRONG_RACE_QUEST — guide targets a specific race but contains a
    # quest whose AllowableRaces bitmask excludes that race.
    # ----------------------------------------------------------------
    if guide_race_bit:
        for i, step in enumerate(guide.get("steps", [])):
            if step.get("type") != "accept_quest" or not step.get("quest_id"):
                continue
            q = data["quests"].get(str(step["quest_id"]))
            if not q:
                continue
            ar = q.get("AllowableRaces", 0) or 0
            if ar in (0, -1, 4294967295):
                continue  # unrestricted
            if not (ar & guide_race_bit):
                issues.append(_issue(
                    guide_rel, i, step,
                    "WRONG_RACE_QUEST", "BLOCKER",
                    f"quest {step['quest_id']} \"{q.get('LogTitle','?')}\" "
                    f"AllowableRaces={ar} excludes guide race '{guide_race}' (bit {guide_race_bit})",
                    db_evidence=f"AllowableRaces={ar}, guide_race_bit={guide_race_bit}",
                    suggested_fix=f"Remove quest {step['quest_id']} from this guide or fix guide race field",
                ))

    steps = guide["steps"]

    for i, step in enumerate(steps):
        qid   = step.get("quest_id")
        stype = step.get("type", "")
        coords    = step.get("coordinates", {})
        step_x    = coords.get("x", 0)
        step_y    = coords.get("y", 0)
        # map_id may live at step level OR inside coordinates
        step_map  = int(step.get("map_id", coords.get("map_id", coords.get("map", 0))) or 0)

        if not qid:
            continue

        qid_str = str(qid)
        quest = data["quests"].get(qid_str)

        # ----------------------------------------------------------------
        # Quest existence
        # ----------------------------------------------------------------
        if not quest:
            issues.append(_issue(guide_rel, i, step, "QUEST_NOT_FOUND", "BLOCKER",
                f"quest {qid} not found in DB",
                db_evidence=f"quests.json has no entry for {qid}",
                suggested_fix="Remove all steps for this quest from the guide"))
            continue

        # ----------------------------------------------------------------
        # Event quest flag
        # ----------------------------------------------------------------
        if qid in data["event_quests"]:
            issues.append(_issue(guide_rel, i, step, "EVENT_QUEST", "WARNING",
                f"quest {qid} '{quest.get('LogTitle','?')}' is a seasonal/event quest",
                db_evidence="Listed in event_quests.txt",
                suggested_fix="Review whether this quest is available on this server; remove if not"))

        # ----------------------------------------------------------------
        # accept_quest
        # ----------------------------------------------------------------
        if stype == "accept_quest":
            # Prereq chain
            prev = quest.get("PrevQuestID", 0) or 0
            if prev > 0 and prev not in quests_seen:
                if str(prev) in data["quests"]:
                    issues.append(_issue(guide_rel, i, step, "MISSING_PREREQ", "WARNING",
                        f"quest {qid} needs prereq {prev} which hasn't been accepted yet",
                        suggested_fix=f"Insert accept+turnin steps for q{prev} before this step"))

            quests_seen.add(qid)
            quest_accept_map[qid] = step_map

            npc_id = step.get("npc_id")
            if npc_id and step_x != 0:
                spawns = data["npc_spawns"].get(str(npc_id), [])
                if not spawns:
                    issues.append(_issue(guide_rel, i, step, "NPC_NO_SPAWN", "BLOCKER",
                        f"accept NPC {npc_id} has zero world spawns",
                        db_evidence=f"npc_spawns['{npc_id}'] is empty",
                        suggested_fix=f"Remove all steps for quest {qid} from guide"))
                else:
                    spawn_maps = set(s.get("map") for s in spawns)
                    if step_map not in spawn_maps:
                        issues.append(_issue(guide_rel, i, step, "NPC_WRONG_MAP", "BLOCKER",
                            f"accept NPC {npc_id} spawns on map(s) {spawn_maps} but step is on map {step_map}",
                            db_evidence=f"spawn maps: {spawn_maps}",
                            suggested_fix=f"Remove steps for quest {qid} or fix map_id"))
                    else:
                        d, best = _nearest_spawn(spawns, step_x, step_y, step_map)
                        if d > 30:
                            issues.append(_issue(guide_rel, i, step, "BAD_COORDS", "BLOCKER",
                                f"accept NPC {npc_id} nearest spawn is {d:.0f} yd away",
                                db_evidence=f"nearest spawn: ({best['x']:.1f},{best['y']:.1f})",
                                suggested_fix=f"Set coords to ({best['x']:.2f},{best['y']:.2f},{best.get('z',0):.2f})"))
                        # Elevated z check
                        for s in spawns:
                            if s.get("map") == step_map and s.get("z", 0) >= 60:
                                issues.append(_issue(guide_rel, i, step, "POTENTIAL_PATHFINDING", "WARNING",
                                    f"accept NPC {npc_id} spawn z={s.get('z',0):.1f} (elevated platform — may be unreachable)",
                                    db_evidence=f"spawn at ({s['x']:.1f},{s['y']:.1f},z={s.get('z',0):.1f})",
                                    suggested_fix="Manual test required; remove quest if bot cannot reach NPC"))
                                break

        # ----------------------------------------------------------------
        # turn_in_quest
        # ----------------------------------------------------------------
        elif stype == "turn_in_quest":
            if qid not in has_objective:
                # Check if quest has objectives we're missing
                has_kill = any(quest.get(f"RequiredNpcOrGo{j}", 0) for j in range(1, 5))
                start_item = quest.get("StartItem", 0) or 0
                has_item = any(
                    (quest.get(f"RequiredItemId{j}", 0) or 0) not in (0, start_item)
                    for j in range(1, 7)
                )
                if has_kill or has_item:
                    issues.append(_issue(guide_rel, i, step, "MISSING_OBJECTIVE", "WARNING",
                        f"quest {qid} '{quest.get('LogTitle','?')}' has objectives but no kill/collect step",
                        suggested_fix="Add an objective step before the turn-in"))

            npc_id = step.get("npc_id")
            if npc_id and step_x != 0:
                spawns = data["npc_spawns"].get(str(npc_id), [])
                if not spawns:
                    issues.append(_issue(guide_rel, i, step, "NPC_NO_SPAWN", "BLOCKER",
                        f"turnin NPC {npc_id} has zero world spawns",
                        db_evidence=f"npc_spawns['{npc_id}'] is empty",
                        suggested_fix=f"Remove all steps for quest {qid} from guide"))
                else:
                    spawn_maps = set(s.get("map") for s in spawns)
                    if step_map not in spawn_maps:
                        issues.append(_issue(guide_rel, i, step, "NPC_WRONG_MAP", "BLOCKER",
                            f"turnin NPC {npc_id} spawns on map(s) {spawn_maps} but step is on map {step_map}",
                            db_evidence=f"spawn maps: {spawn_maps}",
                            suggested_fix=f"Remove steps for quest {qid} or fix map_id"))
                    else:
                        d, best = _nearest_spawn(spawns, step_x, step_y, step_map)
                        if d > 30:
                            issues.append(_issue(guide_rel, i, step, "BAD_TURNIN_COORDS", "BLOCKER",
                                f"turnin NPC {npc_id} nearest spawn is {d:.0f} yd away",
                                db_evidence=f"nearest spawn: ({best['x']:.1f},{best['y']:.1f})",
                                suggested_fix=f"Set coords to ({best['x']:.2f},{best['y']:.2f},{best.get('z',0):.2f})"))

        # ----------------------------------------------------------------
        # kill_mobs
        # ----------------------------------------------------------------
        elif stype == "kill_mobs":
            has_objective[qid] = True

            if step.get("_unsafe"):
                issues.append(_issue(guide_rel, i, step, "UNSAFE_OBJECTIVE", "WARNING",
                    f"generator marked step as unsafe: {step['_unsafe']}"))

            creature_ids = step.get("creature_ids", [])
            item_id = step.get("item_id")

            # Check item loot source exists
            if item_id:
                src = data["item_sources"].get(str(item_id), {})
                if not src:
                    issues.append(_issue(guide_rel, i, step, "ITEM_NO_LOOT", "WARNING",
                        f"item {item_id} has no entry in item_sources.json (item_sources.json may be incomplete)",
                        db_evidence="item_sources lookup empty",
                        suggested_fix=f"Verify item source; item_sources.json may not cover all loot tables"))

            # Categorise each creature so we can emit step-level issues only
            # when NO creature in the list saves the step.
            no_spawn_cids = []     # zero DB spawns
            cross_map_cids = []    # spawns exist but not on step_map  → (cid, spawn_maps)
            far_cids = []          # on correct map but >500 yd         → (cid, d, best)
            nearby_cids = []       # on correct map and <=500 yd (good)

            for cid in creature_ids:
                spawns = data["npc_spawns"].get(str(cid), [])
                if not spawns:
                    no_spawn_cids.append(cid)
                    continue
                spawn_maps = set(s.get("map") for s in spawns)
                if step_map is not None and step_map not in spawn_maps:
                    cross_map_cids.append((cid, spawn_maps))
                    continue
                # Spawns exist on the correct map (or map unspecified).
                if step_x != 0:
                    d, best = _nearest_spawn(spawns, step_x, step_y, step_map)
                    if d > 500 and best is not None:
                        far_cids.append((cid, d, best))
                    else:
                        nearby_cids.append(cid)
                else:
                    nearby_cids.append(cid)

            # NPC_NO_SPAWN: only a BLOCKER when that creature is the *sole* kill
            # target for the step (no other creature in the list can fulfill it).
            # If other creatures in the step have valid spawns, skip it — the bot
            # will kill those and complete the objective without ever needing this one.
            has_any_viable = bool(nearby_cids or far_cids or cross_map_cids)
            for cid in no_spawn_cids:
                if not has_any_viable:
                    issues.append(_issue(guide_rel, i, step, "NPC_NO_SPAWN", "BLOCKER",
                        f"kill target creature {cid} has zero world spawns (step has no other viable targets)",
                        db_evidence=f"npc_spawns['{cid}'] is empty",
                        suggested_fix=f"Remove quest {qid} steps from guide"))

            # Step-level CROSS_MAP_KILL: only when *all* creatures with spawns
            # are on the wrong map (no creature can fulfill the step here).
            if cross_map_cids and not nearby_cids and not far_cids:
                all_wrong_maps = set()
                cid_list = []
                for cid, sm in cross_map_cids:
                    all_wrong_maps |= sm
                    cid_list.append(cid)
                issues.append(_issue(guide_rel, i, step, "CROSS_MAP_KILL", "BLOCKER",
                    f"all kill targets {cid_list} have no spawns on map {step_map} (spawn maps: {all_wrong_maps})",
                    db_evidence=f"creature spawn maps: {all_wrong_maps}",
                    suggested_fix=f"Remove quest {qid} steps — bot would travel cross-continent"))

            # Step-level OBJECTIVE_FAR_FROM_SPAWN: only when no creature is
            # nearby but at least one is on the right map (just far away).
            elif not nearby_cids and far_cids:
                best_entry = min(far_cids, key=lambda x: x[1])
                cid, d, best = best_entry
                sev = "BLOCKER" if d >= 1500 else "WARNING"
                issues.append(_issue(guide_rel, i, step, "OBJECTIVE_FAR_FROM_SPAWN", sev,
                    f"nearest kill target ({cid}) spawn is {d:.0f} yd from step coords",
                    db_evidence=f"nearest: ({best['x']:.0f},{best['y']:.0f})",
                    suggested_fix=f"Update step coordinates to ({best['x']:.2f},{best['y']:.2f}) or remove quest"))

            # QUEST_CROSS_MAP_OBJECTIVE: kill step is on a different map than
            # the quest's accept step. Bot accepts on map A but must travel to
            # map B to kill — only viable if the guide explicitly handles travel.
            if qid in quest_accept_map and quest_accept_map[qid] != step_map:
                accept_map = quest_accept_map[qid]
                issues.append(_issue(guide_rel, i, step, "QUEST_CROSS_MAP_OBJECTIVE", "BLOCKER",
                    f"quest accepted on map {accept_map} but kill step is on map {step_map}",
                    db_evidence=f"accept_map={accept_map} kill_map={step_map}",
                    suggested_fix=f"Remove quest {qid} from guide or add transport steps"))

        # ----------------------------------------------------------------
        # collect_items / interact_gameobject
        # ----------------------------------------------------------------
        elif stype in ("collect_items", "interact_gameobject"):
            has_objective[qid] = True

            if step.get("_unsafe"):
                issues.append(_issue(guide_rel, i, step, "UNSAFE_OBJECTIVE", "WARNING",
                    f"generator marked step as unsafe: {step['_unsafe']}"))

            go_entries = step.get("source_gameobject_entries", [])
            if not go_entries and step.get("gameobject_id"):
                go_entries = [step["gameobject_id"]]

            for go_entry in go_entries:
                if not data["go_spawns"].get(str(go_entry)):
                    issues.append(_issue(guide_rel, i, step, "COLLECT_GO_NO_SPAWNS", "BLOCKER",
                        f"gameobject {go_entry} has zero world spawns",
                        db_evidence=f"go_spawns['{go_entry}'] is empty",
                        suggested_fix=f"Remove quest {qid} steps from guide"))

        # ----------------------------------------------------------------
        # use_item_at_location
        # ----------------------------------------------------------------
        elif stype == "use_item_at_location":
            has_objective[qid] = True

    # --------------------------------------------------------------------
    # Post-pass: quests accepted but no objective step
    # --------------------------------------------------------------------
    for qid in quests_seen:
        if qid in has_objective:
            continue
        quest = data["quests"].get(str(qid))
        if not quest:
            continue
        has_kill = any(quest.get(f"RequiredNpcOrGo{j}", 0) for j in range(1, 5))
        start_item = quest.get("StartItem", 0) or 0
        has_item = any(
            (quest.get(f"RequiredItemId{j}", 0) or 0) not in (0, start_item)
            for j in range(1, 7)
        )
        if has_kill or has_item:
            issues.append({
                "guide":         guide_rel,
                "step_index":    None,
                "step_id":       None,
                "quest_id":      qid,
                "npc_id":        None,
                "go_id":         None,
                "item_id":       None,
                "issue_type":    "QUEST_NO_OBJECTIVE_STEP",
                "severity":      "WARNING",
                "message":       f"quest {qid} '{quest.get('LogTitle','?')}' has objectives in DB but no kill/collect step in guide",
                "db_evidence":   f"has_kill={has_kill}, has_item={has_item}",
                "suggested_fix": "Add objective step or verify quest is purely turn-in",
            })

    return issues


# ---------------------------------------------------------------------------
# Report output
# ---------------------------------------------------------------------------

SEVER_ORDER = {"BLOCKER": 0, "WARNING": 1, "INFO": 2}


def write_json_report(issues, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(issues, f, indent=2)


def write_md_report(issues, path, all_guides_count):
    os.makedirs(os.path.dirname(path), exist_ok=True)

    blocker_count  = sum(1 for x in issues if x["severity"] == "BLOCKER")
    warning_count  = sum(1 for x in issues if x["severity"] == "WARNING")

    by_type = {}
    for x in issues:
        by_type.setdefault(x["issue_type"], 0)
        by_type[x["issue_type"]] += 1

    by_guide = {}
    for x in issues:
        by_guide.setdefault(x["guide"], []).append(x)

    with open(path, "w") as f:
        f.write("# Guide Validation Report\n\n")
        f.write(f"**Guides scanned:** {all_guides_count}  \n")
        f.write(f"**BLOCKER issues:** {blocker_count}  \n")
        f.write(f"**WARNING issues:** {warning_count}  \n\n")

        f.write("## Summary by issue type\n\n")
        f.write("| Issue type | Count |\n|---|---|\n")
        for itype, cnt in sorted(by_type.items(), key=lambda x: (-x[1], x[0])):
            f.write(f"| {itype} | {cnt} |\n")
        f.write("\n")

        f.write("## Details by guide\n\n")
        for guide_rel in sorted(by_guide):
            guide_issues = sorted(by_guide[guide_rel],
                                  key=lambda x: (SEVER_ORDER.get(x["severity"], 9),
                                                 x["step_index"] or -1))
            blockers = sum(1 for x in guide_issues if x["severity"] == "BLOCKER")
            f.write(f"### {guide_rel}  ({blockers} blockers)\n\n")
            f.write("| Step | Quest | NPC/GO | Issue | Sev | Message | Fix |\n")
            f.write("|---|---|---|---|---|---|---|\n")
            for x in guide_issues:
                step_str  = str(x["step_index"]) if x["step_index"] is not None else "—"
                quest_str = str(x["quest_id"]) if x["quest_id"] else "—"
                npc_str   = str(x["npc_id"]) if x["npc_id"] else (str(x["go_id"]) if x["go_id"] else "—")
                msg       = x["message"].replace("|", "\\|")
                fix       = x["suggested_fix"].replace("|", "\\|")
                f.write(f"| {step_str} | {quest_str} | {npc_str} | {x['issue_type']} | {x['severity']} | {msg} | {fix} |\n")
            f.write("\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--guides",   default=DEFAULT_GUIDES)
    parser.add_argument("--data",     default=DEFAULT_DATA)
    parser.add_argument("--json-out", default=os.path.join(DEFAULT_REPORTS, "guide_validation_latest.json"))
    parser.add_argument("--md-out",   default=os.path.join(DEFAULT_REPORTS, "guide_validation_latest.md"))
    parser.add_argument("--severity", default="ALL",
                        choices=["BLOCKER", "WARNING", "INFO", "ALL"],
                        help="Filter output to this severity and above")
    args = parser.parse_args()

    print(f"Loading DB data from {args.data} ...")
    data = load_data(args.data)
    print(f"  {len(data['quests'])} quests, {len(data['npc_spawns'])} NPC spawn entries, "
          f"{len(data['go_spawns'])} GO spawn entries, "
          f"{len(data['event_quests'])} event quest IDs")

    skip_dirs = {
        os.path.join(args.guides, "generated_backup"),
        os.path.join(args.guides, "generated"),
    }
    guide_files = []
    for root, dirs, files in os.walk(args.guides):
        # Skip generated_backup and generated (latter mirrors deployed guides)
        dirs[:] = [d for d in sorted(dirs)
                   if not any(os.path.join(root, d).startswith(sd) for sd in skip_dirs)]
        for fn in sorted(files):
            if fn.endswith(".yaml"):
                guide_files.append(os.path.join(root, fn))

    print(f"Scanning {len(guide_files)} guide files ...\n")

    # Pre-pass: collect all guide IDs and metadata for cross-guide checks.
    all_guide_ids = set()
    guide_meta = {}   # id -> {faction, race, level_min, level_max, next_guide, path}
    for path in guide_files:
        try:
            with open(path) as f:
                g = yaml.safe_load(f)
            if g and g.get("id"):
                gid = g["id"]
                all_guide_ids.add(gid)
                guide_meta[gid] = {
                    "faction":   (g.get("faction") or "").lower(),
                    "race":      (g.get("race") or "").lower(),
                    "level_min": g.get("level_min", 0) or 0,
                    "level_max": g.get("level_max", 0) or 0,
                    "next_guide": g.get("next_guide", ""),
                    "path":      path,
                }
        except Exception:
            pass

    all_issues = []
    sev_filter = {"ALL": {"BLOCKER", "WARNING", "INFO"},
                  "BLOCKER": {"BLOCKER"},
                  "WARNING": {"BLOCKER", "WARNING"},
                  "INFO": {"BLOCKER", "WARNING", "INFO"}}[args.severity]

    for path in guide_files:
        rel = os.path.relpath(path, args.guides)
        issues = validate_guide(path, rel, data, all_guide_ids=all_guide_ids)
        visible = [x for x in issues if x["severity"] in sev_filter]
        if visible:
            blockers = sum(1 for x in visible if x["severity"] == "BLOCKER")
            print(f"  {'✗' if blockers else '⚠'} {rel}: {blockers} blockers, "
                  f"{len(visible)-blockers} warnings")
            for x in sorted(visible, key=lambda x: (SEVER_ORDER.get(x["severity"],9),
                                                      x["step_index"] or -1)):
                step_str = f"step {x['step_index']}" if x["step_index"] is not None else "post-pass"
                print(f"      [{x['severity']}] {x['issue_type']} {step_str}: {x['message']}")
        else:
            print(f"  ✓ {rel}")
        all_issues.extend(issues)

    # Cross-guide post-pass: next_guide faction/race safety.
    for gid, meta in guide_meta.items():
        nxt = meta["next_guide"]
        if not nxt or nxt not in guide_meta:
            continue
        nxt_meta = guide_meta[nxt]
        src_faction = meta["faction"]
        nxt_faction = nxt_meta["faction"]
        if src_faction and nxt_faction and src_faction != nxt_faction and "any" not in (src_faction, nxt_faction):
            rel_path = os.path.relpath(meta["path"], args.guides)
            all_issues.append({
                "guide":         rel_path,
                "step_index":    None,
                "step_id":       None,
                "quest_id":      None,
                "npc_id":        None,
                "go_id":         None,
                "item_id":       None,
                "issue_type":    "NEXT_GUIDE_FACTION_MISMATCH",
                "severity":      "BLOCKER",
                "message":       f"next_guide '{nxt}' is {nxt_faction} but this guide is {src_faction}",
                "db_evidence":   f"source faction={src_faction}, target faction={nxt_faction}",
                "suggested_fix": f"Replace next_guide with a {src_faction} guide",
            })

    blocker_total  = sum(1 for x in all_issues if x["severity"] == "BLOCKER")
    warning_total  = sum(1 for x in all_issues if x["severity"] == "WARNING")
    by_type = {}
    for x in all_issues:
        by_type.setdefault(x["issue_type"], 0)
        by_type[x["issue_type"]] += 1

    print(f"\n{'='*60}")
    print(f"Scanned {len(guide_files)} guides: {blocker_total} BLOCKER, {warning_total} WARNING")
    for itype, cnt in sorted(by_type.items(), key=lambda x: (-x[1], x[0])):
        print(f"  {itype}: {cnt}")

    write_json_report(all_issues, args.json_out)
    write_md_report(all_issues, args.md_out, len(guide_files))
    print(f"\nReports written:")
    print(f"  {args.json_out}")
    print(f"  {args.md_out}")

    return 1 if blocker_total > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
