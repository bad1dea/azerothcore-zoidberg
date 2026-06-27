#!/usr/bin/env bash
# run_idlebot_checkpoint_soak.sh — resilient checkpoint leveling soak.
#
# Survives: DB hiccups, worldserver restarts, temporary container unavailability.
# Does NOT survive: process kill or SIGKILL.
# Writes final summary + marks FAILED_EARLY even when killed via EXIT trap.
#
# Usage (direct, interactive):
#   bash tools/run_idlebot_checkpoint_soak.sh --duration 4h --checkpoint 30m
#
# Usage (detached, recommended):
#   bash tools/start_checkpoint_soak_detached.sh --duration 4h --checkpoint 30m
#
# Options:
#   --duration  <Nh|Nm>   total soak duration (default: 4h)
#   --checkpoint <Nh|Nm>  interval between checkpoint reports (default: 30m)
#   --no-gate             run non-interactively (required when detached)
#   --output <dir>        override output directory
#   --host <ip>           SSH host for remote docker (default: local docker)

set -uo pipefail   # no -e: we handle errors explicitly so the soak doesn't die on DB hiccup

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"

# ---- Defaults ----
DURATION_SEC=$(( 4 * 3600 ))
CHECKPOINT_SEC=$(( 30 * 60 ))
NO_GATE=0
OUT_DIR=""
HOST="${IDLEBOT_HOST:-}"

is_local_host() {
    case "$1" in
        ""|localhost|127.0.0.1) return 0 ;;
    esac

    local short full
    short="$(hostname -s 2>/dev/null || true)"
    full="$(hostname -f 2>/dev/null || true)"
    [ "$1" = "$short" ] || [ "$1" = "$full" ]
}

parse_time() {
    local arg="$1"
    if [[ "$arg" =~ ^([0-9]+)h$ ]]; then echo $(( ${BASH_REMATCH[1]} * 3600 ))
    elif [[ "$arg" =~ ^([0-9]+)m$ ]]; then echo $(( ${BASH_REMATCH[1]} * 60 ))
    elif [[ "$arg" =~ ^([0-9]+)s?$ ]]; then echo "${BASH_REMATCH[1]}"
    else echo ""; fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration)   DURATION_SEC=$(parse_time "$2");   shift 2 ;;
        --checkpoint) CHECKPOINT_SEC=$(parse_time "$2"); shift 2 ;;
        --no-gate)    NO_GATE=1; shift ;;
        --output)     OUT_DIR="$2"; shift 2 ;;
        --host)       HOST="$2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [ -z "$DURATION_SEC" ] || [ -z "$CHECKPOINT_SEC" ]; then
    echo "ERROR: invalid time format. Use 4h, 30m, 3600s." >&2; exit 1
fi

# ---- Output directory ----
if [ -z "$OUT_DIR" ]; then
    TS=$(date +%Y%m%d_%H%M%S)
    STAMPED_DIR="$MODULE_DIR/logs/idlebot-soak/$TS"
    OUT_DIR="$STAMPED_DIR"
    mkdir -p "$OUT_DIR"
    ln -sfn "$OUT_DIR" "$MODULE_DIR/logs/idlebot-soak/latest" 2>/dev/null || true
fi
mkdir -p "$OUT_DIR"

PID_FILE="$OUT_DIR/soak.pid"
HEARTBEAT="$OUT_DIR/heartbeat.txt"
SUMMARY_MD="$OUT_DIR/summary.md"
FAILURES_JSON="$OUT_DIR/failures.json"
QUARANTINE_JSON="$OUT_DIR/quarantine.json"
INVALID_SKIPS_JSON="$OUT_DIR/invalid-skips.json"

echo "$$" > "$PID_FILE"

# ---- Status tracking ----
SOAK_STATUS="RUNNING"
EXIT_REASON=""
START_TIME=$(date +%s)   # set early so write_final_summary works even if gates fail
START_TS=$(date '+%Y-%m-%d %H:%M:%S')
CHECKPOINT_NUM=0

# ---- Cleanup/summary on any exit ----
finish() {
    local ec=$?
    if [ "$SOAK_STATUS" = "RUNNING" ]; then
        SOAK_STATUS="FAILED_EARLY"
        EXIT_REASON="process exited unexpectedly (exit code $ec)"
    fi
    write_final_summary
    if [ "$SOAK_STATUS" != "COMPLETED" ]; then
        exit 1
    fi
    exit 0
}
trap finish EXIT

