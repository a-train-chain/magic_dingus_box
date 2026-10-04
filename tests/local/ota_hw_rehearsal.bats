#!/usr/bin/env bats
# Off-Pi tests for tests/ota_rehearsal/hw_rehearsal.sh (the hardware OTA
# rehearsal): its pure helpers, its parsing of deploy_cpp.sh, and a full
# --dry-run that must never touch ssh/rsync/curl/docker.
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

OTA_DIR="$TESTS_REPO_ROOT/tests/ota_rehearsal"
H="$OTA_DIR/hw_helpers.py"

setup() {
    # shellcheck source=../ota_rehearsal/hw_rehearsal_lib.sh
    . "$OTA_DIR/hw_rehearsal_lib.sh"
    T="$BATS_TEST_TMPDIR"
}

# --- lint ---------------------------------------------------------------------
@test "shellcheck clean: hw_rehearsal.sh + hw_rehearsal_lib.sh" {
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck not installed"
    run shellcheck -x "$OTA_DIR/hw_rehearsal.sh" "$OTA_DIR/hw_rehearsal_lib.sh"
    echo "$output"
    [ "$status" -eq 0 ]
}

# --- anchored pkill pattern ---------------------------------------------------
@test "pkill pattern matches the server's command line" {
    pat="$(hw_pkill_pattern /home/magic/fakegh)"
    [ "$pat" = '^python3 /home/magic/fakegh/fake_[g]ithub\.py' ]
    echo "python3 /home/magic/fakegh/fake_github.py /run/fake-github/leaf.crt /run/fake-github/leaf.key" | grep -qE "$pat"
}

@test "pkill pattern never matches an ssh/bash wrapper or its own pkill command line" {
    pat="$(hw_pkill_pattern /home/magic/fakegh/)"
    # the 2026-10-04 failure: the ssh session's own command line contained the path
    ! echo "bash -c sudo pkill -f /home/magic/fakegh/fake_github.py; rm -rf /home/magic/fakegh" | grep -qE "$pat"
    ! echo "sudo -n pkill -f $pat" | grep -qE "$pat"
    ! echo "pkill -f ^python3 /home/magic/fakegh/fake_[g]ithub\\.py" | grep -qE "$pat"
    ! echo "sudo setsid env FAKE_GH_LOG=x python3 /home/magic/fakegh/fake_github.py a b" | grep -qE "$pat"
    # a dot in the dir is literal, not "any character"
    pat2="$(hw_pkill_pattern /home/magic/fake.gh)"
    ! echo "python3 /home/magic/fakeXgh/fake_github.py" | grep -qE "$pat2"
    echo "python3 /home/magic/fake.gh/fake_github.py" | grep -qE "$pat2"
}

@test "tagged /etc/hosts line is removed exactly, nothing else" {
    printf '127.0.0.1 localhost\n::1 localhost\n127.0.1.1 magicpi-ab12\n' > "$T/hosts"
    cp "$T/hosts" "$T/hosts.orig"
    hw_hosts_line >> "$T/hosts"; echo >> "$T/hosts"
    grep -q 'api.github.com' "$T/hosts"
    sed -e "$(hw_hosts_sed)" "$T/hosts" > "$T/hosts.after"
    cmp "$T/hosts.orig" "$T/hosts.after"
}

@test "hw_kv / hw_assign / hw_safe_name / hw_field_state_file" {
    text=$'hostname=magicpi-ab12\nmodel=Raspberry Pi 5 Model B Rev 1.0\nfree_kib=123'
    [ "$(hw_kv model "$text")" = "Raspberry Pi 5 Model B Rev 1.0" ]
    [ "$(hw_kv missing "$text")" = "" ]
    X='a b "c" $d'
    eval "$(hw_assign X | sed 's/^X=/Y=/')"
    [ "$Y" = "$X" ]
    [ "$(hw_safe_name 'magic@192.168.1.50')" = "192.168.1.50" ]
    [ "$(hw_safe_name 'magic@magicpi ab:12')" = "magicpi_ab_12" ]
    [ "$(hw_field_state_file /d v1.9.14)" = "/d/field_state_1.9.14.txt" ]
    [ "$(hw_field_state_file /d 1.9.14)" = "/d/field_state_1.9.14.txt" ]
}

