#!/bin/bash
#
# Magic Dingus Box — let the kiosk pair Bluetooth controllers.
#
# Settings -> Pair Wireless Controller (platform/bluez_worker.cpp) talks to
# BlueZ over the system D-Bus as the kiosk's own user (User=magic). This
# makes sure it can:
#   1. the kiosk user is in the `bluetooth` group, which Debian's BlueZ
#      D-Bus policy grants org.bluez access. Takes effect at the kiosk's
#      next start (systemd sets supplementary groups at exec).
#   2. bluetooth.service is enabled and started, unless an operator masked
#      it (then it is left alone and the pairing screen says Bluetooth is
#      unavailable).
#   3. the radio is not rfkill soft-blocked.
#
# Both boards have the same on-board radio (Pi 4B and Pi 5), so there is
# nothing per-board here. A box without BlueZ installed skips everything:
# the pairing screen reports that Bluetooth is unavailable, and wired / 2.4
# GHz-dongle pads are unaffected.
#
# Never STOPS anything and never fails its caller: every step is
# best-effort and the script always exits 0. Idempotent. Called from
# setup_memory_tuning.sh (step 1h) — the root hook every delivery path runs
# (deploy, OTA, first boot, source-box sync).
#
# Test seams:
#   MAGIC_TUNING_ROOT    - non-empty = BATS fake root: touch nothing real
#   MAGIC_SKIP_SYSTEMCTL - "true" skips everything that changes the system

TUNING_ROOT="${MAGIC_TUNING_ROOT:-}"
SKIP_SYSTEMCTL="${MAGIC_SKIP_SYSTEMCTL:-false}"
KIOSK_UNIT="magic-dingus-box-cpp.service"

log() { echo "[bluetooth] $1"; }

if [[ -n "$TUNING_ROOT" || "$SKIP_SYSTEMCTL" == "true" ]]; then
    log "SKIP: test mode"
    exit 0
fi
if [[ "$EUID" -ne 0 ]]; then
    log "WARNING: not root; skipping"
    exit 0
fi

if ! getent group bluetooth >/dev/null 2>&1; then
    log "BlueZ is not installed (no bluetooth group); pairing will report unavailable"
    exit 0
fi

# --- 1. kiosk user in the bluetooth group -----------------------------------
kiosk_user="$(systemctl show -p User --value "$KIOSK_UNIT" 2>/dev/null || true)"
[[ -n "$kiosk_user" ]] || kiosk_user="magic"
if id "$kiosk_user" >/dev/null 2>&1; then
    if id -nG "$kiosk_user" | tr ' ' '\n' | grep -qx bluetooth; then
        log "$kiosk_user is in the bluetooth group"
    elif usermod -aG bluetooth "$kiosk_user"; then
        log "added $kiosk_user to the bluetooth group (applies at the kiosk's next start)"
    else
        log "WARNING: could not add $kiosk_user to the bluetooth group"
    fi
else
    log "WARNING: kiosk user '$kiosk_user' does not exist"
fi

# --- 2. bluetooth.service enabled -------------------------------------------
state="$(systemctl is-enabled bluetooth.service 2>/dev/null || true)"
case "$state" in
    enabled|enabled-runtime|static|indirect|alias)
        log "bluetooth.service is $state"
        ;;
    masked|masked-runtime)
        log "bluetooth.service is masked; leaving it (pairing will report unavailable)"
        ;;
    disabled)
        if systemctl enable --now --no-block bluetooth.service >/dev/null 2>&1; then
            log "enabled bluetooth.service"
        else
            log "WARNING: could not enable bluetooth.service"
        fi
        ;;
    *)
        log "bluetooth.service not found (state '${state:-none}'); skipping"
        ;;
esac

# --- 3. radio not soft-blocked ----------------------------------------------
if command -v rfkill >/dev/null 2>&1; then
    if rfkill list bluetooth 2>/dev/null | grep -q "Soft blocked: yes"; then
        if rfkill unblock bluetooth 2>/dev/null; then
            log "unblocked the Bluetooth radio (rfkill)"
        else
            log "WARNING: could not unblock the Bluetooth radio"
        fi
    fi
fi

exit 0
