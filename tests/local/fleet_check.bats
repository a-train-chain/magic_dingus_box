#!/usr/bin/env bats
# Off-Pi tests for scripts/fleet_check.sh (Mac-side fleet acceptance run).
# ssh is replaced by a shim (FLEET_SSH) that, per host, either fails like an
# unreachable box, replays canned box output, or EXECUTES the real remote
# script fleet_check sends against a fake install tree — so the box-side
# half is tested too, with sudo unavailable and a fake verify_box.sh.
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

FLEET="$TESTS_REPO_ROOT/scripts/fleet_check.sh"

setup() {
    F="$BATS_TEST_TMPDIR/fake"
    mkdir -p "$F/bin" "$F/box/magic_dingus_box_cpp/scripts" "$F/box/config"
    export FAKE_DIR="$F"
    export FLEET_SSH="$F/ssh"
    export FLEET_BOX_BASE="$F/box"

    cat > "$F/ssh" <<'SHIM'
#!/usr/bin/env bash
# Fake ssh: [-o opt]... host bash -s -- WS T BASE  (remote script on stdin)
args=("$@"); i=0
while [ "${args[$i]}" = "-o" ]; do i=$((i + 2)); done
host="${args[$i]}"
rest=("${args[@]:$((i + 1))}")          # bash -s -- WS T BASE
while [ "${#rest[@]}" -gt 0 ] && [ "${rest[0]}" != "--" ]; do rest=("${rest[@]:1}"); done
echo "$*" >> "$FAKE_DIR/ssh_calls.log"
script="$(cat)"
printf '%s\n' "$script" > "$FAKE_DIR/remote_script.sh"
# concurrency probe
mkdir "$FAKE_DIR/running.$$"
n=$(ls -d "$FAKE_DIR"/running.* | wc -l | tr -d ' ')
echo "$n" >> "$FAKE_DIR/concurrency.log"
sleep "${FAKE_SLEEP:-0}"
rmdir "$FAKE_DIR/running.$$"
mode="$(cat "$FAKE_DIR/$host.mode" 2>/dev/null || echo exec)"
case "$mode" in
    unreachable) echo "ssh: connect to host $host port 22: Operation timed out" >&2; exit 255 ;;
    canned) cat "$FAKE_DIR/$host.out"; exit 0 ;;
    exec) printf '%s\n' "$script" | PATH="$FAKE_DIR/bin:$PATH" bash -s "${rest[@]}" ;;   # rest starts with --
esac
SHIM
    # Box-side stand-ins: no passwordless sudo here; timeout just runs it.
    printf '#!/bin/sh\nexit 1\n' > "$F/bin/sudo"
    printf '#!/bin/sh\necho fakebox\n' > "$F/bin/hostname"
    cat > "$F/bin/timeout" <<'T'
#!/usr/bin/env bash
while [[ "$1" == --* ]]; do shift; done
shift   # the duration
exec "$@"
T
    cat > "$F/box/magic_dingus_box_cpp/scripts/verify_box.sh" <<'V'
#!/usr/bin/env bash
echo "args:$*" >> "$FAKE_DIR/verify_args.log"
printf '\n== Platform ==\n  [PASS] board ok\n'
printf '\n== Content ==\n  [WARN] no Dreamcast BIOS\n'
if [ -n "${FAKE_FAILS:-}" ]; then
    printf '  [FAIL] first broken thing\n  [FAIL] second broken thing\n'
    printf '\n== RESULT ==\n  1 passed, 2 failed, 1 warnings\n  NOT SHIPPABLE\n'; exit 1
fi
printf '\n== RESULT ==\n  1 passed, 0 failed, 1 warnings\n  SHIPPABLE\n'; exit 0
V
    chmod +x "$F/ssh" "$F/bin/sudo" "$F/bin/hostname" "$F/bin/timeout" "$F/box/magic_dingus_box_cpp/scripts/verify_box.sh"
    echo "1.10.0" > "$F/box/VERSION"
}

