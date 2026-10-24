#!/usr/bin/env bash
# Void Linux build wrapper: always use Qt6 from XBPS by default.
# First-time prerequisites, as root: ./dependencies.sh --install-full
# Compilation itself MUST run as your regular user.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if ! command -v xbps-query >/dev/null 2>&1; then
    echo 'Aviso: no se detectó XBPS. Este script está preparado para Void Linux.' >&2
fi
export QT_MODE="${QT_MODE:-system}"
export PACKAGE_DEB="${PACKAGE_DEB:-0}"
exec "$ROOT/build.sh" "$@"
