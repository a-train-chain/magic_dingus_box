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

@test "every policy row names exactly one in-tree unit file" {
    while IFS= read -r name; do
        [ "$(tree_units | cut -f1 | grep -cxF "$name")" -eq 1 ] || { echo "row without exactly one file: $name"; return 1; }
    done < <(policy_names)
}

@test "rows are well-formed" {
    while read -r name gate absent change dropin extra; do
        [ -n "$name" ] || continue
        [ -z "$extra" ] || { echo "too many columns: $name"; return 1; }
        case "$gate" in
            ignore) [ "$absent $change $dropin" = "- - -" ] || { echo "ignore row with policy: $name"; return 1; } ;;
            base|mb)
                [[ "$absent" =~ ^(skip|install|enable|enable-now)$ ]] || { echo "bad if-absent: $name"; return 1; }
                [[ "$change" =~ ^(none|try-restart|rearm)$ ]] || { echo "bad on-change: $name"; return 1; }
                [[ "$dropin" == "-" || "$dropin" == *.conf ]] || { echo "bad drop-in: $name"; return 1; } ;;
            *) echo "bad gate: $name"; return 1 ;;
        esac
    done <<< "$POLICY"
}

@test "only the six legacy units are ignored" {
    [ "$(printf '%s\n' "$POLICY" | awk '$2 == "ignore" { print $1 }' | sort)" = "$(printf '%s\n' \
        magic-mpv-x11.service magic-mpv.hardened.service magic-mpv.service \
        magic-ui-x11.service magic-ui.hardened.service magic-ui.service | sort)" ]
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
    for u in $PROTECTED; do
        read -r _ _ change _ <<< "$(unit_policy "$u")"
        [ "$change" = none ] || { echo "$u: $change"; return 1; }
    done
}

@test "a unit run by a /usr/local/bin helper is never installed when absent" {
    # Only setup_services.sh creates those helpers (update.sh refreshes but
    # never creates them): installing the unit alone arms a unit that fails.
    while IFS=$'\t' read -r name path; do
        read -r gate absent _ <<< "$(unit_policy "$name")"
        [ "$gate" = ignore ] && continue
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
