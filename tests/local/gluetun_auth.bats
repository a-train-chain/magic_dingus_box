#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# Gluetun's control-server access file (ensure_gluetun_auth.sh). Current
# gluetun makes every control-server route private by default; the file
# keeps the read-only routes this stack calls public. Two ways it breaks:
#   - a caller uses a route the file does not list -> 401 after the next
#     gluetun bump (the healthcheck's port clause then restart-loops the
#     whole stack every ~10 minutes);
#   - the file lists a route gluetun does not know -> gluetun refuses to
#     start ("route path not supported by the control server").

SCRIPT="$CPP_DIR/scripts/ensure_gluetun_auth.sh"

# gluetun v3.41.3 internal/server/middlewares/auth/settings.go validRoutes.
# Update this list from the new version's source when bumping the pin
# (tests/local/compose_image_pins.bats) — and re-check every route the
# access file names is still in it.
VALID_ROUTES=(
    "GET /openvpn/actions/restart" "GET /openvpn/portforwarded"
    "GET /openvpn/settings" "GET /unbound/actions/restart"
    "GET /updater/restart" "GET /v1/version" "GET /v1/vpn/status"
    "PUT /v1/vpn/status" "GET /v1/vpn/settings" "PUT /v1/vpn/settings"
    "GET /v1/openvpn/status" "PUT /v1/openvpn/status"
    "GET /v1/openvpn/portforwarded" "GET /v1/openvpn/settings"
    "GET /v1/dns/status" "PUT /v1/dns/status" "GET /v1/updater/status"
    "PUT /v1/updater/status" "GET /v1/publicip/ip" "GET /v1/portforward"
    "PUT /v1/portforward"
)

setup() {
    SVC="$BATS_TEST_TMPDIR/services"
    mkdir -p "$SVC"
}

# Routes named in the installed file, one per line.
file_routes() {
    grep -E '^routes = ' "$1" | grep -oE '"[A-Z]+ [^"]+"' | tr -d '"'
}

@test "installs the access file on a Media Browser box" {
    : > "$SVC/.env"
    run bash "$SCRIPT" "$SVC"
    [ "$status" -eq 0 ]
    [ -f "$SVC/config/gluetun/auth/config.toml" ]
    grep -q '^auth = "none"$' "$SVC/config/gluetun/auth/config.toml"
    [[ "$output" == *installed* ]]
}

@test "is idempotent: a second run leaves the file alone" {
    : > "$SVC/.env"
    bash "$SCRIPT" "$SVC"
    before="$(ls -i "$SVC/config/gluetun/auth/config.toml")"
    run bash "$SCRIPT" "$SVC"
    [ "$status" -eq 0 ]
    [[ "$output" == *"up to date"* ]]
    [ "$(ls -i "$SVC/config/gluetun/auth/config.toml")" = "$before" ]
}

@test "rewrites a hand-edited or stale file" {
    mkdir -p "$SVC/config/gluetun/auth"
    echo 'routes = ["GET /v1/vpn/status"]' > "$SVC/config/gluetun/auth/config.toml"
    run bash "$SCRIPT" "$SVC"
    [ "$status" -eq 0 ]
    run file_routes "$SVC/config/gluetun/auth/config.toml"
    [[ "$output" == *"GET /v1/portforward"* ]]
    [[ "$output" != *"/v1/vpn/status"* ]]
}

@test "skips a games-only box unless provisioning" {
    run bash "$SCRIPT" "$SVC"
    [ "$status" -eq 0 ]
    [ ! -e "$SVC/config" ]
    run bash "$SCRIPT" --provision "$SVC"
    [ "$status" -eq 0 ]
    [ -f "$SVC/config/gluetun/auth/config.toml" ]
}

@test "never fails: missing services dir and unwritable dir both exit 0" {
    run bash "$SCRIPT" "$BATS_TEST_TMPDIR/nope"
    [ "$status" -eq 0 ]
    if [ "$(id -u)" -eq 0 ]; then skip "root ignores permissions"; fi
    mkdir -p "$SVC/config/gluetun"
    chmod 0555 "$SVC/config/gluetun"
    run bash "$SCRIPT" "$SVC"
    chmod 0755 "$SVC/config/gluetun"
    [ "$status" -eq 0 ]
    [[ "$output" == *WARNING* ]]
}

@test "every route in the file is one gluetun accepts" {
    bash "$SCRIPT" --provision "$SVC"
    run file_routes "$SVC/config/gluetun/auth/config.toml"
    [ "${#lines[@]}" -ge 2 ]
    for route in "${lines[@]}"; do
        found=false
        for v in "${VALID_ROUTES[@]}"; do
            [ "$route" = "$v" ] && found=true
        done
        $found || { echo "gluetun would refuse to start: $route" >&2; false; }
    done
}

@test "every control-server route the tree calls is listed in the file" {
    bash "$SCRIPT" --provision "$SVC"
    listed="$(file_routes "$SVC/config/gluetun/auth/config.toml")"
    cd "$TESTS_REPO_ROOT"
    run bash -c "git grep -hoE 'localhost:8000/[A-Za-z0-9/_-]+' -- \
        ':!docs' ':!*.md' ':!tests' | sed 's#localhost:8000##' | sort -u"
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -ge 2 ]
    for path in "${lines[@]}"; do
        # Every caller is a read (wget/curl GET).
        grep -qxF "GET $path" <<<"$listed" \
            || { echo "route called but not in the access file: GET $path" >&2; false; }
    done
}

@test "setup_memory_tuning and setup_services both deliver it" {
    grep -q 'ensure_gluetun_auth.sh' "$CPP_DIR/scripts/setup_memory_tuning.sh"
    grep -q 'ensure_gluetun_auth.sh" --provision' "$CPP_DIR/scripts/setup_services.sh"
}