# --- manifests (filenames with spaces/newlines) --------------------------------
mkbox() {  # mkbox <root>
    local r="$1" d="$1/magic_dingus_box_cpp/data"
    mkdir -p "$d/playlists" "$d/saves/PCSX-ReARMed" "$d/media/uploads" "$d/roms/nes" \
             "$d/thumbnails/nes" "$d/thumbnails/systems" "$d/intro" "$d/upload_temp" "$r/config"
    echo 'title: My Mix' > "$d/playlists/My Mix.yaml"
    echo save > "$d/saves/PCSX-ReARMed/Final Fantasy VII (Disc 1).srm"
    printf 'x' > "$d/saves/PCSX-ReARMed/line"$'\n'"break.srm"
    echo art > "$d/thumbnails/nes/Super Mario Bros. (World).png"
    echo video > "$d/media/uploads/My Home Video.mp4"
    echo rom > "$d/roms/nes/Zelda (USA).nes"
    echo shipped > "$d/thumbnails/systems/nes.png"
    echo intro > "$d/intro/intro.30fps.mov"
    echo '{"screen": "playlist"}' > "$d/kiosk_status.json"
    echo partial > "$d/upload_temp/up 1.bin"
    echo db > "$d/media_browser.db"; echo wal > "$d/media_browser.db-wal"
    echo '{"audio": {"volume": 72}}' > "$r/config/settings.json"
    echo log > "$r/config/magic_dingus_box.log"; echo log > "$r/config/magic_dingus_box.log.1"
}

@test "manifest: operator data in, bulk/live/release-shipped files out" {
    mkbox "$T/box"
    run python3 "$H" manifest "$T/box" "$T/m.json"
    [ "$status" -eq 0 ]
    run python3 -c 'import json,sys; print("\n".join(sorted(json.load(open(sys.argv[1])))))' "$T/m.json"
    echo "$output"
    [[ "$output" == *"magic_dingus_box_cpp/data/playlists/My Mix.yaml"* ]]
    [[ "$output" == *"Final Fantasy VII (Disc 1).srm"* ]]
    [[ "$output" == *"Super Mario Bros. (World).png"* ]]
    [[ "$output" == *"config/settings.json"* ]]
    [[ "$output" == *$'line\nbreak.srm'* ]]
    [[ "$output" != *"media/uploads"* ]]
    [[ "$output" != *"roms/nes"* ]]
    [[ "$output" != *"kiosk_status.json"* ]]
    [[ "$output" != *"upload_temp"* ]]
    [[ "$output" != *"media_browser.db"* ]]
    [[ "$output" != *"magic_dingus_box.log"* ]]
    [[ "$output" != *"thumbnails/systems"* ]]
    [[ "$output" != *"intro.30fps.mov"* ]]
}

@test "compare: identical manifests pass; a changed space-named save fails and is named" {
    mkbox "$T/box"
    python3 "$H" manifest "$T/box" "$T/a.json"
    # live files churn: not a difference
    echo '{"screen": "settings"}' > "$T/box/magic_dingus_box_cpp/data/kiosk_status.json"
    echo more >> "$T/box/config/magic_dingus_box.log"
    python3 "$H" manifest "$T/box" "$T/b.json"
    run python3 "$H" compare "$T/a.json" "$T/b.json"
    [ "$status" -eq 0 ]
    echo corrupted > "$T/box/magic_dingus_box_cpp/data/saves/PCSX-ReARMed/Final Fantasy VII (Disc 1).srm"
    rm "$T/box/magic_dingus_box_cpp/data/playlists/My Mix.yaml"
    python3 "$H" manifest "$T/box" "$T/c.json"
    run python3 "$H" compare "$T/a.json" "$T/c.json" --label appdata
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" == *"DIFF    changed: magic_dingus_box_cpp/data/saves/PCSX-ReARMed/Final Fantasy VII (Disc 1).srm"* ]]
    [[ "$output" == *"DIFF    removed: magic_dingus_box_cpp/data/playlists/My Mix.yaml"* ]]
    # an allow-glob turns a difference into an allowed one
    run python3 "$H" compare "$T/a.json" "$T/c.json" --allow '*/saves/*' --allow '*/playlists/*'
    [ "$status" -eq 0 ]
    [[ "$output" == *"allowed changed:"* ]]
}

@test "changed-keys lists added, removed and changed keys" {
    echo '{"/a": "1", "/b c": "2", "/d": "x"}' > "$T/a.json"
    echo '{"/a": "1", "/b c": "3", "/e": "y"}' > "$T/b.json"
    run python3 "$H" changed-keys "$T/a.json" "$T/b.json"
    [ "$status" -eq 0 ]
    [ "$output" = $'/b c\n/d\n/e' ]
}

