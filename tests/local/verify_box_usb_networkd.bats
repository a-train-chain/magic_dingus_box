#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# verify_box.sh WARNs when a systemd-networkd .network matches usb0: that is
# a second DHCP server beside dnsmasq, handing cabled laptops a lease with no
# DNS so http://dingus.box never resolves (the 10-usb-gadget.network the old
# `deploy_cpp.sh --usb-gadget` wrote; found live on a Pi 4B, 2026-10-27).
# verify_box.sh runs top-to-bottom against live hardware, so the pure
# function is extracted from it rather than sourced.

setup() {
    local def
    def=$(sed -n '/^usb0_networkd_files() {/,/^}/p' "$CPP_DIR/scripts/verify_box.sh")
    [ -n "$def" ] || { echo "usb0_networkd_files not found in verify_box.sh"; return 1; }
    eval "$def"
    NET="$BATS_TEST_TMPDIR/network"
    mkdir -p "$NET"
}

@test "the legacy deploy file is named" {
    printf '[Match]\nName=usb0\n\n[Network]\nAddress=192.168.7.1/24\nDHCPServer=yes\n' > "$NET/10-usb-gadget.network"
    [ "$(usb0_networkd_files "$NET")" = "$NET/10-usb-gadget.network" ]
}

@test "any other file name that matches usb0 is named too" {
    printf '[Match]\nName=eth0 usb0\n' > "$NET/20-mine.network"
    [ "$(usb0_networkd_files "$NET")" = "$NET/20-mine.network" ]
}

@test "indented Name= and trailing spaces still match" {
    printf '[Match]\n  Name=usb0  \n' > "$NET/30-x.network"
    [ "$(usb0_networkd_files "$NET")" = "$NET/30-x.network" ]
}

@test "other interfaces and look-alike names are not named" {
    printf '[Match]\nName=eth0\n' > "$NET/10-eth.network"
    printf '[Match]\nName=usb01\n' > "$NET/11-usb01.network"
    printf '[Match]\n#Name=usb0\nName=wlan0\n' > "$NET/12-commented.network"
    [ -z "$(usb0_networkd_files "$NET")" ]
}

@test "a retired backup (not *.network) is not named" {
    printf '[Match]\nName=usb0\n' > "$NET/10-usb-gadget.network.bak"
    [ -z "$(usb0_networkd_files "$NET")" ]
}

@test "an empty or missing directory -> nothing, exit 0" {
    run usb0_networkd_files "$NET"
    [ "$status" -eq 0 ] && [ -z "$output" ]
    run usb0_networkd_files "$BATS_TEST_TMPDIR/absent"
    [ "$status" -eq 0 ] && [ -z "$output" ]
}

@test "verify_box WARNs (not FAILs) on a competing usb0 DHCP server" {
    run grep -nE '^\s*warn "\$\{_f\} configures usb0' "$CPP_DIR/scripts/verify_box.sh"
    [ "$status" -eq 0 ]
    run grep -nE '^\s*fail "[^"]*configures usb0' "$CPP_DIR/scripts/verify_box.sh"
    [ "$status" -ne 0 ]
}
