# shellcheck shell=bash
# Pure helpers for hw_rehearsal.sh — no ssh, no side effects, so
# tests/local/ota_hw_rehearsal.bats can exercise them on the Mac.

# The pkill/pgrep pattern for the on-box fake GitHub server.
#
# LESSON (2026-10-04 manual rehearsal): `pkill -f /home/magic/fakegh/...`
# run over ssh matches the ssh session's OWN command line (bash -c '...pkill
# -f /home/magic/fakegh/...'), kills it, and the step "fails" with the
# server still running. Two defences: the pattern is anchored at the start
# of the command line ("^python3 "), so a shell wrapper never matches, and
# one character is a bracket class ("fake_[g]ithub"), so the pattern text
# itself does not match the pattern.
#   hw_pkill_pattern /home/magic/fakegh  ->  ^python3 /home/magic/fakegh/fake_[g]ithub\.py
hw_pkill_pattern() {
    local dir="${1%/}"
    dir="${dir//./\\.}"
    printf '^python3 %s/fake_[g]ithub\\.py' "$dir"
}

# The single line appended to the box's /etc/hosts while the fake GitHub is
# up. The trailing tag lets a fallback cleanup delete exactly this line.
HW_HOSTS_TAG="# mdb-ota-hw-rehearsal"
HW_FAKE_HOSTS="github.com api.github.com codeload.github.com uploads.github.com objects.githubusercontent.com release-assets.githubusercontent.com"
hw_hosts_line() {
    printf '127.0.0.1 %s %s' "$HW_FAKE_HOSTS" "$HW_HOSTS_TAG"
}
# sed program removing only the tagged line (fallback when hosts.orig is gone)
hw_hosts_sed() {
    printf '/ %s$/d' "${HW_HOSTS_TAG//\//\\/}"
}

# "VAR=value" assignment lines, shell-quoted, for a remote script prelude.
#   hw_assign A B  ->  A=<%q of $A>\nB=<%q of $B>
hw_assign() {
    local v
    for v in "$@"; do
        printf '%s=%q\n' "$v" "${!v}"
    done
}

# Value of KEY from "key=value" lines (first match). Missing -> empty.
hw_kv() {
    local key="$1" text="$2" line
    while IFS= read -r line; do
        if [[ "$line" == "${key}="* ]]; then
            printf '%s' "${line#*=}"
            return 0
        fi
    done <<<"$text"
    return 0
}

# A host string reduced to something safe in a directory name.
hw_safe_name() {
    local s="${1##*@}"
    printf '%s' "${s//[^A-Za-z0-9._-]/_}"
}

# Field-state file for an OLD release: field_state_<X.Y.Z>.txt (a leading
# "v" on the version is accepted).
hw_field_state_file() {
    local dir="$1" ver="${2#v}"
    printf '%s/field_state_%s.txt' "$dir" "$ver"
}

# The kiosk restart deploy_cpp.sh uses (stop -> wait inactive -> settle ->
# reset-failed -> start -> wait active). Plain `systemctl restart` races
# the old process for DRM master; see deploy_cpp.sh Step 1.7.
hw_kiosk_restart_snippet() {
    cat <<'EOF'
svc=magic-dingus-box-cpp.service
sudo systemctl stop "$svc" 2>/dev/null || true
for _ in $(seq 1 30); do systemctl is-active --quiet "$svc" || break; sleep 0.5; done
sleep 2
sudo systemctl reset-failed "$svc" 2>/dev/null || true
sudo systemctl start "$svc" 2>/dev/null || true
for _ in $(seq 1 60); do systemctl is-active --quiet "$svc" && break; sleep 0.5; done
echo "kiosk: $(systemctl is-active "$svc" 2>/dev/null || true)"
EOF
}
