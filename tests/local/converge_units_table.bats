#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# converge_system_units.sh's POLICY table is the one place a unit's
# delivery is decided. Every unit the tree ships needs a row (no unit
# ships without someone choosing its policy) and every row a unit. Origin:
# missing_search/'s units lived outside systemd/, the only dir the old
# install blocks read, and were written + documented but never installed.

setup() {
    # shellcheck disable=SC1091
    source "$CPP_DIR/scripts/converge_system_units.sh"
    set +u +o pipefail
    TREE="$TESTS_REPO_ROOT"
}

@test "every unit file in the repo lives in a converged directory" {
    while IFS= read -r f; do
        d="$(dirname "$f")"
        [[ " $UNIT_DIRS " == *" $d "* ]] || { echo "unit outside UNIT_DIRS: $f"; return 1; }
    done < <(cd "$TESTS_REPO_ROOT" && git ls-files '*.service' '*.timer')
}

@test "every in-tree unit has a policy row" {
    while IFS=$'\t' read -r name _; do
        policy_names | grep -qxF "$name" || { echo "no POLICY row: $name"; return 1; }
    done < <(tree_units)
}

@test "every policy row names exactly one unit file in git" {
    # Counted from git, not tree_units: tree_units resolves a name shipped in
    # two directories to one row (deploy staging copies on a real box), but two
    # tracked files with one name are still a bug.
    names="$(cd "$TESTS_REPO_ROOT" && git ls-files '*.service' '*.timer' | sed 's#.*/##')"
    while IFS= read -r name; do
        [ "$(printf '%s\n' "$names" | grep -cxF "$name")" -eq 1 ] || { echo "row without exactly one file in git: $name"; return 1; }
    done < <(policy_names)
}

@test "rows are well-formed" {
    while read -r name gate absent change dropin extra; do
        [ -n "$name" ] || continue
        [ -z "$extra" ] || { echo "too many columns: $name"; return 1; }
        [[ "$gate" =~ ^(base|mb)$ ]] || { echo "bad gate: $name"; return 1; }
        [[ "$absent" =~ ^(skip|install|enable|enable-now)$ ]] || { echo "bad if-absent: $name"; return 1; }
        [[ "$change" =~ ^(none|try-restart|rearm)$ ]] || { echo "bad on-change: $name"; return 1; }
        [[ "$dropin" == "-" || "$dropin" == *.conf ]] || { echo "bad drop-in: $name"; return 1; }
    done <<< "$POLICY"
}

@test "every timer re-arms and only timers re-arm" {
    while read -r name _ _ change _; do
        [ -n "$name" ] || continue
        case "$name" in
            *.timer)   [ "$change" = rearm ] || { echo "timer not rearm: $name"; return 1; } ;;
            *)         [ "$change" != rearm ] || { echo "non-timer rearm: $name"; return 1; } ;;
        esac
    done <<< "$POLICY"
}

@test "the update machinery never restarts on change" {
    # Pin the set itself: the loop below walks the script's own PROTECTED, so
    # an emptied or shrunken list would pass it vacuously.
    [ "$(printf '%s\n' $PROTECTED | sort | tr '\n' ' ')" = \
      "magic-dingus-box-cpp.service magic-dingus-ota-recovery.service magic-dingus-web.service " ] \
        || { echo "PROTECTED is: $PROTECTED"; return 1; }
    for u in $PROTECTED; do
        read -r _ _ change _ <<< "$(unit_policy "$u")"
        [ "$change" = none ] || { echo "$u: $change"; return 1; }
    done
}

@test "a unit run by a /usr/local/bin helper is never installed when absent" {
    # Only setup_services.sh creates those helpers (update.sh refreshes but
    # never creates them): installing the unit alone arms a unit that fails.
    while IFS=$'\t' read -r name path; do
        read -r _ absent _ <<< "$(unit_policy "$name")"
        if grep -qE '^ExecStart=(-)?/usr/local/bin/' "$path"; then
            [ "$absent" = skip ] || { echo "$name installs without its helper"; return 1; }
        fi
    done < <(tree_units)
}