@test "fingerprint counts files and bytes; a missing dir is null" {
    mkdir -p "$T/c/sub dir"; printf 'abc' > "$T/c/sub dir/a b"; printf 'de' > "$T/c/x"
    run python3 "$H" fingerprint "$T/f.json" "$T/c" "$T/nope"
    [ "$status" -eq 0 ]
    [ "$(python3 "$H" get "$T/f.json" "$T/c" files)" = 2 ]
    [ "$(python3 "$H" get "$T/f.json" "$T/c" bytes)" = 5 ]
    [ "$(python3 "$H" get "$T/f.json" "$T/nope")" = "" ]
}

@test "syslisting covers units, drop-ins, wants, /usr/local/bin, udev, hosts" {
    r="$T/root"
    mkdir -p "$r/etc/systemd/system/magic-dingus-box-cpp.service.d" "$r/etc/systemd/system/multi-user.target.wants" \
             "$r/usr/local/bin" "$r/etc/udev/rules.d"
    echo unit > "$r/etc/systemd/system/magic-dingus-box-cpp.service"
    echo unrelated > "$r/etc/systemd/system/sshd-keygen.service"
    echo '[Service]' > "$r/etc/systemd/system/magic-dingus-box-cpp.service.d/memory-protect.conf"
    ln -s /etc/systemd/system/magic-dingus-box-cpp.service "$r/etc/systemd/system/multi-user.target.wants/magic-dingus-box-cpp.service"
    echo helper > "$r/usr/local/bin/qbit-port-sync.sh"
    echo rule > "$r/etc/udev/rules.d/90-magicdingus-uinput.rules"
    echo '127.0.0.1 localhost' > "$r/etc/hosts"
    run python3 "$H" syslisting "$T/s.json" --root "$r"
    [ "$status" -eq 0 ]
    run python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print("\n".join(f"{k} {v}" for k,v in sorted(d.items())))' "$T/s.json"
    echo "$output"
    [[ "$output" == *"/etc/systemd/system/magic-dingus-box-cpp.service "* ]]
    [[ "$output" == *"/etc/systemd/system/magic-dingus-box-cpp.service.d/memory-protect.conf "* ]]
    [[ "$output" == *"multi-user.target.wants/magic-dingus-box-cpp.service symlink:/etc/systemd/system/magic-dingus-box-cpp.service"* ]]
    [[ "$output" == *"/usr/local/bin/qbit-port-sync.sh "* ]]
    [[ "$output" == *"/etc/udev/rules.d/90-magicdingus-uinput.rules "* ]]
    [[ "$output" == *"/etc/hosts "* ]]
    [[ "$output" != *"sshd-keygen"* ]]
}

# --- rsync deletion parsing + the deploy --delete guard -------------------------
@test "deletion parsing: GNU itemized, openrsync, plain -v; spaces kept" {
    printf '%s\n' '*deleting   data/saves/My Game.srm' '*deleting z' 'deleting old dir/' '>f+++++++++ src/main.cpp' \
        './data: deleting' > "$T/out"
    run python3 "$H" rsync-deletions --dest-prefix magic_dingus_box_cpp/ < "$T/out"
    [ "$status" -eq 0 ]
    [ "$output" = $'magic_dingus_box_cpp/data/saves/My Game.srm\nmagic_dingus_box_cpp/z\nmagic_dingus_box_cpp/old dir/' ]
}

@test "deletion violations: data/ and config/ are protected, look-alikes are not" {
    printf '%s\n' '*deleting src/old.cpp' '*deleting static/config.js' '*deleting web/database.py' > "$T/ok"
    run python3 "$H" deletion-violations --dest-prefix magic_dingus_box/web/ < "$T/ok"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    printf '%s\n' '*deleting src/old.cpp' '*deleting data/thumbnails/arcade/Pac Man.png' '*deleting data' > "$T/bad"
    run python3 "$H" deletion-violations --dest-prefix magic_dingus_box_cpp/ < "$T/bad"
    [ "$status" -eq 1 ]
    [[ "$output" == *"magic_dingus_box_cpp/data/thumbnails/arcade/Pac Man.png"* ]]
    printf '%s\n' '*deleting config/settings.json' > "$T/cfg"
    run python3 "$H" deletion-violations < "$T/cfg"
    [ "$status" -eq 1 ]
}

