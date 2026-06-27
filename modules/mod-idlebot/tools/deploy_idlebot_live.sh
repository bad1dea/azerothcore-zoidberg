#!/usr/bin/env bash
# Full live idlebot deploy:
#  1) sync code + guides to build host checkout
#  2) sync guides to live host-mounted guide dir
#  3) rebuild + restart worldserver
#  4) force-copy live guides into running container
#  5) reload guides over SOAP
#  6) optionally validate runtime guide install
#
# Usage:
#   bash modules/mod-idlebot/tools/deploy_idlebot_live.sh
#   bash modules/mod-idlebot/tools/deploy_idlebot_live.sh --no-build
#   bash modules/mod-idlebot/tools/deploy_idlebot_live.sh --guide horde/tauren/01_bloodhoof-6-12.yaml

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

HOST="${IDLEBOT_BUILD_HOST:-khuong@10.10.30.20}"
BUILD_HOST_PATH="${IDLEBOT_BUILD_HOST_PATH:-/home/khuong/build/azerothcore-zoidberg}"
LIVE_GUIDE_ROOT="${IDLEBOT_RUNTIME_GUIDE_ROOT:-/home/khuong/acore-zoidberg/data/guides}"
CONTAINER="${IDLEBOT_CONTAINER:-ac-worldserver}"
SOAP_HOST="${IDLEBOT_SOAP_HOST:-10.10.30.20}"
SOAP_PORT="${IDLEBOT_SOAP_PORT:-7878}"
SOAP_USER="${IDLEBOT_SOAP_USER:-${SOAP_USER:-KHUONG}}"
SOAP_PASS="${IDLEBOT_SOAP_PASS:-${SOAP_PASS:-KHUONG1234}}"
SOAP_RETRIES="${IDLEBOT_SOAP_RETRIES:-12}"
COMPOSE_FILE="${IDLEBOT_COMPOSE_FILE:-/home/khuong/homelab/compose/zoidberg/compose.yml}"
ENV_SHARED="${IDLEBOT_ENV_SHARED:-/home/khuong/secrets/shared.env}"
ENV_HOST="${IDLEBOT_ENV_HOST:-/home/khuong/secrets/zoidberg.env}"
PROJECT="${IDLEBOT_COMPOSE_PROJECT:-zoidberg-stack}"

BUILD=1
VERIFY=1
GUIDE_REL=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build) BUILD=0; shift ;;
        --no-verify) VERIFY=0; shift ;;
        --guide) GUIDE_REL="${2:?missing guide path}"; shift 2 ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

if [[ -n "$GUIDE_REL" ]]; then
    GUIDE_SOURCE="$MODULE_DIR/data/guides/$GUIDE_REL"
    if [[ ! -f "$GUIDE_SOURCE" ]]; then
        echo "Guide not found: $GUIDE_SOURCE" >&2
        exit 1
    fi
fi

timestamp="$(date +%Y%m%d_%H%M%S)"
backup_dir="$MODULE_DIR/backups/deploy_live/$timestamp"
mkdir -p "$backup_dir"

echo "[deploy_idlebot_live] backup dir: $backup_dir"

copy_tree_to_remote() {
    local src="$1"
    local dest="$2"
    tar -C "$src" -cf - . | ssh "$HOST" "bash -lc 'mkdir -p \"$dest\" && tar -C \"$dest\" -xf -'"
}

copy_file_to_remote() {
    local src="$1"
    local dest="$2"
    ssh "$HOST" "bash -lc 'mkdir -p \"$(dirname "$dest")\"'"
    cat "$src" | ssh "$HOST" "bash -lc 'cat > \"$dest\"'"
}

soap_cmd() {
    local cmd="$1"
    local try
    for try in $(seq 1 "$SOAP_RETRIES"); do
        if SOAP_HOST="$SOAP_HOST" SOAP_PORT="$SOAP_PORT" SOAP_USER="$SOAP_USER" SOAP_PASS="$SOAP_PASS" \
            python3 "$MODULE_DIR/tools/soap_admin.py" --cmd "$cmd"; then
            return 0
        fi
        sleep 2
    done
    return 1
}