@test "each retired drop-in's setting is carried by its tree unit" {
    u="$CPP_DIR/systemd"
    grep -qx "TimeoutStopSec=20" "$u/magic-dingus-box-cpp.service"
    grep -qx "TimeoutStartSec=600" "$u/magic-dingus-storage-attach.service"
    grep -qx "TimeoutStartSec=300" "$u/magic-dingus-smoke-test.service"
    t="$CPP_DIR/scripts/missing_search/magic-dingus-missing-search.timer"
    grep -qx "OnBootSec=11min" "$t"
    grep -qx "OnUnitActiveSec=4h" "$t"
    [ "$(printf '%s\n' "$POLICY" | awk '$5 != "-" && NF { print $1 "/" $5 }' | sort)" = "$(printf '%s\n' \
        magic-dingus-box-cpp.service/stop-timeout.conf \
        magic-dingus-missing-search.timer/mdb-boot-delay.conf \
        magic-dingus-smoke-test.service/mdb-timeout.conf \
        magic-dingus-storage-attach.service/mdb-timeout.conf | sort)" ]
}

@test "every delivery path reaches the converge through setup_memory_tuning.sh" {
    grep -q 'converge_system_units.sh' "$CPP_DIR/scripts/setup_memory_tuning.sh"
    grep -q 'scripts/setup_memory_tuning.sh' "$CPP_DIR/scripts/update.sh"
    grep -q 'scripts/setup_memory_tuning.sh' "$CPP_DIR/scripts/deploy_cpp.sh"
    grep -q 'scripts/setup_memory_tuning.sh' "$TESTS_REPO_ROOT/scripts/golden_image/first_boot.sh"
    grep -q 'scripts/setup_memory_tuning.sh' "$TESTS_REPO_ROOT/scripts/golden_image/sync_source_box.sh"
}

@test "sync_source_box pushes the converge script and the units, and gates the image on --check" {
    f="$TESTS_REPO_ROOT/scripts/golden_image/sync_source_box.sh"
    grep -q 'push magic_dingus_box_cpp/scripts/converge_system_units.sh' "$f"
    grep -q 'converge_system_units.sh --check' "$f"
    push_line="$(grep -n 'push magic_dingus_box_cpp/scripts/converge_system_units.sh' "$f" | cut -d: -f1)"
    run_line="$(grep -n '"sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_memory_tuning.sh"' "$f" | head -1 | cut -d: -f1)"
    check_line="$(grep -n 'converge_system_units.sh --check' "$f" | cut -d: -f1)"
    [ "$push_line" -lt "$run_line" ]
    [ "$run_line" -lt "$check_line" ]
}

@test "push() passes -n to every ssh, so a caller looping over stdin is not drained" {
    f="$TESTS_REPO_ROOT/scripts/golden_image/sync_source_box.sh"
    body="$(sed -n '/^push() {/,/^}/p' "$f")"
    ssh_lines="$(printf '%s\n' "$body" | grep -F 'ssh "${SSH_OPTS[@]}"')"
    [ "$(printf '%s\n' "$ssh_lines" | grep -c .)" -ge 2 ] || { echo "no ssh calls found in push()"; return 1; }
    bad="$(printf '%s\n' "$ssh_lines" | grep -vE ' -n( |$)' || true)"
    [ -z "$bad" ] || { echo "ssh without -n in push(): $bad"; return 1; }
}

@test "sync_source_box loops the repo's units through push()" {
    f="$TESTS_REPO_ROOT/scripts/golden_image/sync_source_box.sh"
    grep -qF "git ls-files '*.service' '*.timer'" "$f"
    grep -qF 'while IFS= read -r unit_file; do' "$f"
}

@test "every tracked unit file's name is one the converge accepts (UNIT_NAME_RE)" {
    # tree_units skips a name that fails UNIT_NAME_RE with only a stderr
    # note, so a unit named e.g. '-foo.service' would ship and never install.
    n=0
    while IFS= read -r f; do
        n=$((n + 1))
        [[ "${f##*/}" =~ $UNIT_NAME_RE ]] || { echo "not a well-formed unit name: $f"; return 1; }
    done < <(cd "$TESTS_REPO_ROOT" && git ls-files '*.service' '*.timer')
    [ "$n" -gt 20 ] || { echo "only $n unit files found"; return 1; }
}

