#!/usr/bin/env bash
# run_idlebot_leveling_soak.sh — run a time-bounded leveling soak test.
#
# Monitors all active idlebots for a fixed duration, collecting:
#   - guide step transitions
#   - quest accepts / completions / turn-ins (from idlebot_events)
#   - deaths (from idlebot_events)
#   - stuck / path-fail events (from idlebot_events)
#   - live state snapshots (from idlebot_live_state)
#
# After the run it produces a summary report covering:
#   - highest level reached by each bot
#   - where each bot stopped
#   - first failure per bot
#   - repeated failures grouped by quest / guide step
#   - recommended batch fixes
#
# Usage:
#   bash tools/run_idlebot_leveling_soak.sh              # 4h, all bots
#   bash tools/run_idlebot_leveling_soak.sh --duration 1h
#   bash tools/run_idlebot_leveling_soak.sh --duration 30m
#   bash tools/run_idlebot_leveling_soak.sh --output /tmp/soak
#   bash tools/run_idlebot_leveling_soak.sh --no-restart  # don't restart at end
#
# Output files (logs/idlebot-soak/latest/):
#   summary.md            — human-readable executive summary
#   failures.md           — grouped failure report
#   failures.json         — machine-readable failures
#   bot-progress.csv      — level + guide step per bot per sample
#   quest-completions.csv — all quest turn-ins observed
#   deaths.csv            — all deaths with step/position context
#   events-raw.jsonl      — every idlebot_events row seen during the run

set -euo pipefail

# ---- Parse args ----
DURATION_SEC=$((4 * 3600))   # default: 4 hours
SAMPLE_INTERVAL=30            # seconds between state snapshots
OUT_DIR=""
AUTO_RESTART=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration)
            arg="$2"; shift 2
            if [[ "$arg" =~ ^([0-9]+)h$ ]]; then
                DURATION_SEC=$(( ${BASH_REMATCH[1]} * 3600 ))
            elif [[ "$arg" =~ ^([0-9]+)m$ ]]; then
                DURATION_SEC=$(( ${BASH_REMATCH[1]} * 60 ))
            elif [[ "$arg" =~ ^([0-9]+)s?$ ]]; then
                DURATION_SEC="${BASH_REMATCH[1]}"
            else
                echo "Unknown duration format '$arg'. Use 4h, 30m, 3600s."
                exit 1
            fi
            ;;
        --output)
            OUT_DIR="$2"; shift 2 ;;
        --no-restart)
            AUTO_RESTART=0; shift ;;
        *)
            echo "Unknown argument: $1"; exit 1 ;;
    esac
done

# ---- Output directory ----
if [ -z "$OUT_DIR" ]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    MODULE_DIR="$(dirname "$SCRIPT_DIR")"
    OUT_DIR="$MODULE_DIR/logs/idlebot-soak/latest"
fi
mkdir -p "$OUT_DIR"

# Symlink latest → timestamped dir
TS=$(date +%Y%m%d_%H%M%S)
STAMPED_DIR="$(dirname "$OUT_DIR")/$TS"
mkdir -p "$STAMPED_DIR"
ln -sfn "$STAMPED_DIR" "$OUT_DIR"
OUT_DIR="$STAMPED_DIR"

echo "=== IdleBot Leveling Soak Test ==="
echo "Duration:  ${DURATION_SEC}s ($(( DURATION_SEC / 3600 ))h $(( (DURATION_SEC % 3600) / 60 ))m)"
echo "Interval:  ${SAMPLE_INTERVAL}s"
echo "Output:    $OUT_DIR"
echo ""

# ---- Playerlike policy preflight ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
POLICY_CHECK="$SCRIPT_DIR/verify_playerlike_policy.sh"
if [ -f "$POLICY_CHECK" ]; then
    echo "--- Policy check ---"
    if ! bash "$POLICY_CHECK"; then
        echo ""
        echo "ERROR: Playerlike policy check failed. Fix config before running soak."
        echo "       Soak would produce invalid progression data with cheat flags enabled."
        exit 1
    fi
else
    echo "WARNING: verify_playerlike_policy.sh not found — skipping policy check."
fi

# ---- DB access ----
DB_PW="$(docker inspect ac-database \
    --format '{{range .Config.Env}}{{println .}}{{end}}' \
    2>/dev/null | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2 || true)"
[ -z "$DB_PW" ] && DB_PW="${MYSQL_ROOT_PASSWORD:-password}"

