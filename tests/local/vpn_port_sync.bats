#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

setup() {
    TMP="$(mktemp -d)"
    export ENV_FILE="$TMP/env" EVENT_LOG="$TMP/events"
    export QBIT_PORT_SYNC_SOURCE_DIR="$TMP/absent-source"
    printf 'MDB_QBIT_PASS=super-secret\n' > "$ENV_FILE"
    : > "$EVENT_LOG"
}
teardown() { rm -rf "$TMP"; }

@test "package port operation follows authentication and drive guard" {
    run bash -c '
        source "$1/scripts/qbit_port_sync.sh"
        curl() { echo auth >> "$EVENT_LOG"; echo Ok.; }
        drive_guard_tick() { echo guard >> "$EVENT_LOG"; }
        shared_port_sync() { echo shared >> "$EVENT_LOG"; }
        docker() { echo unexpected-legacy >> "$EVENT_LOG"; }
        main
    ' _ "$CPP_DIR"
    [ "$status" -eq 0 ]
    [ "$(cat "$EVENT_LOG")" = "$(printf 'auth\nguard\nshared')" ]
}

@test "login response never prints credential-bearing body" {
    run bash -c '
        source "$1/scripts/qbit_port_sync.sh"
        curl() { echo super-secret; }
        main
    ' _ "$CPP_DIR"
    [ "$status" -eq 0 ]
    [[ "$output" != *super-secret* ]]
}

@test "package is delegated exactly once with cookie and env paths" {
    run bash -c '
        source "$1/scripts/qbit_port_sync.sh"
        COOKIE="$2/cookie"
        python3() { printf "%s\n" "$@" > "$EVENT_LOG"; }
        shared_port_sync
    ' _ "$CPP_DIR" "$TMP"
    [ "$status" -eq 0 ]
    grep -qx 'vpn_recovery.port_sync' "$EVENT_LOG"
    grep -qx "$TMP/cookie" "$EVENT_LOG"
    grep -qx "$ENV_FILE" "$EVENT_LOG"
    ! grep -q super-secret "$EVENT_LOG"
}

@test "legacy no-port and already-synced exits still follow drive guard" {
    # A standalone/rollback copy has no adjacent package. Both old early exits
    # must preserve the authentication → guard order and no port write.
    cp "$CPP_DIR/scripts/qbit_port_sync.sh" "$TMP/qbit_port_sync.sh"
    for port in 0 1234; do
        : > "$EVENT_LOG"
        run bash -c '
            source "$1/qbit_port_sync.sh"
            PORT_FIXTURE="$2"
            curl() {
                case "$*" in
                    */auth/login*) echo auth >> "$EVENT_LOG"; echo Ok. ;;
                    */app/preferences*) echo "{\"listen_port\":1234}" ;;
                    *) echo unexpected-write >> "$EVENT_LOG" ;;
                esac
            }
            docker() { printf "{\"port\":%s}" "$PORT_FIXTURE"; }
            drive_guard_tick() { echo guard >> "$EVENT_LOG"; }
            main
        ' _ "$TMP" "$port"
        [ "$status" -eq 0 ]
        [ "$(cat "$EVENT_LOG")" = "$(printf 'auth\nguard')" ]
    done
}

@test "installed systemd entry point uses the deployed source package" {
    # Reproduce setup_services.sh's single-shell installation and execute the
    # service's actual ExecStart path beneath an isolated fake system root.
    local exec_start installed deployed source_default
    source_default=$(env -u QBIT_PORT_SYNC_SOURCE_DIR bash -c '
        source "$1/scripts/qbit_port_sync.sh"
        printf "%s" "$QBIT_PORT_SYNC_SOURCE_DIR"
    ' _ "$CPP_DIR")
    [ "$source_default" = /opt/magic_dingus_box/magic_dingus_box_cpp/scripts ]
    exec_start=$(sed -n 's/^ExecStart=//p' "$CPP_DIR/systemd/qbit-port-sync.service")
    [ "$exec_start" = /usr/local/bin/qbit-port-sync.sh ]
    installed="$TMP/system${exec_start}"
    deployed="$TMP/system${source_default}"
    mkdir -p "$(dirname "$installed")" "$deployed" "$TMP/bin"
    install -m 0755 "$CPP_DIR/scripts/qbit_port_sync.sh" "$installed"
    cp -R "$CPP_DIR/scripts/vpn_recovery" "$deployed/vpn_recovery"
    [ ! -e "$(dirname "$installed")/vpn_recovery" ]
    cat > "$TMP/bin/curl" <<'SH'
#!/bin/bash
printf 'auth\n' >> "$EVENT_LOG"
printf 'Ok.\n'
SH
    cat > "$TMP/bin/mountpoint" <<'SH'
#!/bin/bash
printf 'guard\n' >> "$EVENT_LOG"
exit 0
SH
    cat > "$TMP/bin/docker" <<'SH'
#!/bin/bash
printf 'unexpected-legacy\n' >> "$EVENT_LOG"
exit 1
SH
    cat > "$TMP/bin/python3" <<'SH'
#!/bin/bash
[ "$1" = -m ] || exit 3
printf 'shared\n' >> "$EVENT_LOG"
printf '%s\n' "$PYTHONPATH" "$@" > "$MODULE_LOG"
SH
    chmod +x "$TMP/bin/"*
    export MODULE_LOG="$TMP/module" QBIT_PORT_SYNC_SOURCE_DIR="$deployed"
    export GUARD_STATE_DIR="$TMP/guard" LEGACY_GUARD_MARKER="$TMP/legacy"
    run env PATH="$TMP/bin:$PATH" "$installed"
    [ "$status" -eq 0 ]
    [ "$(cat "$EVENT_LOG")" = "$(printf 'auth\nguard\nshared')" ]
    [ "$(head -n 1 "$MODULE_LOG")" = "$deployed${PYTHONPATH:+:$PYTHONPATH}" ]
    grep -qx vpn_recovery.port_sync "$MODULE_LOG"
}
