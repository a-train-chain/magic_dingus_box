#!/bin/bash
# Kiosk ExecStartPre: make sure PulseAudio is up and the default sink
# matches settings.json BEFORE the kiosk starts.
#
# PulseAudio is NOT started here any more. It runs as its own unit,
# magic-dingus-audio.service (see audio_service.sh for why: started from
# here, it daemonized into the kiosk's cgroup and was SIGKILLed on every
# kiosk stop, restart and crash). The kiosk unit is ordered After= that
# unit, so by now PulseAudio is normally already answering and this is a
# quick sink refresh.
#
# Fallback: a box whose audio unit file is NOT installed (an OTA from an
# update.sh too old to run setup_memory_tuning.sh, or a hand-built box)
# still gets sound — audio_service.sh legacy-start does the old
# kill-and-start dance inside this unit. The next install path that runs
# setup_memory_tuning.sh (OTA, deploy_cpp.sh, first_boot.sh,
# sync_source_box.sh) installs the unit and this branch goes dormant.
#
# Must always exit 0: a failing ExecStartPre would keep the kiosk (and
# the picture) from starting over a sound problem.
#
# Test seam: MAGIC_AUDIO_UNIT_FILE (default: the installed unit path).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AUDIO_SERVICE="${SCRIPT_DIR}/audio_service.sh"
AUDIO_UNIT="magic-dingus-audio.service"
AUDIO_UNIT_FILE="${MAGIC_AUDIO_UNIT_FILE:-/etc/systemd/system/${AUDIO_UNIT}}"

# `-f` is false for a masked unit (a symlink to /dev/null), so masking the
# audio unit deliberately falls back to the legacy path.
if [ -f "$AUDIO_UNIT_FILE" ]; then
    if ! bash "$AUDIO_SERVICE" set-sink; then
        # Not answering: the unit may have been installed after the kiosk's
        # start job was queued (no After= ordering yet) or PulseAudio is
        # between restarts. Ask systemd for it — never start a PulseAudio
        # here, that would put it back in the kiosk's cgroup.
        echo "[audio] asking systemd to start ${AUDIO_UNIT}"
        sudo -n systemctl start --no-block "$AUDIO_UNIT" 2>/dev/null || true
        bash "$AUDIO_SERVICE" set-sink \
            || echo "[audio] Warning: PulseAudio still not answering; starting the kiosk without a confirmed sink"
    fi
    exit 0
fi

bash "$AUDIO_SERVICE" legacy-start
exit 0
