#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# verify_box.sh's "unexpected errors this boot" check counts the kiosk
# unit's err-priority journal lines. journalctl prints "-- No entries --"
# when nothing matches (unless -q), and that line used to be counted: a
# box with zero errors read "1 non-ALSA error line(s)" (magicpi5,
# 2026-10-06). The block (ERRS= through the warn line) is extracted and
# run against a stub journalctl that behaves like the real one.

setup() {
    BLOCK=$(sed -n '/^ERRS=\$(journalctl/,/non-ALSA error line/p' "$CPP_DIR/scripts/verify_box.sh")
    [ -n "$BLOCK" ] || { echo "error-line block not found in verify_box.sh"; return 1; }
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    cat > "$BATS_TEST_TMPDIR/bin/journalctl" <<'STUB'
#!/bin/bash
quiet=false
for a in "$@"; do [ "$a" = "-q" ] || [ "$a" = "--quiet" ] && quiet=true; done
if [ -s "$STUB_JOURNAL" ]; then cat "$STUB_JOURNAL"
elif ! $quiet; then echo "-- No entries --"; fi
STUB
    chmod +x "$BATS_TEST_TMPDIR/bin/journalctl"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
    export STUB_JOURNAL="$BATS_TEST_TMPDIR/journal"
    : > "$STUB_JOURNAL"
}

run_block() {
    run bash -c '
        pass() { echo "[PASS] $1"; }
        warn() { echo "[WARN] $1"; }
        UNIT=magic-dingus-box-cpp.service
        eval "$1"
    ' _ "$BLOCK"
}

@test "no error entries -> PASS, not '1 error line'" {
    run_block
    [ "$output" = "[PASS] no unexpected errors this boot" ]
}

@test "ALSA/PulseAudio noise alone -> PASS" {
    printf '%s\n' "Oct 06 kiosk: ALSA lib pcm.c: underrun" \
        "Oct 06 kiosk: pulseaudio: connection refused" > "$STUB_JOURNAL"
    run_block
    [ "$output" = "[PASS] no unexpected errors this boot" ]
}

@test "real error lines -> WARN with the count" {
    printf '%s\n' "Oct 06 kiosk: ALSA lib noise" "Oct 06 kiosk: CRITICAL thing" \
        "Oct 06 kiosk: another failure" > "$STUB_JOURNAL"
    run_block
    [[ "$output" == "[WARN] 2 non-ALSA error line(s)"* ]]
}
