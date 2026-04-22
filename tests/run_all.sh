#!/usr/bin/env bash
set -euo pipefail

# Run the full Magic Dingus Box test suite (local + pi tiers).
#
# Usage:
#   ./tests/run_all.sh                  # both tiers
#   ./tests/run_all.sh --local-only
#   ./tests/run_all.sh --pi-only
#   ./tests/run_all.sh --filter <pat>   # passed to both tiers
#   ./tests/run_all.sh --tap

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

LOCAL_ONLY=0
PI_ONLY=0
declare -a PASSTHROUGH

while [[ $# -gt 0 ]]; do
    case "$1" in
        --local-only) LOCAL_ONLY=1; shift ;;
        --pi-only)    PI_ONLY=1; shift ;;
        --filter)     PASSTHROUGH+=("--filter" "$2"); shift 2 ;;
        --tap)        PASSTHROUGH+=("--tap"); shift ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

echo "Magic Dingus Box Test Suite"
echo "==========================="

EXIT=0

if [[ $PI_ONLY -eq 0 ]]; then
    if [[ ${#PASSTHROUGH[@]} -gt 0 ]]; then
        "$SCRIPT_DIR/run_local_tests.sh" "${PASSTHROUGH[@]}" || EXIT=$?
    else
        "$SCRIPT_DIR/run_local_tests.sh" || EXIT=$?
    fi
    echo ""
fi

if [[ $LOCAL_ONLY -eq 0 ]]; then
    if [[ ${#PASSTHROUGH[@]} -gt 0 ]]; then
        "$SCRIPT_DIR/run_pi_tests.sh" "${PASSTHROUGH[@]}" || EXIT=$?
    else
        "$SCRIPT_DIR/run_pi_tests.sh" || EXIT=$?
    fi
fi

exit $EXIT
