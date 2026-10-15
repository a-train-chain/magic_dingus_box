#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# Two VPN clients on one WireGuard private key knock each other off the
# tunnel. Every clone starts out holding the SOURCE box's key (services/.env,
# and Docker's own container metadata, which the restart policy starts gluetun
# from). The 2026-08-04 image's first boot died before its .env wipe, so its
# units dialled ProtonVPN on the owner's key.
#
# These tests pin the defence (scripts/golden_image/source_secrets_lib.sh):
#   - prepare records salted fingerprints (never values) of the source's
#     secrets into the image;
#   - first_boot wipes .env and purges inherited containers EARLY and
#     non-fatally;
#   - verify_box FAILs a unit still using a fingerprinted secret, and never
#     flags the source board itself;
#   - scan_image_for_secrets.sh can audit an old image for just the VPN key.

GI_DIR="$TESTS_REPO_ROOT/scripts/golden_image"
LIB="$GI_DIR/source_secrets_lib.sh"
VERIFY="$CPP_DIR/scripts/verify_box.sh"

WG_SRC='kO2sZ+XnZ4k8w0M7sL9b1p0yqT3cV6rN5dE8fG1hJ2k='
WG_OWN='yQ9fA1bC2dE3fG4hI5jK6lM7nO8pQ9rS0tU1vW2xY3s='
RADARR_SRC='0123456789abcdef0123456789abcdef'
QBIT_SRC='s3cr3t-qbit-pass-77'
FLASK_SRC='flask-hmac-secret-value-abcdef-0123'

setup() {
    T="$(mktemp -d "${BATS_TMPDIR:-/tmp}/mdb-fp.XXXXXX")"
    export MDB_BOARD_SERIAL_FILE="$T/serial"
    export MDB_SOURCE_FP_FILE="$T/etc/source_secret_fingerprints"
    export MDB_DOCKER_CONTAINERS_DIR="$T/docker/containers"
    printf 'SRC0000000000001\0' > "$MDB_BOARD_SERIAL_FILE"
    cat > "$T/source.env" <<EOF
PUID=1000
TZ=America/Los_Angeles
STORAGE_ROOT=/mnt/ssd
RADARR_API_KEY=${RADARR_SRC}
SONARR_API_KEY=__REPLACE_ME__
PROWLARR_API_KEY=__WILL_BE_SET_AFTER_FIRST_START__
QBITTORRENT_ADMIN_PASSWORD=${QBIT_SRC}
export MDB_QBIT_PASS="${QBIT_SRC}"
VPN_COUNTRIES=Netherlands
WIREGUARD_PRIVATE_KEY="${WG_SRC}"
WIREGUARD_PUBLIC_KEY=PeErPuBlIcKeYsharedByEveryProtonCustomer000=
EOF
    printf '%s\n' "$FLASK_SRC" > "$T/flask_secret.key"
    # shellcheck disable=SC1090
    source "$LIB"
    # Stubs directory, first on PATH only for the tests that need it.
    STUBS="$T/stubs"
    mkdir -p "$STUBS"
}

teardown() {
    rm -rf "$T"
}

record_source() {
    mdb_fp_record "$MDB_SOURCE_FP_FILE" "$T/source.env" "flask-secret=$T/flask_secret.key"
}

become_clone_board() {
    printf 'CLONE00000000002\0' > "$MDB_BOARD_SERIAL_FILE"
}

assert_no_secret_in() {
    local f="$1" v
    for v in "$WG_SRC" "$RADARR_SRC" "$QBIT_SRC" "$FLASK_SRC"; do
        if grep -qF -- "$v" "$f"; then
            echo "secret value leaked into $f"
            return 1
        fi
    done
}

# Assertions that hold under bash 3.2 too (macOS /bin/bash): there, `set -e`
# ignores a failing [[ ]] and a `! cmd` never fails a test, so neither can be
# used as a mid-test assertion.
assert_has() {
    case "$output" in *"$1"*) return 0 ;; esac
    echo "output lacks: $1"
    return 1
}
refute_has() {
    case "$output" in *"$1"*) echo "output unexpectedly contains a forbidden string"; return 1 ;; esac
    return 0
}
refute() {
    if "$@"; then echo "unexpectedly succeeded: $1"; return 1; fi
    return 0
}
inherited() {   # NAME ENV-LINE
    printf '%s\n' "$2" | mdb_fp_container_inherited "$1" "$MDB_SOURCE_FP_FILE"
}

