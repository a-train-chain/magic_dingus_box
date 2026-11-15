#!/bin/bash
# Systemd's start/stop path. Rollback trees without the package keep legacy
# operation; a present but failed native package never bypasses its fences.
set -euo pipefail
source_dir="${VPN_RECOVERY_SOURCE_DIR:-$(cd -- "$(dirname -- "$0")" && pwd)}"
base="${MDB_BASE:-$(cd -- "$source_dir/../.." && pwd)}"
services="${COMPOSE_DIR:-$base/services}"
if [[ -f "$source_dir/vpn_recoveryctl.py" && -d "$source_dir/vpn_recovery" ]]; then
    exec python3 "$source_dir/vpn_recoveryctl.py" --base "$base" --services "$services" lifecycle "$1"
fi
cd "$services"
case "$1" in
    start) exec docker compose up -d ;;
    stop) exec docker compose down ;;
    *) exit 2 ;;
esac
