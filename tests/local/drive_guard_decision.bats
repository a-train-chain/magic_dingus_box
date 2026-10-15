#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# qbit_port_sync.sh's drive-absent guard keeps torrents from writing into
# the SD card while the movie drive is unplugged. Its first version
# stopped everything ONCE behind a tmpfs marker: a later resume_all (game
# exit, movie exit, the kiosk's boot recovery) or a newly added download
# then wrote into the SD card until it filled, and a reboot lost the
# marker so the guard's stops became permanent. These tests pin the
# decision function and the enforce/release behavior against a fake qBit.

setup() {
    # shellcheck disable=SC1091
    source "$CPP_DIR/scripts/qbit_port_sync.sh"
    TMP="$(mktemp -d)"
    GUARD_STATE_DIR="$TMP/state"
    GUARD_HASHES="$GUARD_STATE_DIR/stopped_hashes"
    GUARD_PREFS="$GUARD_STATE_DIR/add_stopped_prev.json"
    LEGACY_GUARD_MARKER="$TMP/legacy_marker"
    STORAGE_ROOT="$TMP/ssd"
    mkdir -p "$STORAGE_ROOT/downloads"
    COOKIE="$TMP/cookie"
    CURL_LOG="$TMP/curl.log"
    : > "$CURL_LOG"
    # Default fake qBit state.
    TORRENTS='[{"hash":"aaa","state":"downloading"},{"hash":"bbb","state":"stoppedDL"},{"hash":"ccc","state":"stalledUP"}]'
    PREFS='{"add_stopped_enabled":false,"listen_port":1234}'
    MOUNTED=0
    QBIT_SEES=""
    export CURL_LOG
}

teardown() {
    rm -rf "$TMP"
}

# --- fakes (shadow the real binaries inside the sourced functions) -------
curl() {
    local url="" arg data=""
    for arg in "$@"; do
        case "$arg" in
            http*) url="$arg" ;;
            hashes=*|json=*) data="$arg" ;;
        esac
    done
    echo "${url##*/api/v2/} ${data}" >> "$CURL_LOG"
    case "$url" in
        */torrents/info) printf '%s' "$TORRENTS" ;;
        */app/preferences) printf '%s' "$PREFS" ;;
        */torrents/stop|*/torrents/start|*/app/setPreferences)
            # -w '%{http_code}' callers want a code; -f callers want rc 0.
            case " $* " in *http_code*) printf '200' ;; esac ;;
        *) case " $* " in *http_code*) printf '404' ;; esac ;;
    esac
    return 0
}
mountpoint() { [ "$MOUNTED" = "1" ]; }
docker() {
    # docker exec mdb_qbittorrent cat /downloads/.mdb-guard-probe
    if [ "$QBIT_SEES" = "drive" ]; then cat "$STORAGE_ROOT/downloads/.mdb-guard-probe"; fi
}

# --- pure decision -------------------------------------------------------
@test "drive absent -> enforce, whether or not already engaged" {
    [ "$(guard_decision 0 0 unknown)" = "enforce" ]
    [ "$(guard_decision 0 1 unknown)" = "enforce" ]
}

@test "drive present, guard idle -> none" {
    [ "$(guard_decision 1 0 unknown)" = "none" ]
}

@test "drive present and visible to qBit -> release" {
    [ "$(guard_decision 1 1 live)" = "release" ]
}

@test "drive back on host but qBit still on the stale bind -> keep enforcing" {
    [ "$(guard_decision 1 1 stale)" = "enforce" ]
    [ "$(guard_decision 1 1 unknown)" = "enforce" ]
}

@test "active_hashes skips stopped and paused, keeps everything else" {
    run bash -c "source '$CPP_DIR/scripts/qbit_port_sync.sh'; printf '%s' '[{\"hash\":\"a\",\"state\":\"downloading\"},{\"hash\":\"b\",\"state\":\"stoppedUP\"},{\"hash\":\"c\",\"state\":\"pausedDL\"},{\"hash\":\"d\",\"state\":\"queuedDL\"},{\"hash\":\"e\",\"state\":\"missingFiles\"}]' | active_hashes"
    [ "$status" -eq 0 ]
    [ "$output" = "$(printf 'a\nd\ne')" ]
}

@test "active_hashes fails on a non-list (so the caller falls back to stop-all)" {
    run bash -c "source '$CPP_DIR/scripts/qbit_port_sync.sh'; printf 'Forbidden' | active_hashes"
    [ "$status" -ne 0 ]
}

