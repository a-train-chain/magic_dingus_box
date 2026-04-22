#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

setup() { require_pi; }

# Allowlist of journal substrings that are known-benign and may appear
# even on a healthy Pi. Add to this list (with a justification comment)
# when you confirm a new warning is non-actionable.
ALLOWLIST=(
    'Could not grab rotary device'    # rotary may be open by another process; non-fatal
    'pulseaudio.*Unable to contact D-Bus'  # PulseAudio without D-Bus; expected in service mode
    'X11'                              # no X11 expected; messages mentioning it are info-only
    'Found left-over process'          # systemd housekeeping during service restart
)

@test "no unallowlisted errors in current boot's journal" {
    run pi_ssh 'sudo journalctl -u magic-dingus-box-cpp.service -b --no-pager 2>/dev/null'
    [ "$status" -eq 0 ]

    # Filter the journal output: keep lines that match err/fail/critical and
    # are NOT in the allowlist.
    bad_lines=""
    while IFS= read -r line; do
        echo "$line" | grep -qiE 'error|fail|critical' || continue
        skip_line=0
        for pattern in "${ALLOWLIST[@]}"; do
            if echo "$line" | grep -qE "$pattern"; then
                skip_line=1
                break
            fi
        done
        if [ $skip_line -eq 0 ]; then
            bad_lines="$bad_lines"$'\n'"$line"
        fi
    done <<< "$output"

    if [ -n "$bad_lines" ]; then
        echo "Unallowlisted error lines found:"
        echo "$bad_lines"
        false
    fi
}
