#!/usr/bin/env bats
load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

VERSION_FILE="$TESTS_REPO_ROOT/VERSION"
CHANGELOG="$TESTS_REPO_ROOT/CHANGELOG.md"

@test "VERSION file exists" {
    [ -f "$VERSION_FILE" ]
}

@test "VERSION file contains a semver string" {
    content=$(cat "$VERSION_FILE")
    echo "$content" | grep -qE "^[0-9]+\.[0-9]+\.[0-9]+$" \
        || { echo "VERSION not semver: $content"; false; }
}

@test "VERSION matches latest non-Unreleased CHANGELOG entry (or warns if Unreleased present)" {
    version=$(cat "$VERSION_FILE")
    latest_changelog=$(grep -E "^## \[[0-9]+\.[0-9]+\.[0-9]+\]" "$CHANGELOG" | head -1 | sed -E 's/^## \[([0-9]+\.[0-9]+\.[0-9]+)\].*/\1/')

    # If CHANGELOG has [Unreleased], it's an in-progress release — warn but pass.
    if grep -q "^## \[Unreleased\]" "$CHANGELOG"; then
        if [ "$version" != "$latest_changelog" ]; then
            echo "WARN: VERSION=$version, latest CHANGELOG release=$latest_changelog (Unreleased section present — bump VERSION before tagging)"
        fi
        return 0
    fi

    [ "$version" = "$latest_changelog" ] \
        || { echo "VERSION ($version) != latest CHANGELOG release ($latest_changelog)"; false; }
}
