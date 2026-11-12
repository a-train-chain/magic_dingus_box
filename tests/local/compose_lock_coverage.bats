#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# Every tracked script that starts, stops, restarts, recreates or removes a
# container of the Media Browser stack MUST take the shared compose lock
# (/run/lock/mdb-compose.lock: flock, bounded wait, proceed WITHOUT the lock
# on timeout — never a deadlock). Two unlocked actors on the same containers
# race: the cascade watcher's `compose restart` + `up -d` against a
# Reconfigure's `compose up` dies on "Conflict. The container name ... is
# already in use", and an unlocked `up -d` revives containers a movie or game
# stopped to free RAM on a 2 GB box.
#
# A 2026-11 review found three actors that did not take it (setup_services.sh,
# the web admin's Media Browser reset, magic-dingus-services.service). This
# test keeps the class closed for scripts: a new container command in a
# script that never mentions the lock fails CI.
#
# Convention enforced: the file must REFERENCE the lock path somewhere
# (`mdb-compose.lock`). That proves the author knew about the protocol, not
# that every call site is inside the critical section — the lock is
# deliberately copied into each script (each must stand alone when an older
# updater runs it), so review still owns the span. Comment lines and operator
# message text (echo/log/warn/fail/... "...docker compose up -d...") are not
# commands and are not scanned.
#
# Systemd unit files are out of scope here: magic-dingus-services.service's
# ExecStart/ExecStop run `compose up`/`down` unlocked. Unit edits ride the
# converge mechanism and its release-compatibility rules, so that one is a
# separate decision (see CLAUDE.md "System units converge").

LOCK_NAME="mdb-compose.lock"

# Deliberate exceptions — path relative to the repo root, with WHY.
ALLOWLIST=(
    # Clone tooling, attended, on the SOURCE box: stops the stack (and then
    # dockerd itself) before imaging. Nothing else may run alongside it.
    "scripts/golden_image/prepare_for_cloning.sh"
    "scripts/golden_image/prepare_golden_image.sh"
    # First boot of a clone: ordered Before= the kiosk and before
    # magic-dingus-services, so no playback pause can exist; it is tearing
    # down/deleting inherited containers and deletes services/.env, after
    # which the cascade watcher's convergence skips.
    "scripts/golden_image/first_boot.sh"
    "scripts/golden_image/source_secrets_lib.sh"
    # Runs on the operator's Mac and drives the box over ssh; the lock lives
    # on the box, so an ssh-held flock would be needed. Attended migration of
    # the owner's own box — a known, accepted gap (the cascade watcher could
    # restart a container it paused for its DB copy).
    "scripts/golden_image/migrate_box.sh"
)

# Container-affecting command patterns. Shell: `docker [compose] VERB` (also
# via a wrapper ending in `compose`, e.g. local_compose). Python: a
# subprocess argv list or a docker("VERB", ...) helper.
SH_RE='(docker|compose)([[:space:]]+-[-a-zA-Z]+([[:space:]]+[^[:space:]-][^[:space:]]*)?)*[[:space:]]+(up|restart|rm|stop|start|create|down|kill)([[:space:]]|$|")'
PY_RE='(["'"'"']docker["'"'"'][[:space:]]*,[[:space:]]*(["'"'"']compose["'"'"'][[:space:]]*,[[:space:]]*)?["'"'"'](up|restart|rm|stop|start|create|down|kill)["'"'"']|docker\([[:space:]]*["'"'"'](up|restart|rm|stop|start|create|down|kill)["'"'"'])'
# Lines that are comments or operator-facing message text, not commands.
SKIP_RE='^[0-9]+:[[:space:]]*#|^[0-9]+:[[:space:]]*(echo|printf|log[_a-z]*|warn|fail|pass|info|say|die|err[a-z]*)[[:space:]]|(^|[[:space:];])(warn|fail|log|say|echo)[[:space:]]+"[^"]*docker'

# Prints "path:line:text" for every container-affecting command in $1.
container_commands() {
    local f="$1" re
    case "$f" in
        *.py) re="$PY_RE" ;;
        *)    re="$SH_RE" ;;
    esac
    grep -nE "$re" "$TESTS_REPO_ROOT/$f" | grep -vE "$SKIP_RE" | sed "s|^|$f:|" || true
}

tracked_scripts() {
    git -C "$TESTS_REPO_ROOT" ls-files '*.sh' '*.py' '*.bash' \
        | grep -vE '(^|/)tests?/|(^|/)test_[^/]*\.py$|/dev/pisim/' || true
}

is_allowlisted() {
    local f="$1" a
    for a in "${ALLOWLIST[@]}"; do
        [ "$a" = "$f" ] && return 0
    done
    return 1
}

@test "every script that acts on stack containers references the compose lock" {
    local offenders="" f hits
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        hits=$(container_commands "$f")
        [ -n "$hits" ] || continue
        is_allowlisted "$f" && continue
        if ! grep -q "$LOCK_NAME" "$TESTS_REPO_ROOT/$f"; then
            offenders+="$hits"$'\n'
        fi
    done < <(tracked_scripts)
    if [ -n "$offenders" ]; then
        echo "Container commands in scripts that never take ${LOCK_NAME}:" >&2
        echo "$offenders" >&2
        echo "Take the shared compose lock (copy the compose_lock snippet from" >&2
        echo "recreate_gluetun.sh, or ComposeLock from clear_radarr_cooldowns.py)," >&2
        echo "or add the file to ALLOWLIST with the reason it is exempt." >&2
        false
    fi
}

@test "every allowlisted file still exists and still acts on containers" {
    # A stale allowlist entry silently exempts whatever later lands at that
    # path; drop entries that no longer need it.
    local a
    for a in "${ALLOWLIST[@]}"; do
        [ -f "$TESTS_REPO_ROOT/$a" ] || { echo "allowlisted but missing: $a" >&2; false; }
        [ -n "$(container_commands "$a")" ] \
            || { echo "allowlisted but no container command any more: $a" >&2; false; }
    done
}

@test "sanity: the scan sees the known container actors" {
    # Guard the guard: if the patterns rot and match nothing, the main test
    # passes vacuously. Each of these is known to run container commands.
    local known=(
        magic_dingus_box_cpp/scripts/gluetun_cascade_restart.sh
        magic_dingus_box_cpp/scripts/playback_services_pause.sh
        magic_dingus_box_cpp/scripts/storage_attach.sh
        magic_dingus_box_cpp/scripts/recreate_gluetun.sh
        magic_dingus_box_cpp/scripts/migrate_hardlink_layout.sh
        magic_dingus_box_cpp/scripts/setup_services.sh
        magic_dingus_box_cpp/scripts/clear_radarr_cooldowns.py
        magic_dingus_box/web/admin_media_browser_ops.py
    )
    local f
    for f in "${known[@]}"; do
        [ -n "$(container_commands "$f")" ] \
            || { echo "scan found no container command in $f" >&2; false; }
    done
}

@test "sanity: message text and comments are not mistaken for commands" {
    # These two files only MENTION container commands, in operator messages.
    [ -z "$(container_commands magic_dingus_box_cpp/scripts/verify_box.sh)" ]
    [ -z "$(container_commands magic_dingus_box_cpp/scripts/hw_validate.sh)" ]
}
