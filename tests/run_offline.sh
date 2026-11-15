#!/usr/bin/env bash
set -euo pipefail

# Run every suite that needs no Pi, and print one summary.
#
# CI splits these across test-local.yml and test-ota.yml; this is the one
# command a developer runs before pushing (or before tagging a release,
# which now waits on both workflows).
#
# Suites (in order):
#   local-bats     tests/local/*.bats                    (run_local_tests.sh)
#   local-python   tests/local/test_*.py                 (unittest)
#   ota-bats       magic_dingus_box_cpp/scripts/tests/*.bats
#   ota-python     magic_dingus_box_cpp/scripts/tests/test_*.py (unittest)
#   scripts-bats   scripts/tests/*.bats                  (only if present)
#   scripts-python scripts/tests/test_*.py               (only if present)
#   web-pytest     magic_dingus_box/web/tests/           (pytest)
#   cpp-unit       C++ Catch2/CTest suites — OPT-IN (--with-cpp): it needs a
#                  CMake build (BUILD_KIOSK=OFF), which is slow and big.
#
# Usage:
#   ./tests/run_offline.sh                   # every suite except cpp-unit
#   ./tests/run_offline.sh --with-cpp        # ...plus the C++ unit suites
#   ./tests/run_offline.sh --suite web-pytest [--suite ota-bats]
#   ./tests/run_offline.sh --quiet           # only failing suites' output
#
# Environment:
#   PYTHON          interpreter for the Python suites (default: python3).
#                   web-pytest needs flask, pyyaml and pytest in it; for the
#                   production-server legs also gunicorn and flask-sock, and
#                   ffmpeg/ffprobe on PATH (see tests/README.md).
#   CPP_BUILD_DIR   --with-cpp build dir (default:
#                   magic_dingus_box_cpp/build-test, git-ignored)
#   MEDIA_BROWSER   --with-cpp ENABLE_MEDIA_BROWSER (default: ON, as shipped)
#   MAKE_JOBS       --with-cpp parallelism (default: CPU count)
#   CI              when set (not "false"/"0"), a suite that cannot run
#                   because a tool is missing is a FAILURE, not a warning —
#                   and the web suite's dependency skips fail too.
#
# Exit code: 0 if every suite that ran passed (and, under CI, every suite
# could run); 1 otherwise; 2 on a usage error.
#
# Runs under the Mac's stock bash 3.2: no associative arrays, and no
# "${arr[@]}" expansion of a possibly-empty array under set -u.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PYTHON="${PYTHON:-python3}"
CPP_BUILD_DIR="${CPP_BUILD_DIR:-$REPO_ROOT/magic_dingus_box_cpp/build-test}"
WITH_CPP=0
QUIET=0
ONLY=$'\n'   # newline-delimited set of --suite names; empty = all
n_only=0

ALL_SUITES="local-bats local-python ota-bats ota-python scripts-bats scripts-python web-pytest cpp-unit"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --with-cpp) WITH_CPP=1; shift ;;
        --quiet)    QUIET=1; shift ;;
        --suite)
            [[ $# -ge 2 ]] || { echo "--suite needs a name" >&2; exit 2; }
            [[ " $ALL_SUITES " == *" $2 "* ]] || { echo "Unknown suite: $2 (one of: $ALL_SUITES)" >&2; exit 2; }
            ONLY+="$2"$'\n'; n_only=$((n_only + 1))
            [[ "$2" == cpp-unit ]] && WITH_CPP=1
            shift 2 ;;
        -h|--help) sed -n '4,44p' "$0"; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

in_ci() {
    case "$(printf '%s' "${CI:-}" | tr '[:upper:]' '[:lower:]')" in
        ""|0|false|no) return 1 ;;
        *) return 0 ;;
    esac
}

LOG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mdb_offline.XXXXXX")"
trap 'rm -rf "$LOG_DIR"' EXIT

SUMMARY=""     # one preformatted line per suite
FAILED=0
NOT_RUN=0

record() {  # name result pass fail skip [note]
    SUMMARY+="$(printf '  %-15s %-8s %6s %6s %6s  %s' "$1" "$2" "$3" "$4" "$5" "${6:-}")"$'\n'
}

wanted() {
    [[ $n_only -eq 0 || "$ONLY" == *$'\n'"$1"$'\n'* ]]
}

# not_run NAME REASON — a suite that should run but cannot (tool missing).
not_run() {
    NOT_RUN=$((NOT_RUN + 1))
    if in_ci; then
        FAILED=$((FAILED + 1))
        record "$1" "NOT RUN" - - - "FAIL (CI): $2"
        echo "ERROR: $1 could not run: $2" >&2
    else
        record "$1" "NOT RUN" - - - "warning: $2"
        echo "WARNING: $1 could not run: $2" >&2
    fi
}

