#!/usr/bin/env bash
set -euo pipefail

# Run the Pi tier of the test suite over SSH.
#
# Usage:
#   ./tests/run_pi_tests.sh                          # default PI_HOST
#   PI_HOST=magic@magicpi.local ./tests/run_pi_tests.sh
#   ./tests/run_pi_tests.sh --filter <pattern>
#   ./tests/run_pi_tests.sh --tap
#
# Exit code: 0 if all tests pass, non-zero otherwise.
# Tests that require absent hardware (controller, etc.) skip cleanly.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PI_DIR="$SCRIPT_DIR/pi"

: "${PI_HOST:=magic@10.55.0.1}"
export PI_HOST

TAP_MODE=0
FILTER=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tap) TAP_MODE=1; shift ;;
        --filter) FILTER="$2"; shift 2 ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

# Preflight: Pi reachable?
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$PI_HOST" true 2>/dev/null; then
    echo "ERROR: Pi not reachable at $PI_HOST" >&2
    echo "  Check connectivity, or override PI_HOST=user@host" >&2
    exit 2
fi

bats_files=()
if [[ -d "$PI_DIR" ]]; then
    for f in "$PI_DIR"/*.bats; do
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
    echo "Pi tests: 0 files found in $PI_DIR"
    exit 0
fi

echo "Pi tests (${#bats_files[@]} files, $PI_HOST):"
if [[ $TAP_MODE -eq 1 ]]; then
    bats --tap "${bats_files[@]}"
else
    bats "${bats_files[@]}"
fi
