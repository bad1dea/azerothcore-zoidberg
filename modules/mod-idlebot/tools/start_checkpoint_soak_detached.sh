#!/usr/bin/env bash
# start_checkpoint_soak_detached.sh — launch the checkpoint soak detached from the
# terminal so it survives SSH disconnect. Uses systemd-run (preferred on modern
# Linux), then nohup as fallback.
#
# Usage:
#   bash tools/start_checkpoint_soak_detached.sh
#   bash tools/start_checkpoint_soak_detached.sh --duration 4h --checkpoint 30m
#   bash tools/start_checkpoint_soak_detached.sh --host 10.10.30.20
#
# All args are forwarded to run_idlebot_checkpoint_soak.sh.
# The soak always runs with --no-gate (non-interactive) when detached.
#
# After starting:
#   tail -f logs/idlebot-soak/latest/soak.out
#   cat  logs/idlebot-soak/latest/soak.pid
#   cat  logs/idlebot-soak/latest/heartbeat.txt
#   cat  logs/idlebot-soak/latest/checkpoint-030m.md

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
SOAK_SCRIPT="$SCRIPT_DIR/run_idlebot_checkpoint_soak.sh"

if [ ! -f "$SOAK_SCRIPT" ]; then
    echo "ERROR: $SOAK_SCRIPT not found." >&2
    exit 1
fi

# ---- Create output directory ----
TS=$(date +%Y%m%d_%H%M%S)
OUT_BASE="$MODULE_DIR/logs/idlebot-soak"
STAMPED_DIR="$OUT_BASE/$TS"
LATEST_LINK="$OUT_BASE/latest"
mkdir -p "$STAMPED_DIR"
ln -sfn "$STAMPED_DIR" "$LATEST_LINK"

PID_FILE="$STAMPED_DIR/soak.pid"
OUT_LOG="$STAMPED_DIR/soak.out"
ERR_LOG="$STAMPED_DIR/soak.err"

# ---- Check not already running ----
EXISTING_LATEST="$LATEST_LINK/soak.pid"
if [ -f "$EXISTING_LATEST" ]; then
    OLD_PID=$(cat "$EXISTING_LATEST" 2>/dev/null || true)
    if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
        echo "ERROR: soak already running (PID $OLD_PID). Stop it first or wait for it to complete." >&2
        echo "  Log: $(readlink -f "$LATEST_LINK")/soak.out" >&2
        exit 1
    fi
fi

echo "=== IdleBot Checkpoint Soak — Detached Launch ==="
echo "Output dir: $STAMPED_DIR"
echo "Logs:       $OUT_LOG"
echo "PID file:   $PID_FILE"
echo ""

# Forward all args + force --no-gate + pass output dir
SOAK_ARGS=("--no-gate" "--output" "$STAMPED_DIR" "$@")

# ---- Launch detached ----
if systemd-run --user --scope --unit="idlebot-soak-$TS" \
        bash "$SOAK_SCRIPT" "${SOAK_ARGS[@]}" \
        >"$OUT_LOG" 2>"$ERR_LOG" &
then
    SOAK_PID=$!
    LAUNCH_METHOD="systemd-run"
else
    # Fallback: nohup + disown
    nohup bash "$SOAK_SCRIPT" "${SOAK_ARGS[@]}" >"$OUT_LOG" 2>"$ERR_LOG" &
    SOAK_PID=$!
    disown "$SOAK_PID"
    LAUNCH_METHOD="nohup"
fi

echo "$SOAK_PID" > "$PID_FILE"
echo "Launched via $LAUNCH_METHOD (PID $SOAK_PID)"
echo ""

# ---- Verify alive after 10s ----
echo -n "Verifying process alive in 10s..."
for i in $(seq 1 10); do
    if ! kill -0 "$SOAK_PID" 2>/dev/null; then
        echo ""
        echo "ERROR: soak process (PID $SOAK_PID) died within 10s." >&2
        echo "Last lines of stderr:" >&2
        tail -20 "$ERR_LOG" >&2
        exit 1
    fi
    printf "."
    sleep 1
done
echo " alive."
echo ""

# ---- Print next checkpoint time ----
# Parse --checkpoint arg if provided (default 30m)
CP_SEC=1800
for i in "${!SOAK_ARGS[@]}"; do
    if [[ "${SOAK_ARGS[$i]}" == "--checkpoint" ]]; then
        NEXT_IDX=$(( i + 1 ))
        CP_ARG="${SOAK_ARGS[$NEXT_IDX]:-30m}"
        if [[ "$CP_ARG" =~ ^([0-9]+)h$ ]]; then CP_SEC=$(( ${BASH_REMATCH[1]} * 3600 ))
        elif [[ "$CP_ARG" =~ ^([0-9]+)m$ ]]; then CP_SEC=$(( ${BASH_REMATCH[1]} * 60 ))
        fi
    fi
done

NEXT_CP=$(date -d "+${CP_SEC} seconds" '+%H:%M' 2>/dev/null || date -v "+${CP_SEC}S" '+%H:%M' 2>/dev/null || echo "~$(( CP_SEC / 60 ))m from now")

echo "Soak is running detached."
echo ""
echo "  Next checkpoint:  ~$NEXT_CP"
echo "  Live output:      tail -f $OUT_LOG"
echo "  Errors:           tail -f $ERR_LOG"
echo "  Heartbeat:        cat $STAMPED_DIR/heartbeat.txt"
echo "  Stop soak:        kill \$(cat $PID_FILE)"
echo ""
echo "First checkpoint report will appear at:"
echo "  $STAMPED_DIR/checkpoint-030m.md"
