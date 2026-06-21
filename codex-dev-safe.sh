#!/usr/bin/env bash
set -euo pipefail
exec codex --sandbox workspace-write --ask-for-approval on-request "$@"