count() {  # count REGEX FILE — matching lines, 0 when none
    grep -cE "$1" "$2" 2>/dev/null || true
}

# The integer before WORD on LINE ("3 skipped" -> 3), 0 when absent.
num_before() {
    local n
    n="$(printf '%s\n' "$1" | grep -oE "[0-9]+ $2" | head -n1 | grep -oE '^[0-9]+' || true)"
    echo "${n:-0}"
}

# run_cmd NAME DIR CMD... — runs CMD in DIR, output to the log (and to the
# terminal unless --quiet). Sets RC and LOG.
run_cmd() {
    local name="$1" dir="$2"; shift 2
    LOG="$LOG_DIR/$name.log"
    echo ""
    echo "=== $name ==="
    set +e
    if [[ $QUIET -eq 1 ]]; then
        (cd "$dir" && "$@") >"$LOG" 2>&1
        RC=$?
    else
        (cd "$dir" && "$@") 2>&1 | tee "$LOG"
        RC=${PIPESTATUS[0]}
    fi
    set -e
    if [[ $QUIET -eq 1 && $RC -ne 0 ]]; then
        tail -n 80 "$LOG"
    fi
}

finish() {  # finish NAME PASS FAIL SKIP TOTAL — classify from RC + counts
    local name="$1" pass="$2" fail="$3" skip="$4" total="$5"
    if [[ $RC -ne 0 || $fail -gt 0 ]]; then
        FAILED=$((FAILED + 1))
        record "$name" FAIL "$pass" "$fail" "$skip" "exit $RC"
    elif [[ $total -eq 0 ]]; then
        FAILED=$((FAILED + 1))
        record "$name" FAIL 0 0 0 "no tests ran"
    else
        record "$name" PASS "$pass" "$fail" "$skip"
    fi
}

# ---------------------------------------------------------------- bats

