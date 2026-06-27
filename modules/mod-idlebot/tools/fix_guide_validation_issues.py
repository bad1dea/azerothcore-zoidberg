#!/usr/bin/env python3
"""Fix BLOCKER issues identified by validate_guides_against_db.py.

Reads reports/guide_validation_latest.json and applies fixes to guide YAML files.
Idempotent: safe to run multiple times.

Fix strategies:
  NPC_NO_SPAWN / NPC_WRONG_MAP / CROSS_MAP_KILL / OBJECTIVE_WRONG_MAP /
  OBJECTIVE_FAR_FROM_SPAWN (>500 yd):
      Remove the entire quest chain (all steps sharing quest_id).

  BAD_COORDS / BAD_TURNIN_COORDS:
      Update step coordinates to nearest matching spawn.

  COLLECT_GO_NO_SPAWNS:
      Remove the entire quest chain.

  QUEST_NOT_FOUND:
      Remove all steps with that quest_id.

  WARNING-only issues (MISSING_PREREQ, EVENT_QUEST, POTENTIAL_PATHFINDING,
  UNSAFE_OBJECTIVE, QUEST_NO_OBJECTIVE_STEP, MISSING_OBJECTIVE):
      Not auto-fixed. Listed in output.

Usage:
  python3 fix_guide_validation_issues.py [--report FILE] [--guides-dir DIR]
      [--dry-run] [--backup]

  --dry-run   Print what would change, don't write files.
  --backup    Write <file>.bak before modifying.
"""

import argparse
import json
import math
import os
import shutil
import sys

import yaml

MODULE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_REPORT    = os.path.join(MODULE_DIR, "reports", "guide_validation_latest.json")
DEFAULT_GUIDES    = os.path.join(MODULE_DIR, "data", "guides")
DEFAULT_DATA      = os.path.join(MODULE_DIR, "data", "generated")

# Issue types that cause full quest-chain removal
REMOVE_QUEST_CHAIN = {
    "NPC_NO_SPAWN",
    "NPC_WRONG_MAP",
    "CROSS_MAP_KILL",
    "OBJECTIVE_WRONG_MAP",
    "COLLECT_GO_NO_SPAWNS",
    "QUEST_NOT_FOUND",
    "QUEST_CROSS_MAP_OBJECTIVE",
}
# Large-distance threshold for OBJECTIVE_FAR_FROM_SPAWN
FAR_SPAWN_THRESHOLD = 500

# Issue types that update NPC coords
COORD_FIX_TYPES = {"BAD_COORDS", "BAD_TURNIN_COORDS"}

# Warnings not auto-fixed
WARN_ONLY = {
    "MISSING_PREREQ", "EVENT_QUEST", "POTENTIAL_PATHFINDING",
    "UNSAFE_OBJECTIVE", "QUEST_NO_OBJECTIVE_STEP", "MISSING_OBJECTIVE",
}


def dist(x1, y1, x2, y2):
    return math.sqrt((x1 - x2) ** 2 + (y1 - y2) ** 2)


def load_npc_spawns(data_dir):
    path = os.path.join(data_dir, "npc_spawns.json")
    with open(path) as f:
        return json.load(f)


def load_guide(path):
    with open(path) as f:
        return yaml.safe_load(f)


def dump_guide(guide, path):
    with open(path, "w") as f:
        yaml.dump(guide, f, default_flow_style=False, sort_keys=False,
                  allow_unicode=True)


def _nearest_spawn_for_npc(npc_id, step_map, step_x, step_y, npc_spawns):
    spawns = npc_spawns.get(str(npc_id), [])
    same_map = [s for s in spawns if s.get("map") == step_map]
    if not same_map:
        return None
    return min(same_map, key=lambda s: dist(s["x"], s["y"], step_x, step_y))


# ---------------------------------------------------------------------------
# Core fix logic per guide
# ---------------------------------------------------------------------------

