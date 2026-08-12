#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# Every curl in the provisioning path MUST carry a total time bound
# (--max-time). The failure class this guards against was observed live
# (2026-08-11, Sonarr): a container is up and the port is bound, but the
# app inside is wedged mid-init — it ACCEPTS the socket and never answers.
# --connect-timeout does not cover that state, and a curl without
# --max-time then blocks forever, silently hanging first-boot provisioning
# or an OTA's service reconfigure with nothing in any log to explain it.
#
# v1.9.10 bounded ONE probe at the call site; v1.9.11 bounded the class
# (17 call sites in setup_services.sh). This test keeps it bounded: a new
# curl added without --max-time fails CI instead of shipping the 18th
# unbounded hang.
#
# Convention enforced: the bound must appear on the SAME physical line as
# the `curl` invocation (continuation lines are not scanned). That is a
# deliberate simplicity trade — every existing call site follows it, and
# it keeps this check greppable.

SETUP_SERVICES="$CPP_DIR/scripts/setup_services.sh"

@test "setup_services.sh exists" {
    [ -f "$SETUP_SERVICES" ]
}

@test "every curl invocation in setup_services.sh carries --max-time" {
    run bash -c '
        grep -nE "(^|[[:space:]]|\`|\$\(|\()curl[[:space:]]" "'"$SETUP_SERVICES"'" \
            | grep -vE "^[0-9]+:[[:space:]]*#" \
            | grep -v -- "--max-time" || true
    '
    [ "$status" -eq 0 ]
    if [ -n "$output" ]; then
        echo "curl invocations missing --max-time:" >&2
        echo "$output" >&2
        false
    fi
}

@test "sanity: the scan actually sees the known curl call sites" {
    # Guard the guard: if the grep pattern ever rots and matches nothing,
    # the main test would vacuously pass. setup_services.sh has had 15+
    # curl call sites since v1.9.2; require a sane floor.
    count=$(grep -cE '(^|[[:space:]]|`|\$\(|\()curl[[:space:]]' "$SETUP_SERVICES")
    [ "$count" -ge 10 ]
}
