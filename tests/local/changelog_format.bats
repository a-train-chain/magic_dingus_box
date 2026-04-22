#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

CHANGELOG="$TESTS_REPO_ROOT/CHANGELOG.md"

@test "CHANGELOG.md exists" {
    [ -f "$CHANGELOG" ]
}

@test "CHANGELOG has at least one version heading" {
    grep -E "^## \[(Unreleased|[0-9]+\.[0-9]+\.[0-9]+)\]" "$CHANGELOG" | head -1
}

@test "every release heading matches semver pattern" {
    # Lines like "## [1.3.0] - 2026-02-22" — check the version part
    while IFS= read -r line; do
        # Allow Unreleased plus semver
        echo "$line" | grep -E "^## \[(Unreleased|[0-9]+\.[0-9]+\.[0-9]+)\]" \
            || { echo "malformed heading: $line"; false; }
    done < <(grep "^## \[" "$CHANGELOG")
}

@test "every dated release heading uses YYYY-MM-DD" {
    # Skip Unreleased since it has no date
    while IFS= read -r line; do
        echo "$line" | grep -E " - [0-9]{4}-[0-9]{2}-[0-9]{2}$" \
            || { echo "bad date format: $line"; false; }
    done < <(grep -E "^## \[[0-9]+\.[0-9]+\.[0-9]+\]" "$CHANGELOG")
}
