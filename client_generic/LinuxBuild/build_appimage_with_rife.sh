#!/usr/bin/env bash
set -euo pipefail
INFINIDREAM_ENABLE_RIFE=ON "$(dirname "$0")/build_appimage.sh" "$@"