@test "deploy-rsyncs finds exactly deploy_cpp.sh's three --delete rsyncs, with its filter list" {
    run python3 "$H" deploy-rsyncs "$CPP_DIR/scripts/deploy_cpp.sh" \
        --var "CPP_DIR=/src/cpp" --var "PI_HOST=magic@box" --var "PI_DIR=/opt/magic_dingus_box"
    echo "$output"
    [ "$status" -eq 0 ]
    [ "$(printf '%s\n' "$output" | grep -c .)" -eq 3 ]
    [[ "$output" == *'"dest": "magic@box:/opt/magic_dingus_box/magic_dingus_box_cpp/"'* ]]
    [[ "$output" == *'"dest": "magic@box:/opt/magic_dingus_box/magic_dingus_box/web/"'* ]]
    [[ "$output" == *'"dest": "magic@box:/opt/magic_dingus_box/scripts/golden_image/"'* ]]
    [[ "$output" == *'["--exclude", "data/media/*"]'* ]]
    [[ "$output" == *'["--filter", "P data/thumbnails/arcade"]'* ]]
    [[ "$output" == *'"src": "/src/cpp/"'* ]]
    [[ "$output" != *'${'* ]]
}

@test "deploy-rsyncs: quoted, continued, = and comment forms parse; non-delete rsyncs ignored" {
    cat > "$T/deploy.sh" <<'EOF'
echo "Step 9: thing"
rsync -avz --checksum \
    --delete \
    --exclude 'data/my media/*' \
    --exclude=build \
    --filter 'P data/keep me' \
    "${CPP_DIR}/" \
    "${PI_HOST}:${PI_DIR}/x/"   # trailing comment
rsync -avz "${CPP_DIR}/VERSION" "${PI_HOST}:${PI_DIR}/"
EOF
    run python3 "$H" deploy-rsyncs "$T/deploy.sh" --var CPP_DIR=/s --var PI_HOST=h --var PI_DIR=/opt/m
    echo "$output"
    [ "$status" -eq 0 ]
    [ "$(printf '%s\n' "$output" | grep -c .)" -eq 1 ]
    [[ "$output" == *'"step": "Step 9"'* ]]
    [[ "$output" == *'["--exclude", "data/my media/*"]'* ]]
    [[ "$output" == *'["--exclude", "build"]'* ]]
    [[ "$output" == *'["--filter", "P data/keep me"]'* ]]
    [[ "$output" == *'"dest": "h:/opt/m/x/"'* ]]
}

@test "deploy-guard --print-only prints the three dry-run rsyncs without running them" {
    run python3 "$H" deploy-guard "$CPP_DIR/scripts/deploy_cpp.sh" --print-only \
        --var "CPP_DIR=$CPP_DIR" --var "PI_HOST=magic@box" --var "PI_DIR=/opt/magic_dingus_box"
    echo "$output"
    [ "$status" -eq 0 ]
    [ "$(grep -c 'rsync -n --itemize-changes' <<<"$output")" -eq 3 ]
}

# --- field-state plan -----------------------------------------------------------
@test "field-plan merges the file with git-derived units/drop-ins, deduped and ordered" {
    cat > "$T/fs.txt" <<'EOF'
# comment
unit magic-dingus-audio.service
dropin /etc/systemd/system/magic-dingus-box-cpp.service.d/audio-service.conf
package python3-gunicorn
pulse-client-conf
run magic_dingus_box_cpp/scripts/setup_memory_tuning.sh   # trailing comment
EOF
    printf '%s\n' systemd/magic-dingus-web.service magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service > "$T/old"
    printf '%s\n' systemd/magic-dingus-web.service magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service \
        magic_dingus_box_cpp/systemd/magic-dingus-audio.service magic_dingus_box_cpp/systemd/magic-dingus-ota-recovery.service \
        magic_dingus_box_cpp/systemd/setup_boot_service.sh > "$T/new"
    printf '%s\n' magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service magic_dingus_box_cpp/systemd/setup_boot_service.sh > "$T/chg"
    printf '%s\n' systemd/system/system.slice.d/mdb-memory.conf > "$T/od"
    printf '%s\n' systemd/system/system.slice.d/mdb-memory.conf systemd/system/magic-dingus-box-cpp.service.d/audio-service.conf \
        systemd/system/magic-dingus-box-cpp.service.d/stop-timeout.conf > "$T/nd"
    run python3 "$H" field-plan --file "$T/fs.txt" --old-units "$T/old" --new-units "$T/new" --changed-units "$T/chg" \
        --old-dropins "$T/od" --new-dropins "$T/nd"
    echo "$output"
    [ "$status" -eq 0 ]
    expected="old-unit magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service
unit magic-dingus-audio.service
unit magic-dingus-ota-recovery.service
dropin /etc/systemd/system/magic-dingus-box-cpp.service.d/audio-service.conf
dropin /etc/systemd/system/magic-dingus-box-cpp.service.d/stop-timeout.conf
package python3-gunicorn
pulse-client-conf
run magic_dingus_box_cpp/scripts/setup_memory_tuning.sh"
    [ "$output" = "$expected" ]
}