# ---------------------------------------------------------------------------
# Recording
# ---------------------------------------------------------------------------

@test "record: fingerprints every secret-named .env key, nothing else" {
    record_source
    f="$MDB_SOURCE_FP_FILE"
    grep -q '^fp env:WIREGUARD_PRIVATE_KEY [0-9a-f]\{32\}$' "$f"
    grep -q '^fp env:RADARR_API_KEY ' "$f"
    grep -q '^fp env:QBITTORRENT_ADMIN_PASSWORD ' "$f"
    grep -q '^fp env:MDB_QBIT_PASS ' "$f"
    grep -q '^fp flask-secret ' "$f"
    # Identical on every correct box -> must never be fingerprinted, or every
    # clean unit would "match" the source.
    for k in PUID TZ STORAGE_ROOT VPN_COUNTRIES WIREGUARD_PUBLIC_KEY SONARR_API_KEY PROWLARR_API_KEY; do
        refute grep -q "^fp env:${k} " "$f"
    done
    [ "$(mdb_fp_count "$f")" -eq 5 ]
}

@test "record: the file holds no secret value and no unsalted hash of one" {
    record_source
    assert_no_secret_in "$MDB_SOURCE_FP_FILE"
    plain="$(printf '%s' "$WG_SRC" | mdb_fp_sha256)"
    refute grep -q "${plain:0:32}" "$MDB_SOURCE_FP_FILE"
    grep -q '^salt=[0-9a-f]\{32\}$' "$MDB_SOURCE_FP_FILE"
    grep -q '^source_board=[0-9a-f]\{32\}$' "$MDB_SOURCE_FP_FILE"
}

@test "record: a fresh salt per image (two images' files cannot be linked)" {
    record_source
    cp "$MDB_SOURCE_FP_FILE" "$T/first"
    record_source
    [ "$(mdb_fp_field "$T/first" salt)" != "$(mdb_fp_field "$MDB_SOURCE_FP_FILE" salt)" ]
    a="$(awk '$2 == "env:WIREGUARD_PRIVATE_KEY" { print $3 }' "$T/first")"
    b="$(awk '$2 == "env:WIREGUARD_PRIVATE_KEY" { print $3 }' "$MDB_SOURCE_FP_FILE")"
    [ -n "$a" ]
    [ "$a" != "$b" ]
}

@test "record: an unprovisioned source (no .env) still writes a valid file" {
    mdb_fp_record "$MDB_SOURCE_FP_FILE" "$T/absent.env"
    [ -f "$MDB_SOURCE_FP_FILE" ]
    [ "$(mdb_fp_count "$MDB_SOURCE_FP_FILE")" -eq 0 ]
    [ -n "$(mdb_fp_field "$MDB_SOURCE_FP_FILE" salt)" ]
}

@test "record CLI: 'record -' prints a file, never a value" {
    run env MDB_SERVICES_ENV="$T/source.env" bash "$LIB" record -
    [ "$status" -eq 0 ]
    assert_has "fp env:WIREGUARD_PRIVATE_KEY "
    refute_has "$WG_SRC"
    run bash "$LIB" bogus
    [ "$status" -eq 2 ]
}

# ---------------------------------------------------------------------------
# Matching
# ---------------------------------------------------------------------------

@test "scan_env: the source's own .env matches (quotes, export, CRLF)" {
    record_source
    run mdb_fp_scan_env "$MDB_SOURCE_FP_FILE" < "$T/source.env"
    assert_has "env:WIREGUARD_PRIVATE_KEY"
    assert_has "env:RADARR_API_KEY"
    printf "WIREGUARD_PRIVATE_KEY='%s'\r\n" "$WG_SRC" > "$T/crlf.env"
    run mdb_fp_scan_env "$MDB_SOURCE_FP_FILE" < "$T/crlf.env"
    [ "$output" = "env:WIREGUARD_PRIVATE_KEY" ]
}

@test "scan_env: docker inspect env lines (compose-unquoted) match" {
    record_source
    run mdb_fp_scan_env "$MDB_SOURCE_FP_FILE" <<<"PATH=/usr/bin
VPN_TYPE=wireguard
WIREGUARD_PRIVATE_KEY=${WG_SRC}"
    [ "$output" = "env:WIREGUARD_PRIVATE_KEY" ]
}

