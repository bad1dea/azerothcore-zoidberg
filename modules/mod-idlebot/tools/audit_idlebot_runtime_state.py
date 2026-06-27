#!/usr/bin/env python3
"""Audit idlebot runtime state against guide files and DB.

Reports:
  - Bots with no guide assigned
  - Bots whose guide ID doesn't exist on disk
  - Bots whose guide doesn't match their race/class/faction
  - Bots whose step_index is out of range for their guide
  - Bots with high death counts (configurable threshold)
  - Bots whose death_count_total > 50 (suspicious for early levels)
  - Summary of all bots with guide/step/death/level

Writes:
  reports/runtime_state_audit_latest.json
  reports/runtime_state_audit_latest.md

Usage:
  python3 tools/audit_idlebot_runtime_state.py [--deaths-threshold N]

Requires SSH access to khuong@10.10.30.20 (the database host).
"""

import argparse
import json
import os
import subprocess
import sys
import yaml

DB_HOST = "10.10.30.20"
DB_CONTAINER = "ac-database"
DB_PASSWORD = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"

MODULE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUIDES_DIR = os.path.join(MODULE_DIR, "data", "guides")
REPORTS_DIR = os.path.join(MODULE_DIR, "reports")

# AzerothCore race IDs
RACE_FACTION = {
    1: "Alliance",   # Human
    3: "Alliance",   # Dwarf
    4: "Alliance",   # Night Elf
    7: "Alliance",   # Gnome
    11: "Alliance",  # Draenei
    2: "Horde",      # Orc
    5: "Horde",      # Undead
    6: "Horde",      # Tauren
    8: "Horde",      # Troll
    10: "Horde",     # Blood Elf
}

RACE_NAME = {
    1: "Human", 2: "Orc", 3: "Dwarf", 4: "NightElf", 5: "Undead",
    6: "Tauren", 7: "Gnome", 8: "Troll", 10: "BloodElf", 11: "Draenei",
}

CLASS_NAME = {
    1: "Warrior", 2: "Paladin", 3: "Hunter", 4: "Rogue", 5: "Priest",
    6: "DeathKnight", 7: "Shaman", 8: "Mage", 9: "Warlock", 11: "Druid",
}

HIGH_DEATHS_THRESHOLD_TOTAL = 200   # flag if total deaths >= this
HIGH_DEATHS_CURRENT_STEP    = 3     # flag if deaths on current step >= this
SKIP_RATE_THRESHOLD         = 0.4   # flag if failures/quests > this


def run_ssh(cmd):
    result = subprocess.run(
        ["ssh", DB_HOST, cmd],
        capture_output=True, text=True
    )
    return result.stdout.strip()


def db_query(sql):
    cmd = (
        f"docker exec {DB_CONTAINER} mysql -uroot -p'{DB_PASSWORD}' "
        f"acore_characters -sN -e \"{sql}\" 2>/dev/null"
    )
    return run_ssh(cmd)


def db_query_world(sql):
    cmd = (
        f"docker exec {DB_CONTAINER} mysql -uroot -p'{DB_PASSWORD}' "
        f"acore_world -sN -e \"{sql}\" 2>/dev/null"
    )
    return run_ssh(cmd)


def load_all_guides():
    guides = {}
    for root, dirs, files in os.walk(GUIDES_DIR):
        dirs[:] = [d for d in dirs if d not in ("generated", "generated_backup")]
        for fn in files:
            if not fn.endswith(".yaml"):
                continue
            path = os.path.join(root, fn)
            try:
                g = yaml.safe_load(open(path))
                if g and g.get("id"):
                    guides[g["id"]] = {
                        "path": os.path.relpath(path, GUIDES_DIR),
                        "steps": g.get("steps", []),
                        "faction": g.get("faction"),
                        "race": g.get("race"),
                        "class": g.get("class"),
                        "min_level": g.get("min_level"),
                        "max_level": g.get("max_level"),
                    }
            except Exception:
                pass
    return guides


def get_bot_event_counts():
    rows = db_query(
        "SELECT ib.bot_name, ie.event_type, COUNT(*) as cnt "
        "FROM idlebot_events ie "
        "JOIN idlebot_bots ib ON ib.id = ie.bot_id "
        "GROUP BY ib.bot_name, ie.event_type"
    )
    counts = {}
    for line in rows.splitlines():
        parts = line.split("\t")
        if len(parts) == 3:
            name, etype, cnt = parts[0], parts[1], int(parts[2])
            counts.setdefault(name, {})[etype] = cnt
    return counts