canned() {  # canned <host> <verify_exit> [body lines...]
    local h="$1" rc="$2"; shift 2
    echo canned > "$F/$h.mode"
    {
        echo "@@host=boxname-$rc"
        echo "@@model=Raspberry Pi 4 Model B Rev 1.5"
        echo "@@version=1.9.14"
        echo "@@channel=stable"
        echo "@@uptime=3 days, 4 hours, 12 minutes"
        echo "@@verify_mode=sudo"
        echo "@@verify_begin"
        printf '%s\n' "$@"
        echo "@@verify_end"
        echo "@@verify_exit=$rc"
    } > "$F/$h.out"
}

@test "usage errors exit 2: no hosts, unknown option, bad -j" {
    run "$FLEET"
    [ "$status" -eq 2 ]
    run "$FLEET" --bogus magic@a
    [ "$status" -eq 2 ]
    run "$FLEET" -j 0 magic@a
    [ "$status" -eq 2 ]
    run "$FLEET" --hosts "$F/does-not-exist"
    [ "$status" -eq 2 ]
}

@test "hosts that could be read as ssh options or shell are refused" {
    run "$FLEET" -- "-oProxyCommand=touch$F/pwned"
    [ "$status" -eq 2 ]
    run "$FLEET" 'magic@a;reboot'
    [ "$status" -eq 2 ]
    run "$FLEET" 'magic@$(id)'
    [ "$status" -eq 2 ]
    [ ! -e "$F/ssh_calls.log" ]
}

@test "all shippable: exit 0, one row per host, version + channel + board" {
    run "$FLEET" magic@box-a magic@box-b
    echo "$output"
    [ "$status" -eq 0 ]
    [ "$(grep -c 'SHIPPABLE' <<< "$output")" -eq 2 ]
    grep -q '^magic@box-a .*1\.10\.0 *stable .*1/1/0 *SHIPPABLE' <<< "$output"
    grep -q '2 host(s): 2 shippable, 0 not shippable/error, 0 unreachable' <<< "$output"
}

@test "box side really ran: sudo unavailable falls back to the user run, without --with-services" {
    run "$FLEET" magic@box-a
    [ "$status" -eq 0 ]
    [ "$(cat "$F/verify_args.log")" = "args:" ]
    grep -q -- '-o BatchMode=yes' "$F/ssh_calls.log"
    grep -q -- '-o ConnectTimeout=10' "$F/ssh_calls.log"
    grep -q -- "magic@box-a bash -s -- 0 180 $F/box\$" "$F/ssh_calls.log"
}

@test "--with-services is passed through, with the 600 s deadline" {
    run "$FLEET" --with-services --connect-timeout 3 magic@box-a
    [ "$status" -eq 0 ]
    [ "$(cat "$F/verify_args.log")" = "args:--with-services" ]
    grep -q -- '-o ConnectTimeout=3' "$F/ssh_calls.log"
    grep -q -- "bash -s -- 1 600 $F/box\$" "$F/ssh_calls.log"
}

@test "a failing box: exit 1, counts from RESULT, FIRST failing check only" {
    FAKE_FAILS=1 run "$FLEET" magic@box-a
    echo "$output"
    [ "$status" -eq 1 ]
    grep -q '1/1/2 *NOT-SHIPPABLE *first broken thing$' <<< "$output"
    ! grep -q 'second broken thing' <<< "$output" || false
}

@test "beta channel file is reported as beta (same rule as update.sh)" {
    printf ' beta \n' > "$F/box/config/update_channel"
    run "$FLEET" magic@box-a
    grep -q ' beta ' <<< "$output"
    printf 'Beta\n' > "$F/box/config/update_channel"
    run "$FLEET" magic@box-a
    grep -q ' stable ' <<< "$output"
}

@test "no verify_box.sh on the box: NO-VERIFY, exit 1" {
    rm "$F/box/magic_dingus_box_cpp/scripts/verify_box.sh"
    run "$FLEET" magic@box-a
    [ "$status" -eq 1 ]
    grep -q 'NO-VERIFY *verify_box.sh not installed' <<< "$output"
}