@test "hashes_param joins with | and collapses to all on the ALL sentinel" {
    [ "$(printf 'b\na\n' | hashes_param)" = "a|b" ]
    [ "$(printf 'a\nALL\n' | hashes_param)" = "all" ]
}

# --- behavior against the fake qBit -------------------------------------
@test "enforce stops only active torrents, records them persistently, sets add-stopped" {
    drive_guard_tick
    grep -q '^torrents/stop hashes=aaa|ccc' "$CURL_LOG"
    [ "$(cat "$GUARD_HASHES")" = "$(printf 'aaa\nccc')" ]
    grep -q '"add_stopped_enabled": true' "$CURL_LOG"
    grep -q '"add_stopped_enabled": false' "$GUARD_PREFS"
}

@test "enforce re-applies on every tick (torrents restarted behind its back)" {
    drive_guard_tick
    : > "$CURL_LOG"
    # Kiosk resume_all restarted everything, plus a new download appeared.
    TORRENTS='[{"hash":"aaa","state":"downloading"},{"hash":"bbb","state":"downloading"},{"hash":"ddd","state":"metaDL"}]'
    drive_guard_tick
    grep -q '^torrents/stop hashes=aaa|bbb|ddd' "$CURL_LOG"
    # The saved preference is NOT overwritten with the guard's own value.
    run grep -c 'setPreferences' "$CURL_LOG"
    [ "$output" = "0" ]
    grep -q '"add_stopped_enabled": false' "$GUARD_PREFS"
}

@test "nothing active -> no stop call, but still engaged" {
    TORRENTS='[{"hash":"bbb","state":"stoppedDL"}]'
    drive_guard_tick
    run grep -c 'torrents/stop' "$CURL_LOG"
    [ "$output" = "0" ]
    [ -f "$GUARD_HASHES" ]
}

@test "drive back but qBit cannot see it -> still stopped, nothing released" {
    drive_guard_tick
    : > "$CURL_LOG"
    MOUNTED=1
    QBIT_SEES=""
    TORRENTS='[{"hash":"aaa","state":"downloading"}]'
    drive_guard_tick
    run grep -c 'torrents/start' "$CURL_LOG"
    [ "$output" = "0" ]
    grep -q '^torrents/stop hashes=aaa' "$CURL_LOG"
    [ -f "$GUARD_HASHES" ]
}

@test "release starts exactly what the guard stopped and restores the pref" {
    drive_guard_tick
    : > "$CURL_LOG"
    MOUNTED=1
    QBIT_SEES=drive
    drive_guard_tick
    grep -q '^torrents/start hashes=aaa|ccc' "$CURL_LOG"
    grep -q 'setPreferences json={"add_stopped_enabled": false}' "$CURL_LOG"
    [ ! -f "$GUARD_HASHES" ]
    [ ! -f "$GUARD_PREFS" ]
    # bbb was stopped by the operator before the drive vanished: untouched.
    run grep -c 'bbb' "$CURL_LOG"
    [ "$output" = "0" ]
}

@test "state survives a reboot (persistent dir, not tmpfs)" {
    [[ "$(bash -c "unset GUARD_STATE_DIR; source '$CPP_DIR/scripts/qbit_port_sync.sh'; echo \$GUARD_STATE_DIR")" != /tmp/* ]]
    [[ "$(bash -c "unset GUARD_STATE_DIR; source '$CPP_DIR/scripts/qbit_port_sync.sh'; echo \$GUARD_STATE_DIR")" == /var/lib/* ]]
}

@test "legacy tmpfs marker is folded in and released as stop-all" {
    touch "$LEGACY_GUARD_MARKER"
    MOUNTED=1
    QBIT_SEES=drive
    drive_guard_tick
    grep -q '^torrents/start hashes=all' "$CURL_LOG"
    [ ! -f "$LEGACY_GUARD_MARKER" ]
}

@test "idle when mounted and never engaged: no qBit calls at all" {
    MOUNTED=1
    drive_guard_tick
    [ ! -s "$CURL_LOG" ]
}

@test "sourcing the script runs nothing (main is guarded)" {
    run bash -c "source '$CPP_DIR/scripts/qbit_port_sync.sh' && echo sourced-ok"
    [ "$status" -eq 0 ]
    [ "$output" = "sourced-ok" ]
}
