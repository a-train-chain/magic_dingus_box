#!/usr/bin/env bash
# ShellCheck gate: lint EVERY tracked shell script at one severity.
#
# Scope: every git-tracked *.sh / *.bash file, plus extensionless tracked
# files whose shebang is sh/bash (NetworkManager dispatcher hooks, the OTA
# rehearsal's systemctl stand-in). .bats files are not in scope — they are
# test code with bats-specific idioms.
#
# Severity is info: everything but pure style (SC2001 sed-vs-${//}, SC2005
# echo-of-$(...)) — the style rewrites change output bytes for no safety
# gain. CI pins the ShellCheck version (test-local.yml) because each
# release adds checks; a newer local ShellCheck may report more.
#
# Global exclusions and the source-following settings live in the repo's
# .shellcheckrc (picked up automatically). Deliberate patterns are
# silenced inline with `# shellcheck disable=SCxxxx  # <reason>` at the
# site — never by lowering the severity here.
#
# Usage:
#   tests/shellcheck_gate.sh                       # lint everything at -S info
#   tests/shellcheck_gate.sh --exclude path/a.sh   # skip a file (repeatable)
#   tests/shellcheck_gate.sh --list                # print the file set and exit
#   SHELLCHECK_SEVERITY=style tests/shellcheck_gate.sh   # look below the gate
#
# Exit: 0 clean, 1 findings, 2 usage/tooling error.
#
# Runs under the Mac's stock bash 3.2 too: no associative arrays, and no
# "${arr[@]}" expansion of a possibly-empty array under set -u.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

SEVERITY="${SHELLCHECK_SEVERITY:-info}"
LIST_ONLY=0
EXCLUDED=$'\n'     # newline-delimited set; membership = substring "\n<path>\n"
n_excluded=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --exclude)
            [[ $# -ge 2 ]] || { echo "--exclude needs a path" >&2; exit 2; }
            EXCLUDED+="$2"$'\n'; n_excluded=$((n_excluded + 1))
            # A stale entry should shrink the list, not silently outlive its file.
            [[ -f "$2" ]] || echo "WARN: excluded path does not exist (stale exclusion?): $2" >&2
            shift 2 ;;
        --list) LIST_ONLY=1; shift ;;
        -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

command -v shellcheck >/dev/null 2>&1 || { echo "shellcheck not installed" >&2; exit 2; }
command -v git >/dev/null 2>&1 || { echo "git not installed" >&2; exit 2; }

is_shell_shebang() {
    local first
    IFS= read -r first < "$1" 2>/dev/null || return 1
    [[ "$first" =~ ^\#\![[:space:]]*(/usr)?/bin/(env[[:space:]]+)?(ba)?sh([[:space:]]|$) ]]
}

files=()
while IFS= read -r -d '' f; do
    [[ -f "$f" ]] || continue
    [[ "$EXCLUDED" == *$'\n'"$f"$'\n'* ]] && continue
    base="${f##*/}"
    case "$base" in
        *.sh|*.bash) files+=("$f") ;;
        *.*) ;;   # any other extension (.bats, .py, .json, ...): not in scope
        *) if is_shell_shebang "$f"; then files+=("$f"); fi ;;
    esac
done < <(git ls-files -z)

if [[ ${#files[@]} -eq 0 ]]; then
    echo "ShellCheck gate: no shell scripts found" >&2
    exit 2
fi

if [[ $LIST_ONLY -eq 1 ]]; then
    printf '%s\n' "${files[@]}"
    exit 0
fi

echo "ShellCheck $(shellcheck --version | sed -n 's/^version: //p'):" \
     "${#files[@]} files at -S $SEVERITY ($n_excluded excluded)"
if shellcheck -S "$SEVERITY" "${files[@]}"; then
    echo "ShellCheck gate: clean"
else
    echo "ShellCheck gate: FAILED — fix the findings above, or silence a deliberate" \
         "pattern inline with a reasoned '# shellcheck disable=SCxxxx'" >&2
    exit 1
fi