remote_build_worldserver() {
    ssh "$HOST" "bash -lc '
        set -euo pipefail
        cd \"$BUILD_HOST_PATH\"
        DOCKER_BUILDKIT=1 docker build --target worldserver \
          -t acore/ac-wotlk-worldserver:master \
          -f apps/docker/Dockerfile .
        docker compose -p \"$PROJECT\" \
          --env-file \"$ENV_SHARED\" \
          --env-file \"$ENV_HOST\" \
          -f \"$COMPOSE_FILE\" \
          up -d --no-deps --force-recreate ac-worldserver
    '"
}

echo "[deploy_idlebot_live] syncing idlebot module source to build host"
copy_tree_to_remote "$MODULE_DIR/src" "$BUILD_HOST_PATH/modules/mod-idlebot/src"
copy_tree_to_remote "$MODULE_DIR/tools" "$BUILD_HOST_PATH/modules/mod-idlebot/tools"

if [[ -n "$GUIDE_REL" ]]; then
    echo "[deploy_idlebot_live] syncing single guide to build host + live guide root: $GUIDE_REL"
    copy_file_to_remote "$GUIDE_SOURCE" "$BUILD_HOST_PATH/modules/mod-idlebot/data/guides/$GUIDE_REL"
    copy_file_to_remote "$GUIDE_SOURCE" "$LIVE_GUIDE_ROOT/$GUIDE_REL"
else
    echo "[deploy_idlebot_live] syncing full guide tree to build host"
    copy_tree_to_remote "$MODULE_DIR/data/guides" "$BUILD_HOST_PATH/modules/mod-idlebot/data/guides"
    echo "[deploy_idlebot_live] syncing full guide tree to live guide root"
    copy_tree_to_remote "$MODULE_DIR/data/guides" "$LIVE_GUIDE_ROOT"
fi

if [[ "$BUILD" -eq 1 ]]; then
    echo "[deploy_idlebot_live] rebuilding/restarting worldserver"
    remote_build_worldserver
fi

echo "[deploy_idlebot_live] forcing live guides into running container"
if [[ -n "$GUIDE_REL" ]]; then
    copy_file_to_remote "$GUIDE_SOURCE" "/tmp/$(basename "$GUIDE_REL")"
    ssh "$HOST" "docker cp '/tmp/$(basename "$GUIDE_REL")' '$CONTAINER:/tmp/$(basename "$GUIDE_REL")' && docker exec '$CONTAINER' sh -lc 'mkdir -p \"/azerothcore/modules/mod-idlebot/data/guides/$(dirname "$GUIDE_REL")\" && cp \"/tmp/$(basename "$GUIDE_REL")\" \"/azerothcore/modules/mod-idlebot/data/guides/$GUIDE_REL\"'"
else
    tar -C "$MODULE_DIR/data/guides" -cf - . | ssh "$HOST" "cat > /tmp/idlebot-guides.tar && docker cp /tmp/idlebot-guides.tar '$CONTAINER:/tmp/idlebot-guides.tar' && docker exec '$CONTAINER' sh -lc 'rm -rf /tmp/idlebot-guides && mkdir -p /tmp/idlebot-guides && tar -C /tmp/idlebot-guides -xf /tmp/idlebot-guides.tar && cp -a /tmp/idlebot-guides/. /azerothcore/modules/mod-idlebot/data/guides/'"
fi

echo "[deploy_idlebot_live] reloading guides via SOAP"
if [[ -n "$GUIDE_REL" ]]; then
    soap_cmd ".idlebot reload guide $GUIDE_REL"
else
    soap_cmd ".idlebot reload guides"
fi

if [[ "$VERIFY" -eq 1 ]]; then
    echo "[deploy_idlebot_live] verifying runtime guides"
    bash "$SCRIPT_DIR/verify_runtime_guides_installed.sh" --host 10.10.30.20
fi

echo "[deploy_idlebot_live] done"
