#!/usr/bin/env python3
"""Analyze idlebot death patterns from idlebot_events and idlebot_bots.

Reports:
  - Total deaths per bot with rate (deaths/hour)
  - Quest-skips due to death loops (FAILURE events)
  - Circuit-breaker activations (rate-limit pauses)
  - Death hotspots: guide+step combos with repeated deaths
  - Recent deaths in the last window (configurable hours)

Writes:
  reports/death_audit_latest.md
  reports/death_audit_latest.json

Usage:
  python3 tools/analyze_idlebot_deaths.py [--hours N] [--md-out FILE] [--json-out FILE]

  --hours N    Look back N hours for "recent" deaths (default 24)
"""

import argparse
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timedelta

DB_HOST      = "10.10.30.20"
DB_CONTAINER = "ac-database"
DB_PASSWORD  = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"
MODULE_DIR   = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORTS_DIR  = os.path.join(MODULE_DIR, "reports")


def run_ssh(cmd):
    result = subprocess.run(["ssh", DB_HOST, cmd], capture_output=True, text=True)
    if result.returncode != 0 and result.stderr:
        print(f"  [ssh error] {result.stderr.strip()[:120]}", file=sys.stderr)
    return result.stdout.strip()


def db_query(sql):
    safe = sql.replace('"', '\\"')
    cmd = (
        f'docker exec {DB_CONTAINER} mysql -uroot -p\'{DB_PASSWORD}\' '
        f'acore_characters -sN -e "{safe}" 2>/dev/null'
    )
    return run_ssh(cmd)


def parse_rows(raw, sep="\t"):
    rows = []
    for line in raw.splitlines():
        if line.strip():
            rows.append(line.split(sep))
    return rows


# ---------------------------------------------------------------------------
# Data fetching
# ---------------------------------------------------------------------------

def fetch_bot_names():
    raw = db_query("SELECT id, bot_name FROM idlebot_bots")
    return {int(r[0]): r[1] for r in parse_rows(raw) if len(r) >= 2}


def fetch_death_summary(bot_names):
    """Return {bot_name: {total, recent, last_death, ...}}."""
    sql = (
        "SELECT b.bot_name, "
        "COUNT(*) AS total_deaths, "
        "SUM(e.event_type = 'DEATH') AS death_events, "
        "SUM(e.event_type = 'FAILURE') AS failure_events, "
        "MAX(e.created_at) AS last_event "
        "FROM idlebot_events e "
        "JOIN idlebot_bots b ON b.id = e.bot_id "
        "WHERE e.event_type IN ('DEATH','FAILURE') "
        "GROUP BY b.bot_name ORDER BY total_deaths DESC"
    )
    rows = parse_rows(db_query(sql))
    result = {}
    for r in rows:
        if len(r) < 5:
            continue
        result[r[0]] = {
            "total":    int(r[1] or 0),
            "deaths":   int(r[2] or 0),
            "failures": int(r[3] or 0),
            "last":     r[4],
        }
    return result


def fetch_recent_deaths(hours):
    sql = (
        f"SELECT b.bot_name, e.event_type, e.detail, e.created_at "
        f"FROM idlebot_events e "
        f"JOIN idlebot_bots b ON b.id = e.bot_id "
        f"WHERE e.event_type IN ('DEATH','FAILURE') "
        f"AND e.created_at >= NOW() - INTERVAL {hours} HOUR "
        f"ORDER BY e.created_at DESC LIMIT 200"
    )
    return parse_rows(db_query(sql))


def fetch_circuit_breaker_events():
    sql = (
        "SELECT b.bot_name, e.detail, e.created_at "
        "FROM idlebot_events e "
        "JOIN idlebot_bots b ON b.id = e.bot_id "
        "WHERE e.event_type = 'FAILURE' "
        "AND e.detail LIKE '%rate circuit breaker%' "
        "ORDER BY e.created_at DESC LIMIT 50"
    )
    return parse_rows(db_query(sql))


