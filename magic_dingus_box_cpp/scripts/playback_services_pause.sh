#!/bin/bash
# Pause/resume the Media Browser background services during movie playback
# so the kiosk's video pipeline doesn't compete with Radarr metadata
# fetches, Prowlarr indexer syncs, or Byparr Cloudflare-challenge solving.
#
# Frees ~300 MB RAM and ~6% CPU for the duration of the movie. Resumed on
# leave() so the operator's normal browsing experience is unaffected.
#
# Idempotent and safe to call from any state — if a container is already
# paused (or doesn't exist on this Pi), the corresponding `docker pause`
# call is a quiet no-op.
#
# Called by PlaybackScreen::enter() / leave() via std::system(). The
# kiosk runs as `magic`; for this script to work without sudo, `magic`
# must be in the `docker` group (handled by setup_services.sh).
#
# qBit + Gluetun are intentionally NOT touched here:
#   - qBit: paused via its own HTTP API (qbit_->pause_all()) which is
#     much faster + preserves internal state better than docker pause.
#   - Gluetun: pausing it would tear down the netns the other 4
#     containers share, breaking them entirely. Stays running.
#
# Usage:
#   playback_services_pause.sh pause     # before movie
#   playback_services_pause.sh unpause   # after movie

set -u  # NOTE: no -e: a `docker pause` failure on one container should
        # NOT stop us from trying the others.

if [ "${1:-}" != "pause" ] && [ "${1:-}" != "unpause" ]; then
    echo "Usage: $0 {pause|unpause}" >&2
    exit 1
fi

ACTION="${1}"
CONTAINERS=(mdb_radarr mdb_prowlarr mdb_byparr)

# When pausing, prefer best-effort + silent. When unpausing, surface
# errors more loudly because a stuck-paused container will look broken
# in the operator's web admin until they manually unpause.
for c in "${CONTAINERS[@]}"; do
    # Skip if the container doesn't exist on this Pi (unprovisioned setup).
    if ! docker inspect "$c" >/dev/null 2>&1; then
        continue
    fi
    if [ "$ACTION" = "pause" ]; then
        docker pause "$c" >/dev/null 2>&1 || true
    else
        if ! docker unpause "$c" >/dev/null 2>&1; then
            echo "[playback-services] WARN: docker unpause $c failed" >&2
        fi
    fi
done

# Tiny success log so journalctl shows when each transition fired.
echo "[playback-services] ${ACTION}d background services"
