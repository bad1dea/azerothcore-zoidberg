#!/usr/bin/env python3
"""Fix bot step_index values after guide files change.

When quest chains are removed from a guide (e.g. by fix_guide_validation_issues.py),
all step indices after the removal shift left. Bots persisted with old step_index
values will land at wrong steps when the new guide is loaded.

This script:
  1. Reads the new guide YAML files.
  2. Queries the live DB for current bot guide/step_index values.
  3. Reads the OLD guide from git HEAD (pre-fix) to find the step ID at the old index.
  4. Finds that same step ID in the new guide.
  5. Updates the DB with the corrected index.

Run this AFTER stopping the server and BEFORE starting with new guides.

Usage:
  python3 tools/fix_bot_step_indices.py [--dry-run]

  --dry-run   Print what would change, don't write to DB.

Requires:
  SSH access to khuong@10.10.30.20 (the database host).
"""

import argparse
import subprocess
import sys
import os
import yaml


DB_HOST = "10.10.30.20"
DB_CONTAINER = "ac-database"
DB_PASSWORD = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"

MODULE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUIDES_DIR = os.path.join(MODULE_DIR, "data", "guides")


def run_ssh(cmd):
    result = subprocess.run(
        ["ssh", DB_HOST, cmd],
        capture_output=True, text=True
    )
    return result.stdout.strip(), result.stderr.strip(), result.returncode


def db_query(sql):
    cmd = f"docker exec {DB_CONTAINER} mysql -uroot -p'{DB_PASSWORD}' acore_characters -sN -e \"{sql}\" 2>/dev/null"
    out, err, rc = run_ssh(cmd)
    return out


def db_execute(sql, dry_run=False):
    if dry_run:
        print(f"  [DRY RUN] SQL: {sql}")
        return
    cmd = f"docker exec {DB_CONTAINER} mysql -uroot -p'{DB_PASSWORD}' acore_characters -e \"{sql}\" 2>/dev/null"
    run_ssh(cmd)


def load_guide(guide_path):
    if not os.path.exists(guide_path):
        return None
    with open(guide_path) as f:
        return yaml.safe_load(f)


def get_old_guide(guide_id_path_rel):
    """Get old guide steps from git HEAD using git show."""
    result = subprocess.run(
        ["git", "-C", os.path.join(MODULE_DIR, "..", ".."),
         "show", f"HEAD:modules/mod-idlebot/data/guides/{guide_id_path_rel}"],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        return None
    return yaml.safe_load(result.stdout)


def guide_id_to_path(guide_id):
    """Walk data/guides/ to find a guide file matching the guide id field."""
    for root, dirs, files in os.walk(GUIDES_DIR):
        dirs[:] = [d for d in dirs if d not in ("generated", "generated_backup")]
        for fn in files:
            if not fn.endswith(".yaml"):
                continue
            path = os.path.join(root, fn)
            try:
                g = yaml.safe_load(open(path))
                if g and g.get("id") == guide_id:
                    return path, os.path.relpath(path, GUIDES_DIR)
            except Exception:
                pass
    return None, None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    # Fetch all bots with a guide assigned
    rows = db_query("SELECT bot_name, guide_id, step_index FROM idlebot_bots WHERE guide_id IS NOT NULL AND guide_id != ''")
    if not rows:
        print("No bots with assigned guides found.")
        return 0

    bots = []
    for line in rows.splitlines():
        parts = line.split("\t")
        if len(parts) == 3:
            bots.append({"name": parts[0], "guide_id": parts[1], "step_index": int(parts[2])})

    print(f"Found {len(bots)} bot(s) with guides.")
    fixes = 0

    for bot in bots:
        name = bot["name"]
        guide_id = bot["guide_id"]
        old_step_idx = bot["step_index"]

        guide_path, guide_rel = guide_id_to_path(guide_id)
        if not guide_path:
            print(f"  {name}: guide '{guide_id}' not found on disk — skipping")
            continue

        new_guide = load_guide(guide_path)
        if not new_guide or "steps" not in new_guide:
            continue
        new_steps = new_guide["steps"]

        old_guide = get_old_guide(guide_rel)
        if not old_guide or "steps" not in old_guide:
            print(f"  {name}: no old guide in git HEAD for '{guide_rel}' — no adjustment needed")
            continue
        old_steps = old_guide["steps"]

        if len(old_steps) == len(new_steps):
            # Guide unchanged — no adjustment needed
            continue

        if old_step_idx >= len(old_steps):
            print(f"  {name}: old step_index {old_step_idx} >= old guide length {len(old_steps)} — clamping")
            new_step_idx = len(new_steps) - 1
        else:
            old_step_id = old_steps[old_step_idx].get("id", "")
            if not old_step_id:
                print(f"  {name}: old step {old_step_idx} has no id — skipping")
                continue

            # Find this step_id in the new guide
            new_step_idx = None
            for j, ns in enumerate(new_steps):
                if ns.get("id") == old_step_id:
                    new_step_idx = j
                    break

            if new_step_idx is None:
                # Step was removed — advance to the next surviving step
                for k in range(old_step_idx + 1, len(old_steps)):
                    sid = old_steps[k].get("id", "")
                    for j, ns in enumerate(new_steps):
                        if ns.get("id") == sid:
                            new_step_idx = j
                            break
                    if new_step_idx is not None:
                        break
                if new_step_idx is None:
                    new_step_idx = len(new_steps) - 1
                print(f"  {name}: step '{old_step_id}' was removed → advancing to step {new_step_idx} ('{new_steps[new_step_idx].get('id')}')")

        if new_step_idx != old_step_idx:
            print(f"  {name} [{guide_id}]: step {old_step_idx} → {new_step_idx} ('{new_steps[min(new_step_idx, len(new_steps)-1)].get('id')}')")
            db_execute(
                f"UPDATE idlebot_bots SET step_index={new_step_idx} WHERE bot_name='{name}'",
                dry_run=args.dry_run
            )
            fixes += 1
        else:
            print(f"  {name} [{guide_id}]: step {old_step_idx} unchanged")

    print(f"\n{'[DRY RUN] ' if args.dry_run else ''}Fixed {fixes} bot(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
