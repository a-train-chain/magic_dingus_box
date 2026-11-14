#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"
setup() {
    T="$BATS_TEST_TMPDIR/native";mkdir -p "$T/tree/magic_dingus_box_cpp/scripts" "$T/bin" "$T/tree/services" "$T/run"
    S="$T/tree/magic_dingus_box_cpp/scripts"
    cp -R "$CPP_DIR/scripts/vpn_recovery" "$S/"
    cp "$CPP_DIR/scripts/"{vpn_recoveryctl.py,ensure_gluetun_auth.sh,setup_memory_tuning.sh,restart_stale_cascade_watcher.sh,gluetun_cascade_restart.sh,service_stack.sh} "$S/"
    export MDB_VPN_STATE="$T/state" MDB_VPN_RUNTIME="$T/run" MDB_COMPOSE_LOCK="$T/compose.lock"
    export CALLS="$T/calls" STUB_RUNNING=true WATCHER_STATE="$T/watcher-active"
    echo active > "$WATCHER_STATE"
    export MAGIC_CASCADE_WATCHER_BIN="$S/gluetun_cascade_restart.sh" MAGIC_VPN_RECOVERY_SOURCE_DIR="$S" MAGIC_VPN_RECOVERY_FINGERPRINT="$T/run/running-fingerprint.json"
    export PATH="$T/bin:$PATH"
    export SOURCE_SCRIPTS="$S"
    cat > "$T/bin/systemctl" <<'EOF'
#!/bin/bash
case "$1" in
    is-active)
        case "$*" in *gluetun*) cat "$WATCHER_STATE"; [[ "$(cat "$WATCHER_STATE")" == active ]] ;; *) echo active ;; esac ;;
    stop)
        echo "systemctl $*" >> "$CALLS"
        [[ "${STUB_STOP_FAIL:-0}" == 1 ]] && exit 1
        echo inactive > "$WATCHER_STATE" ;;
    start)
        echo "systemctl $*" >> "$CALLS"; echo active > "$WATCHER_STATE"
        fp=$(python3 "$SOURCE_SCRIPTS/vpn_recoveryctl.py" fingerprint)
        printf '{"fingerprint":"%s"}\n' "$fp" > "$MDB_VPN_RUNTIME/running-fingerprint.json" ;;
    *) echo "systemctl $*" >> "$CALLS" ;;
esac
EOF
    cat > "$T/bin/docker" <<'EOF'
#!/bin/bash
if [[ "$1" == inspect ]]; then
    case "$*" in
        *Health.Status*) echo healthy ;;
        *OOMKilled*) printf '%064d 42 2026-11-14T00:00:00Z %s false running\n' 0 "$STUB_RUNNING" ;;
        *) printf '%064d 42 2026-11-14T00:00:00Z %s\n' 0 "$STUB_RUNNING" ;;
    esac
elif [[ "$*" == *'ip -o link show dev tun0'* ]]; then
    echo '2: tun0: <UP>'
else
    echo "docker $*" >> "$CALLS"