@test "field-plan rejects unknown directives and malformed paths" {
    echo 'remove-everything /' > "$T/a.txt"
    run python3 "$H" field-plan --file "$T/a.txt"
    [ "$status" -ne 0 ]
    echo 'dropin etc/systemd/system/x.service.d/y.conf' > "$T/b.txt"
    run python3 "$H" field-plan --file "$T/b.txt"
    [ "$status" -ne 0 ]
    echo 'dropin /etc/passwd' > "$T/c.txt"
    run python3 "$H" field-plan --file "$T/c.txt"
    [ "$status" -ne 0 ]
    echo 'unit /etc/systemd/system/x.service' > "$T/d.txt"
    run python3 "$H" field-plan --file "$T/d.txt"
    [ "$status" -ne 0 ]
}

@test "every shipped field_state_*.txt parses" {
    n=0
    for f in "$OTA_DIR"/field_state_*.txt; do
        [ -f "$f" ] || continue
        run python3 "$H" field-plan --file "$f"
        echo "$f: $output"
        [ "$status" -eq 0 ]
        n=$((n + 1))
    done
    [ "$n" -ge 1 ]
}

# --- tarball + kiosk checks -------------------------------------------------------
@test "check-tarball rejects AppleDouble ._ entries and git-lfs pointers" {
    mkdir -p "$T/t/magic_dingus_box_cpp/data/intro"
    echo real > "$T/t/magic_dingus_box_cpp/README"
    head -c 4096 /dev/urandom > "$T/t/magic_dingus_box_cpp/data/intro/intro.30fps.mov"
    COPYFILE_DISABLE=1 tar -C "$T/t" -czf "$T/clean.tar.gz" .
    run python3 "$H" check-tarball "$T/clean.tar.gz"
    [ "$status" -eq 0 ]
    printf 'version https://git-lfs.github.com/spec/v1\noid sha256:abc\nsize 123\n' > "$T/t/magic_dingus_box_cpp/data/intro/intro.30fps.mov"
    echo junk > "$T/t/magic_dingus_box_cpp/._README"
    COPYFILE_DISABLE=1 tar -C "$T/t" -czf "$T/dirty.tar.gz" .
    run python3 "$H" check-tarball "$T/dirty.tar.gz"
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" == *"AppleDouble entry: ./magic_dingus_box_cpp/._README"* ]]
    [[ "$output" == *"git-lfs pointer, not content: ./magic_dingus_box_cpp/data/intro/intro.30fps.mov"* ]]
}

@test "kiosk-idle: playlist + no game = idle; game, other screen, garbage = not" {
    echo '{"screen": "playlist", "retroarch": null, "ts": 1}' > "$T/k1"
    run python3 "$H" kiosk-idle "$T/k1"; [ "$status" -eq 0 ]
    echo '{"screen": "playlist", "retroarch": {"core": "pcsx_rearmed"}}' > "$T/k2"
    run python3 "$H" kiosk-idle "$T/k2"; [ "$status" -eq 1 ]
    echo '{"screen": "settings", "retroarch": null}' > "$T/k3"
    run python3 "$H" kiosk-idle "$T/k3"; [ "$status" -eq 1 ]
    echo '{"screen": "playlist"}' > "$T/k4"
    run python3 "$H" kiosk-idle "$T/k4"; [ "$status" -eq 1 ]
    echo 'not json' > "$T/k5"
    run python3 "$H" kiosk-idle "$T/k5"; [ "$status" -eq 1 ]
}

# --- the driver itself ------------------------------------------------------------
@test "hw_rehearsal.sh refuses to run without an explicit PI_HOST" {
    run env -u PI_HOST "$OTA_DIR/hw_rehearsal.sh" --dry-run
    [ "$status" -eq 2 ]
    [[ "$output" == *"set PI_HOST explicitly"* ]]
}

