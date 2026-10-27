#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# setup_usb_gadget_dns.sh — the USB-C cable's DHCP + catch-all DNS
# (http://dingus.box). Until 2026-10, `deploy_cpp.sh --usb-gadget` wrote a
# systemd-networkd 10-usb-gadget.network (192.168.7.1, DHCPServer=yes,
# EmitDNS=no): a second DHCP server on usb0 handing laptops a lease with no
# DNS, so dingus.box never resolved (found live on a Pi 4B, 2026-10-27).
# These tests run the script against a fake root (MAGIC_USB_ROOT) in test
# mode (MAGIC_SKIP_SYSTEMCTL=true), so nothing touches the real /etc.

SCRIPT="$CPP_DIR/scripts/setup_usb_gadget_dns.sh"
DEPLOY="$CPP_DIR/scripts/deploy_cpp.sh"
INSTALL_DEPS="$CPP_DIR/scripts/install_deps.sh"

# Exactly what the pre-2026-10 deploy wrote.
LEGACY_NETWORK=$'[Match]\nName=usb0\n\n[Network]\nAddress=192.168.7.1/24\nDHCPServer=yes\n\n[DHCPServer]\nPoolOffset=100\nPoolSize=20\nEmitDNS=no\n'

setup() {
    export MAGIC_USB_ROOT="$BATS_TEST_TMPDIR/root"
    export MAGIC_SKIP_SYSTEMCTL=true
    NET="$MAGIC_USB_ROOT/etc/systemd/network"
    RETIRED="$MAGIC_USB_ROOT/var/lib/magic-dingus/retired"
    mkdir -p "$NET"
}

plant_legacy() { printf '%s' "$LEGACY_NETWORK" > "$NET/10-usb-gadget.network"; }

@test "--retire-legacy moves the legacy networkd file aside, content intact" {
    plant_legacy
    run bash "$SCRIPT" --retire-legacy
    [ "$status" -eq 0 ]
    [ ! -e "$NET/10-usb-gadget.network" ]
    backups=("$RETIRED"/10-usb-gadget.network.*)
    [ "${#backups[@]}" -eq 1 ] && [ -f "${backups[0]}" ]
    [ "$(cat "${backups[0]}")" = "$(printf '%s' "$LEGACY_NETWORK")" ]
    [[ "$output" == *"retired legacy usb0 DHCP server"* ]]
}

@test "--retire-legacy on a clean box is a silent no-op" {
    run bash "$SCRIPT" --retire-legacy
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    [ ! -e "$RETIRED" ]
}

@test "--retire-legacy leaves a same-named file for another interface alone" {
    printf '[Match]\nName=eth1\n\n[Network]\nDHCP=yes\n' > "$NET/10-usb-gadget.network"
    run bash "$SCRIPT" --retire-legacy
    [ "$status" -eq 0 ]
    [ -f "$NET/10-usb-gadget.network" ]
    [ ! -e "$RETIRED" ]
}

@test "--retire-legacy never provisions the dnsmasq conf" {
    run bash "$SCRIPT" --retire-legacy
    [ "$status" -eq 0 ]
    [ ! -e "$MAGIC_USB_ROOT/etc/dnsmasq.d/usb0.conf" ]
}

@test "--retire-legacy never fails, even when the file cannot be moved" {
    plant_legacy
    mkdir -p "$(dirname "$RETIRED")"
    : > "$RETIRED"      # a FILE where the backup directory should go
    run bash "$SCRIPT" --retire-legacy
    [ "$status" -eq 0 ]
    [ -f "$NET/10-usb-gadget.network" ]
    [[ "$output" == *"WARNING: could not move"* ]]
}

@test "install mode writes the shipped usb0.conf and retires the legacy file" {
    plant_legacy
    run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    cmp -s "$CPP_DIR/scripts/data/dnsmasq-usb0.conf" "$MAGIC_USB_ROOT/etc/dnsmasq.d/usb0.conf"
    [ ! -e "$NET/10-usb-gadget.network" ]
    [ -n "$(ls "$RETIRED")" ]
}

@test "install mode is idempotent" {
    run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    cmp -s "$CPP_DIR/scripts/data/dnsmasq-usb0.conf" "$MAGIC_USB_ROOT/etc/dnsmasq.d/usb0.conf"
}

@test "an unknown argument is a usage error" {
    run bash "$SCRIPT" --bogus
    [ "$status" -eq 2 ]
}

@test "the shipped usb0.conf serves the dingus.box catch-all DNS on 10.55.0.x" {
    conf="$CPP_DIR/scripts/data/dnsmasq-usb0.conf"
    grep -qx 'interface=usb0' "$conf"
    grep -qx 'address=/#/10.55.0.1' "$conf"
    grep -qE '^dhcp-range=10\.55\.0\.10,10\.55\.0\.100,' "$conf"
    ! grep -qE '^port=0' "$conf"
}

@test "deploy --usb-gadget no longer writes a networkd DHCP server on usb0" {
    ! grep -nE '10-usb-gadget\.network <<|DHCPServer=yes|EmitDNS=no|Address=192\.168\.7\.1' "$DEPLOY"
    ! grep -nE 'enable systemd-networkd' "$DEPLOY"
    ! grep -n '192\.168\.7\.1:5000' "$DEPLOY"
}

@test "deploy --usb-gadget converges through setup_usb_gadget_dns.sh and points at 10.55.0.1" {
    usb_block="$(sed -n '/^# Step 5: Setup USB Gadget/,/^echo "=== Deployment Complete ==="/p' "$DEPLOY")"
    [ -n "$usb_block" ]
    [[ "$usb_block" == *'scripts/setup_usb_gadget_dns.sh"'* ]]
    # The owner-facing cable address is the IP: with the laptop's Wi-Fi on,
    # macOS resolves through Wi-Fi first and dingus.box never answers
    # (Pi 4B, 2026-10-27). The name stays an extra.
    grep -q 'Open browser to: http://10.55.0.1"' "$DEPLOY"
}

@test "install_deps.sh delegates the dnsmasq conf + redirect to the shared script" {
    grep -q 'setup_usb_gadget_dns.sh' "$INSTALL_DEPS"
    ! grep -nE '^[^#]*(dnsmasq-usb0\.conf|/etc/dnsmasq\.d/usb0\.conf)' "$INSTALL_DEPS"
    ! grep -nE '^[^#]*--only content-manager-redirect' "$INSTALL_DEPS"
}

@test "setup_memory_tuning.sh retires the legacy file before the cmdline step can exit" {
    tuning="$CPP_DIR/scripts/setup_memory_tuning.sh"
    helper_line="$(grep -n 'bash "$USB_DNS_HELPER" --retire-legacy' "$tuning" | head -1 | cut -d: -f1)"
    cmdline_line="$(grep -n '^# --- 4\. kernel cmdline' "$tuning" | cut -d: -f1)"
    [ -n "$helper_line" ] && [ -n "$cmdline_line" ]
    [ "$helper_line" -lt "$cmdline_line" ]
}