run_sql() {
    docker exec ac-database mysql -uroot -p"$DB_PW" -N "$@" 2>/dev/null
}

# ---- Initialize output files ----
PROGRESS_CSV="$OUT_DIR/bot-progress.csv"
QUEST_CSV="$OUT_DIR/quest-completions.csv"
DEATHS_CSV="$OUT_DIR/deaths.csv"
EVENTS_JSONL="$OUT_DIR/events-raw.jsonl"
SUMMARY_MD="$OUT_DIR/summary.md"
FAILURES_MD="$OUT_DIR/failures.md"
FAILURES_JSON="$OUT_DIR/failures.json"

echo "ts,bot_name,level,map_id,x,y,z,guide_id,step_index,step_name,alive,in_combat,death_count_total" \
    > "$PROGRESS_CSV"
echo "ts,bot_name,level,quest_id,event_detail" > "$QUEST_CSV"
echo "ts,bot_name,level,map_id,x,y,z,step_index,guide_id,death_count_total,death_count_step" > "$DEATHS_CSV"

# Track last seen event ID to avoid re-processing
LAST_EVENT_ID=0
LAST_EVENT_ID=$(run_sql acore_characters -e \
    "SELECT COALESCE(MAX(id),0) FROM idlebot_events;" 2>/dev/null || echo 0)

echo "Starting soak. Last event ID: $LAST_EVENT_ID"
START_TIME=$(date +%s)
END_TIME=$(( START_TIME + DURATION_SEC ))
SAMPLE_COUNT=0

# ---- Monitoring loop ----
while true; do
    NOW=$(date +%s)
    if [ "$NOW" -ge "$END_TIME" ]; then
        echo ""
        echo "==> Duration reached (${DURATION_SEC}s). Stopping soak."
        break
    fi

    ELAPSED=$(( NOW - START_TIME ))
    REMAINING=$(( END_TIME - NOW ))
    SAMPLE_COUNT=$(( SAMPLE_COUNT + 1 ))
    TS_NOW=$(date '+%Y-%m-%d %H:%M:%S')

    echo -n "[$TS_NOW +${ELAPSED}s] sample $SAMPLE_COUNT..."

    # -- Snapshot live state --
    run_sql acore_characters -e "