@test "scan_env: a unit with its OWN key and fresh API keys matches nothing" {
    record_source
    run mdb_fp_scan_env "$MDB_SOURCE_FP_FILE" <<<"PUID=1000
TZ=America/Los_Angeles
RADARR_API_KEY=ffffffffffffffffffffffffffffffff
WIREGUARD_PRIVATE_KEY=${WG_OWN}
WIREGUARD_PUBLIC_KEY=PeErPuBlIcKeYsharedByEveryProtonCustomer000="
    [ -z "$output" ]
}

@test "scan_env: no fingerprint file -> nothing (and stdin is drained)" {
    run mdb_fp_scan_env "$T/missing" <<<"WIREGUARD_PRIVATE_KEY=${WG_SRC}"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "match_value: the Flask HMAC secret (trailing newline ignored)" {
    record_source
    [ "$(mdb_fp_match_value "$MDB_SOURCE_FP_FILE" "$FLASK_SRC")" = "flask-secret" ]
    [ -z "$(mdb_fp_match_value "$MDB_SOURCE_FP_FILE" "some-other-secret")" ]
}

# ---------------------------------------------------------------------------
# The source box never flags itself
# ---------------------------------------------------------------------------

@test "source board is recognised; a clone board is not" {
    record_source
    mdb_fp_is_source_board "$MDB_SOURCE_FP_FILE"
    become_clone_board
    refute mdb_fp_is_source_board "$MDB_SOURCE_FP_FILE"
}

@test "no board serial (not a Pi) is never taken for the source" {
    rm -f "$MDB_BOARD_SERIAL_FILE"
    record_source
    refute grep -q '^source_board=' "$MDB_SOURCE_FP_FILE"
    refute mdb_fp_is_source_board "$MDB_SOURCE_FP_FILE"
}

@test "restore: removes the file on the source board" {
    record_source
    mdb_fp_remove_on_source "$MDB_SOURCE_FP_FILE"
    [ ! -e "$MDB_SOURCE_FP_FILE" ]
}

@test "restore: a CLONE running restore keeps its fingerprints" {
    record_source
    become_clone_board
    mdb_fp_remove_on_source "$MDB_SOURCE_FP_FILE"
    [ -f "$MDB_SOURCE_FP_FILE" ]
}

@test "restore_after_cloning.sh removes the file, best-effort, before restoring secrets" {
    f="$GI_DIR/restore_after_cloning.sh"
    run grep -n 'mdb_fp_remove_on_source "$MDB_SOURCE_FP_FILE" || true' "$f"
    [ "$status" -eq 0 ]
    rm_line="${output%%:*}"
    step0_line="$(grep -n '^mdb_restore_secrets ' "$f" | head -1 | cut -d: -f1)"
    [ "$rm_line" -lt "$step0_line" ]
}

# ---------------------------------------------------------------------------
# prepare_for_cloning.sh
# ---------------------------------------------------------------------------

@test "prepare records fingerprints BEFORE .env is stashed, fatally on failure" {
    f="$GI_DIR/prepare_for_cloning.sh"
    rec="$(grep -n '^if ! mdb_fp_record "$MDB_SOURCE_FP_FILE" "${SERVICES_DIR}/.env"' "$f" | cut -d: -f1)"
    [ -n "$rec" ]
    stash="$(grep -n 'log "\[2c/5\] Removing application secrets' "$f" | cut -d: -f1)"
    boot="$(grep -n '^BOOT_FW="/boot/firmware"' "$f" | cut -d: -f1)"
    [ "$rec" -lt "$stash" ]
    [ "$rec" -lt "$boot" ]
    sed -n "${rec},$((rec + 4))p" "$f" | grep -q 'exit 1'
}

@test "prepare's post-scrub leak check asserts no Docker config.v2.json ships" {
    grep -qF "'/var/lib/docker/containers|config.v2.json'" "$GI_DIR/prepare_for_cloning.sh"
}

# ---------------------------------------------------------------------------
# Inherited containers (first boot)
# ---------------------------------------------------------------------------

@test "container decision: stack names, any WG key, or a fingerprinted secret" {
    record_source
    become_clone_board
    inherited /mdb_gluetun 'A=b'
    inherited /other "WIREGUARD_PRIVATE_KEY=${WG_OWN}"
    inherited /other "RADARR_API_KEY=${RADARR_SRC}"
    refute inherited /other 'A=b'
    refute inherited /other 'WIREGUARD_PRIVATE_KEY='
}

make_container() {   # ID NAME ENV...
    local id="$1" name="$2"; shift 2
    local d="$MDB_DOCKER_CONTAINERS_DIR/$id" env="" e
    mkdir -p "$d"
    for e in "$@"; do env="${env:+$env,}\"$e\""; done
    printf '{"ID":"%s","Name":"/%s","Config":{"Hostname":"x","Env":[%s]},"State":{"Running":false}}' \
        "$id" "$name" "$env" > "$d/config.v2.json"
    printf '{"RestartPolicy":{"Name":"unless-stopped"}}' > "$d/hostconfig.json"
}

stub() {   # NAME BODY
    printf '#!/bin/bash\n%s\n' "$2" > "$STUBS/$1"
    chmod +x "$STUBS/$1"
}

@test "docker_cfg_dump: name, then Config.Env, from config.v2.json" {
    make_container aaa mdb_gluetun "VPN_TYPE=wireguard" "WIREGUARD_PRIVATE_KEY=${WG_SRC}"
    run mdb_fp_docker_cfg_dump "$MDB_DOCKER_CONTAINERS_DIR/aaa/config.v2.json"
    [ "${lines[0]}" = "/mdb_gluetun" ]
    [ "${lines[1]}" = "VPN_TYPE=wireguard" ]
    [ "${lines[2]}" = "WIREGUARD_PRIVATE_KEY=${WG_SRC}" ]
}

@test "purge (dockerd down): removes inherited container state, keeps the rest" {
    record_source
    become_clone_board
    make_container aaa mdb_gluetun "WIREGUARD_PRIVATE_KEY=${WG_SRC}"
    make_container bbb mdb_radarr "PUID=1000"
    make_container ccc someone_elses "RADARR_API_KEY=${RADARR_SRC}"
    make_container ddd unrelated "FOO=bar"
    mkdir -p "$MDB_DOCKER_CONTAINERS_DIR/eee"   # no config.v2.json: dockerd skips it too
    stub systemctl 'exit 3'
    stub pgrep 'exit 1'
    PATH="$STUBS:$PATH" run mdb_purge_inherited_containers "$MDB_SOURCE_FP_FILE"
    [ "$status" -eq 0 ]
    [ ! -e "$MDB_DOCKER_CONTAINERS_DIR/aaa" ]
    [ ! -e "$MDB_DOCKER_CONTAINERS_DIR/bbb" ]
    [ ! -e "$MDB_DOCKER_CONTAINERS_DIR/ccc" ]
    [ -d "$MDB_DOCKER_CONTAINERS_DIR/ddd" ]
    [ -d "$MDB_DOCKER_CONTAINERS_DIR/eee" ]
    refute_has "$WG_SRC"
}

@test "purge (dockerd up): docker rm -f exactly the inherited containers" {
    record_source
    become_clone_board
    make_container aaa mdb_gluetun "WIREGUARD_PRIVATE_KEY=${WG_SRC}"
    make_container ddd unrelated "FOO=bar"
    stub systemctl 'exit 0'
    stub pgrep 'exit 0'
    stub docker "
D=\"$MDB_DOCKER_CONTAINERS_DIR\"
case \"\$1\" in
  ps) ls \"\$D\" ;;
  inspect)
    fmt=\"\$3\"; id=\"\$4\"
    if [[ \"\$fmt\" == '{{.Name}}' ]]; then
      python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[\"Name\"])' \"\$D/\$id/config.v2.json\"
    else
      python3 -c 'import json,sys; [print(e) for e in json.load(open(sys.argv[1]))[\"Config\"][\"Env\"]]' \"\$D/\$id/config.v2.json\"
    fi ;;
  rm) echo \"\$3\" >> \"$T/removed\" ;;
esac"
    PATH="$STUBS:$PATH" run mdb_purge_inherited_containers "$MDB_SOURCE_FP_FILE"
    [ "$status" -eq 0 ]
    [ "$(cat "$T/removed")" = "aaa" ]
    # dockerd owns its state: the online path never deletes directories.
    [ -d "$MDB_DOCKER_CONTAINERS_DIR/aaa" ]
}

@test "purge: dockerd running but never active -> non-zero, nothing edited" {
    make_container aaa mdb_gluetun "WIREGUARD_PRIVATE_KEY=${WG_SRC}"
    stub systemctl 'exit 3'
    stub pgrep 'exit 0'
    MDB_DOCKERD_WAIT_S=0 PATH="$STUBS:$PATH" run mdb_purge_inherited_containers "$MDB_SOURCE_FP_FILE"
    [ "$status" -ne 0 ]
    [ -d "$MDB_DOCKER_CONTAINERS_DIR/aaa" ]
}

@test "purge: no docker at all is a clean no-op" {
    rm -rf "$MDB_DOCKER_CONTAINERS_DIR"
    stub systemctl 'exit 3'
    stub pgrep 'exit 1'
    # A PATH with the stubs and the basics only, so a Mac's own docker CLI
    # (Docker Desktop) is not found.
    PATH="$STUBS:/usr/bin:/bin" run mdb_purge_inherited_containers "$MDB_SOURCE_FP_FILE"
    [ "$status" -eq 0 ]
}

# ---------------------------------------------------------------------------
# first_boot.sh Step 1c
# ---------------------------------------------------------------------------

@test "first_boot: Step 1c (wipe .env + purge) runs BEFORE the Step 2 expand" {
    f="$GI_DIR/first_boot.sh"
    s1c="$(grep -n '^if revoke_source_vpn_identity; then' "$f" | cut -d: -f1)"
    s2="$(grep -n 'log "\[2/7\] Expanding root filesystem' "$f" | cut -d: -f1)"
    s6="$(grep -n 'log "\[6/7\] Cleaning Media Browser' "$f" | cut -d: -f1)"
    [ -n "$s1c" ]
    [ "$s1c" -lt "$s2" ]
    [ "$s1c" -lt "$s6" ]
}

# Runs the real Step 1c text under first_boot's own shell options and ERR trap,
# against a scratch install dir: a failing purge must be LOGGED, not abort.
run_step_1c() {
    local f="$GI_DIR/first_boot.sh" body
    body="$(sed -n '/^revoke_source_vpn_identity() {/,/^fi$/p' "$f")"
    [ -n "$body" ] || { echo "Step 1c not found"; return 1; }
    cat > "$T/step1c.sh" <<EOF
set -euo pipefail
trap 'echo "=== FAILED at line \$LINENO"' ERR
log() { echo "\$1"; }
INSTALL_DIR="$T/opt"
SOURCE_SECRETS_LIB="${1:-$LIB}"
$body
echo "REACHED STEP 2"
EOF
    PATH="$STUBS:$PATH" bash "$T/step1c.sh"
}

@test "first_boot Step 1c: wipes .env and the inherited containers" {
    record_source
    become_clone_board
    mkdir -p "$T/opt/services"
    cp "$T/source.env" "$T/opt/services/.env"
    make_container aaa mdb_gluetun "WIREGUARD_PRIVATE_KEY=${WG_SRC}"
    stub systemctl 'exit 3'
    stub pgrep 'exit 1'
    run run_step_1c
    [ "$status" -eq 0 ]
    [ ! -e "$T/opt/services/.env" ]
    [ ! -e "$MDB_DOCKER_CONTAINERS_DIR/aaa" ]
    assert_has "Source VPN identity revoked"
    assert_has "REACHED STEP 2"
    refute_has "FAILED"
    refute_has "$WG_SRC"
}

@test "first_boot Step 1c: a failure is loud but NEVER aborts first boot" {
    mkdir -p "$T/opt/services"
    cp "$T/source.env" "$T/opt/services/.env"
    make_container aaa mdb_gluetun "WIREGUARD_PRIVATE_KEY=${WG_SRC}"
    stub systemctl 'exit 3'
    stub pgrep 'exit 0'
    export MDB_DOCKERD_WAIT_S=0
    run run_step_1c
    [ "$status" -eq 0 ]
    [ ! -e "$T/opt/services/.env" ]
    assert_has "could NOT fully remove the source box's VPN identity"
    assert_has "REACHED STEP 2"
    refute_has "=== FAILED"
    # And with the library missing altogether: .env still goes.
    cp "$T/source.env" "$T/opt/services/.env"
    run run_step_1c "$T/no-such-lib.sh"
    [ "$status" -eq 0 ]
    [ ! -e "$T/opt/services/.env" ]
    assert_has "REACHED STEP 2"
}

# ---------------------------------------------------------------------------
# verify_box.sh
# ---------------------------------------------------------------------------

# Runs verify_box.sh's real clone-source block (from FP_LIB= to its closing
# fi) with pass/fail/warn captured, against a scratch box.
run_verify_block() {
    local body
    body="$(sed -n '/^FP_LIB="\$(cd/,/^fi$/p' "$VERIFY")"
    [ -n "$body" ] || { echo "verify block not found"; return 1; }
    mkdir -p "$T/box/scripts/golden_image" "$T/box/services" "$T/box/app/data"
    cp "$LIB" "$T/box/scripts/golden_image/source_secrets_lib.sh"
    cat > "$T/verify.sh" <<EOF
set -uo pipefail
pass() { echo "PASS \$1"; }
fail() { echo "FAIL \$1"; }
warn() { echo "WARN \$1"; }
BASE="$T/box"
APP="$T/box/app"
DATA="$T/box/app/data"
$body
EOF
    PATH="$STUBS:$PATH" bash "$T/verify.sh"
}

docker_stub_from_dir() {
    stub sudo 'shift; exec "$@"'
    stub docker "
D=\"$MDB_DOCKER_CONTAINERS_DIR\"
case \"\$1\" in
  ps) [ -d \"\$D\" ] && ls \"\$D\"; exit 0 ;;
  inspect)
    if [[ \"\$3\" == '{{.Name}}' ]]; then
      python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[\"Name\"])' \"\$D/\$4/config.v2.json\"
    else
      python3 -c 'import json,sys; [print(e) for e in json.load(open(sys.argv[1]))[\"Config\"][\"Env\"]]' \"\$D/\$4/config.v2.json\"
    fi ;;
esac"
}

@test "verify_box: FAILs a clone whose .env holds the source's VPN key — no value printed" {
    record_source
    become_clone_board
    docker_stub_from_dir
    run run_verify_block
    # .env first: copy it in after run_verify_block has created the box tree.
    cp "$T/source.env" "$T/box/services/.env"
    run run_verify_block
    assert_has "FAIL this unit is using the SOURCE box's WireGuard VPN key (services/.env)"
    assert_has "FAIL this unit carries the SOURCE box's per-box secret(s): env:"
    assert_has "Reset Media Browser"
    refute_has "$WG_SRC"
    refute_has "$RADARR_SRC"
    refute_has "$QBIT_SRC"
}

@test "verify_box: FAILs a clone whose STOPPED container holds the key" {
    record_source
    become_clone_board
    docker_stub_from_dir
    make_container aaa mdb_gluetun "WIREGUARD_PRIVATE_KEY=${WG_SRC}"
    run run_verify_block
    assert_has "FAIL this unit is using the SOURCE box's WireGuard VPN key (container mdb_gluetun)"
    refute_has "$WG_SRC"
}

@test "verify_box: FAILs a clone still holding the source's Flask secret" {
    record_source
    become_clone_board
    docker_stub_from_dir
    run run_verify_block
    cp "$T/flask_secret.key" "$T/box/app/data/flask_secret.key"
    run run_verify_block
    assert_has "FAIL this unit carries the SOURCE box's per-box secret(s): flask-secret"
    refute_has "$FLASK_SRC"
}

@test "verify_box: PASSes a clone provisioned with its OWN key" {
    record_source
    become_clone_board
    docker_stub_from_dir
    make_container aaa mdb_gluetun "WIREGUARD_PRIVATE_KEY=${WG_OWN}"
    run run_verify_block
    printf 'WIREGUARD_PRIVATE_KEY=%s\nRADARR_API_KEY=ffffffffffffffffffffffffffffffff\n' "$WG_OWN" > "$T/box/services/.env"
    run run_verify_block
    assert_has "PASS no clone-source secrets in use (5 source fingerprint(s) checked"
    refute_has "FAIL"
}

@test "verify_box: the SOURCE box never flags itself (leftover file, same board)" {
    record_source
    docker_stub_from_dir
    make_container aaa mdb_gluetun "WIREGUARD_PRIVATE_KEY=${WG_SRC}"
    run run_verify_block
    cp "$T/source.env" "$T/box/services/.env"
    run run_verify_block
    assert_has "PASS this board is the clone SOURCE"
    refute_has "FAIL"
}

@test "verify_box: no fingerprint file (source after restore / old unit) passes" {
    docker_stub_from_dir
    run run_verify_block
    assert_has "PASS no clone-source fingerprints"
    refute_has "FAIL"
}

@test "verify_box: docker not accessible -> WARN, not a silent pass" {
    record_source
    become_clone_board
    stub sudo 'exit 1'
    stub docker 'exit 1'
    run run_verify_block
    assert_has "WARN no clone-source secret found, but NOT checked: Docker containers"
}

# ---------------------------------------------------------------------------
# Auditing an existing image
# ---------------------------------------------------------------------------

@test "scan --pi harvest keeps WIREGUARD_PRIVATE_KEY (not on the non-secret skip list)" {
    skip_line="$(grep -E 'grep -vE "\^\(PUID' "$GI_DIR/scan_image_for_secrets.sh")"
    [ -n "$skip_line" ]
    output="$skip_line"
    refute_has "WIREGUARD_PRIVATE_KEY"
}

make_image() {   # OUT.img.gz [with-key]
    python3 - "$1" "${2:-}" "$WG_SRC" "$RADARR_SRC" <<'PY'
import gzip, os, sys
out, with_key, wg, radarr = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
blob = bytearray(os.urandom(48 * 1024))
cfg = '{"Name":"/mdb_gluetun","Config":{"Env":["VPN_TYPE=wireguard","WIREGUARD_PRIVATE_KEY=%s"]}}' % (wg if with_key else "")
blob[20000:20000] = cfg.encode()
blob[30000:30000] = ("RADARR_API_KEY=%s" % radarr).encode()
with gzip.open(out, "wb") as fh:
    fh.write(bytes(blob))
PY
}

@test "scan --vpn-key: finds the WG key inside a Docker config.v2.json, never prints it" {
    make_image "$T/old.img.gz" with-key
    printf 'env:WIREGUARD_PRIVATE_KEY\t%s\nenv:RADARR_API_KEY\t%s\n' "$WG_SRC" "$RADARR_SRC" > "$T/needles"
    run bash "$GI_DIR/scan_image_for_secrets.sh" --image "$T/old.img.gz" --needles "$T/needles" --vpn-key
    [ "$status" -eq 1 ]
    assert_has "LEAK: env:WIREGUARD_PRIVATE_KEY (44 chars) appears 1 time(s)"
    refute_has "RADARR_API_KEY"
    refute_has "$WG_SRC"
}

@test "scan --vpn-key: an image without the key is CLEAN even if other secrets are present" {
    make_image "$T/new.img.gz"
    printf 'env:WIREGUARD_PRIVATE_KEY\t%s\nenv:RADARR_API_KEY\t%s\n' "$WG_SRC" "$RADARR_SRC" > "$T/needles"
    run bash "$GI_DIR/scan_image_for_secrets.sh" --image "$T/new.img.gz" --needles "$T/needles" --vpn-key
    [ "$status" -eq 0 ]
    assert_has "CLEAN"
}

@test "scan --vpn-key: no WG needle to look for is NOT a pass" {
    make_image "$T/new.img.gz"
    printf 'env:RADARR_API_KEY\t%s\n' "$RADARR_SRC" > "$T/needles"
    run bash "$GI_DIR/scan_image_for_secrets.sh" --image "$T/new.img.gz" --needles "$T/needles" --vpn-key
    [ "$status" -eq 2 ]
}

# ---------------------------------------------------------------------------
# Lint
# ---------------------------------------------------------------------------

@test "shellcheck clean: the library and every script it touches" {
    command -v shellcheck >/dev/null 2>&1 || skip "shellcheck not installed"
    run shellcheck -S warning "$LIB" "$GI_DIR/first_boot.sh" "$GI_DIR/prepare_for_cloning.sh" \
        "$GI_DIR/restore_after_cloning.sh" "$GI_DIR/scan_image_for_secrets.sh" "$VERIFY"
    [ "$status" -eq 0 ]
}