@test "unreachable host: UNREACHABLE with ssh's reason, others still checked, exit 1" {
    echo unreachable > "$F/magic@gone.mode"
    run "$FLEET" magic@gone magic@box-a
    echo "$output"
    [ "$status" -eq 1 ]
    grep -q '^magic@gone .*UNREACHABLE *ssh: connect to host magic@gone port 22: Operation timed out' <<< "$output"
    grep -q '^magic@box-a .*SHIPPABLE' <<< "$output"
    grep -q '1 shippable, 0 not shippable/error, 1 unreachable' <<< "$output"
}

@test "canned Pi 4 box: board shortened, uptime compacted, hostname shown for an IP" {
    canned magic@10.0.0.9 0 '  [PASS] a' '== RESULT ==' '  1 passed, 0 failed, 0 warnings'
    run "$FLEET" magic@10.0.0.9
    echo "$output"
    [ "$status" -eq 0 ]
    grep -q '^magic@10\.0\.0\.9 (boxname-0) *Pi 4 *1\.9\.14 *stable *3d 4h 12m *1/0/0 *SHIPPABLE' <<< "$output"
}

@test "verify_box.sh hit the deadline: TIMEOUT, counts from what printed" {
    canned magic@slow 124 '  [PASS] a' '  [PASS] b' '  [WARN] c'
    run "$FLEET" magic@slow
    echo "$output"
    [ "$status" -eq 1 ]
    grep -q '2/1/0 *TIMEOUT *verify_box.sh exceeded 180s' <<< "$output"
}

@test "connection dropped mid-run (no verify_exit): ERROR" {
    echo canned > "$F/magic@drop.mode"
    printf '@@host=x\n@@verify_begin\n  [PASS] a\n' > "$F/magic@drop.out"
    run "$FLEET" magic@drop
    [ "$status" -eq 1 ]
    grep -q 'ERROR *connection dropped' <<< "$output"
}

@test "--hosts file: comments, blanks and duplicates; merged with positional hosts" {
    printf '# fleet\nmagic@box-a   # owner box\n\n  magic@box-b\nmagic@box-a\n' > "$F/fleet.txt"
    run "$FLEET" --hosts "$F/fleet.txt" magic@box-c magic@box-b
    echo "$output"
    [ "$status" -eq 0 ]
    [ "$(wc -l < "$F/ssh_calls.log" | tr -d ' ')" -eq 3 ]
    grep -q '3 host(s): 3 shippable' <<< "$output"
}

@test "-j bounds concurrency" {
    FAKE_SLEEP=0.4 run "$FLEET" -j 2 magic@h1 magic@h2 magic@h3 magic@h4 magic@h5
    [ "$status" -eq 0 ]
    max=$(sort -n "$F/concurrency.log" | tail -1)
    echo "max concurrent: $max"
    [ "$max" -le 2 ]
    [ "$max" -ge 2 ]   # and it did run in parallel
}

@test "--log-dir keeps each host's full output" {
    run "$FLEET" --log-dir "$F/logs" magic@box-a
    [ "$status" -eq 0 ]
    grep -q '^@@verify_exit=0$' "$F/logs/magic_box-a.log"
    grep -q 'no Dreamcast BIOS' "$F/logs/magic_box-a.log"
}

@test "remote script is read-only: no mutating command, sudo only for timeout+verify_box.sh" {
    run "$FLEET" magic@box-a
    [ "$status" -eq 0 ]
    R="$F/remote_code.sh"
    grep -v '^[[:space:]]*#' "$F/remote_script.sh" > "$R"   # code only, no comments
    [ -s "$R" ]
    ! grep -nE '(^|[;&|[:space:]])(rm|mv|cp|install|tee|chmod|chown|ln|mkdir|touch|dd|systemctl|docker|reboot|shutdown|apt|apt-get|pip|kill|pkill)([[:space:]]|$)' "$R" || false
    ! grep -nE '[^0-9&]>[^&]|>>' "$R" | grep -v '2>/dev/null' || false
    ! grep -n 'update\.sh' "$R" || false
    # every sudo use is `sudo -n true` (probe) or the fixed verify command
    [ "$(grep -o 'sudo -n [^ ]*' "$R" | sort -u | tr '\n' ' ')" = "sudo -n /usr/bin/timeout sudo -n true " ]
}