bats_suite() {  # NAME DIR [ENV=VAL...]
    local name="$1" dir="$2"; shift 2
    wanted "$name" || return 0
    local files=() f
    if [[ -d "$dir" ]]; then
        for f in "$dir"/*.bats; do [[ -f "$f" ]] && files+=("$f"); done
    fi
    if [[ ${#files[@]} -eq 0 ]]; then
        record "$name" absent - - - "no *.bats in ${dir#"$REPO_ROOT"/}"
        return 0
    fi
    command -v bats >/dev/null 2>&1 || { not_run "$name" "bats not installed"; return 0; }
    run_cmd "$name" "$dir" env "$@" bats --tap "${files[@]}"
    local ok skip fail
    ok="$(count '^ok ' "$LOG")"
    skip="$(count '^ok .*# skip' "$LOG")"
    fail="$(count '^not ok ' "$LOG")"
    finish "$name" "$((ok - skip))" "$fail" "$skip" "$((ok + fail))"
}

# ------------------------------------------------------------ unittest

unittest_suite() {  # NAME DIR
    local name="$1" dir="$2"
    wanted "$name" || return 0
    if ! ls "$dir"/test_*.py >/dev/null 2>&1; then
        record "$name" absent - - - "no test_*.py in ${dir#"$REPO_ROOT"/}"
        return 0
    fi
    command -v "$PYTHON" >/dev/null 2>&1 || { not_run "$name" "$PYTHON not found"; return 0; }
    run_cmd "$name" "$dir" "$PYTHON" -m unittest discover -s . -p 'test_*.py'
    local total res fail skip xfail
    total="$(grep -oE '^Ran [0-9]+ test' "$LOG" | tail -n1 | grep -oE '[0-9]+' || true)"
    total="${total:-0}"
    res="$(grep -E '^(OK|FAILED)' "$LOG" | tail -n1 || true)"
    fail=$(( $(printf '%s' "$res" | grep -oE 'failures=[0-9]+' | grep -oE '[0-9]+' || echo 0) \
           + $(printf '%s' "$res" | grep -oE 'errors=[0-9]+' | grep -oE '[0-9]+' || echo 0) ))
    skip="$(printf '%s' "$res" | grep -oE 'skipped=[0-9]+' | grep -oE '[0-9]+' || echo 0)"
    xfail="$(printf '%s' "$res" | grep -oE 'expected failures=[0-9]+' | grep -oE '[0-9]+' || echo 0)"
    finish "$name" "$((total - fail - skip - xfail))" "$fail" "$skip" "$total"
}

# -------------------------------------------------------------- pytest

web_suite() {
    local name=web-pytest dir="$REPO_ROOT/magic_dingus_box/web/tests"
    wanted "$name" || return 0
    command -v "$PYTHON" >/dev/null 2>&1 || { not_run "$name" "$PYTHON not found"; return 0; }
    local missing
    missing="$("$PYTHON" - <<'EOF' 2>/dev/null || echo "(python failed)"
import importlib
out = []
for m in ("flask", "yaml", "pytest"):
    try:
        importlib.import_module(m)
    except ImportError:
        out.append(m)
print(" ".join(out))
EOF
)"
    if [[ -n "$missing" ]]; then
        not_run "$name" "$PYTHON lacks: $missing (pip install flask pyyaml pytest)"
        return 0
    fi
    # From the repo root: from web/ the package-style imports fail collection.
    run_cmd "$name" "$REPO_ROOT" "$PYTHON" -m pytest "$dir" -q -rs -p no:cacheprovider
    local line
    line="$(grep -E '[0-9]+ (passed|failed|skipped|errors?)' "$LOG" | grep -E ' in [0-9.]+s' | tail -n1 || true)"
    local pass fail err skip
    pass="$(num_before "$line" passed)"
    fail="$(num_before "$line" failed)"
    err="$(num_before "$line" 'errors?')"
    skip="$(num_before "$line" skipped)"
    finish "$name" "$pass" "$((fail + err))" "$skip" "$((pass + fail + err + skip))"
}

# ---------------------------------------------------------------- C++

cpu_count() {
    getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2
}

# BUILD_KIOSK=OFF skips every Linux-kiosk dependency, exactly like
# test-local.yml's cpp-unit job and the Mac configure.
cpp_build_and_test() {  # BUILD_DIR MEDIA_BROWSER JOBS
    cmake -S magic_dingus_box_cpp -B "$1" -DBUILD_KIOSK=OFF -DENABLE_MEDIA_BROWSER="$2" &&
    cmake --build "$1" -j"$3" &&
    ctest --test-dir "$1" --output-on-failure
}

cpp_suite() {
    local name=cpp-unit
    if [[ $WITH_CPP -eq 0 ]]; then
        [[ $n_only -eq 0 ]] && record "$name" skipped - - - "opt-in: --with-cpp"
        return 0
    fi
    wanted "$name" || return 0
    local t
    for t in cmake ctest; do
        command -v "$t" >/dev/null 2>&1 || { not_run "$name" "$t not installed"; return 0; }
    done
    local jobs="${MAKE_JOBS:-$(cpu_count)}"
    run_cmd "$name" "$REPO_ROOT" cpp_build_and_test "$CPP_BUILD_DIR" "${MEDIA_BROWSER:-ON}" "$jobs"
    local line pass fail
    line="$(grep -E 'tests passed, [0-9]+ tests? failed out of [0-9]+' "$LOG" | tail -n1 || true)"
    fail="$(printf '%s' "$line" | grep -oE '[0-9]+ tests? failed' | grep -oE '^[0-9]+' || echo 0)"
    local total
    total="$(printf '%s' "$line" | grep -oE 'out of [0-9]+' | grep -oE '[0-9]+' || echo 0)"
    pass=$((total - fail))
    finish "$name" "$pass" "$fail" 0 "$total"
}

# ------------------------------------------------------------------ run

echo "Magic Dingus Box offline tests (no Pi)"
echo "======================================"
in_ci && echo "CI mode: a suite that cannot run is a failure."

bats_suite     local-bats     "$REPO_ROOT/tests/local"
unittest_suite local-python   "$REPO_ROOT/tests/local"
bats_suite     ota-bats       "$REPO_ROOT/magic_dingus_box_cpp/scripts/tests" \
               MAGIC_SKIP_SYSTEMCTL=true MAGIC_SKIP_BUILD=true
unittest_suite ota-python     "$REPO_ROOT/magic_dingus_box_cpp/scripts/tests"
bats_suite     scripts-bats   "$REPO_ROOT/scripts/tests"
unittest_suite scripts-python "$REPO_ROOT/scripts/tests"
web_suite
cpp_suite

echo ""
echo "Offline test summary"
printf '  %-15s %-8s %6s %6s %6s  %s\n' suite result pass fail skip note
printf '%s' "$SUMMARY"
if [[ $NOT_RUN -gt 0 ]] && ! in_ci; then
    echo "WARNING: $NOT_RUN suite(s) could not run (a missing tool) — a failure under CI."
fi
if [[ $FAILED -gt 0 ]]; then
    echo "RESULT: FAILED ($FAILED suite(s))"
    exit 1
fi
echo "RESULT: all suites that ran passed"