write_final_summary() {
    local elapsed=$(( $(date +%s) - START_TIME ))
    local elapsed_min=$(( elapsed / 60 ))
    {
        echo "# IdleBot Checkpoint Soak — Final Summary"
        echo ""
        echo "**Status:** $SOAK_STATUS"
        [ -n "$EXIT_REASON" ] && echo "**Reason:** $EXIT_REASON"
        echo "**Started:** $START_TS"
        echo "**Ended:** $(date '+%Y-%m-%d %H:%M:%S')"
        echo "**Ran:** ${elapsed_min}m / $(( DURATION_SEC / 60 ))m"
        echo "**Checkpoints completed:** $CHECKPOINT_NUM"
        echo ""
        if [ "$SOAK_STATUS" != "COMPLETED" ]; then
            echo "> **FAILED_EARLY** — This soak did not complete its full duration."
            echo "> Data collected up to ${elapsed_min}m is still valid."
            echo ""
        fi
        echo "See checkpoint-*.md files for per-checkpoint roster."
    } > "$SUMMARY_MD"
    date '+%Y-%m-%d %H:%M:%S status='"$SOAK_STATUS" > "$HEARTBEAT"
}

# ---- DB helpers ----
if [ -n "$HOST" ] && ! is_local_host "$HOST"; then
    DB_PW="$(ssh "khuong@$HOST" \
        "docker inspect ac-database --format '{{range .Config.Env}}{{println .}}{{end}}' 2>/dev/null \
         | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2" 2>/dev/null || true)"
    [ -z "$DB_PW" ] && DB_PW="password"
    q() { ssh "khuong@$HOST" \
        "docker exec ac-database mysql -uroot -p'$DB_PW' -N '$1' -e \"$2\"" 2>/dev/null || true; }
    container_running() {
        ssh "khuong@$HOST" "docker inspect -f '{{.State.Running}}' ac-worldserver 2>/dev/null" 2>/dev/null | grep -q true
    }
else
    DB_PW="$(docker inspect ac-database \
        --format '{{range .Config.Env}}{{println .}}{{end}}' 2>/dev/null \
        | grep '^MYSQL_ROOT_PASSWORD=' | cut -d= -f2 || true)"
    [ -z "$DB_PW" ] && DB_PW="password"
    q() { docker exec ac-database mysql -uroot -p"$DB_PW" -N "$1" -e "$2" 2>/dev/null || true; }
    container_running() {
        docker inspect -f '{{.State.Running}}' ac-worldserver 2>/dev/null | grep -q true
    }
fi

# ---- Pre-soak gates ----
echo ""
echo "=== IdleBot Checkpoint Soak ==="
echo "Duration:    $(( DURATION_SEC / 3600 ))h $(( (DURATION_SEC % 3600) / 60 ))m"
echo "Checkpoint:  every $(( CHECKPOINT_SEC / 60 ))m"
echo "Gate:        $([ "$NO_GATE" = "1" ] && echo "disabled (--no-gate)" || echo "interactive")"
echo "Output:      $OUT_DIR"
echo ""

for gate_script in \
    "$SCRIPT_DIR/verify_playerlike_policy.sh" \
    "$SCRIPT_DIR/verify_idlebot_test_roster_reset.sh" \
    "$SCRIPT_DIR/verify_quest_state_integrity.sh" \
    "$SCRIPT_DIR/verify_no_invalid_skips.sh" \
    "$SCRIPT_DIR/verify_runtime_guides_installed.sh" \
    "$SCRIPT_DIR/verify_guide_prereqs.sh" \
    "$SCRIPT_DIR/verify_guide_objectives.sh" \
    "$SCRIPT_DIR/verify_quest_loot_regression.sh"
do
    if [ -f "$gate_script" ]; then
        GATE_ARGS=()
        [ -n "$HOST" ] && GATE_ARGS+=("--host" "$HOST")
        if ! bash "$gate_script" "${GATE_ARGS[@]}"; then
            SOAK_STATUS="GATE_FAILED"
            EXIT_REASON="pre-soak gate $(basename "$gate_script") failed"
            echo "ERROR: $EXIT_REASON" >&2
            write_final_summary
            trap - EXIT
            exit 1
        fi
    fi
done

echo ""
echo "All pre-soak gates passed. Starting soak..."

START_TIME=$(date +%s)   # reset to actual soak start (not script start)
START_TS=$(date '+%Y-%m-%d %H:%M:%S')
END_TIME=$(( START_TIME + DURATION_SEC ))
NEXT_CHECKPOINT=$(( START_TIME + CHECKPOINT_SEC ))
CHECKPOINT_NUM=0
SAMPLE_INTERVAL=30