def fix_guide(guide_path, issues_for_guide, npc_spawns, dry_run, backup):
    """Apply fixes to one guide file. Returns (changed, summary_lines)."""
    guide = load_guide(guide_path)
    if not guide or "steps" not in guide:
        return False, []

    steps = guide["steps"]
    summary = []

    # ----------------------------------------------------------------
    # Collect quest IDs to remove entirely
    # ----------------------------------------------------------------
    quests_to_remove = set()
    coord_fixes = {}   # step_id -> {"x":, "y":, "z":}

    for issue in issues_for_guide:
        itype  = issue["issue_type"]
        qid    = issue["quest_id"]
        step_i = issue["step_index"]

        if itype in REMOVE_QUEST_CHAIN:
            if qid:
                quests_to_remove.add(qid)
        elif itype == "OBJECTIVE_FAR_FROM_SPAWN":
            if qid:
                quests_to_remove.add(qid)
        elif itype in COORD_FIX_TYPES:
            if step_i is not None and step_i < len(steps):
                step = steps[step_i]
                npc_id   = step.get("npc_id")
                coords   = step.get("coordinates", {})
                # map_id may be at step level OR inside coordinates
                step_map = int(step.get("map_id", coords.get("map_id", coords.get("map", 0))) or 0)
                step_x   = coords.get("x", 0)
                step_y   = coords.get("y", 0)
                if npc_id:
                    best = _nearest_spawn_for_npc(npc_id, step_map, step_x, step_y, npc_spawns)
                    if best:
                        coord_fixes[step["id"]] = {"x": best["x"], "y": best["y"],
                                                    "z": best.get("z", coords.get("z", 0))}

    # ----------------------------------------------------------------
    # Apply quest chain removals (idempotent: filter by quest_id)
    # ----------------------------------------------------------------
    if quests_to_remove:
        original_count = len(steps)
        kept = []
        removed_ids = set()
        for step in steps:
            if step.get("quest_id") in quests_to_remove:
                removed_ids.add(step.get("id", "?"))
            else:
                kept.append(step)

        removed_count = original_count - len(kept)
        if removed_count > 0:
            guide["steps"] = kept
            for qid in sorted(quests_to_remove):
                summary.append(f"  REMOVED quest chain q{qid} ({removed_count} total steps removed)")

    # ----------------------------------------------------------------
    # Apply coordinate fixes
    # ----------------------------------------------------------------
    for step in guide["steps"]:
        sid = step.get("id", "")
        if sid in coord_fixes:
            coords = step.get("coordinates", {})
            old_x, old_y = coords.get("x"), coords.get("y")   # capture before mutation
            new_coords = coord_fixes[sid]
            if "coordinates" not in step:
                step["coordinates"] = {}
            step["coordinates"]["x"] = round(new_coords["x"], 2)
            step["coordinates"]["y"] = round(new_coords["y"], 2)
            step["coordinates"]["z"] = round(new_coords["z"], 2)
            summary.append(f"  FIXED coords for step '{sid}': ({old_x},{old_y}) "
                           f"-> ({new_coords['x']:.2f},{new_coords['y']:.2f})")

    if not summary:
        return False, []

    if not dry_run:
        if backup:
            shutil.copy2(guide_path, guide_path + ".bak")
        dump_guide(guide, guide_path)

    return True, summary


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--report",    default=DEFAULT_REPORT)
    parser.add_argument("--guides-dir", default=DEFAULT_GUIDES)
    parser.add_argument("--data",      default=DEFAULT_DATA)
    parser.add_argument("--dry-run",   action="store_true")
    parser.add_argument("--backup",    action="store_true")
    args = parser.parse_args()

    if not os.path.exists(args.report):
        print(f"ERROR: report not found: {args.report}", file=sys.stderr)
        print("Run validate_guides_against_db.py first.", file=sys.stderr)
        return 1

    with open(args.report) as f:
        all_issues = json.load(f)

    npc_spawns = load_npc_spawns(args.data)

    # Group by guide path
    by_guide = {}
    for issue in all_issues:
        if issue["severity"] == "BLOCKER":
            by_guide.setdefault(issue["guide"], []).append(issue)

    warn_only_issues = [x for x in all_issues if x["severity"] == "WARNING"]
    if warn_only_issues:
        print(f"  (Skipping {len(warn_only_issues)} WARNING issues — manual review required)")
        warn_by_type = {}
        for x in warn_only_issues:
            warn_by_type.setdefault(x["issue_type"], 0)
            warn_by_type[x["issue_type"]] += 1
        for itype, cnt in sorted(warn_by_type.items()):
            print(f"    {itype}: {cnt}")
        print()

    if not by_guide:
        print("No BLOCKER issues to fix.")
        return 0

    if args.dry_run:
        print("[DRY RUN] No files will be written.\n")

    changed_guides = 0
    for guide_rel in sorted(by_guide):
        guide_abs = os.path.join(args.guides_dir, guide_rel)
        if not os.path.exists(guide_abs):
            print(f"  SKIP (not found): {guide_rel}")
            continue

        changed, summary = fix_guide(guide_abs, by_guide[guide_rel],
                                     npc_spawns, args.dry_run, args.backup)
        if changed:
            changed_guides += 1
            tag = "[DRY RUN] " if args.dry_run else ""
            print(f"{tag}{guide_rel}:")
            for line in summary:
                print(line)
        else:
            print(f"  (already clean) {guide_rel}")

    print(f"\n{'[DRY RUN] ' if args.dry_run else ''}Fixed {changed_guides} guide file(s).")
    if not args.dry_run:
        print("Re-run validate_guides_against_db.py to confirm blocker count dropped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