def fetch_bot_current_state():
    sql = (
        "SELECT b.bot_name, b.guide_id, b.step_index, "
        "b.death_count_total, b.death_count_current_step, b.active, b.paused "
        "FROM idlebot_bots b ORDER BY b.bot_name"
    )
    rows = parse_rows(db_query(sql))
    result = {}
    for r in rows:
        if len(r) < 7:
            continue
        result[r[0]] = {
            "guide":       r[1] if r[1] != "NULL" else "",
            "step":        int(r[2] or 0),
            "deaths_total": int(r[3] or 0),
            "deaths_step":  int(r[4] or 0),
            "active":      r[5] == "1",
            "paused":      r[6] == "1",
        }
    return result


def fetch_failure_details():
    """Get FAILURE events with context for death-loop skip analysis."""
    sql = (
        "SELECT b.bot_name, e.detail, e.created_at "
        "FROM idlebot_events e "
        "JOIN idlebot_bots b ON b.id = e.bot_id "
        "WHERE e.event_type = 'FAILURE' "
        "ORDER BY e.created_at DESC LIMIT 100"
    )
    return parse_rows(db_query(sql))


def fetch_death_rate(hours, bot_name=None):
    """Deaths per hour per bot over the last N hours."""
    bot_filter = f"AND b.bot_name = '{bot_name}'" if bot_name else ""
    sql = (
        f"SELECT b.bot_name, "
        f"DATE_FORMAT(e.created_at, '%Y-%m-%d %H:00:00') AS hour_bucket, "
        f"COUNT(*) AS deaths "
        f"FROM idlebot_events e "
        f"JOIN idlebot_bots b ON b.id = e.bot_id "
        f"WHERE e.event_type = 'DEATH' "
        f"AND e.created_at >= NOW() - INTERVAL {hours} HOUR "
        f"{bot_filter} "
        f"GROUP BY b.bot_name, hour_bucket "
        f"ORDER BY b.bot_name, hour_bucket"
    )
    return parse_rows(db_query(sql))


# ---------------------------------------------------------------------------
# Analysis helpers
# ---------------------------------------------------------------------------

def parse_step_from_detail(detail):
    """Extract step number from death/failure detail strings."""
    m = re.search(r'step\s+(\d+)', detail or "", re.IGNORECASE)
    return int(m.group(1)) if m else None


def parse_death_count_from_detail(detail):
    """Extract '#N total' from death detail strings."""
    m = re.search(r'#(\d+)\s+total', detail or "")
    return int(m.group(1)) if m else None


# ---------------------------------------------------------------------------
# Report generation
# ---------------------------------------------------------------------------