CLASSES=(- Warrior Paladin Hunter Rogue Priest DK Shaman Mage Warlock - Druid)
RACES=(- Human Orc Dwarf NElf Undead Tauren Gnome Troll - BElf Draen)

# Track consecutive DB failures so we can report prolonged outage
DB_FAIL_COUNT=0
DB_FAIL_MAX=20   # ~10 min of failures before we log a warning

# ---- Checkpoint function ----
emit_checkpoint() {
    local elapsed_sec="$1"
    local elapsed_min=$(( elapsed_sec / 60 ))
    CHECKPOINT_NUM=$(( CHECKPOINT_NUM + 1 ))
    local label
    printf -v label "%03d" "$elapsed_min"
    local cp_file="$OUT_DIR/checkpoint-${label}m.md"
    local ts_now
    ts_now=$(date '+%Y-%m-%d %H:%M:%S')

    echo ""
    echo "══════════════════════════════════════════════════════════════════"
    echo "  CHECKPOINT ${label}m  ($ts_now)"
    echo "══════════════════════════════════════════════════════════════════"

    # ---- Worldserver alive? ----
    if ! container_running; then
        echo "  WARNING: ac-worldserver container is not running — checkpoint data may be stale"
    fi

    # ---- Roster ----
    echo ""
    echo "ROSTER:"
    printf "  %-14s %3s %-8s %-7s %5s %5s %5s %5s %-8s\n" \
        "Bot" "Lvl" "Class" "Race" "Step" "Death" "QDone" "QSkip" "State"
    echo "  ─────────────────────────────────────────────────────────────────────────"

    ROSTER_ROWS=$(q acore_characters \
        "SELECT c.name, c.level, c.class, c.race, b.step_index,
                b.death_count_total,
                COALESCE(b.step_state,'idle') AS step_state,
                (SELECT COUNT(*) FROM character_queststatus_rewarded r WHERE r.guid=c.guid) AS qdone,
                (SELECT COUNT(*) FROM idlebot_events e
                  WHERE e.bot_id=b.id AND e.event_type='FAILURE') AS qskip
           FROM idlebot_bots b
           JOIN characters c ON c.name=b.bot_name
          ORDER BY c.name;")

    QUARANTINED_BOTS=()

    if [ -n "$ROSTER_ROWS" ]; then
        DB_FAIL_COUNT=0
        while IFS=$'\t' read -r name level cls race step deaths state qdone qskip; do
            classname="${CLASSES[${cls:-0}]:-C$cls}"
            racename="${RACES[${race:-0}]:-R$race}"
            state_flag=""
            if [[ "${state:-}" == "paused" || "${state:-}" == "quarantine" ]]; then
                state_flag=" !"
                QUARANTINED_BOTS+=("$name")
            fi
            printf "  %-14s %3s %-8s %-7s %5s %5s %5s %5s %-8s%s\n" \
                "$name" "${level:-?}" "$classname" "$racename" \
                "${step:-?}" "${deaths:-0}" "${qdone:-0}" "${qskip:-0}" \
                "${state:-idle}" "$state_flag"
        done <<< "$ROSTER_ROWS"
    else
        DB_FAIL_COUNT=$(( DB_FAIL_COUNT + 1 ))
        echo "  (DB unavailable — sample skipped, failure #$DB_FAIL_COUNT)"
        if [ "$DB_FAIL_COUNT" -ge "$DB_FAIL_MAX" ]; then
            echo "  WARNING: DB has been unreachable for ~$(( DB_FAIL_COUNT * SAMPLE_INTERVAL / 60 ))m"
        fi
    fi

    echo ""

    # ---- Quarantine detail ----
    if [ "${#QUARANTINED_BOTS[@]}" -gt 0 ]; then
        echo "QUARANTINED BOTS (paused — others still running):"
        for qbot in "${QUARANTINED_BOTS[@]}"; do
            echo ""
            echo "  ! $qbot"
            RECENT=$(q acore_characters \
                "SELECT detail, created_at
                   FROM idlebot_events e
                   JOIN idlebot_bots b ON b.id=e.bot_id
                  WHERE b.bot_name='$qbot' AND e.event_type='FAILURE'
                  ORDER BY e.created_at DESC LIMIT 3;" || true)
            if [ -n "$RECENT" ]; then
                while IFS=$'\t' read -r detail created_at; do
                    echo "    [$created_at] $detail"
                done <<< "$RECENT"
            else
                echo "    (no FAILURE events found)"
            fi
        done
        echo ""
    fi

    # ---- FAILURE event counts ----
    FAILURE_COUNT=$(q acore_characters \
        "SELECT COUNT(*) FROM idlebot_events WHERE event_type='FAILURE';" || echo "?")
    echo "Total FAILURE events since soak start: ${FAILURE_COUNT}"
    echo ""

    # ---- Write checkpoint file ----
    {
        echo "## Checkpoint ${label}m — $ts_now"
        echo ""
        echo "| Bot | Lvl | Class | Race | Step | Deaths | QDone | QSkip | State |"
        echo "|-----|-----|-------|------|------|--------|-------|-------|-------|"
        if [ -n "$ROSTER_ROWS" ]; then
            while IFS=$'\t' read -r name level cls race step deaths state qdone qskip; do
                classname="${CLASSES[${cls:-0}]:-C$cls}"
                racename="${RACES[${race:-0}]:-R$race}"
                echo "| $name | ${level:-?} | $classname | $racename | ${step:-?} | ${deaths:-0} | ${qdone:-0} | ${qskip:-0} | ${state:-idle} |"
            done <<< "$ROSTER_ROWS"
        fi
        echo ""
        if [ "${#QUARANTINED_BOTS[@]}" -gt 0 ]; then
            echo "**Quarantined:** ${QUARANTINED_BOTS[*]}"
            echo ""
        fi
        echo "_Generated at $ts_now_"
    } > "$cp_file"

    # Update heartbeat
    echo "$(date '+%Y-%m-%d %H:%M:%S') checkpoint-${label}m written. status=RUNNING" > "$HEARTBEAT"

    # ---- Decision gate (interactive only) ----
    if [ "$NO_GATE" = "0" ]; then
        local remaining=$(( END_TIME - $(date +%s) ))
        echo "Next checkpoint in $(( CHECKPOINT_SEC / 60 ))m. $(( remaining / 60 ))m remaining."
        echo "  [Enter] continue   stop = abort   status = re-print"
        read -r -t "$CHECKPOINT_SEC" USER_INPUT 2>/dev/null || USER_INPUT=""
        case "${USER_INPUT:-}" in
            stop|quit|q)
                SOAK_STATUS="STOPPED_BY_USER"
                EXIT_REASON="user typed 'stop' at checkpoint"
                write_final_summary
                trap - EXIT
                echo "Soak stopped by user."
                exit 0
                ;;
        esac
    fi
}

# ---- Monitoring loop ----
echo "Soak started at $START_TS (PID $$)"
echo ""

LAST_CHECKPOINT_ATTEMPT=0

while true; do
    NOW=$(date +%s)
    ELAPSED=$(( NOW - START_TIME ))

    # Update heartbeat every sample interval
    echo "$(date '+%Y-%m-%d %H:%M:%S') elapsed=${ELAPSED}s status=RUNNING" > "$HEARTBEAT"

    # Duration done?
    if [ "$NOW" -ge "$END_TIME" ]; then
        echo ""
        echo "Duration reached. Emitting final checkpoint."
        emit_checkpoint "$ELAPSED"
        break
    fi

    # Time for a checkpoint?
    if [ "$NOW" -ge "$NEXT_CHECKPOINT" ]; then
        emit_checkpoint "$ELAPSED"
        NEXT_CHECKPOINT=$(( NOW + CHECKPOINT_SEC ))
    fi

    # Worldserver restart detection: log if container went down
    if ! container_running; then
        echo "[$(date '+%H:%M:%S')] WARNING: ac-worldserver not running — waiting for it to come back"
        # Don't exit; just wait and retry next cycle
    fi

    sleep "$SAMPLE_INTERVAL"
done

SOAK_STATUS="COMPLETED"
EXIT_REASON=""
write_final_summary

echo ""
echo "=== SOAK COMPLETE ==="
ELAPSED_FINAL=$(( $(date +%s) - START_TIME ))
echo "Ran for $(( ELAPSED_FINAL / 3600 ))h $(( (ELAPSED_FINAL % 3600) / 60 ))m"
echo ""
echo "Reports in: $OUT_DIR"
echo "  summary.md         — final summary"
echo "  checkpoint-*.md    — per-checkpoint rosters"
echo "  soak.out / soak.err — full logs"

trap - EXIT
exit 0
