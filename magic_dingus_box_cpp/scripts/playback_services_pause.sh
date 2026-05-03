#!/bin/bash
# Stop/start the Media Browser background services during movie playback so
# the kiosk's video pipeline doesn't compete with Radarr metadata fetches,
# Prowlarr indexer syncs, or Byparr Cloudflare-challenge solving.
#
# Frees ~320 MB RAM (Radarr ~155 MB + Prowlarr ~165 MB on the Pi 4B) and
# ~6 % CPU for the duration of the movie. Re-started on leave() so the
# operator's normal browsing experience is unaffected after ~10-15 s
# cold-start.
#
# History note: this script originally used `docker pause`/`docker unpause`
# (kept the containers resident, just suspended scheduling). On the
# memory-constrained Pi 4B 2 GB variant that wasn't enough — the kiosk
# pages still got swapped out to make room for v4l2h264dec DMA buffers,
# producing the "frozen frame, then 5-second catch-up burst" symptom. We
# switched to `docker stop`/`docker start` so the RSS is actually freed
# during playback. The 10-15 s cold-start on the next Movies-tab visit
# is acceptable; the alternative is unwatchable 1080p H.264 playback.
#
# Idempotent and safe to call from any state — if a container is already
# stopped (or doesn't exist on this Pi), the corresponding `docker stop`
# call is a quiet no-op.
#
# Called by PlaybackScreen::enter() / leave() via std::system(). The
# kiosk runs as `magic`; for this script to work without sudo, `magic`
# must be in the `docker` group (handled by setup_services.sh).
#
# qBit + Gluetun are intentionally NOT touched here:
#   - qBit: paused via its own HTTP API (qbit_->pause_all()) which is
#     much faster + preserves internal state better than docker pause.
#   - Gluetun: stopping it would tear down the netns the other 4
#     containers share, breaking them entirely. Stays running.
#
# Usage (the action verbs are kept as pause/unpause so the C++ caller
# doesn't need to change; internally we now use docker stop/start):
#   playback_services_pause.sh pause     # before movie  → docker stop
#   playback_services_pause.sh unpause   # after movie   → docker start

set -u  # NOTE: no -e: a `docker stop` failure on one container should
        # NOT stop us from trying the others.

if [ "${1:-}" != "pause" ] && [ "${1:-}" != "unpause" ]; then
    echo "Usage: $0 {pause|unpause}" >&2
    exit 1
fi

ACTION="${1}"
CONTAINERS=(mdb_radarr mdb_prowlarr mdb_byparr)

# `docker stop` waits up to STOP_TIMEOUT_S for SIGTERM-clean shutdown,
# then SIGKILLs. 5 s is enough for Radarr/Prowlarr/Byparr to flush their
# SQLite state cleanly; longer doesn't help and just delays playback start.
STOP_TIMEOUT_S=5

for c in "${CONTAINERS[@]}"; do
    # Skip if the container doesn't exist on this Pi (unprovisioned setup).
    if ! docker inspect "$c" >/dev/null 2>&1; then
        continue
    fi
    if [ "$ACTION" = "pause" ]; then
        # `docker stop` is a no-op on already-stopped containers (exit 0
        # with a warning to stderr). `|| true` swallows the warning so
        # journalctl stays clean for the idempotent re-entry case.
        docker stop -t "$STOP_TIMEOUT_S" "$c" >/dev/null 2>&1 || true
    else
        if ! docker start "$c" >/dev/null 2>&1; then
            echo "[playback-services] WARN: docker start $c failed" >&2
        fi
    fi
done

# Tiny success log so journalctl shows when each transition fired.
# The action verb stays "pause"/"unpause" externally; internally we now
# stop/start. The log line keeps the legacy verb so existing log-grep
# tooling continues to work.
echo "[playback-services] ${ACTION}d background services"
