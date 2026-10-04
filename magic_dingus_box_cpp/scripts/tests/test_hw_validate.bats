#!/usr/bin/env bats
#
# BATS tests for hw_validate.sh — the pre-release hardware validation kit.
#
# Run with: bats test_hw_validate.bats
#
# The script runs against a fake box tree (MAGIC_DATA_DIR / MAGIC_BASE_DIR
# / HOME in a temp dir) with `sudo` and `systemctl` stubbed on PATH, so no
# test touches a real service. The headline test drives the SAFETY
# contract end to end: a fake smoke test scribbles over saves/states and
# the run must put every byte back.

SCRIPT_DIR="$(cd "$(dirname "$BATS_TEST_FILENAME")" && pwd)"
HWV="$SCRIPT_DIR/../hw_validate.sh"

setup() {
    T="$(mktemp -d)"
    export HOME="$T/home"
    export MAGIC_DATA_DIR="$T/data"
    export MAGIC_BASE_DIR="$T/base"
    export HWV_MODEL_FILE="$T/model"
    export HWV_VERIFY_BOX="$T/verify_box_stub.sh"
    export HWV_SMOKE="$T/fake_smoke.py"
    export CALLS="$T/calls.log"
    mkdir -p "$HOME/.config/retroarch/states/PCSX-ReARMed" \
             "$MAGIC_DATA_DIR/saves/PCSX-ReARMed" "$MAGIC_DATA_DIR/states" \
             "$MAGIC_BASE_DIR/config" "$T/bin"
    printf 'Raspberry Pi 4 Model B Rev 1.5\0' > "$HWV_MODEL_FILE"
    echo '{"audio": {"output": "auto"}, "display": {"mode": "crt_native"}}' \
        > "$MAGIC_BASE_DIR/config/settings.json"
    echo "user sram" > "$MAGIC_DATA_DIR/saves/PCSX-ReARMed/game.srm"
    echo "user state" > "$HOME/.config/retroarch/states/PCSX-ReARMed/game.state.auto"
    touch -t 202001010000 "$MAGIC_DATA_DIR/saves/PCSX-ReARMed/game.srm"
    write_status playlist ""

    cat > "$HWV_VERIFY_BOX" <<'EOF'
#!/bin/bash
echo "== Platform =="
echo "  [PASS] board: Raspberry Pi 4 Model B Rev 1.5"
echo "== RESULT =="
echo "  1 passed, 0 failed, 0 warnings"
echo "  SHIPPABLE"
EOF
    # sudo: log, allow `-n true` and `-n systemctl ...`, refuse the rest.
    cat > "$T/bin/sudo" <<'EOF'
#!/bin/bash
echo "sudo $*" >> "$CALLS"
[[ "$1" == "-n" ]] && shift
case "$1" in
    true) exit 0 ;;
    systemctl) shift; exec systemctl "$@" ;;
    *) exit 1 ;;
esac
EOF
    cat > "$T/bin/systemctl" <<'EOF'
#!/bin/bash
echo "systemctl $*" >> "$CALLS"
case "$1" in
    is-active) exit 0 ;;
    *) exit 0 ;;
esac
EOF
    chmod +x "$T/bin/sudo" "$T/bin/systemctl"
    export PATH="$T/bin:$PATH"
}

teardown() {
    chmod -R u+w "$T" 2>/dev/null
    rm -rf "$T"
}

write_status() {  # screen now_playing_kind
    python3 - "$MAGIC_DATA_DIR/kiosk_status.json" "$1" "$2" <<'PY'
import json, sys, time
path, screen, kind = sys.argv[1:4]
json.dump({"screen": screen, "ts": time.time(),
           "retroarch": {"core": "x"} if screen == "retroarch" else None,
           "now_playing": {"kind": kind},
           "settings": {"active": False,
                        "game_playlist_names": ["SNES Games", "NES Games"]}},
          open(path, "w"))
PY
}

pair_fake_remote() {
    echo "secret" > "$MAGIC_DATA_DIR/flask_secret.key"
    echo '{"devices": [{"id": "dev1"}]}' > "$MAGIC_DATA_DIR/paired_remotes.json"
}

