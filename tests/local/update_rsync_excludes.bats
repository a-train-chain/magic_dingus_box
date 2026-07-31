#!/usr/bin/env bats
# Verify that the four rsync `--exclude` lists in update.sh stay consistent.
#
# Background: update.sh has four rsync invocations — backup creation, install,
# internal rollback (mid-install failure), and user-initiated rollback.
# Operator content (media, ROMs, saves, .env, etc.) must be excluded from
# all rsyncs that could clobber it. v1.4.3's CHANGELOG records that drift
# between these lists once caused operator data loss; this test prevents
# that class of drift from recurring silently.
#
# The four blocks are NOT identical by design:
#   - Block 1 (backup):    captures everything EXCEPT excluded paths into BACKUP_DIR.
#                          Excludes only the bulky operator paths we don't need
#                          to round-trip (multi-GB ROMs, downloaded media).
#                          Implicitly INCLUDES build/* — backup captures the
#                          pre-update binary so rollback can restore it.
#   - Block 2 (install):   the most comprehensive list — protects every
#                          operator-content path PLUS build/* (new binary
#                          will be built fresh post-install).
#   - Blocks 3, 4 (rollback): rsync BACKUP_DIR → INSTALL_DIR. Must exclude
#                             every operator-content path Block 2 protects,
#                             EXCEPT build/* (rollback's whole purpose is to
#                             restore the pre-update binary from backup).
#
# Invariant: (install-excludes \ {build/*}) == rollback-excludes for both
# rollback blocks. Plus rollback-1 == rollback-2 (same restore semantics).

load "$BATS_TEST_DIRNAME/../lib/helpers.bash"

UPDATE_SH="$BATS_TEST_DIRNAME/../../magic_dingus_box_cpp/scripts/update.sh"
BUILD_EXCLUDE='magic_dingus_box_cpp/build/*'

# Extract the exclude list from each rsync block as a sorted, newline-separated
# string. Returns 4 sections separated by `===BLOCK N===` markers.
extract_blocks() {
    python3 <<'PY'
import re, sys
with open("magic_dingus_box_cpp/scripts/update.sh") as f:
    src = f.read()
# Match rsync command + its filter list up to the source/dest pair on the
# closing line. Endpoint vars vary in case ($content_dir, $INSTALL_DIR,
# $BACKUP_DIR), so the regex is permissive about variable names. Blocks
# carry --include lines as well as --exclude since the 2026-07-30 OTA fix
# (thumbnails/systems/*** must flow before the blanket thumbnails
# exclude) — the filter-line matcher accepts both, or it finds 0 blocks.
pattern = r'rsync -[a-z]+\s+--delete[^\n]*\n((?:\s*--(?:exclude|include)\s+\'[^\']+\'[^\n]*\n)+)\s*"\$[A-Za-z_]+/?"\s+"\$[A-Za-z_]+/?"'
blocks = re.findall(pattern, src)
if len(blocks) != 4:
    sys.exit(f"Expected exactly 4 rsync blocks; found {len(blocks)}")
for i, body in enumerate(blocks, 1):
    excludes = sorted(re.findall(r"--exclude\s+'([^']+)'", body))
    print(f"===BLOCK {i}===")
    for e in excludes:
        print(e)
PY
}

# Parse the python output into bash arrays. Sets BLOCK1, BLOCK2, BLOCK3, BLOCK4
# (each a newline-separated sorted list).
parse_blocks() {
    cd "$BATS_TEST_DIRNAME/../.."
    local raw
    raw=$(extract_blocks)
    BLOCK1=$(awk '/^===BLOCK 1===/{flag=1; next} /^===BLOCK 2===/{flag=0} flag' <<<"$raw")
    BLOCK2=$(awk '/^===BLOCK 2===/{flag=1; next} /^===BLOCK 3===/{flag=0} flag' <<<"$raw")
    BLOCK3=$(awk '/^===BLOCK 3===/{flag=1; next} /^===BLOCK 4===/{flag=0} flag' <<<"$raw")
    BLOCK4=$(awk '/^===BLOCK 4===/{flag=1} flag && !/^===BLOCK 4===/' <<<"$raw")
}

@test "update.sh has exactly 4 rsync blocks" {
    cd "$BATS_TEST_DIRNAME/../.."
    run extract_blocks
    [ "$status" -eq 0 ]
    [ "$(echo "$output" | grep -c '^===BLOCK')" -eq 4 ]
}