# unit_file_writes <file>: its logical lines (backslash continuations joined,
# comment lines dropped, drop-in paths removed) that write a unit FILE into
# /etc/systemd/system — with cp/install/mv/ln/tee/rsync or a redirect on the
# line, or by assigning such a path to a variable (the setup_*.sh scripts
# used to `install "$SRC" "$DEST"`).
unit_file_writes() {
    awk '
        { line = (pending == "" ? $0 : pending " " $0) }
        /\\$/ { sub(/\\$/, "", line); pending = line; next }
        { pending = ""; if (line !~ /^[[:space:]]*#/) print FNR ": " line }
    ' "$1" \
    | sed -E "s#etc/systemd/system/[^ \"']*\\.d([/\"' ]|\$)[^ \"']*##g" \
    | grep -E 'etc/systemd/system(/[^ "'"'"'}]|/?([ "'"'"']|$))' \
    | grep -E '(^|[;&|( [:space:]])(cp|install|mv|ln|tee|rsync)[[:space:]]|>[[:space:]]*"?[^ ]*etc/systemd/system|[A-Za-z_][A-Za-z0-9_]*="?[^ ]*etc/systemd/system/' \
    || true
}

@test "converge_system_units.sh is the only script that writes unit files" {
    # Nine scripts used to copy units into /etc/systemd/system, each for its
    # own subset and only when it happened to run; they now call
    # `converge_system_units.sh --only <unit>` and keep their own
    # enable/start. Drop-ins (<unit>.d/) and `rm` are not unit-file writes.
    # Read-only: init_audio.sh's AUDIO_UNIT_FILE (it only tests the file).
    local found="" f hits
    while IFS= read -r f; do
        case "$f" in
            magic_dingus_box_cpp/scripts/converge_system_units.sh) continue ;;
            tests/*|*/tests/*) continue ;;
        esac
        hits="$(unit_file_writes "$TESTS_REPO_ROOT/$f")"
        if [[ "$f" == magic_dingus_box_cpp/scripts/init_audio.sh ]]; then
            hits="$(printf '%s\n' "$hits" | grep -v 'AUDIO_UNIT_FILE=' || true)"
        fi
        [[ -z "$hits" ]] || found="${found}${f}:"$'\n'"${hits}"$'\n'
    done < <(cd "$TESTS_REPO_ROOT" && git ls-files '*.sh')
    [ -z "$found" ] || { printf 'unit files written outside the converge:\n%s' "$found"; return 1; }
}

@test "the unit-file write detector catches the forms it claims to" {
    f="$BATS_TEST_TMPDIR/w.sh"
    printf '%s\n' \
        'sudo cp a.service /etc/systemd/system/' \
        'sudo install -m 0644 \' '    "$S/x.timer" \' '    /etc/systemd/system/x.timer' \
        'DEST="${ROOT}/etc/systemd/system/${UNIT}"' \
        'echo "[Unit]" > /etc/systemd/system/y.service' \
        '# cp a.service /etc/systemd/system/ (a comment)' \
        'install -m 0644 "$t" /etc/systemd/system/k.service.d/a.conf' \
        'DROPIN_DIR="${ROOT}/etc/systemd/system/${KIOSK_UNIT}.d"' \
        'rm -f /etc/systemd/system/old.service' \
        'if [ -f /etc/systemd/system/z.service ]; then :; fi' \
        'local sd_dir="${MAGIC_SYSTEMD_DIR:-/etc/systemd/system}"' > "$f"
    hits="$(unit_file_writes "$f")"
    [ "$(printf '%s\n' "$hits" | cut -d: -f1 | tr '\n' ' ')" = "1 4 5 6 " ] \
        || { printf 'detector hits:\n%s\n' "$hits"; return 1; }
}
