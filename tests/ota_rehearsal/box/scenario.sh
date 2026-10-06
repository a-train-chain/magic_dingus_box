#!/bin/bash
# OTA rehearsal driver — runs INSIDE the box container, as root.
#   scenario.sh old_cli | old_web | new_path
# Mounted: /w (artifacts, ro), /harness (this dir, ro), /out (logs).
# Every check prints PASS/FAIL/NOTE and is appended to /out/results.txt;
# the exit status is non-zero if any FAIL was recorded.
set -uo pipefail

SCEN="${1:?scenario}"
# shellcheck disable=SC1091
. /w/versions.env                     # OLD_VER NEW_VER OLD_SHA NEW_SHA
H=/harness OUT=/out REL=/w/release
INSTALL=/opt/magic_dingus_box
DATA=$INSTALL/magic_dingus_box_cpp/data
SCRIPTS=$INSTALL/magic_dingus_box_cpp/scripts
BIN=$INSTALL/magic_dingus_box_cpp/build/magic_dingus_box_cpp
BACKUP=/home/magic/.magic_dingus_box_backup
MARKER=${BACKUP}.ota_in_progress
SLUG=a-train-chain/magic_dingus_box
SHIM=/run/mdb-shim
IFS=. read -r _MAJ _MIN _PAT <<<"$NEW_VER"
V1="${_MAJ}.${_MIN}.$((_PAT + 1))"     # source-only fake release
V2="${_MAJ}.${_MIN}.$((_PAT + 2))"     # binary fake release
V3="${_MAJ}.${_MIN}.$((_PAT + 3))"     # binary fake release
src_url() { echo "https://github.com/${SLUG}/releases/download/v$1/magic-dingus-box-$1.tar.gz"; }

: > "$OUT/results.txt"
FAILS=0
pass() { echo "  PASS  $*"; echo "PASS  $*" >> "$OUT/results.txt"; }
fail() { FAILS=$((FAILS + 1)); echo "  FAIL  $*"; echo "FAIL  $*" >> "$OUT/results.txt"; }
note() { echo "  NOTE  $*"; echo "NOTE  $*" >> "$OUT/results.txt"; }
step() { echo; echo "=== $* ==="; echo "=== $*" >> "$OUT/results.txt"; }
check() { local d="$1"; shift; if "$@" >/dev/null 2>&1; then pass "$d"; else fail "$d"; fi; }
eq() { if [[ "$2" == "$3" ]]; then pass "$1 ($2)"; else fail "$1: got '$2', want '$3'"; fi; }
has() { if grep -qE -- "$2" "$3" 2>/dev/null; then pass "$1"; else fail "$1 — /$2/ not in $(basename "$3")"; fi; }
hasnt() { if grep -qE -- "$2" "$3" 2>/dev/null; then fail "$1 — /$2/ found in $(basename "$3"): $(grep -m1 -E -- "$2" "$3")"; else pass "$1"; fi; }
sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
ver() { tr -d '[:space:]' < "$INSTALL/VERSION"; }

# ---------------------------------------------------------------------------
# The update.sh process environment. Through the OLD web admin it is the web
# unit's environment (User=magic, WorkingDirectory=/opt/magic_dingus_box,
# PYTHONPATH, MAGIC_DATA_DIR) with TMPDIR re-pointed at data/upload_temp by
# create_app(). MAGIC_CF_PROBE_ATTEMPTS is the one rehearsal-only addition.
UPDATER_ENV=(HOME=/home/magic USER=magic LOGNAME=magic PATH=/usr/local/bin:/usr/bin:/bin
             "PYTHONPATH=$INSTALL" "MAGIC_DATA_DIR=$DATA" "TMPDIR=$DATA/upload_temp"
             MAGIC_CF_PROBE_ATTEMPTS=2)