report_json() { ls "$HOME"/hw_validate_*.json 2>/dev/null | head -1; }

@test "--help prints usage and exits 0" {
    run bash "$HWV" --help
    [ "$status" -eq 0 ]
    [[ "$output" == *"--yes"* ]]
    [[ "$output" == *"--dry-run"* ]]
}

@test "unknown option exits 2" {
    run bash "$HWV" --bogus
    [ "$status" -eq 2 ]
}

@test "refuses while a game is running" {
    write_status retroarch ""
    run bash "$HWV" --yes
    [ "$status" -eq 2 ]
    [[ "$output" == *"Refusing to run"*"game is running"* ]]
    [ -z "$(report_json)" ]
    ! grep -q "systemctl stop" "$CALLS" 2>/dev/null
}

@test "refuses while a movie is playing" {
    write_status media_browser movie
    run bash "$HWV"
    [ "$status" -eq 2 ]
    [[ "$output" == *"movie is playing"* ]]
}

@test "read-only run changes nothing and writes a JSON report" {
    before="$(cat "$MAGIC_BASE_DIR/config/settings.json")"
    run bash "$HWV"
    [[ "$output" == *"Re-run with --yes"* ]]
    [[ "$output" == *"[PASS] board: Raspberry Pi 4 Model B Rev 1.5"* ]]
    ! grep -qE "systemctl (stop|start|restart)" "$CALLS" 2>/dev/null
    [ "$(cat "$MAGIC_BASE_DIR/config/settings.json")" == "$before" ]
    ! ls -d "$HOME"/hw_validate_backup_* 2>/dev/null
    json="$(report_json)"
    [ -n "$json" ]
    python3 -c "import json,sys; r=json.load(open(sys.argv[1])); assert r['board']=='pi4' and r['results'] and r['manual_checklist']" "$json"
}

@test "exit status is 0 only when nothing FAILs (read-only run on a fake box fails)" {
    run bash "$HWV"
    [ "$status" -eq 1 ]
    python3 -c "import json,sys; r=json.load(open(sys.argv[1])); assert r['verdict']=='FAIL' and r['summary']['fail']>0" "$(report_json)"
}

@test "--dry-run --yes prints the plan and executes no state change" {
    before="$(md5sum "$MAGIC_BASE_DIR/config/settings.json" | cut -d' ' -f1)"
    run bash "$HWV" --dry-run --yes
    [[ "$output" == *"DRY-RUN would run: sudo -n systemctl stop magic-dingus-box-cpp.service"* ]]
    [[ "$output" == *"set-audio-output"*"headphone"* ]]
    [[ "$output" == *"DRY-RUN would run: rsync -a --delete"* ]]
    ! grep -qE "systemctl (stop|start|restart)" "$CALLS" 2>/dev/null
    [ "$(md5sum "$MAGIC_BASE_DIR/config/settings.json" | cut -d' ' -f1)" == "$before" ]
    ! ls -d "$HOME"/hw_validate_backup_* 2>/dev/null
}

@test "Pi 5 skips the headphone output" {
    printf 'Raspberry Pi 5 Model B Rev 1.0\0' > "$HWV_MODEL_FILE"
    run bash "$HWV" --dry-run --yes
    [[ "$output" == *"for output in auto hdmi:"* ]]
    ! grep -q "set-audio-output.*headphone" <<<"$output"
    grep -q "set-audio-output.* hdmi$" <<<"$output"
}

@test "--yes without a paired remote refuses the state-changing stages" {
    run bash "$HWV" --yes --skip-audio --skip-screenshots
    [ "$status" -eq 1 ]
    [[ "$output" == *"pair a phone once"* ]]
    [[ "$output" == *"state-changing stages NOT run"* ]]
    ! ls -d "$HOME"/hw_validate_backup_* 2>/dev/null
}

