#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

# The beta update channel's release half (magic_dingus_box_cpp/docs/RELEASING.md):
# a tag vX.Y.Z-beta.N must publish a GitHub PRERELEASE — GitHub never returns
# a prerelease from releases/latest, which is the only endpoint stable boxes
# and every updater <= 1.10.0 query. If this flag ever regressed, the next
# beta tag would ship to every customer box.

RELEASE_YML="$TESTS_REPO_ROOT/.github/workflows/release.yml"
UPDATE_SH="$CPP_DIR/scripts/update.sh"

@test "release.yml is valid YAML" {
    python3 -c 'import sys, yaml; yaml.safe_load(open(sys.argv[1]))' "$RELEASE_YML"
}

@test "Create Release marks tags containing '-' as prerelease" {
    python3 - "$RELEASE_YML" <<'EOF'
import sys, yaml
wf = yaml.safe_load(open(sys.argv[1]))
step = next(s for s in wf["jobs"]["release"]["steps"] if s.get("name") == "Create Release")
assert step["uses"].startswith("softprops/action-gh-release@"), step["uses"]
pre = str(step["with"].get("prerelease", "")).replace(" ", "")
assert pre == "${{contains(github.ref_name,'-')}}", f"prerelease input is {pre!r}"
EOF
}

@test "release job still waits on the full test suite and both builds" {
    python3 - "$RELEASE_YML" <<'EOF'
import sys, yaml
wf = yaml.safe_load(open(sys.argv[1]))
assert set(wf["jobs"]["release"]["needs"]) == {"tests", "build-arm64", "build-source"}
EOF
}

@test "every Extract version step refuses a tag outside vX.Y.Z / vX.Y.Z-beta.N" {
    local n
    n=$(grep -c "name: Extract version" "$RELEASE_YML")
    [ "$n" -eq 3 ]
    [ "$(grep -cF 'if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-beta\.[0-9]+)?$ ]]; then' "$RELEASE_YML")" -eq 3 ]
}

@test "the workflow's tag grammar is update.sh's VERSION_RE" {
    grep -qF "VERSION_RE='^[0-9]+\.[0-9]+\.[0-9]+(-beta\.[0-9]+)?\$'" "$UPDATE_SH"
}

@test "the Extract version script accepts stable + beta tags and rejects the rest" {
    local script
    script=$(python3 - "$RELEASE_YML" <<'EOF'
import sys, yaml
wf = yaml.safe_load(open(sys.argv[1]))
print(next(s for s in wf["jobs"]["release"]["steps"] if s.get("name") == "Extract version")["run"])
EOF
)
    local tag out
    for tag in v1.10.1 v1.10.1-beta.1 v2.0.0-beta.12; do
        out=$(GITHUB_REF="refs/tags/$tag" GITHUB_OUTPUT=/dev/stdout bash -c "$script")
        [ "$out" = "VERSION=${tag#v}" ] || { echo "$tag -> $out"; false; }
    done
    for tag in v1.10 v1.10.1-rc.1 v1.10.1-beta v1.10.1-Beta.1 vx.y.z v1.10.1-beta.1-x; do
        run env GITHUB_REF="refs/tags/$tag" GITHUB_OUTPUT=/dev/null bash -c "$script"
        [ "$status" -ne 0 ] || { echo "accepted $tag"; false; }
    done
}

@test "release-blocking binary assertions are all still present" {
    grep -q 'grep -q aarch64' "$RELEASE_YML"
    grep -q 'grep -q "READY=1"' "$RELEASE_YML"
    grep -q 'grep -qi "prowlarr"' "$RELEASE_YML"
    grep -q 'grep -q "/connect?code="' "$RELEASE_YML"
    grep -q 'compiled without HAVE_GPIOD' "$RELEASE_YML"
    grep -q 'Assert tarball layout' "$RELEASE_YML"
}

@test "the OTA rehearsal can still lift the release steps (beta version)" {
    local out="$BATS_TEST_TMPDIR/steps"
    python3 "$TESTS_REPO_ROOT/tests/ota_rehearsal/extract_release_steps.py" \
        "$RELEASE_YML" 1.10.1-beta.1 "$out"
    grep -q "magic-dingus-box-1.10.1-beta.1.tar.gz" "$out/release_files.txt"
    grep -q "magic_dingus_box_cpp-arm64-1.10.1-beta.1.tar.gz" "$out/release_files.txt"
}