# run_updater <logname> <script> <args...> — exec'd directly (the web admin
# runs [UPDATE_SCRIPT, ...], so the +x bit and shebang are part of the test).
run_updater() {
    local name="$1"; shift
    local t0=$SECONDS rc=0
    ( cd "$INSTALL" && runuser -u magic -- env -i "${UPDATER_ENV[@]}" "$@" ) \
        > "$OUT/$name.stdout" 2> "$OUT/$name.stderr" || rc=$?
    echo "[$name] rc=$rc in $((SECONDS - t0))s" | tee -a "$OUT/timings.txt"
    return $rc
}
last_json() { grep -E '^\{|^\}|^ ' "$1" | python3 -c '
import json,sys
buf=sys.stdin.read(); objs=[]; dec=json.JSONDecoder(); i=0
while i < len(buf):
    j=buf.find("{", i)
    if j<0: break
    try: o,k=dec.raw_decode(buf[j:]); objs.append(o); i=j+k
    except ValueError: i=j+1
print(json.dumps(objs[-1] if objs else {}))'; }
jget() { python3 -c "import json,sys; d=json.loads(sys.argv[1]); v=eval('d'+sys.argv[2]); print(json.dumps(v) if isinstance(v,(dict,list)) else v)" "$1" "$2" 2>/dev/null; }

snap() { python3 "$H/manifest.py" snap "$INSTALL" "$OUT/snap_$1.json"; }
snap_sys() { mkdir -p "/tmp/sys_$1"; for d in /etc /boot/firmware /usr/local/bin; do
    python3 "$H/manifest.py" snap "$d" "$OUT/sys_$1$(echo $d | tr / _).json"; done; }
sys_diff() {  # informational: what the hooks changed outside the tree
    for d in /etc /boot/firmware /usr/local/bin; do
        python3 "$H/manifest.py" same "$OUT/sys_$1$(echo $d | tr / _).json" "$OUT/sys_$2$(echo $d | tr / _).json" '*' \
            | grep -v '^tree compare' | grep -vE 'ld.so.cache|/ssl/certs|ca-certificates|/hosts$' | sed "s|: |: $d/|"
    done > "$OUT/sysdiff_$1_to_$2.txt"
}

# ---------------------------------------------------------------------------
setup_system() {
    step "container: systemctl shim, fake GitHub (TLS, /etc/hosts), Pi-like /boot"
    install -m 0755 "$H/systemctl" /usr/local/bin/systemctl
    install -d -m 0755 "$SHIM"
    : > "$OUT/systemctl_calls.log"; chmod 0666 "$OUT/systemctl_calls.log"
    echo "MAGIC_CF_PROBE_ATTEMPTS=2" > "$SHIM/web.extra_env"
    echo active > "$SHIM/kiosk_mode"

    local g=/run/fake-github; install -d -m 0755 "$g"
    (
        cd "$g" || exit 1
        openssl req -x509 -newkey rsa:2048 -nodes -days 3 -subj "/CN=MDB OTA rehearsal CA (container-local)" \
            -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
            -keyout ca.key -out ca.crt 2>/dev/null
        openssl req -newkey rsa:2048 -nodes -subj "/CN=github.com" -keyout leaf.key -out leaf.csr 2>/dev/null
        printf '%s\n' "subjectAltName=DNS:github.com,DNS:api.github.com,DNS:codeload.github.com,DNS:uploads.github.com,DNS:objects.githubusercontent.com,DNS:release-assets.githubusercontent.com" \
            "basicConstraints=CA:FALSE" "extendedKeyUsage=serverAuth" > ext.cnf
        openssl x509 -req -in leaf.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 3 -extfile ext.cnf -out leaf.crt 2>/dev/null
    )
    cp "$g/ca.crt" /usr/local/share/ca-certificates/mdb-ota-rehearsal.crt
    update-ca-certificates >/dev/null 2>&1
    echo "127.0.0.1 github.com api.github.com codeload.github.com uploads.github.com objects.githubusercontent.com release-assets.githubusercontent.com" >> /etc/hosts
    set_control "$NEW_VER" release false
    FAKE_GH_TEMPLATE_VERSION="$NEW_VER" FAKE_GH_LOG="$OUT/fake_github_requests.jsonl" \
        setsid python3 "$H/fake_github.py" "$g/leaf.crt" "$g/leaf.key" > "$OUT/fake_github.stderr" 2>&1 < /dev/null &
    for _ in $(seq 1 50); do curl -fsS -o /dev/null "https://api.github.com/repos/$SLUG/releases/latest" 2>/dev/null && break; sleep 0.2; done
    check "fake api.github.com answers over TLS trusted by the box's CA store" \
        curl -fsS -o /dev/null "https://api.github.com/repos/$SLUG/releases/latest"
    : > "$OUT/fake_github_requests.jsonl"

    # Raspberry Pi OS runs NetworkManager; the daemon is absent here (nmcli
    # calls in setup_network_hardening.sh no-op) but its config dirs exist.
    install -d /etc/NetworkManager/conf.d /etc/NetworkManager/dispatcher.d
    # A Pi's /boot/firmware, so setup_memory_tuning.sh's cmdline step runs.
    install -d /boot/firmware
    tar -xOf /w/old.tar magic_dingus_box_cpp/scripts/data/boot-cmdline.reference.txt \
        | sed 's/ cgroup_enable=memory cgroup_memory=1//' > /boot/firmware/cmdline.txt
    tar -xOf /w/old.tar magic_dingus_box_cpp/scripts/data/boot-config.reference.txt > /boot/firmware/config.txt
}

set_control() {  # set_control <latest> <asset_order> <minify>
    printf '{"latest": "%s", "asset_order": "%s", "minify": %s}\n' "$1" "$2" "$3" > /run/fake-github/control.json
}

provision_box() {
    step "box: ${OLD_VER} tree (git archive ${OLD_SHA:0:7}) + provisioned system state + operator data"
    install -d -o magic -g magic "$INSTALL"
    runuser -u magic -- tar -xf /w/old.tar -C "$INSTALL"
    eq "box VERSION" "$(ver)" "$OLD_VER"
    # deploy_cpp.sh's flattened compose file, as on every provisioned box
    runuser -u magic -- mkdir -p "$INSTALL/services"
    runuser -u magic -- cp "$INSTALL/magic_dingus_box_cpp/services/docker-compose.yml" \
        "$INSTALL/magic_dingus_box_cpp/services/.env.example" "$INSTALL/services/"
    # build/: a long-lived on-box build dir. The ${OLD_VER} kiosk binary is a
    # stand-in (an aarch64 ELF) — rollback only has to bring THESE bytes back.
    runuser -u magic -- bash -c "
        mkdir -p '$INSTALL/magic_dingus_box_cpp/build/CMakeFiles' '$INSTALL/magic_dingus_box_cpp/build/_deps/spdlog-src'
        cp /usr/bin/true '$BIN'
        echo 'CMAKE_BUILD_TYPE:STRING=Release' > '$INSTALL/magic_dingus_box_cpp/build/CMakeCache.txt'
        head -c 8192 /dev/urandom > '$INSTALL/magic_dingus_box_cpp/build/CMakeFiles/main.cpp.o'
        echo 1.15.0 > '$INSTALL/magic_dingus_box_cpp/build/_deps/spdlog-src/VERSION'"

    # --- provisioned system state (what setup_services/install_deps left) ---
    local t; t=$(mktemp -d); tar -xf /w/old.tar -C "$t"
    install -D -m 0644 "$t/magic_dingus_box_cpp/scripts/data/90-magicdingus-uinput.rules" /etc/udev/rules.d/90-magicdingus-uinput.rules
    install -D -m 0644 "$t/magic_dingus_box_cpp/scripts/data/dnsmasq-usb0.conf" /etc/dnsmasq.d/usb0.conf
    for h in playback_services_pause.sh gluetun_cascade_restart.sh clear_radarr_cooldowns.py sync_qbit_password.sh auto_blocklist_stuck_warnings.py; do
        [[ -f "$t/magic_dingus_box_cpp/scripts/$h" ]] && install -m 0755 "$t/magic_dingus_box_cpp/scripts/$h" "/usr/local/bin/$h"
    done
    install -m 0755 "$t/magic_dingus_box_cpp/scripts/qbit_port_sync.sh" /usr/local/bin/qbit-port-sync.sh
    install -m 0644 "$t/magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service" /etc/systemd/system/magic-dingus-box-cpp.service 2>/dev/null \
        || { install -d /etc/systemd/system; install -m 0644 "$t/magic_dingus_box_cpp/systemd/magic-dingus-box-cpp.service" /etc/systemd/system/; }
    install -m 0644 "$t/systemd/magic-dingus-web.service" /etc/systemd/system/magic-dingus-web.service
    # Every emulator core any shipped playlist references, so the core
    # bootstrap is a no-op (a box that has them) rather than an apt run.
    runuser -u magic -- mkdir -p /home/magic/.config/retroarch/cores
    grep -rhE '^[[:space:]]*emulator_core:' "$t/magic_dingus_box_cpp/data/playlists/" | awk -F: '{gsub(/[ "\047]/,"",$2); print $2}' \
        | sort -u | while read -r c; do runuser -u magic -- touch "/home/magic/.config/retroarch/cores/$c.so"; done
    rm -rf "$t"

    # --- operator data ---
    mk() { runuser -u magic -- mkdir -p "$(dirname "$1")"; runuser -u magic -- head -c "$2" /dev/urandom > "$1"; chown magic:magic "$1"; }
    mk "$DATA/media/Sacred_Steel/clip01.mp4" 3145728
    mk "$DATA/media/uploads/My Home Video.mp4" 1048576
    mk "$DATA/roms/nes/Super Mario Bros. (World).nes" 40976
    mk "$DATA/roms/atari7800/Asteroids (USA).zip" 16384        # makes games_atari7800.yaml playable
    mk "$DATA/saves/Nestopia/Super Mario Bros. (World).srm" 8192
    mk "$DATA/states/Nestopia/Super Mario Bros. (World).state.auto" 65536
    mk "$DATA/thumbnails/nes/Super Mario Bros. (World).png" 20480
    runuser -u magic -- sqlite3 "$DATA/media_browser.db" \
        "CREATE TABLE watch_state(id TEXT PRIMARY KEY, position_s REAL, watched INTEGER);
         INSERT INTO watch_state VALUES('tmdb:603',4211.5,0),('tvdb:121361:s01e01',0,1);"
    mk "$DATA/media_browser.db-wal" 4096
    mk "$DATA/media_browser.db-shm" 32768
    runuser -u magic -- bash -c "
        cd '$DATA'
        echo '{\"devices\": [{\"device_id\": \"d-7f3a\", \"name\": \"Alex iPhone\", \"paired_at\": \"2026-09-01T10:00:00Z\"}]}' > paired_remotes.json
        head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > flask_secret.key; chmod 0600 flask_secret.key
        echo '2026-09-01T10:00:00Z pair ok d-7f3a' > pairing_audit.log
        echo '{\"uuid\": \"6c1f\", \"hostname\": \"magicpi-6c1f\", \"device_name\": \"Living Room\"}' > device_info.json
        echo '{\"state\": \"menu\"}' > kiosk_status.json
        echo 'd-0ld0' > pending_revocations.txt
        mkdir -p upload_temp && head -c 100000 /dev/urandom > upload_temp/partial-upload.bin
        cd playlists && rm -f games_atari7800.yaml games_dreamcast.yaml
        printf -- '- title: \"Operator added\"\n  source_type: local\n  path: \"data/media/uploads/My Home Video.mp4\"\n' >> games_nes.yaml
        printf 'title: \"My Mix\"\nitems:\n- title: \"Clip\"\n  source_type: local\n  path: \"data/media/Sacred_Steel/clip01.mp4\"\n' > My_Mix.yaml
        mkdir -p '$INSTALL/config' && cd '$INSTALL/config'
        echo '{\"display\": {\"mode\": \"crt\", \"bezel\": \"wood\"}, \"audio\": {\"output\": \"hdmi\", \"volume\": 72}, \"playback\": {\"media_browser_unlocked\": true}}' > settings.json
        echo '{\"045e:028e\": {\"style\": \"ps\", \"captured_at\": \"2026-09-02T00:00:00Z\"}}' > controller_profiles.json
        mkdir -p '$INSTALL/tmp' && echo 'in-flight upload' > '$INSTALL/tmp/web-upload.part'
        mkdir -p '$INSTALL/services/config/radarr'
        echo '<Config><ApiKey>0123456789abcdef0123456789abcdef</ApiKey></Config>' > '$INSTALL/services/config/radarr/config.xml'"
    printf 'WIREGUARD_PRIVATE_KEY=fakeKEYfakeKEYfakeKEYfakeKEYfakeKEYfake0=\nRADARR_API_KEY=0123456789abcdef0123456789abcdef\nSONARR_API_KEY=fedcba9876543210fedcba9876543210\nMDB_QBIT_PASS=s3cret\n' \
        > "$INSTALL/services/.env"
    chown root:root "$INSTALL/services/.env"; chmod 0600 "$INSTALL/services/.env"
    install -d -o root -g root -m 0700 "$INSTALL/services/config/gluetun"
    echo '{"servers": []}' > "$INSTALL/services/config/gluetun/servers.json"; chmod 0600 "$INSTALL/services/config/gluetun/servers.json"
    STUB_SHA=$(sha "$BIN")
}

start_web() {
    systemctl start magic-dingus-web.service
    wait_web_version "" 40 >/dev/null
}
# wait_web_version <want|""> <secs> — echoes the version the web admin reports
wait_web_version() {
    local want="$1" secs="$2" v=""
    for _ in $(seq 1 $((secs * 2))); do
        v=$(curl -fsS --max-time 3 http://127.0.0.1:5000/admin/update/version 2>/dev/null | jq -r '.data.version // empty' 2>/dev/null)
        if [[ -n "$v" && ( -z "$want" || "$v" == "$want" ) ]]; then echo "$v"; return 0; fi
        sleep 0.5
    done
    echo "$v"; return 1
}

ci_binary_sha() {  # sha of the kiosk binary inside the v<ver> release asset
    local d; d=$(mktemp -d)
    tar -xzf "$REL/v$1/magic_dingus_box_cpp-arm64-$1.tar.gz" -C "$d" && sha "$d/magic_dingus_box_cpp"; rm -rf "$d"
}
extract_release() {  # extract_release <ver> -> /tmp/rel_<ver>
    rm -rf "/tmp/rel_$1"; mkdir -p "/tmp/rel_$1"
    tar -xzf "$REL/v$1/magic-dingus-box-$1.tar.gz" -C "/tmp/rel_$1"
}

# ---------------------------------------------------------------------------
check_old_check_output() {  # <json-file> <current> <latest>
    local j; j=$(last_json "$1")
    eq "check: ok" "$(jget "$j" '["ok"]')" "True"
    eq "check: current_version" "$(jget "$j" '["data"]["current_version"]')" "$2"
    eq "check: latest_version" "$(jget "$j" '["data"]["latest_version"]')" "$3"
    eq "check: update_available" "$(jget "$j" '["data"]["update_available"]')" "True"
    eq "check: download_url is the SOURCE tarball asset" "$(jget "$j" '["data"]["download_url"]')" "$(src_url "$3")"
}

# Deep post-install assertions shared by every path that installs ${NEW_VER}
# (or a fake follow-up). <ver> <stdout> <stderr> <expected-binary-sha|"source">
assert_installed() {
    # shellcheck disable=SC2034  # se: positional slot every caller passes; not asserted on yet
    local v="$1" so="$2" se="$3" want_bin="$4"
    step "post-install assertions for $v"
    eq "VERSION stamped" "$(ver)" "$v"
    local j; j=$(last_json "$so")
    eq "final JSON stage" "$(jget "$j" '["stage"]')" "complete"
    eq "final JSON new_version" "$(jget "$j" '["new_version"]')" "$v"
    check "kiosk binary present + executable" test -x "$BIN"
    if file "$BIN" | grep -q 'ARM aarch64'; then pass "kiosk binary is aarch64 ($(file -b "$BIN" | cut -d, -f1-2))"; else fail "kiosk binary not aarch64: $(file -b "$BIN")"; fi
    if ldd "$BIN" > "$OUT/ldd_$v.txt" 2>&1 && ! grep -q 'not found' "$OUT/ldd_$v.txt"; then
        pass "ldd: all $(grep -c '=>' "$OUT/ldd_$v.txt") shared libraries resolve against this Trixie userspace"
    else fail "ldd: unresolved libraries: $(grep 'not found' "$OUT/ldd_$v.txt" | tr '\n' ' ')"; fi
    strings "$BIN" > /tmp/kiosk.strings
    check "binary has sd_notify (READY=1)" grep -q 'READY=1' /tmp/kiosk.strings
    check "binary has Media Browser (prowlarr)" grep -qi prowlarr /tmp/kiosk.strings
    check "binary has GPIO (no 'compiled without HAVE_GPIOD')" bash -c "! grep -q 'compiled without HAVE_GPIOD' /tmp/kiosk.strings"
    if [[ "$want_bin" == source ]]; then
        if [[ "$(sha "$BIN")" != "$(ci_binary_sha "$NEW_VER")" ]]; then pass "binary was compiled on the box (differs from the CI asset)"; else fail "binary equals the CI asset — expected an on-box build"; fi
    else
        eq "installed binary == release asset binary (sha256)" "$(sha "$BIN")" "$want_bin"
    fi
    extract_release "$v"
    if python3 "$H/manifest.py" release "/tmp/rel_$v" "$INSTALL" > "$OUT/release_compare_$v.txt" 2>&1; then
        pass "installed tree == release tree ($(tail -1 "$OUT/release_compare_$v.txt"))"
    else fail "installed tree != release tree: $(grep VIOLATION "$OUT/release_compare_$v.txt" | head -5 | tr '\n' ';')"; fi
    check "update.sh in the new tree is executable (web admin execs it directly)" test -x "$SCRIPTS/update.sh"
    eq "services/docker-compose.yml == release's flattened copy" "$(sha "$INSTALL/services/docker-compose.yml")" "$(sha "/tmp/rel_$v/services/docker-compose.yml")"
    check "no OTA in-progress marker left behind" test ! -e "$MARKER"
    check "temp dir cleaned" test ! -e /tmp/magic_update
}

assert_user_data() {  # <before-snap> <after-snap> <label>
    if python3 "$H/manifest.py" user "$OUT/snap_$1.json" "$OUT/snap_$2.json" > "$OUT/userdata_$1_vs_$2.txt" 2>&1; then
        pass "operator data byte-identical ($3): $(tail -1 "$OUT/userdata_$1_vs_$2.txt")"
    else fail "operator data changed ($3): $(grep VIOLATION "$OUT/userdata_$1_vs_$2.txt" | head -5 | tr '\n' ';')"; fi
    grep '^note:' "$OUT/userdata_$1_vs_$2.txt" | while read -r l; do note "$3: $l"; done
}

# Differences a rollback may legitimately leave vs. the pre-install tree.
ROLLBACK_ALLOW=('*/__pycache__*' 'tmp' 'tmp/*' 'magic_dingus_box_cpp/data/kiosk_status.json'
                'magic_dingus_box_cpp/data/upload_temp' 'magic_dingus_box_cpp/data/upload_temp/*'
                'magic_dingus_box_cpp/data/pending_revocations.txt'
                'magic_dingus_box_cpp/data/playlists/games_atari7800.yaml'
                'magic_dingus_box_cpp/data/screenshots*' 'config/magic_dingus_box.log*')
assert_rolled_back_to() {  # <snap-before-install> <snap-now> <label> <want-version> <want-bin-sha>
    eq "$3: VERSION restored" "$(ver)" "$4"
    eq "$3: kiosk binary restored (sha256)" "$(sha "$BIN")" "$5"
    if python3 "$H/manifest.py" same "$OUT/snap_$1.json" "$OUT/snap_$2.json" "${ROLLBACK_ALLOW[@]}" "${EXTRA_ALLOW[@]}" \
        > "$OUT/tree_$1_vs_$2.txt" 2>&1; then
        pass "$3: whole tree == pre-install tree (allowed diffs: $(grep -c '^allowed' "$OUT/tree_$1_vs_$2.txt"))"
    else fail "$3: tree differs from pre-install: $(grep VIOLATION "$OUT/tree_$1_vs_$2.txt" | head -6 | tr '\n' ';')"; fi
    assert_user_data "$1" "$2" "$3"
}
EXTRA_ALLOW=()

check_hooks_ran_old() {  # <stderr> — the hooks the OLD update.sh runs from the NEW tree
    step "post-install hooks (run by the OLD update.sh from the NEW tree, as root via sudo -n)"
    has "old updater took the pre-compiled binary path" 'Using pre-compiled ARM64 binary' "$1"
    hasnt "no on-box compile" 'Building application from source' "$1"
    has "Phone Remote bootstrap skipped (box provisioned)" 'deps already provisioned; skipping bootstrap' "$1"
    has "emulator core bootstrap: all present" 'All referenced emulator cores present' "$1"
    has "network hardening hook ran" 'Applying network hardening' "$1"
    hasnt "network hardening did not fail" 'network hardening install failed' "$1"
    has "memory tuning hook ran" 'Converging playback memory posture' "$1"
    hasnt "memory tuning did not fail" 'memory tuning failed' "$1"
    has "Custom Format convergence hook ran" 'Converging Radarr/Sonarr Custom Formats' "$1"
    hasnt "Custom Format convergence did not fail" 'Custom Format convergence failed' "$1"
    check "memory-protect drop-in installed" test -f /etc/systemd/system/magic-dingus-box-cpp.service.d/memory-protect.conf
    check "stop-timeout drop-in installed" test -f /etc/systemd/system/magic-dingus-box-cpp.service.d/stop-timeout.conf
    check "system.slice companion installed" test -f /etc/systemd/system/system.slice.d/mdb-memory.conf
    check "zram sysctl installed" test -f /etc/sysctl.d/99-mdb-zram.conf
    check "magic-dingus-audio.service installed (setup_audio_service.sh via memory tuning)" test -f /etc/systemd/system/magic-dingus-audio.service
    check "kiosk audio drop-in installed" test -f /etc/systemd/system/magic-dingus-box-cpp.service.d/audio-service.conf
    check "cmdline.txt gained the memory cgroup flags" grep -q 'cgroup_enable=memory cgroup_memory=1' /boot/firmware/cmdline.txt
    eq "cmdline.txt is still ONE line" "$(wc -l < /boot/firmware/cmdline.txt)" "1"
    check "IPv6-off NetworkManager conf installed" test -f /etc/NetworkManager/conf.d/90-magic-ipv6-disable.conf
    local n=0 s
    for h in playback_services_pause.sh gluetun_cascade_restart.sh clear_radarr_cooldowns.py sync_qbit_password.sh auto_blocklist_stuck_warnings.py; do
        [[ -f /usr/local/bin/$h ]] || continue
        s=$(sha "$SCRIPTS/$h"); [[ "$(sha /usr/local/bin/$h)" == "$s" ]] && n=$((n + 1)) || fail "out-of-tree helper not refreshed: $h"
    done
    pass "out-of-tree helpers refreshed to the new tree ($n)"
    eq "qbit-port-sync.sh refreshed" "$(sha /usr/local/bin/qbit-port-sync.sh)" "$(sha "$SCRIPTS/qbit_port_sync.sh")"
    eq "usb0 dnsmasq conf refreshed" "$(sha /etc/dnsmasq.d/usb0.conf)" "$(sha "$SCRIPTS/data/dnsmasq-usb0.conf")"
    check "OLD updater does not install magic-dingus-ota-recovery.service (arrives with the NEXT update)" \
        test ! -e /etc/systemd/system/magic-dingus-ota-recovery.service
    grep -E 'cf-converge' "$1" | head -4 | sed 's/^/        /'
}

check_shim_sequence() {  # <from-line> — the systemctl calls one install made
    tail -n +"$1" "$OUT/systemctl_calls.log" | grep -v -- '-> rc' | grep -E 'magic-dingus-(box-cpp|web)|daemon-reload' \
        | sed 's/^[^ ]* //' | uniq -c | sed 's/^/        /'
}

check_kiosk_smoke() {
    local f; f=$(ls -t "$OUT"/kiosk_smoke_*.log 2>/dev/null | head -1)
    [[ -n "$f" ]] || { note "no kiosk smoke run captured"; return; }
    sleep 9
    if grep -q 'error while loading shared libraries' "$f"; then fail "kiosk smoke run: loader error: $(grep -m1 'error while' "$f")"
    else note "kiosk smoke run ($(basename "$f")): $(tail -1 "$f"); last output: $(grep -v '^---' "$f" | tail -3 | tr '\n' '|' | cut -c1-240)"; fi
}

web_checks() {  # import/create the web app with and without gunicorn
    step "web admin ${1}: import + create_app, without and with gunicorn"
    local py='import os; from magic_dingus_box.web import serve; s=serve.choose_server(); app=serve.build_app(); print("server", s, "routes", len(list(app.url_map.iter_rules())))'
    local envs=(HOME=/home/magic PATH=/usr/bin:/bin "PYTHONPATH=$INSTALL" "MAGIC_DATA_DIR=$DATA" "TMPDIR=$INSTALL/tmp")
    local out
    out=$(cd "$INSTALL" && runuser -u magic -- env -i "${envs[@]}" python3 -c "$py" 2>&1 | grep -v Warning | tail -1)
    if [[ "$out" == "server werkzeug routes "* ]]; then pass "no gunicorn: create_app ok, serve picks werkzeug ($out)"; else fail "no gunicorn: $out"; fi
    out=$(cd "$INSTALL" && runuser -u magic -- env -i "${envs[@]}" python3 -c 'import magic_dingus_box.web.wsgi as w; print(type(w.app).__name__)' 2>&1 | tail -1)
    eq "no gunicorn: wsgi module-level app builds" "$out" "Flask"
    has "running web (unit command line) serves with Werkzeug fallback" 'gunicorn not installed' "$(ls -t "$OUT"/web_*.log | head -1)"

    if apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq --no-install-recommends python3-gunicorn > "$OUT/apt_gunicorn.log" 2>&1; then
        out=$(cd "$INSTALL" && runuser -u magic -- env -i "${envs[@]}" python3 -c "$py" 2>&1 | grep -v Warning | tail -1)
        if [[ "$out" == "server gunicorn routes "* ]]; then pass "with gunicorn: create_app ok, serve picks gunicorn ($out)"; else fail "with gunicorn: $out"; fi
        systemctl restart magic-dingus-web.service
        local v; v=$(wait_web_version "$(ver)" 40)
        eq "with gunicorn: unit command line serves the Content Manager" "$v" "$(ver)"
        has "with gunicorn: served by gunicorn gthread" 'serving with gunicorn' "$(ls -t "$OUT"/web_*.log | head -1)"
        apt-get purge -y -qq python3-gunicorn >/dev/null 2>&1; apt-get autoremove -y -qq >/dev/null 2>&1
        systemctl restart magic-dingus-web.service; wait_web_version "" 40 >/dev/null
        check "gunicorn removed again (box state restored)" bash -c '! python3 -c "import gunicorn" 2>/dev/null'
    else
        note "apt could not install python3-gunicorn (offline?) — gunicorn half of the web check SKIPPED"
    fi
}

# ===========================================================================
scenario_old_cli() {
    setup_system; provision_box; start_web
    eq "OLD web admin is serving" "$(wait_web_version "" 5)" "$OLD_VER"
    snap s0; snap_sys s0
    local CI_SHA; CI_SHA=$(ci_binary_sha "$NEW_VER")

    step "OLD update.sh check (the exact CLI the ${OLD_VER} web admin runs)"
    run_updater check_pretty "$SCRIPTS/update.sh" check
    check_old_check_output "$OUT/check_pretty.stdout" "$OLD_VER" "$NEW_VER"
    eq "check: has_backup (no backup yet)" "$(jget "$(last_json "$OUT/check_pretty.stdout")" '["data"]["has_backup"]')" "False"
    set_control "$NEW_VER" release true
    run_updater check_minified "$SCRIPTS/update.sh" check
    eq "check (compact JSON): download_url" "$(jget "$(last_json "$OUT/check_minified.stdout")" '["data"]["download_url"]')" "$(src_url "$NEW_VER")"
    set_control "$NEW_VER" reversed false
    run_updater check_reversed "$SCRIPTS/update.sh" check
    eq "check (binary asset listed FIRST): still picks the source tarball" "$(jget "$(last_json "$OUT/check_reversed.stdout")" '["data"]["download_url"]')" "$(src_url "$NEW_VER")"
    set_control "$NEW_VER" release false
    note "release_notes as the OLD updater parses them: $(jget "$(last_json "$OUT/check_pretty.stdout")" '["data"]["release_notes"]' | cut -c1-120)"

    step "OLD update.sh install ${NEW_VER} (binary path)"
    local L0; L0=$(( $(wc -l < "$OUT/systemctl_calls.log") + 1 ))
    : > "$OUT/fake_github_requests.jsonl"
    if run_updater install "$SCRIPTS/update.sh" install "$NEW_VER" "$(src_url "$NEW_VER")"; then pass "install exited 0"; else fail "install exited non-zero"; fi
    echo "      systemctl calls made by the install:"; check_shim_sequence "$L0"
    python3 - "$OUT/fake_github_requests.jsonl" <<'EOF' | sed 's/^/      /'
import json, sys
for l in open(sys.argv[1]):
    r = json.loads(l); print(f"{r['status']} {r['method']} {r['host']}{r['path'][:95]}  ({r['bytes']} B, {r['ua'][:12]})")
EOF
    check "only GETs reached (fake) GitHub — nothing written" bash -c "! grep -v '\"method\": \"GET\"' '$OUT/fake_github_requests.jsonl' | grep -q ."
    check "binary asset was looked up via releases/tags/v${NEW_VER}" grep -q "releases/tags/v${NEW_VER}" "$OUT/fake_github_requests.jsonl"
    check "binary asset downloaded (CDN 200)" grep -q "mdb_asset=magic_dingus_box_cpp-arm64-${NEW_VER}" "$OUT/fake_github_requests.jsonl"
    snap s1; snap_sys s1; sys_diff s0 s1
    assert_installed "$NEW_VER" "$OUT/install.stdout" "$OUT/install.stderr" "$CI_SHA"
    assert_user_data s0 s1 "after install"
    check "add-only sync: games_atari7800.yaml added (its ROM is on the box)" test -f "$DATA/playlists/games_atari7800.yaml"
    check "add-only sync: games_dreamcast.yaml NOT added (no content)" test ! -e "$DATA/playlists/games_dreamcast.yaml"
    has "playlist sync logged the dreamcast skip" 'Skipping games_dreamcast.yaml' "$OUT/install.stderr"
    eq "backup completion marker = old version" "$(tr -d '[:space:]' < "$BACKUP/VERSION")" "$OLD_VER"
    eq "backup holds the old kiosk binary" "$(sha "$BACKUP/magic_dingus_box_cpp/build/magic_dingus_box_cpp")" "$STUB_SHA"
    [[ -e "$DATA/pending_revocations.txt" ]] || note "OLD install rsync deleted data/pending_revocations.txt (not on the ${OLD_VER} exclude list; the kiosk also clears it at start)"
    [[ -e "$DATA/upload_temp/partial-upload.bin" ]] || note "OLD install rsync deleted data/upload_temp/ (not on the ${OLD_VER} exclude list; also the OLD web admin's TMPDIR)"
    [[ -e "$INSTALL/tmp/web-upload.part" ]] || note "install rsync --delete removed /opt/magic_dingus_box/tmp/* (the web unit's TMPDIR) — the web restart's ExecStartPre recreates the dir"
    check_hooks_ran_old "$OUT/install.stderr"
    eq "web admin restarted onto the NEW tree" "$(wait_web_version "$NEW_VER" 40)" "$NEW_VER"
    local cj; cj=$(curl -fsS http://127.0.0.1:5000/admin/update/check)
    eq "NEW web admin check: current_version" "$(jget "$cj" '["data"]["current_version"]')" "$NEW_VER"
    eq "NEW web admin check: update_available" "$(jget "$cj" '["data"]["update_available"]')" "False"
    eq "NEW web admin check: has_backup" "$(jget "$cj" '["data"]["has_backup"]')" "True"
    check "NEW web admin serves the Content Manager page" curl -fsS -o /dev/null http://127.0.0.1:5000/
    check_kiosk_smoke
    web_checks "${NEW_VER}"

    step "rollback with the OLD update.sh (the backup's copy = ${OLD_VER}'s)"
    eq "backup's update.sh is byte-identical to ${OLD_VER}'s" "$(sha "$BACKUP/magic_dingus_box_cpp/scripts/update.sh")" \
        "$(tar -xOf /w/old.tar magic_dingus_box_cpp/scripts/update.sh | sha256sum | cut -d' ' -f1)"
    snap s1b
    if run_updater rollback_old "$BACKUP/magic_dingus_box_cpp/scripts/update.sh" rollback; then pass "OLD rollback exited 0"; else fail "OLD rollback exited non-zero"; fi
    local rj; rj=$(last_json "$OUT/rollback_old.stdout")
    eq "rollback JSON message" "$(jget "$rj" '["message"]')" "Rollback complete!"
    eq "rollback JSON version" "$(jget "$rj" '["version"]')" "$OLD_VER"
    snap s2; snap_sys s2; sys_diff s1 s2
    EXTRA_ALLOW=('magic_dingus_box_cpp/build/_deps/spdlog-src/VERSION')
    assert_rolled_back_to s0 s2 "after OLD rollback" "$OLD_VER" "$STUB_SHA"
    [[ -e "$INSTALL/magic_dingus_box_cpp/build/_deps/spdlog-src/VERSION" ]] \
        || note "OLD rollback deleted build/_deps/spdlog-src/VERSION — the ${OLD_VER} backup's unanchored 'VERSION' exclude never backed it up (fixed in ${NEW_VER}, '/VERSION')"
    EXTRA_ALLOW=()
    eq "web admin restarted back onto ${OLD_VER}" "$(wait_web_version "$OLD_VER" 40)" "$OLD_VER"
    [[ "$(sha /usr/local/bin/qbit-port-sync.sh)" == "$(sha "$SCRIPTS/qbit_port_sync.sh")" ]] \
        || note "OLD rollback leaves /usr/local/bin helpers at ${NEW_VER} (refresh-on-rollback only exists from ${NEW_VER} on)"
    [[ -f /etc/systemd/system/magic-dingus-box-cpp.service.d/audio-service.conf ]] && \
        note "after rollback the ${NEW_VER} audio drop-in (Wants=magic-dingus-audio.service, ExecStopPost= cleared) stays; the audio unit is ConditionPathExists=scripts/audio_service.sh, which the rollback removed$( [[ -e $SCRIPTS/audio_service.sh ]] && echo ' — BUT IT IS STILL THERE')"

    step "OLD update.sh install with NO TV connected (kiosk exits 69) — expect the documented one-time rollback"
    echo nodisplay > "$SHIM/kiosk_mode"
    snap s2b
    if run_updater install_tv_off "$SCRIPTS/update.sh" install "$NEW_VER" "$(src_url "$NEW_VER")"; then
        fail "TV-off install reported success (old updater should roll back on a non-active kiosk)"
    else pass "TV-off install failed as documented (old updater's 2 s is-active check)"; fi
    has "old updater rolled back internally" 'Service failed to start, rolling back' "$OUT/install_tv_off.stderr"
    snap s3
    EXTRA_ALLOW=('magic_dingus_box_cpp/build/_deps/spdlog-src/VERSION')
    assert_rolled_back_to s2b s3 "after TV-off auto-rollback" "$OLD_VER" "$STUB_SHA"
    EXTRA_ALLOW=()
    local fj; fj=$(last_json "$OUT/install_tv_off.stdout")
    note "what the web admin would show for the TV-off attempt: $(jget "$fj" '["stage"]' 2>/dev/null) / $(jget "$fj" '["message"]' 2>/dev/null)"

    step "retry with the TV on — the box takes ${NEW_VER} again"
    echo active > "$SHIM/kiosk_mode"
    if run_updater install_retry "$SCRIPTS/update.sh" install "$NEW_VER" "$(src_url "$NEW_VER")"; then pass "retry install exited 0"; else fail "retry install failed"; fi
    snap s4
    eq "retry: VERSION" "$(ver)" "$NEW_VER"
    eq "retry: binary" "$(sha "$BIN")" "$CI_SHA"
    assert_user_data s0 s4 "after rollback + retry"
}

scenario_old_web() {
    setup_system; provision_box; start_web
    snap s0
    local CI_SHA; CI_SHA=$(ci_binary_sha "$NEW_VER")
    step "OLD web admin: GET /admin/update/check"
    local cj; cj=$(curl -fsS http://127.0.0.1:5000/admin/update/check); echo "$cj" > "$OUT/web_check.json"
    eq "web check: update_available" "$(jget "$cj" '["data"]["update_available"]')" "True"
    eq "web check: latest_version" "$(jget "$cj" '["data"]["latest_version"]')" "$NEW_VER"
    local url ver_; url=$(jget "$cj" '["data"]["download_url"]'); ver_=$(jget "$cj" '["data"]["latest_version"]')

    step "OLD web admin: POST /admin/update/install {version, download_url} (what manager.js sends)"
    local tok jid
    tok=$(curl -fsS http://127.0.0.1:5000/admin/csrf-token | jq -r .data.token)
    jid=$(curl -fsS -X POST -H 'Content-Type: application/json' -H "X-CSRF-Token: $tok" \
        -d "{\"version\": \"$ver_\", \"download_url\": \"$url\"}" http://127.0.0.1:5000/admin/update/install | tee "$OUT/web_install_start.json" | jq -r .data.job_id)
    [[ -n "$jid" && "$jid" != null ]] && pass "install job started ($jid)" || fail "install job did not start: $(cat "$OUT/web_install_start.json")"
    local st="" t0=$SECONDS last="" seen_complete=0
    while (( SECONDS - t0 < 900 )); do
        st=$(curl -fsS --max-time 5 "http://127.0.0.1:5000/admin/update/status/$jid" 2>/dev/null) || { echo "      [web] status poll failed (web restarting?)"; break; }
        local line; line="$(jget "$st" '["data"]["status"]')/$(jget "$st" '["data"]["stage"]')/$(jget "$st" '["data"]["progress"]')"
        [[ "$line" != "$last" ]] && { echo "      [web] $line  $(jget "$st" '["data"]["message"]')"; echo "$st" >> "$OUT/web_job_status.jsonl"; last="$line"; }
        [[ "$(jget "$st" '["data"]["status"]')" == complete ]] && { seen_complete=1; break; }
        [[ "$(jget "$st" '["data"]["status"]')" == error ]] && break
        sleep 1
    done
    [[ $seen_complete == 1 ]] && pass "web job reported status=complete before the web restart" \
        || note "web job never reported complete to the poller (last: $last) — manager.js then falls back to polling /admin/update/check"
    # manager.js: after the job, poll /check until current_version == latest
    local v; v=$(for _ in $(seq 1 120); do
        c=$(curl -fsS --max-time 3 http://127.0.0.1:5000/admin/update/check 2>/dev/null | jq -r '.data.current_version // empty' 2>/dev/null)
        [[ "$c" == "$NEW_VER" ]] && { echo "$c"; break; }; sleep 1; done)
    eq "Settings tab's verification poll sees the new version" "$v" "$NEW_VER"
    sleep 2; snap s1
    # stdout/stderr of the job are not persisted by the OLD admin; synthesize from markers
    : > "$OUT/web_install.stdout"; echo '{"ok": true, "stage": "complete", "progress": 100, "new_version": "'"$(ver)"'"}' > "$OUT/web_install.stdout"
    assert_installed "$NEW_VER" "$OUT/web_install.stdout" /dev/null "$CI_SHA"
    assert_user_data s0 s1 "after web-driven install"
    eq "web admin restarted onto the NEW tree" "$(wait_web_version "$NEW_VER" 40)" "$NEW_VER"
    # The browser still runs the OLD manager.js: on 3 non-ok status polls it
    # fetches a CSRF token and verifies via /check. The NEW backend must
    # answer the OLD job id with a non-ok 404 (not a 500 / crash).
    local sc; sc=$(curl -s -o "$OUT/web_status_after_restart.json" -w '%{http_code}' "http://127.0.0.1:5000/admin/update/status/$jid")
    eq "NEW admin answers the OLD job id (old manager.js poll) with 404" "$sc" "404"
    check "NEW admin still serves /admin/csrf-token (old manager.js verify path)" \
        bash -c "curl -fsS http://127.0.0.1:5000/admin/csrf-token | jq -e '.data.token' >/dev/null"

    step "NEW web admin: Rollback button (POST /admin/update/rollback -> the NEW update.sh rollback)"
    tok=$(curl -fsS http://127.0.0.1:5000/admin/csrf-token | jq -r .data.token)
    local rr rc=0
    rr=$(curl -sS --max-time 240 -X POST -H 'Content-Type: application/json' -H "X-CSRF-Token: $tok" -d '{}' \
        http://127.0.0.1:5000/admin/update/rollback 2>&1) || rc=$?
    echo "$rr" > "$OUT/web_rollback_response.txt"
    if [[ $rc -eq 0 ]]; then note "rollback POST answered: $(echo "$rr" | tr '\n' ' ' | cut -c1-200)"
    else note "rollback POST got no answer (curl rc=$rc: the job restarts the web service under its own request) — manager.js's verify loop takes over"; fi
    v=$(for _ in $(seq 1 60); do
        c=$(curl -fsS --max-time 3 http://127.0.0.1:5000/admin/update/check 2>/dev/null | jq -r '.data.current_version // empty' 2>/dev/null)
        [[ -n "$c" && "$c" != "$NEW_VER" ]] && { echo "$c"; break; }; sleep 1; done)
    eq "manager.js verify loop (6 x 3 s) would see the rolled-back version" "$v" "$OLD_VER"
    cp /tmp/mdb_jobs_1000_*/*.log "$OUT/" 2>/dev/null && note "NEW admin's detached rollback job log copied to /out ($(ls /tmp/mdb_jobs_1000_*/*.log 2>/dev/null | wc -l) file)"
    sleep 1; snap s2
    # The backup was taken by the OLD update.sh, whose unanchored 'VERSION'
    # exclude never copied nested files named VERSION — any restore from it
    # deletes them. Benign for build/_deps; reported, not failed.
    EXTRA_ALLOW=('magic_dingus_box_cpp/build/_deps/spdlog-src/VERSION')
    assert_rolled_back_to s0 s2 "after NEW-admin rollback" "$OLD_VER" "$STUB_SHA"
    EXTRA_ALLOW=()
    [[ -e "$INSTALL/magic_dingus_box_cpp/build/_deps/spdlog-src/VERSION" ]] \
        || note "rollback from an OLD-updater backup deleted build/_deps/spdlog-src/VERSION (never backed up: ${OLD_VER}'s unanchored 'VERSION' exclude)"
    eq "web admin back on ${OLD_VER}" "$(wait_web_version "$OLD_VER" 40)" "$OLD_VER"
}

# NEW update.sh, from a box already on NEW_VER.
observe() {  # observe <file> — background sampler of marker / build.new / kiosk state
    local k
    while :; do
        k=$(sed -n 's/^ActiveState=//p' "$SHIM/kiosk.state" 2>/dev/null)
        printf '%s marker=%s build.new=%s build.old=%s kiosk=%s VERSION=%s\n' "$(date +%T)" \
            "$([[ -e $MARKER ]] && echo 1 || echo 0)" \
            "$([[ -e $INSTALL/magic_dingus_box_cpp/build.new ]] && echo 1 || echo 0)" \
            "$([[ -e $INSTALL/magic_dingus_box_cpp/build.old ]] && echo 1 || echo 0)" \
            "${k:-?}" "$(ver)" >> "$1"
        sleep 0.5
    done
}
scenario_new_path() {
    setup_system; provision_box; start_web
    snap s0
    local CI_SHA; CI_SHA=$(ci_binary_sha "$NEW_VER")
    step "bring the box to ${NEW_VER} with the OLD updater"
    run_updater install "$SCRIPTS/update.sh" install "$NEW_VER" "$(src_url "$NEW_VER")" && pass "old -> ${NEW_VER} ok" || fail "old -> ${NEW_VER} failed"
    eq "VERSION" "$(ver)" "$NEW_VER"
    wait_web_version "$NEW_VER" 40 >/dev/null

    step "NEW web admin installs source-only ${V1} (NEW update.sh: build.new + verify + promote, marker, recovery unit)"
    set_control "$V1" release false
    local cj; cj=$(curl -fsS http://127.0.0.1:5000/admin/update/check)
    eq "NEW admin check sees ${V1}" "$(jget "$cj" '["data"]["latest_version"]')" "$V1"
    local tok jid
    tok=$(curl -fsS http://127.0.0.1:5000/admin/csrf-token | jq -r .data.token)
    observe "$OUT/observer_${V1}.log" & local OBS=$!
    snap n0
    jid=$(curl -fsS -X POST -H 'Content-Type: application/json' -H "X-CSRF-Token: $tok" \
        -d "{\"version\": \"$V1\", \"download_url\": \"$(jget "$cj" '["data"]["download_url"]')\"}" \
        http://127.0.0.1:5000/admin/update/install | tee "$OUT/web_install_${V1}.json" | jq -r .data.job_id)
    [[ -n "$jid" && "$jid" != null ]] && pass "NEW admin started job $jid" || fail "NEW admin refused: $(cat "$OUT/web_install_${V1}.json")"
    local t0=$SECONDS st last=""
    while (( SECONDS - t0 < 2400 )); do
        st=$(curl -fsS --max-time 5 "http://127.0.0.1:5000/admin/update/status/$jid" 2>/dev/null) || { sleep 2; continue; }
        local line; line="$(jget "$st" '["data"]["status"]')/$(jget "$st" '["data"]["stage"]')"
        [[ "$line" != "$last" ]] && { echo "      [web $((SECONDS - t0))s] $line  $(jget "$st" '["data"]["message"]')"; last="$line"; }
        [[ "$(jget "$st" '["data"]["status"]')" =~ ^(complete|error)$ ]] && break
        sleep 2
    done
    kill $OBS 2>/dev/null; wait $OBS 2>/dev/null
    uniq -f1 "$OUT/observer_${V1}.log" > "$OUT/observer_${V1}.transitions.log"
    local jl; jl=$(ls -t /tmp/mdb_jobs_1000_*/"$jid".log 2>/dev/null | head -1)
    cp "$jl" "$OUT/install_${V1}.log" 2>/dev/null
    echo "[install_${V1} via NEW web admin] $((SECONDS - t0))s" | tee -a "$OUT/timings.txt"
    eq "job status survived the web restart and reads complete" "$(jget "$st" '["data"]["status"]')" "complete"
    has "job ran update.sh to exit 0" '__MDB_JOB_EXIT__ 0' "$OUT/install_${V1}.log"
    snap n1
    assert_installed "$V1" "$OUT/install_${V1}.log" "$OUT/install_${V1}.log" source
    eq "sentinel says ${V1}" "$(cat "$INSTALL/magic_dingus_box_cpp/REHEARSAL_RELEASE" 2>/dev/null)" "$V1"
    has "compiled in build.new/" 'Building clean in build.new/' "$OUT/install_${V1}.log"
    has "no pre-compiled binary for a source-only release" 'No pre-compiled binary found' "$OUT/install_${V1}.log"
    check "observer saw the in-progress marker during the install" grep -q 'marker=1' "$OUT/observer_${V1}.log"
    check "observer saw build.new/ during the compile" grep -q 'build.new=1' "$OUT/observer_${V1}.log"
    check "observer: live build/ binary never vanished while build.new compiled (VERSION unchanged until the end)" \
        bash -c "! grep 'build.new=1' '$OUT/observer_${V1}.log' | grep -qv 'VERSION=$NEW_VER'"
    check "build.new/ and build.old/ gone after promote" bash -c "test ! -e $INSTALL/magic_dingus_box_cpp/build.new -a ! -e $INSTALL/magic_dingus_box_cpp/build.old"
    check "magic-dingus-ota-recovery.service installed by the NEW update.sh" test -f /etc/systemd/system/magic-dingus-ota-recovery.service
    check "...and enabled" grep -qx magic-dingus-ota-recovery.service "$SHIM/enabled"
    has "verify_kiosk_started ran (no 'SKIP: kiosk start verification')" 'VERSION updated to' "$OUT/install_${V1}.log"
    hasnt "kiosk start verification did not fail" 'Kiosk did not start|did not stay up' "$OUT/install_${V1}.log"
    grep -E 'gunicorn' "$OUT/install_${V1}.log" | head -3 | while read -r l; do note "ensure_web_server_dep: $l"; done
    assert_user_data s0 n1 "after NEW updater source install"
    eq "web admin (now ${V1}) answers" "$(wait_web_version "$V1" 60)" "$V1"
    grep -hE 'serving with|gunicorn not installed' "$(ls -t "$OUT"/web_*.log | head -1)" | tail -1 | while read -r l; do note "web after ${V1}: $l"; done

    step "NEW update.sh rollback ${V1} -> ${NEW_VER}"
    run_updater rollback_new "$SCRIPTS/update.sh" rollback && pass "NEW rollback exit 0" || fail "NEW rollback failed"
    snap n2
    assert_rolled_back_to n0 n2 "after NEW rollback" "$NEW_VER" "$CI_SHA"
    check "sentinel gone" test ! -e "$INSTALL/magic_dingus_box_cpp/REHEARSAL_RELEASE"

    step "power cut mid-install of ${V2} (at the kiosk start, after rsync + binary), then boot-time 'recover'"
    set_control "$V2" release false
    echo "reset-failed magic-dingus-box-cpp.service" > "$SHIM/powercut_on"
    run_updater install_cut "$SCRIPTS/update.sh" install "$V2" "$(src_url "$V2")"; local rc=$?
    [[ $rc -ne 0 ]] && pass "install was killed mid-flight (rc=$rc)" || fail "install was not interrupted (rc=0)"
    check "in-progress marker survived the cut" test -f "$MARKER"
    note "marker: $(tr '\n' ' ' < "$MARKER" 2>/dev/null)"
    eq "VERSION not stamped" "$(ver)" "$NEW_VER"
    eq "tree is half-installed (sentinel ${V2} present)" "$(cat "$INSTALL/magic_dingus_box_cpp/REHEARSAL_RELEASE" 2>/dev/null)" "$V2"
    local L0; L0=$(( $(wc -l < "$OUT/systemctl_calls.log") + 1 ))
    # exactly what magic-dingus-ota-recovery.service runs (User=magic, HOME, ExecStart)
    ( cd / && runuser -u magic -- env -i HOME=/home/magic PATH=/usr/local/bin:/usr/bin:/bin \
        /bin/bash "$BACKUP/magic_dingus_box_cpp/scripts/update.sh" recover ) > "$OUT/recover.stdout" 2> "$OUT/recover.stderr"
    eq "recover exit" "$?" "0"
    snap n3
    assert_rolled_back_to n0 n3 "after boot recovery" "$NEW_VER" "$CI_SHA"
    check "marker cleared" test ! -e "$MARKER"
    check "recover never stopped/started the kiosk (it runs Before= the kiosk)" \
        bash -c "! tail -n +$L0 '$OUT/systemctl_calls.log' | grep -E 'systemctl (stop|start) magic-dingus-box-cpp'"
    has "recover explained itself" 'was interrupted' "$OUT/recover.stderr"

    step "power cut again, then the NEXT install restores first and goes through"
    echo "reset-failed magic-dingus-box-cpp.service" > "$SHIM/powercut_on"
    run_updater install_cut2 "$SCRIPTS/update.sh" install "$V2" "$(src_url "$V2")"
    check "marker left by the second cut" test -f "$MARKER"
    run_updater install_after_cut "$SCRIPTS/update.sh" install "$V2" "$(src_url "$V2")" && pass "install after cut exit 0" || fail "install after cut failed"
    has "it restored the interrupted update first" 'did not finish' "$OUT/install_after_cut.stderr"
    eq "VERSION" "$(ver)" "$V2"
    eq "binary (pre-compiled path, staged .new + mv)" "$(sha "$BIN")" "$(ci_binary_sha "$V2")"
    has "pre-compiled path used" 'Using pre-compiled ARM64 binary' "$OUT/install_after_cut.stderr"
    check "marker cleared" test ! -e "$MARKER"
    snap n4; assert_user_data s0 n4 "after cut + reinstall"

    step "NEW updater with NO TV (kiosk exits 69): the update is KEPT"
    echo nodisplay > "$SHIM/kiosk_mode"
    run_updater install_tv_off "$SCRIPTS/update.sh" install "$V2" "$(src_url "$V2")" && pass "TV-off install exit 0 (kept)" || fail "TV-off install failed"
    has "logged the no-display acceptance" 'no connected display' "$OUT/install_tv_off.stderr"
    eq "VERSION" "$(ver)" "$V2"

    step "NEW updater, kiosk crash-loops after ${V3}: verified-start fails -> automatic rollback to ${V2}"
    echo fail > "$SHIM/kiosk_mode"; set_control "$V3" release false
    snap n5
    run_updater install_crash "$SCRIPTS/update.sh" install "$V3" "$(src_url "$V3")" && fail "crash install reported success" || pass "crash install reported failure"
    local xj; xj=$(last_json "$OUT/install_crash.stdout")
    note "message shown to the owner: $(jget "$xj" '["error"]["message"]')"
    snap n6
    EXTRA_ALLOW=()
    assert_rolled_back_to n5 n6 "after crash auto-rollback" "$V2" "$(ci_binary_sha "$V2")"
    eq "sentinel back to ${V2}" "$(cat "$INSTALL/magic_dingus_box_cpp/REHEARSAL_RELEASE" 2>/dev/null)" "$V2"
    check "marker cleared after the completed rollback" test ! -e "$MARKER"
    echo active > "$SHIM/kiosk_mode"
}

case "$SCEN" in
    old_cli) scenario_old_cli ;;
    old_web) scenario_old_web ;;
    new_path) scenario_new_path ;;
    *) echo "unknown scenario $SCEN"; exit 2 ;;
esac

echo
echo "=== $SCEN: $(grep -c '^PASS' "$OUT/results.txt") passed, $FAILS failed, $(grep -c '^NOTE' "$OUT/results.txt") notes ==="
[[ $FAILS -eq 0 ]]