def get_recent_failures():
    rows = run_ssh(
        f"docker exec {DB_CONTAINER} mysql -uroot -p'{DB_PASSWORD}' "
        f"acore_characters -sN -e \""
        f"SELECT ib.bot_name, HEX(ie.detail), ie.created_at "
        f"FROM idlebot_events ie "
        f"JOIN idlebot_bots ib ON ib.id = ie.bot_id "
        f"WHERE ie.event_type='FAILURE' "
        f"ORDER BY ie.created_at DESC LIMIT 50"
        f"\" 2>/dev/null"
    )
    failures = []
    for line in rows.splitlines():
        parts = line.split("\t")
        if len(parts) == 3:
            name, hex_detail, dt = parts
            try:
                detail = bytes.fromhex(hex_detail).decode("utf-8", errors="replace")
            except Exception:
                detail = hex_detail
            failures.append({"bot": name, "detail": detail, "time": dt})
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--deaths-threshold", type=int, default=HIGH_DEATHS_THRESHOLD_TOTAL)
    args = parser.parse_args()

    print("Loading guides...")
    guides = load_all_guides()

    print("Querying bot state...")
    rows = db_query(
        "SELECT ib.bot_name, ib.guide_id, ib.step_index, ib.step_state, "
        "ib.death_count_total, ib.death_count_current_step, ib.last_trained_level, ib.active, "
        "c.level, c.race, c.class, c.map, "
        "ROUND(c.position_x,1), ROUND(c.position_y,1), ROUND(c.position_z,1), c.online "
        "FROM idlebot_bots ib "
        "JOIN characters c ON c.name = ib.bot_name "
        "ORDER BY ib.bot_name"
    )

    bots = []
    for line in rows.splitlines():
        parts = line.split("\t")
        if len(parts) < 16:
            continue
        bots.append({
            "name": parts[0],
            "guide_id": parts[1] if parts[1] != "NULL" else None,
            "step_index": int(parts[2]),
            "step_state": parts[3],
            "deaths_total": int(parts[4]),
            "deaths_step": int(parts[5]),
            "last_trained_level": int(parts[6]),
            "active": parts[7] == "1",
            "level": int(parts[8]),
            "race": int(parts[9]),
            "class": int(parts[10]),
            "map": int(parts[11]),
            "x": float(parts[12]),
            "y": float(parts[13]),
            "z": float(parts[14]),
            "online": parts[15] == "1",
        })

    print(f"Found {len(bots)} bot(s). Fetching event counts...")
    event_counts = get_bot_event_counts()
    recent_failures = get_recent_failures()

    issues = []
    bot_reports = []

    for bot in bots:
        name = bot["name"]
        guide_id = bot["guide_id"]
        race_name = RACE_NAME.get(bot["race"], f"race{bot['race']}")
        class_name = CLASS_NAME.get(bot["class"], f"class{bot['class']}")
        faction = RACE_FACTION.get(bot["race"], "Unknown")
        bot_issues = []

        guide = None
        if not guide_id:
            bot_issues.append("NO_GUIDE: bot has no guide assigned — will log warnings every 60 ticks")
        else:
            guide = guides.get(guide_id)
            if not guide:
                bot_issues.append(f"GUIDE_NOT_FOUND: guide '{guide_id}' not on disk — server will clear it on load")
            else:
                step_count = len(guide["steps"])

                # Step index out of range
                if bot["step_index"] >= step_count:
                    bot_issues.append(
                        f"STEP_OOB: step_index {bot['step_index']} >= guide length {step_count}"
                    )

                # Guide faction mismatch
                guide_faction = guide.get("faction")
                if guide_faction and guide_faction.lower() != faction.lower():
                    bot_issues.append(
                        f"FACTION_MISMATCH: bot is {faction}, guide is for {guide_faction}"
                    )

                # Guide race mismatch (if guide specifies a race)
                guide_race = guide.get("race")
                if guide_race:
                    guide_races = [r.strip().lower() for r in str(guide_race).split(",")]
                    if race_name.lower() not in guide_races and "all" not in guide_races:
                        bot_issues.append(
                            f"RACE_MISMATCH: bot is {race_name}, guide race is '{guide_race}'"
                        )

        # High total death count
        if bot["deaths_total"] >= args.deaths_threshold:
            bot_issues.append(
                f"HIGH_DEATHS: {bot['deaths_total']} total deaths (threshold {args.deaths_threshold})"
            )

        # High death count on current step
        if bot["deaths_step"] >= HIGH_DEATHS_CURRENT_STEP:
            bot_issues.append(
                f"STEP_DEATHS: {bot['deaths_step']} deaths on current step (threshold {HIGH_DEATHS_CURRENT_STEP})"
            )

        # High skip/failure rate
        bot_events = event_counts.get(name, {})
        failure_count = bot_events.get("FAILURE", 0)
        quest_count = bot_events.get("QUEST", 0)
        if quest_count > 10 and failure_count / quest_count > SKIP_RATE_THRESHOLD:
            bot_issues.append(
                f"HIGH_SKIP_RATE: {failure_count} failures vs {quest_count} quest events "
                f"({100*failure_count/quest_count:.0f}% failure rate)"
            )

        # Not online but active
        if bot["active"] and not bot["online"]:
            bot_issues.append("OFFLINE: bot is active but not currently online")

        step_id = "?"
        if guide and 0 <= bot["step_index"] < len(guide["steps"]):
            step_id = guide["steps"][bot["step_index"]].get("id", "?")

        bot_report = {
            "name": name,
            "level": bot["level"],
            "race": race_name,
            "class": class_name,
            "faction": faction,
            "guide_id": guide_id,
            "step_index": bot["step_index"],
            "step_id": step_id,
            "step_state": bot["step_state"],
            "deaths_total": bot["deaths_total"],
            "deaths_step": bot["deaths_step"],
            "last_trained_level": bot["last_trained_level"],
            "map": bot["map"],
            "x": bot["x"],
            "y": bot["y"],
            "z": bot["z"],
            "online": bot["online"],
            "event_counts": bot_events,
            "issues": bot_issues,
        }
        bot_reports.append(bot_report)

        for issue in bot_issues:
            issues.append({"bot": name, "issue": issue})

    # Write JSON report
    os.makedirs(REPORTS_DIR, exist_ok=True)
    json_path = os.path.join(REPORTS_DIR, "runtime_state_audit_latest.json")
    md_path = os.path.join(REPORTS_DIR, "runtime_state_audit_latest.md")

    report = {
        "date": "2026-06-25",
        "total_bots": len(bots),
        "total_issues": len(issues),
        "bots": bot_reports,
        "recent_failures": recent_failures[:20],
        "issues": issues,
    }
    with open(json_path, "w") as f:
        json.dump(report, f, indent=2)
    print(f"Wrote {json_path}")

    # Write Markdown report
    with open(md_path, "w") as f:
        f.write(f"# Idlebot Runtime State Audit\n\n")
        f.write(f"Generated: 2026-06-25  \n")
        f.write(f"Total bots: {len(bots)}  \n")
        f.write(f"Total issues: {len(issues)}  \n\n")

        f.write("## Bot Summary\n\n")
        f.write("| Bot | Level | Race | Class | Guide | Step | Deaths | Issues |\n")
        f.write("|---|---|---|---|---|---|---|---|\n")
        for b in bot_reports:
            guide_str = b["guide_id"] or "**NONE**"
            step_str = f"{b['step_index']} ({b['step_id']})" if b["guide_id"] else "-"
            issue_str = str(len(b["issues"])) if b["issues"] else "0"
            if b["issues"]:
                issue_str = f"**{len(b['issues'])}**"
            f.write(f"| {b['name']} | {b['level']} | {b['race']} | {b['class']} | "
                    f"{guide_str} | {step_str} | {b['deaths_total']} | {issue_str} |\n")

        if issues:
            f.write("\n## Issues\n\n")
            for i in issues:
                f.write(f"- **{i['bot']}**: {i['issue']}\n")

        f.write("\n## Per-Bot Details\n\n")
        for b in bot_reports:
            f.write(f"### {b['name']}\n")
            f.write(f"- Level {b['level']} {b['race']} {b['class']} ({b['faction']})\n")
            f.write(f"- Guide: {b['guide_id'] or '(none)'}\n")
            f.write(f"- Step: {b['step_index']} / {b['step_id']} [{b['step_state']}]\n")
            f.write(f"- Deaths: {b['deaths_total']} total, {b['deaths_step']} on current step\n")
            f.write(f"- Last trained level: {b['last_trained_level']}\n")
            f.write(f"- Map: {b['map']} at ({b['x']}, {b['y']}, {b['z']})\n")
            f.write(f"- Online: {b['online']}\n")
            evts = b["event_counts"]
            if evts:
                evt_str = ", ".join(f"{k}={v}" for k, v in sorted(evts.items()))
                f.write(f"- Events: {evt_str}\n")
            if b["issues"]:
                f.write("- **Issues**:\n")
                for iss in b["issues"]:
                    f.write(f"  - {iss}\n")
            f.write("\n")

        if recent_failures:
            f.write("## Recent Failures (last 20)\n\n")
            f.write("| Time | Bot | Detail |\n")
            f.write("|---|---|---|\n")
            for rf in recent_failures:
                f.write(f"| {rf['time']} | {rf['bot']} | {rf['detail']} |\n")

    print(f"Wrote {md_path}")

    # Console summary
    print(f"\n{'='*60}")
    print(f"AUDIT SUMMARY: {len(bots)} bots, {len(issues)} issues")
    print(f"{'='*60}")
    for b in bot_reports:
        status = "OK" if not b["issues"] else f"ISSUES({len(b['issues'])})"
        guide_str = b["guide_id"] or "NO_GUIDE"
        print(f"  {b['name']:15} Lv{b['level']:2} {b['race']:10} {b['class']:11} "
              f"guide={guide_str:30} step={b['step_index']:3} deaths={b['deaths_total']:4} [{status}]")

    if issues:
        print(f"\nISSUES:")
        for i in issues:
            print(f"  [{i['bot']}] {i['issue']}")

    return 1 if issues else 0


if __name__ == "__main__":
    sys.exit(main())