@test "full --dry-run walks every step and never invokes ssh/rsync/curl/docker" {
    git -C "$TESTS_REPO_ROOT" rev-parse --verify -q 'v1.9.14^{commit}' >/dev/null || skip "v1.9.14 tag not in this clone"
    mkdir -p "$T/bin"
    for c in ssh rsync curl docker scp; do
        printf '#!/bin/sh\necho "%s $*" >> "%s/invoked"\nexit 99\n' "$c" "$T" > "$T/bin/$c"
        chmod +x "$T/bin/$c"
    done
    run env PATH="$T/bin:$PATH" PI_HOST=magic@dry.invalid OTA_HW_LOGROOT="$T/logs" \
        "$OTA_DIR/hw_rehearsal.sh" --dry-run --old v1.9.14 --new HEAD
    echo "$output" | tail -40
    [ "$status" -eq 0 ]
    [ ! -e "$T/invoked" ]
    [[ "$output" == *"DRY RUN complete"* ]]
    for s in "1. preflight" "2. NEW release artifacts" "3. snapshot" "4. downgrade" "5. field-state strip" \
             "6. fake GitHub" "7. install" "8. verify" "9. rollback" "9b. verify" "10a. teardown" \
             "10b. restore" "10c. verify the restored" "10d. delete the on-box snapshot"; do
        [[ "$output" == *"=== $s"* ]] || { echo "missing step: $s"; false; }
    done
    # the safety-critical commands are all there
    [[ "$output" == *'sudo -n pkill -f "$PKILL_PATTERN"'* ]]
    [[ "$output" == *"--no-specials --no-devices"* ]]
    [[ "$output" == *"update-ca-certificates --fresh"* ]]
    [[ "$output" == *'sudo -n cp "$FAKEGH/hosts.orig" /etc/hosts'* ]]
    [[ "$output" == *"Host: localhost"* ]]
    [[ "$output" == *"/admin/update/install"* ]]
    [[ "$output" == *"/admin/update/rollback"* ]]
    [ "$(grep -c '\[deploy-guard\] Step' <<<"$output")" -eq 3 ]
    [[ "$output" == *"unit magic-dingus-audio.service"* ]]
    [[ "$output" == *'sudo -n rm -rf "$SNAP" "$FAKEGH"'* ]]
    # every remote/local command is also logged for review
    log="$(ls -d "$T"/logs/*_dryrun)"
    grep -q 'update.sh install 1.9.14' "$log/remote_commands.sh" || grep -q 'update.sh\\ install\\ 1.9.14' "$log/remote_commands.sh"
}

@test "real run without --yes stops after preflight with nothing changed" {
    git -C "$TESTS_REPO_ROOT" rev-parse --verify -q 'v1.9.14^{commit}' >/dev/null || skip "v1.9.14 tag not in this clone"
    # ssh that answers the preflight like a healthy, idle box on this checkout;
    # anything else (rsync, a second kind of ssh call) is recorded as a violation.
    mkdir -p "$T/bin"
    ver="$(tr -d '[:space:]' < "$TESTS_REPO_ROOT/VERSION")"
    cat > "$T/bin/ssh" <<EOF
#!/bin/bash
args="\$*"
body="\$(cat)"
case "\$body" in
  *'echo "hostname='*) printf 'hostname=magicpi-test\nmodel=Raspberry Pi 5 Model B Rev 1.0\nversion=$ver\nfree_kib=99999999\nmarker=absent\nsudo=ok\nport443=free\n' ;;
  *'kiosk-idle'*|*'def cmd_kiosk_idle'*) echo "kiosk idle"; exit 0 ;;
  *) echo "ssh \$args" >> "$T/violations"; exit 99 ;;
esac
EOF
    printf '#!/bin/sh\necho "rsync $*" >> "%s/violations"; exit 99\n' "$T" > "$T/bin/rsync"
    printf '#!/bin/sh\nexit 0\n' > "$T/bin/curl"
    printf '#!/bin/sh\nexit 0\n' > "$T/bin/docker"
    chmod +x "$T/bin/"*
    run env PATH="$T/bin:$PATH" PI_HOST=magic@test.invalid OTA_HW_LOGROOT="$T/logs" "$OTA_DIR/hw_rehearsal.sh"
    echo "$output" | tail -20
    [ "$status" -ne 0 ]
    [[ "$output" == *"Re-run with --yes"* ]]
    [[ "$output" == *"hostname: magicpi-test"* ]]
    [[ "$output" == *"nothing on the box was changed"* ]]
    [ ! -e "$T/violations" ]
}
