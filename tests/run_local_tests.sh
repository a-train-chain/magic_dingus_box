#!/usr/bin/env bash
set -euo pipefail

# Run the local tier of the Magic Dingus Box test suite.
# Local tests do NOT require a Pi to be reachable; they validate
# repo state, schema, lints, etc.
#
# Usage:
#   ./tests/run_local_tests.sh           # human-readable output
#   ./tests/run_local_tests.sh --tap     # TAP output for CI consumption
#   ./tests/run_local_tests.sh --filter <pattern>
#
# Exit code: 0 if all tests pass, non-zero otherwise.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCAL_DIR="$SCRIPT_DIR/local"

TAP_MODE=0
FILTER=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tap) TAP_MODE=1; shift ;;
        --filter) FILTER="$2"; shift 2 ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

# Discover .bats files
bats_files=()
if [[ -d "$LOCAL_DIR" ]]; then
    for f in "$LOCAL_DIR"/*.bats; do
        [[ -f "$f" ]] && bats_files+=("$f")
    done
fi

if [[ -n "$FILTER" && ${#bats_files[@]} -gt 0 ]]; then
    filtered=()
    for f in "${bats_files[@]}"; do
        [[ "$(basename "$f")" == *"$FILTER"* ]] && filtered+=("$f")
    done
    bats_files=("${filtered[@]}")
fi

if [[ ${#bats_files[@]} -eq 0 ]]; then
    echo "Local tests: 0 files found in $LOCAL_DIR"
    exit 0
fi

echo "Local tests (${#bats_files[@]} files):"
if [[ $TAP_MODE -eq 1 ]]; then
    bats --tap "${bats_files[@]}"
else
    bats "${bats_files[@]}"
fi