@test "rollback blocks (3 and 4) are identical to each other" {
    parse_blocks
    [ "$BLOCK3" = "$BLOCK4" ] || {
        echo "Block 3 (internal rollback):"; echo "$BLOCK3"
        echo "Block 4 (user rollback):"; echo "$BLOCK4"
        echo "These must match — both rsync BACKUP_DIR → INSTALL_DIR with the same restore semantics."
        return 1
    }
}

@test "every install exclude EXCEPT build/* appears in both rollback blocks" {
    parse_blocks
    local missing_in_3=()
    local missing_in_4=()
    while IFS= read -r path; do
        [ -z "$path" ] && continue
        [ "$path" = "$BUILD_EXCLUDE" ] && continue   # intentional drift
        if ! grep -qxF "$path" <<<"$BLOCK3"; then
            missing_in_3+=("$path")
        fi
        if ! grep -qxF "$path" <<<"$BLOCK4"; then
            missing_in_4+=("$path")
        fi
    done <<<"$BLOCK2"

    if [ ${#missing_in_3[@]} -gt 0 ] || [ ${#missing_in_4[@]} -gt 0 ]; then
        echo "DRIFT DETECTED: install excludes that are NOT in rollback excludes."
        echo "  This means a rollback would CLOBBER these operator-content paths."
        [ ${#missing_in_3[@]} -gt 0 ] && { echo "  Missing from Block 3 (internal rollback):"; printf '    %s\n' "${missing_in_3[@]}"; }
        [ ${#missing_in_4[@]} -gt 0 ] && { echo "  Missing from Block 4 (user rollback):"; printf '    %s\n' "${missing_in_4[@]}"; }
        echo "  Fix: add the missing --exclude lines to update.sh's rollback rsyncs."
        return 1
    fi
}

@test "install (Block 2) explicitly excludes build/* (rollback design relies on this)" {
    parse_blocks
    grep -qxF "$BUILD_EXCLUDE" <<<"$BLOCK2" || {
        echo "Block 2 (install) is missing the build/* exclude."
        echo "If install rsync overwrites build/*, fresh-built binary may be partial/corrupted."
        return 1
    }
}

@test "rollback blocks do NOT exclude build/* (so old binary gets restored)" {
    parse_blocks
    if grep -qxF "$BUILD_EXCLUDE" <<<"$BLOCK3"; then
        echo "Block 3 (internal rollback) excludes build/* — rollback won't restore the pre-update binary."
        return 1
    fi
    if grep -qxF "$BUILD_EXCLUDE" <<<"$BLOCK4"; then
        echo "Block 4 (user rollback) excludes build/* — rollback won't restore the pre-update binary."
        return 1
    fi
}

@test "every backup exclude (Block 1) is also an install exclude (Block 2)" {
    parse_blocks
    # If a path is excluded from backup, it's operator content we're choosing
    # not to round-trip. Install MUST also exclude it, otherwise the install
    # rsync would clobber operator content the backup didn't capture.
    local missing=()
    while IFS= read -r path; do
        [ -z "$path" ] && continue
        if ! grep -qxF "$path" <<<"$BLOCK2"; then
            missing+=("$path")
        fi
    done <<<"$BLOCK1"
    if [ ${#missing[@]} -gt 0 ]; then
        echo "Backup excludes that are NOT in install excludes (= install would clobber them):"
        printf '    %s\n' "${missing[@]}"
        return 1
    fi
}

@test "all 4 blocks carry the thumbnails/systems/*** include before the blanket exclude" {
    # System tiles are delivered content: the include must appear in every
    # block, and it must come BEFORE the thumbnails exclude on the command
    # line or rsync never sees it (first-match-wins). Regression guard for
    # the 2026-07-30 OTA fix; this suite went silently red when the
    # include lines were first added, so this pins them explicitly.
    local count
    count=$(grep -c -- "--include 'magic_dingus_box_cpp/data/thumbnails/systems/\*\*\*'" "$UPDATE_SH")
    [ "$count" -eq 4 ]
    # In each block the include line number must precede the thumbnails
    # exclude line number.
    python3 - "$UPDATE_SH" <<'PY'
import sys
src = open(sys.argv[1]).read().splitlines()
inc = [i for i, l in enumerate(src) if "--include 'magic_dingus_box_cpp/data/thumbnails/systems/***'" in l]
exc = [i for i, l in enumerate(src) if "--exclude 'magic_dingus_box_cpp/data/thumbnails/*'" in l]
assert len(inc) == 4, f"expected 4 include lines, found {len(inc)}"
assert len(exc) == 4, f"expected 4 thumbnails excludes, found {len(exc)}"
for i, e in zip(inc, exc):
    assert i < e, f"include at line {i+1} does not precede exclude at line {e+1}"
PY
}
