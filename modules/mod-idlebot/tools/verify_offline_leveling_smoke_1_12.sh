#!/usr/bin/env bash
# verify_offline_leveling_smoke_1_12.sh
#
# Pre-soak gate: runs offline leveling simulation for the smoke roster (1→12).
# Exits non-zero if any bot fails offline validation.
#
# Usage:
#   ./verify_offline_leveling_smoke_1_12.sh             # run, fail on error
#   ./verify_offline_leveling_smoke_1_12.sh --report    # run and open report
#   ./verify_offline_leveling_smoke_1_12.sh --patches   # also write patches

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"

WRITE_PATCHES=0
OPEN_REPORT=0

for arg in "$@"; do
    case "$arg" in
        --patches) WRITE_PATCHES=1 ;;
        --report)  OPEN_REPORT=1   ;;
    esac
done

CMD=(
    python3 "$SCRIPT_DIR/simulate_leveling_offline.py"
    --profile smoke
    --target-level 12
    --output both
    --fail-on-error
)

if [ "$WRITE_PATCHES" = "1" ]; then
    CMD+=(--write-patches)
fi

echo "=== Offline leveling simulation: smoke roster 1→12 ==="
echo ""

"${CMD[@]}"
STATUS=$?

REPORT_DIR="$MODULE_DIR/reports/offline-sim/smoke-1-12"

if [ "$STATUS" -eq 0 ]; then
    echo ""
    echo "✅ Offline validation PASSED — safe to start live soak."
else
    echo ""
    echo "❌ Offline validation FAILED — do NOT start live soak."
    echo "   Fix the issues above, then re-run this gate before soak."
fi

if [ "$OPEN_REPORT" = "1" ] && [ -f "$REPORT_DIR/summary.md" ]; then
    if command -v xdg-open &>/dev/null; then
        xdg-open "$REPORT_DIR/summary.md"
    elif command -v open &>/dev/null; then
        open "$REPORT_DIR/summary.md"
    fi
fi

exit "$STATUS"