fi
EOF
    printf '#!/bin/bash\necho converge >> "$CALLS"\n' > "$S/converge_system_units.sh"
    chmod +x "$T/bin/"*
    printf 'VPN_PORT_FORWARDING=on\nVPN_COUNTRIES=Canada\n' > "$T/tree/services/.env"
    touch "$T/tree/services/docker-compose.yml"
}
ctl() { python3 "$S/vpn_recoveryctl.py" --base "$T/tree" "$@"; }
delivery() {
    # Execute the exact new-tree shared-hook block, as an OLD updater does.
    # Other tuning/convergence paths would touch /etc and are outside this test.
    local block
    block=$(sed -n '/^# Credentials must exist/,/^# --- 1g\./p' "$S/setup_memory_tuning.sh" | sed '$d')
    printf '%s\n' 'TUNING_ROOT="" SKIP_SYSTEMCTL=false' 'log() { echo "$*"; }' "$block" > "$S/test-delivery.sh"
    bash "$S/test-delivery.sh"
}
@test "old updater delivery installs auth before running adoption and coalesces reload" {
    delivery
    [ -f "$T/tree/services/config/gluetun/auth/recovery.key" ]
    ctl status >/dev/null
    grep -q '"enabled": true' "$T/state/intent.json"
    [ "$(grep -c '^docker restart mdb_gluetun$' "$CALLS")" -eq 1 ]
    delivery
    [ "$(grep -c '^docker restart mdb_gluetun$' "$CALLS")" -eq 1 ]
    [ "$(grep -c '^systemctl stop gluetun-cascade-restart.service$' "$CALLS")" -eq 1 ]
}
@test "delivery preserves explicit stop even when container is still reported running" {
    ctl intent stop
    delivery
    ! grep -q '^docker restart' "$CALLS" 2>/dev/null
    grep -q '"enabled": false' "$T/state/intent.json"
}
@test "delivery initializes stopped unconfigured boxes without starting stack" {
    rm "$T/tree/services/.env"
    STUB_RUNNING=false delivery
    ! grep -q '^docker ' "$CALLS" 2>/dev/null
    [ ! -f "$T/state/intent.json" ]
}
@test "module-only watcher change reloads and equal content is idempotent" {
    ctl provision
    fingerprint=$(ctl fingerprint)
    printf '{"fingerprint":"%s"}\n' "$fingerprint" > "$MAGIC_VPN_RECOVERY_FINGERPRINT"
    bash "$S/restart_stale_cascade_watcher.sh"
    ! grep -q try-restart "$CALLS" 2>/dev/null
    printf '\n# changed module\n' >> "$S/vpn_recovery/policy.py"
    bash "$S/restart_stale_cascade_watcher.sh"
    grep -q 'systemctl try-restart --no-block gluetun-cascade-restart.service' "$CALLS"
}
@test "rollback absent package uses legacy wrapper without recovery state" {
    rm -rf "$S/vpn_recovery" "$S/vpn_recoveryctl.py"
    MDB_BASE="$T/tree" bash "$S/service_stack.sh" start
    grep -q '^docker compose up -d$' "$CALLS"
    [ ! -f "$T/state/intent.json" ]
}

@test "systemd boot start and explicit stop release maintenance and retain stop intent" {
    ctl provision
    MDB_BASE="$T/tree" bash "$S/service_stack.sh" start
    grep -q '"enabled": true' "$T/state/intent.json"
    MDB_BASE="$T/tree" bash "$S/service_stack.sh" stop
    grep -q '"enabled": false' "$T/state/intent.json"
    python3 - "$T/state/intent.json" <<'PYTHON'
import json,sys
assert json.load(open(sys.argv[1]))['inhibits']=={}
PYTHON
    grep -q '^docker compose down$' "$CALLS"
}

@test "failed OTA releases owner without resurrecting an explicitly stopped box" {
    ctl provision
    ctl intent stop
    run ctl run-inhibited --reason ota --timeout 5 -- bash -c 'exit 7'
    [ "$status" -eq 7 ]
    grep -q '"enabled": false' "$T/state/intent.json"
    python3 - "$T/state/intent.json" <<'PYTHON'
import json,sys
assert json.load(open(sys.argv[1]))['inhibits']=={}
PYTHON
    ! grep -q '^docker restart' "$CALLS" 2>/dev/null
}

@test "native migration quiesces active watcher before auth restart and restores after convergence" {
    delivery
    python3 - "$CALLS" <<'PYTHON'
import sys
lines=open(sys.argv[1]).read().splitlines()
stop=next(i for i,l in enumerate(lines) if l=='systemctl stop gluetun-cascade-restart.service')
restart=lines.index('docker restart mdb_gluetun')
converge=lines.index('converge')
resume=next(i for i,l in enumerate(lines) if l=='systemctl start --no-block gluetun-cascade-restart.service')
assert stop<restart<converge<resume,lines
PYTHON
}

@test "inactive watcher remains inactive through credential activation" {
    echo inactive > "$WATCHER_STATE"
    delivery
    [ "$(cat "$WATCHER_STATE")" == inactive ]
    ! grep -q '^systemctl start\|^systemctl stop' "$CALLS" 2>/dev/null
}

@test "failed watcher quiesce defers auth restart and restores prior active unit" {
    export STUB_STOP_FAIL=1
    delivery
    ! grep -q '^docker restart' "$CALLS" 2>/dev/null
    grep -q '^systemctl start --no-block gluetun-cascade-restart.service$' "$CALLS"
    [ "$(cat "$WATCHER_STATE")" == active ]
}
