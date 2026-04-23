#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# Verify the rootfs has enough free space to (a) absorb a typical
# update without running out, and (b) be imaged onto a target SD card
# with the same nominal size (image creation captures used blocks +
# slack; if the rootfs is full, the image won't shrink and won't fit
# on a same-size target card).

setup() { require_pi; }

@test "rootfs has at least 5 GB free" {
    # df -BG / output: "Filesystem 1G-blocks Used Avail Use% Mounted on" then the data row
    run pi_ssh "df -BG / | tail -1 | awk '{print \$4}' | sed 's/G//'"
    [ "$status" -eq 0 ]
    free_gb="$output"
    [ "$free_gb" -ge 5 ] || { echo "rootfs free: ${free_gb}G (want >= 5G)"; false; }
    if [ "$free_gb" -lt 10 ]; then
        echo "WARN: rootfs free is ${free_gb}G; consider cleanup before imaging"
    fi
}

@test "rootfs is not at >90% capacity" {
    # df --output=pcent gives "Use%" with literal %; strip it
    run pi_ssh "df --output=pcent / | tail -1 | tr -d '% '"
    [ "$status" -eq 0 ]
    used_pct="$output"
    [ "$used_pct" -le 90 ] || { echo "rootfs ${used_pct}% full (want <= 90%)"; false; }
}

@test "/boot/firmware partition is not at >90% capacity" {
    # config.txt + kernel etc. live here; if it's full, OTA updates can fail
    run pi_ssh "df --output=pcent /boot/firmware 2>/dev/null | tail -1 | tr -d '% '"
    if [ "$status" -ne 0 ]; then
        skip "/boot/firmware not a separate mountpoint on this Pi"
    fi
    used_pct="$output"
    [ "$used_pct" -le 90 ] || { echo "/boot/firmware ${used_pct}% full"; false; }
}

@test "no large unexpected files in /tmp (>500MB)" {
    # /tmp on a kiosk should be small; large files there suggest a leak
    # (e.g., RetroArch state dump, crash log, etc.). Use sudo so we can
    # see files owned by other users (e.g. root).
    run pi_ssh "sudo find /tmp -maxdepth 2 -type f -size +500M 2>/dev/null"
    [ "$status" -eq 0 ]
    if [ -n "$output" ]; then
        echo "Unexpected large files in /tmp:"
        echo "$output"
        false
    fi
}