def write_md(issues, bot_state, summary, recent, failures, circuit, hourly, hours, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    now_str = datetime.utcnow().strftime("%Y-%m-%d %H:%M UTC")

    paused_bots = [n for n, s in bot_state.items() if s.get("paused")]
    total_deaths = sum(v["deaths"] for v in summary.values())
    total_failures = sum(v["failures"] for v in summary.values())

    with open(path, "w") as f:
        f.write(f"# Death Audit Report\n\n")
        f.write(f"**Generated:** {now_str}  \n")
        f.write(f"**Total deaths:** {total_deaths}  \n")
        f.write(f"**Total quest-skips:** {total_failures}  \n")
        f.write(f"**Bots paused by circuit breaker:** {len(paused_bots)}  \n\n")

        if paused_bots:
            f.write(f"## ⚠ Paused bots (require manual review)\n\n")
            for name in paused_bots:
                st = bot_state[name]
                f.write(f"- **{name}** — guide `{st['guide']}` step {st['step']}, "
                        f"{st['deaths_total']} total deaths\n")
            f.write("\n")

        f.write("## Deaths per bot\n\n")
        f.write("| Bot | Deaths | Skips | Deaths this step | Guide | Step | Paused |\n")
        f.write("|---|---|---|---|---|---|---|\n")
        for name in sorted(bot_state.keys()):
            st = bot_state[name]
            sm = summary.get(name, {})
            paused_str = "⚠ YES" if st["paused"] else "—"
            f.write(f"| {name} | {sm.get('deaths',0)} | {sm.get('failures',0)} | "
                    f"{st['deaths_step']} | {st['guide'] or '—'} | {st['step']} | {paused_str} |\n")
        f.write("\n")

        if circuit:
            f.write("## Circuit breaker activations\n\n")
            for row in circuit:
                bot, detail, ts = row[0], row[1] if len(row) > 1 else "", row[2] if len(row) > 2 else ""
                f.write(f"- **{ts}** [{bot}]: {detail}\n")
            f.write("\n")

        if failures:
            f.write(f"## Recent quest-skips (FAILURE events)\n\n")
            f.write("| Bot | Detail | When |\n|---|---|---|\n")
            for row in failures[:30]:
                bot, detail, ts = row[0], row[1] if len(row) > 1 else "", row[2] if len(row) > 2 else ""
                f.write(f"| {bot} | {detail.replace('|','\\|')[:80]} | {ts} |\n")
            f.write("\n")

        if recent:
            f.write(f"## Recent deaths (last {hours}h)\n\n")
            f.write("| Bot | Type | Detail | When |\n|---|---|---|---|\n")
            for row in recent[:50]:
                bot = row[0]; etype = row[1] if len(row)>1 else ""; detail = row[2] if len(row)>2 else ""; ts = row[3] if len(row)>3 else ""
                f.write(f"| {bot} | {etype} | {detail.replace('|','\\|')[:70]} | {ts} |\n")
            f.write("\n")

        if hourly:
            f.write("## Hourly death rate\n\n")
            f.write("| Bot | Hour | Deaths |\n|---|---|---|\n")
            for row in hourly:
                bot, hour, deaths = row[0], row[1] if len(row)>1 else "", row[2] if len(row)>2 else "0"
                f.write(f"| {bot} | {hour} | {deaths} |\n")
            f.write("\n")


def write_json(data, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f, indent=2, default=str)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hours", type=int, default=24,
                        help="Look-back window for recent deaths (default 24)")
    parser.add_argument("--md-out",
                        default=os.path.join(REPORTS_DIR, "death_audit_latest.md"))
    parser.add_argument("--json-out",
                        default=os.path.join(REPORTS_DIR, "death_audit_latest.json"))
    args = parser.parse_args()

    print("Fetching death data from DB ...")
    bot_state = fetch_bot_current_state()
    summary   = fetch_death_summary({})
    recent    = fetch_recent_deaths(args.hours)
    failures  = fetch_failure_details()
    circuit   = fetch_circuit_breaker_events()
    hourly    = fetch_death_rate(args.hours)

    paused = [n for n, s in bot_state.items() if s.get("paused")]

    print(f"\nBots: {len(bot_state)}")
    print(f"Total deaths: {sum(v['deaths'] for v in summary.values())}")
    print(f"Total quest-skips: {sum(v['failures'] for v in summary.values())}")
    if paused:
        print(f"\n⚠  PAUSED BOTS (circuit breaker): {', '.join(paused)}")
        print("   Use '.idlebot resume <name>' to unpause after investigating.")

    print("\nDeaths per bot (last all-time):")
    for name in sorted(bot_state.keys()):
        st = bot_state[name]
        sm = summary.get(name, {})
        flag = " ⚠ PAUSED" if st["paused"] else ""
        guide = st.get("guide") or "—"
        print(f"  {name:20s} deaths={sm.get('deaths',0):4d} skips={sm.get('failures',0):3d} "
              f"step={st['step']:4d} guide={guide}{flag}")

    if circuit:
        print(f"\n⚡ Circuit breaker activations: {len(circuit)}")
        for row in circuit[:5]:
            print(f"  [{row[2] if len(row)>2 else '?'}] {row[0]}: {row[1][:80] if len(row)>1 else ''}")

    data = {
        "bot_state": bot_state,
        "summary":   summary,
        "circuit_breaker": [{"bot": r[0], "detail": r[1] if len(r)>1 else "", "ts": r[2] if len(r)>2 else ""} for r in circuit],
        "recent_deaths_count": len(recent),
        "failure_count": len(failures),
    }
    write_json(data, args.json_out)
    write_md(None, bot_state, summary, recent, failures, circuit, hourly, args.hours, args.md_out)

    print(f"\nReports written:")
    print(f"  {args.md_out}")
    print(f"  {args.json_out}")


if __name__ == "__main__":
    main()