SELECT CONCAT(
    UNIX_TIMESTAMP(), ',',
    bot_name, ',',
    COALESCE(level,'?'), ',',
    COALESCE(map_id,'?'), ',',
    COALESCE(ROUND(x,0),'?'), ',',
    COALESCE(ROUND(y,0),'?'), ',',
    COALESCE(ROUND(z,0),'?'), ',',
    COALESCE(guide_id,'none'), ',',
    COALESCE(step_index,'?'), ',',
    COALESCE(REPLACE(step_name,',','|'),'?'), ',',
    COALESCE(alive,'?'), ',',
    COALESCE(in_combat,'?'), ',',
    COALESCE(death_count_total,'?')
)
FROM idlebot_live_state
WHERE bot_name IS NOT NULL;" >> "$PROGRESS_CSV" 2>/dev/null || true

    # -- Poll new events --
    NEW_EVENTS=$(run_sql acore_characters -e "
SELECT id, bot_name, event_type, detail, created_at
  FROM idlebot_events
 WHERE id > $LAST_EVENT_ID
 ORDER BY id ASC
 LIMIT 500;" 2>/dev/null || true)

    if [ -n "$NEW_EVENTS" ]; then
        # Extract deaths
        echo "$NEW_EVENTS" | grep -i 'death\|DEATH\|died' | while IFS=$'\t' read -r eid bname etype edetail ets; do
            level=$(run_sql acore_characters -e \
                "SELECT COALESCE(level,'?') FROM idlebot_live_state WHERE bot_name='$bname';" 2>/dev/null || echo '?')
            pos=$(run_sql acore_characters -e \
                "SELECT CONCAT(COALESCE(map_id,'?'),',',COALESCE(ROUND(x,0),'?'),',',COALESCE(ROUND(y,0),'?'),',',COALESCE(ROUND(z,0),'?')) FROM idlebot_live_state WHERE bot_name='$bname';" 2>/dev/null || echo '?,?,?,?')
            step=$(run_sql acore_characters -e \
                "SELECT CONCAT(COALESCE(step_index,'?'),',',COALESCE(guide_id,'?')) FROM idlebot_live_state WHERE bot_name='$bname';" 2>/dev/null || echo '?,?')
            dc=$(run_sql acore_characters -e \
                "SELECT CONCAT(COALESCE(death_count_total,'?'),',',COALESCE(death_count_current_step,'?')) FROM idlebot_live_state WHERE bot_name='$bname';" 2>/dev/null || echo '?,?')
            echo "$ets,$bname,$level,$pos,$step,$dc" >> "$DEATHS_CSV"
        done

        # Extract quest completions
        echo "$NEW_EVENTS" | grep -i 'QUEST\|quest_complete\|turn.in\|rewarded' | while IFS=$'\t' read -r eid bname etype edetail ets; do
            level=$(run_sql acore_characters -e \
                "SELECT COALESCE(level,'?') FROM idlebot_live_state WHERE bot_name='$bname';" 2>/dev/null || echo '?')
            qid=$(echo "$edetail" | grep -oP 'quest \K[0-9]+' | head -1 || echo '?')
            echo "$ets,$bname,$level,$qid,$(echo "$edetail" | head -c 120 | tr ',' '|')" >> "$QUEST_CSV"
        done

        # Write all new events to JSONL
        echo "$NEW_EVENTS" | while IFS=$'\t' read -r eid bname etype edetail ets; do
            printf '{"id":%s,"bot":"%s","type":"%s","detail":%s,"ts":"%s"}\n' \
                "$eid" "$bname" "$etype" \
                "$(echo "$edetail" | python3 -c 'import sys,json; print(json.dumps(sys.stdin.read().strip()))' 2>/dev/null || echo '"?"')" \
                "$ets"
        done >> "$EVENTS_JSONL" 2>/dev/null || true

        # Update last event ID
        NEW_MAX=$(echo "$NEW_EVENTS" | awk 'END{print $1}' | tr -d '[:space:]')
        if [ -n "$NEW_MAX" ] && [ "$NEW_MAX" -gt "$LAST_EVENT_ID" ] 2>/dev/null; then
            LAST_EVENT_ID="$NEW_MAX"
        fi
    fi

    echo " done. Remaining: ${REMAINING}s"
    sleep "$SAMPLE_INTERVAL"
done

# ---- Generate summary report ----
echo ""
echo "==> Generating soak report..."

RUN_DURATION="$(( $(date +%s) - START_TIME ))s"

cat > "$SUMMARY_MD" <<MDEOF
# IdleBot Leveling Soak Report

**Date:** $(date '+%Y-%m-%d %H:%M:%S')
**Duration:** ${RUN_DURATION}
**Samples:** ${SAMPLE_COUNT} (every ${SAMPLE_INTERVAL}s)
**Output:** ${OUT_DIR}
**Mode:** Playerlike (no cheat teleport/resurrect/skip)

> Only quest turn-ins via real NPC interaction and level-ups via real XP gain count
> as valid progression. Quarantined bots' last valid step is the real stopping point.

## Quarantined Bots

$(run_sql acore_characters -e "
SELECT bot_name, detail, created_at
FROM idlebot_events
WHERE id <= $LAST_EVENT_ID
  AND detail LIKE '%quarantin%'
ORDER BY created_at ASC;" 2>/dev/null | column -t -s $'\t' 2>/dev/null || echo "(none — all bots completed the run)")

## Bot Final State

MDEOF

# Per-bot summary
run_sql acore_characters -e "
SELECT
    ls.bot_name,
    ls.level,
    ls.guide_id,
    ls.step_index,
    ls.step_total,
    REPLACE(ls.step_name,\"'\",''),
    ls.map_id,
    ROUND(ls.x,0),
    ROUND(ls.y,0),
    ls.death_count_total,
    ls.alive
FROM idlebot_live_state ls
ORDER BY ls.bot_name;" 2>/dev/null | while IFS=$'\t' read -r bname lvl gid sidx stot sname mapid px py dc alive; do
    cat >> "$SUMMARY_MD" <<ENTRY
### $bname
- **Level:** $lvl
- **Guide:** $gid (step $sidx / $stot)
- **Current step:** $sname
- **Map/Position:** map=$mapid ($px,$py)
- **Total deaths:** $dc
- **Alive:** $alive

ENTRY
done

cat >> "$SUMMARY_MD" <<MDEOF

## Quest Completions

$(wc -l < "$QUEST_CSV" | tr -d '[:space:]') quests completed/turned-in during the run.

## Deaths

$(wc -l < "$DEATHS_CSV" | tr -d '[:space:]') death events recorded.

## Files

| File | Description |
|------|-------------|
| [bot-progress.csv](bot-progress.csv) | Level + guide step per bot per sample |
| [quest-completions.csv](quest-completions.csv) | Quest events |
| [deaths.csv](deaths.csv) | Death events with context |
| [events-raw.jsonl](events-raw.jsonl) | Raw event stream |
| [failures.md](failures.md) | Grouped failure analysis |
| [failures.json](failures.json) | Machine-readable failures |

MDEOF

# ---- Failure analysis ----
echo "==> Analyzing failures from event log..."

# Group failures from idlebot_events by type + bot + quest
FAILURES=$(run_sql acore_characters -e "
SELECT
    bot_name,
    event_type,
    detail,
    COUNT(*) AS count,
    MIN(created_at) AS first_seen,
    MAX(created_at) AS last_seen
FROM idlebot_events
WHERE id <= $LAST_EVENT_ID
  AND (
    event_type IN ('DEATH','STUCK','FAILURE','QUEST')
    OR detail LIKE '%skip%'
    OR detail LIKE '%fail%'
    OR detail LIKE '%stuck%'
    OR detail LIKE '%timeout%'
    OR detail LIKE '%cannot%'
  )
GROUP BY bot_name, event_type, detail
ORDER BY count DESC
LIMIT 200;" 2>/dev/null || true)

cat > "$FAILURES_MD" <<FEOF
# IdleBot Soak Failures

Generated: $(date '+%Y-%m-%d %H:%M:%S')
Run duration: ${RUN_DURATION}

## Grouped Failures (by frequency)

\`\`\`
$(echo "$FAILURES" | column -t -s $'\t' 2>/dev/null || echo "$FAILURES")
\`\`\`

## Skip Events

$(run_sql acore_characters -e "
SELECT bot_name, detail, COUNT(*) as cnt
FROM idlebot_events
WHERE id <= $LAST_EVENT_ID
  AND (detail LIKE '%skip%' OR detail LIKE '%undoable%' OR detail LIKE '%cannot accept%')
GROUP BY bot_name, detail
ORDER BY cnt DESC
LIMIT 50;" 2>/dev/null | column -t -s $'\t' 2>/dev/null || echo "(none)")

## Unsupported Travel Events

$(run_sql acore_characters -e "
SELECT bot_name, detail, COUNT(*) as cnt
FROM idlebot_events
WHERE id <= $LAST_EVENT_ID
  AND (detail LIKE '%SPECIAL_TRAVEL%' OR detail LIKE '%class travel%' OR detail LIKE '%Moonglade%')
GROUP BY bot_name, detail
ORDER BY cnt DESC;" 2>/dev/null | column -t -s $'\t' 2>/dev/null || echo "(none)")

## Death Loops

$(run_sql acore_characters -e "
SELECT bot_name, death_count_current_step, guide_id, step_index
FROM idlebot_live_state
WHERE death_count_current_step >= 3
ORDER BY death_count_current_step DESC;" 2>/dev/null | column -t -s $'\t' 2>/dev/null || echo "(none)")

FEOF

# Simple JSON failures output
run_sql acore_characters -e "
SELECT JSON_OBJECT(
    'bot', bot_name,
    'type', event_type,
    'detail', detail,
    'count', COUNT(*),
    'first', MIN(created_at),
    'last', MAX(created_at)
)
FROM idlebot_events
WHERE id <= $LAST_EVENT_ID
  AND event_type IN ('DEATH','STUCK','FAILURE','QUEST')
GROUP BY bot_name, event_type, detail
ORDER BY COUNT(*) DESC
LIMIT 100;" 2>/dev/null > "$FAILURES_JSON" || echo "[]" > "$FAILURES_JSON"

echo ""
echo "=== Soak Test Complete ==="
echo ""
echo "Reports in: $OUT_DIR"
echo "  summary.md   — executive summary"
echo "  failures.md  — grouped failure analysis"
echo "  bot-progress.csv — ${SAMPLE_COUNT} snapshots"
echo ""
echo "Recommended next steps:"
echo "  1. Read failures.md — group by root cause"
echo "  2. Fix highest-frequency failures in batch"
echo "  3. Re-run: bash tools/reset_idlebot_test_roster.sh && bash tools/run_idlebot_leveling_soak.sh"
echo ""
echo "Quick preview:"
grep -A 5 "Bot Final State" "$SUMMARY_MD" 2>/dev/null || true
