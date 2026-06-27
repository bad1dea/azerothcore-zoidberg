#!/usr/bin/env bash
# Legacy entrypoint kept for compatibility.
# Use the full idlebot live deploy flow so code, guides, runtime mounts, container
# files, and SOAP reload all stay in sync.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec bash "$SCRIPT_DIR/deploy_idlebot_live.sh" "$@"
