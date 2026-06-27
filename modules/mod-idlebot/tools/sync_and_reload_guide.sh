#!/usr/bin/env bash
# sync_and_reload_guide.sh — sync a source guide to the runtime container path
# and hot-reload it via the worldserver admin command.
#
# Usage:
#   sync_and_reload_guide.sh <relative_guide_path>
#   e.g. sync_and_reload_guide.sh alliance/nightelf/00_shadowglen-1-6.yaml
#
# Requirements:
#   - SSH access to build host ($IDLEBOT_BUILD_HOST, default khuong@10.10.30.20)
#   - Containers ac-worldserver and ac-database running on build host
#   - .idlebot reload guide command available in worldserver (built with live-reload support)
#   - SOAP enabled on worldserver and credentials available, OR explicit
#     IDLEBOT_ALLOW_ATTACH_FALLBACK=1 if you really want the old attach path.
#
# Source of truth: modules/mod-idlebot/data/guides/<relative_guide_path>
# Runtime path:    host-mounted live guide dir (default /home/khuong/acore-zoidberg/data/guides)
# GuideDirectory:  must match IdleBot.GuideDirectory in mod_idlebot.conf
#
# Does NOT restart the worldserver. chmod +x this file after creation.

set -euo pipefail

GUIDE_REL="${1:-}"
if [[ -z "$GUIDE_REL" ]]; then
    echo "Usage: $0 <relative_guide_path>" >&2
    echo "  e.g. $0 alliance/nightelf/00_shadowglen-1-6.yaml" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE_FILE="$MODULE_DIR/data/guides/$GUIDE_REL"

if [[ ! -f "$SOURCE_FILE" ]]; then
    echo "ERROR: source file not found: $SOURCE_FILE" >&2
    exit 1
fi

HOST="${IDLEBOT_BUILD_HOST:-khuong@10.10.30.20}"
RUNTIME_GUIDE_ROOT="${IDLEBOT_RUNTIME_GUIDE_ROOT:-/home/khuong/acore-zoidberg/data/guides}"
RUNTIME_FILE="$RUNTIME_GUIDE_ROOT/$GUIDE_REL"
CONTAINER="${IDLEBOT_CONTAINER:-ac-worldserver}"
BUILD_HOST_BASE="${IDLEBOT_BUILD_HOST_PATH:-~/build/azerothcore-zoidberg}"
BUILD_HOST_FILE="$BUILD_HOST_BASE/modules/mod-idlebot/data/guides/$GUIDE_REL"
BUILD_HOST_DIR="$(dirname "$BUILD_HOST_FILE")"
SOAP_HOST="${IDLEBOT_SOAP_HOST:-127.0.0.1}"
SOAP_PORT="${IDLEBOT_SOAP_PORT:-7878}"
SOAP_USER="${IDLEBOT_SOAP_USER:-${SOAP_USER:-}}"
SOAP_PASS="${IDLEBOT_SOAP_PASS:-${SOAP_PASS:-}}"
ALLOW_ATTACH_FALLBACK="${IDLEBOT_ALLOW_ATTACH_FALLBACK:-0}"

echo "[sync_and_reload] guide:   $GUIDE_REL"
echo "[sync_and_reload] source:  $SOURCE_FILE"
echo "[sync_and_reload] runtime: $HOST:$RUNTIME_FILE"

# Step 1: sync source file to build host working copy
cat "$SOURCE_FILE" | ssh "$HOST" "mkdir -p $BUILD_HOST_DIR && cat > $BUILD_HOST_FILE"
echo "[sync_and_reload] synced to build host at $BUILD_HOST_FILE"

# Step 2: sync to the live host-mounted guide directory the worldserver actually reads.
ssh "$HOST" "mkdir -p $(dirname "$RUNTIME_FILE") && cat > $RUNTIME_FILE" < "$SOURCE_FILE"
echo "[sync_and_reload] synced to live guide dir"

# Step 3: send .idlebot reload guide command via SOAP by default.
if [[ -n "$SOAP_USER" && -n "$SOAP_PASS" ]]; then
    ssh "$HOST" \
        "cd $BUILD_HOST_BASE && SOAP_HOST='$SOAP_HOST' SOAP_PORT='$SOAP_PORT' SOAP_USER='$SOAP_USER' SOAP_PASS='$SOAP_PASS' \
         python3 modules/mod-idlebot/tools/soap_admin.py --cmd '.idlebot reload guide $GUIDE_REL'"
    echo "[sync_and_reload] SOAP reload command sent"
elif [[ "$ALLOW_ATTACH_FALLBACK" == "1" ]]; then
    echo "[sync_and_reload] WARN: SOAP credentials not set; using unsafe docker attach fallback" >&2
    ssh -tt "$HOST" "timeout 8 docker attach $CONTAINER" <<< ".idlebot reload guide $GUIDE_REL" 2>/dev/null || true
    echo "[sync_and_reload] attach fallback command sent"
else
    echo "[sync_and_reload] ERROR: reload command not sent." >&2
    echo "[sync_and_reload] Set IDLEBOT_SOAP_USER and IDLEBOT_SOAP_PASS (preferred)," >&2
    echo "[sync_and_reload] or explicitly opt into the old fallback with IDLEBOT_ALLOW_ATTACH_FALLBACK=1." >&2
    exit 1
fi