@test "SAFETY: saves/states scribbled on by the game stage are restored byte-for-byte" {
    pair_fake_remote
    cp -a "$MAGIC_DATA_DIR/saves" "$T/orig_saves"
    cp -a "$HOME/.config/retroarch/states" "$T/orig_ra_states"
    cat > "$HWV_SMOKE" <<'EOF'
import os, sys
d = os.environ["MAGIC_DATA_DIR"]; h = os.environ["HOME"]
open(f"{d}/saves/PCSX-ReARMed/game.srm", "w").write("CLOBBERED")
os.makedirs(f"{d}/states/PCSX-ReARMed", exist_ok=True)
open(f"{d}/states/PCSX-ReARMed/new.state.auto", "w").write("new")
open(f"{h}/.config/retroarch/states/PCSX-ReARMed/game.state.auto", "w").write("CLOBBERED")
print("EMULATOR SMOKE TEST REPORT")
print("[PASS] snes9x2010_libretro       'Mario                       ' launch=   100ms return=   100ms")
print("Games: 1/1 clean pass")
EOF
    run bash "$HWV" --yes --skip-audio --skip-screenshots
    [[ "$output" == *"[PASS] user saves/states/screenshots backed up"* ]]
    [[ "$output" == *"[PASS] snes9x2010_libretro"* ]]
    [[ "$output" == *"zero differences"* ]]
    diff -r "$T/orig_saves" "$MAGIC_DATA_DIR/saves"
    diff -r "$T/orig_ra_states" "$HOME/.config/retroarch/states"
    [ ! -e "$MAGIC_DATA_DIR/states/PCSX-ReARMed/new.state.auto" ]
    # mtime preserved too (rsync -a)
    [ "$(stat -c %Y "$MAGIC_DATA_DIR/saves/PCSX-ReARMed/game.srm" 2>/dev/null \
         || stat -f %m "$MAGIC_DATA_DIR/saves/PCSX-ReARMed/game.srm")" \
      == "$(stat -c %Y "$T/orig_saves/PCSX-ReARMed/game.srm" 2>/dev/null \
         || stat -f %m "$T/orig_saves/PCSX-ReARMed/game.srm")" ]
    # backup removed only after the verified restore
    ! ls -d "$HOME"/hw_validate_backup_* 2>/dev/null
    # a directory that did not exist before the run is gone again
    [ ! -d "$MAGIC_DATA_DIR/screenshots" ]
}

@test "SAFETY: a restore that cannot be verified keeps the backup and FAILs" {
    [ "$(id -u)" -ne 0 ] || skip "root ignores the read-only dir this test relies on"
    pair_fake_remote
    cat > "$HWV_SMOKE" <<'EOF'
import os
d = os.environ["MAGIC_DATA_DIR"]
locked = f"{d}/saves/locked"
os.makedirs(locked)
open(f"{locked}/junk.srm", "w").write("x")
os.chmod(locked, 0o555)   # restore cannot delete junk.srm
print("EMULATOR SMOKE TEST REPORT")
print("Games: 0/0 clean pass")
EOF
    run bash "$HWV" --yes --skip-audio --skip-screenshots
    [ "$status" -eq 1 ]
    [[ "$output" == *"backup KEPT at $HOME/hw_validate_backup_"* ]]
    ls -d "$HOME"/hw_validate_backup_*
    [ "$(cat "$HOME"/hw_validate_backup_*/data_saves/PCSX-ReARMed/game.srm)" == "user sram" ]
}

@test "SAFETY: SIGTERM mid-stage still restores" {
    pair_fake_remote
    cat > "$HWV_SMOKE" <<'EOF'
import os, time
d = os.environ["MAGIC_DATA_DIR"]
open(f"{d}/saves/PCSX-ReARMed/game.srm", "w").write("CLOBBERED")
open(os.environ["HOME"] + "/smoke_started", "w").close()
time.sleep(60)
EOF
    bash "$HWV" --yes --skip-audio --skip-screenshots > "$T/out.txt" 2>&1 &
    pid=$!
    for _ in $(seq 1 100); do [ -e "$HOME/smoke_started" ] && break; sleep 0.2; done
    [ -e "$HOME/smoke_started" ]
    kill -TERM "$pid"
    wait "$pid" || rc=$?
    [ "${rc:-0}" -eq 130 ]
    grep -q "run aborted by signal" "$T/out.txt"
    [ "$(cat "$MAGIC_DATA_DIR/saves/PCSX-ReARMed/game.srm")" == "user sram" ]
    ! ls -d "$HOME"/hw_validate_backup_* 2>/dev/null
}
