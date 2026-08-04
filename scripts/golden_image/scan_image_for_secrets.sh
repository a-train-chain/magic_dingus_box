#!/usr/bin/env bash
#
# Magic Dingus Box - Scan a golden image for credential remnants
#
# Runs on the OPERATOR'S MAC. Answers one question about an .img / .img.gz:
# does it contain any of this operator's live secrets?
#
# clone_live_sd.sh runs this check automatically on every image it makes.
# This standalone form exists for images made BEFORE that gate existed --
# the v1.9.3 image shipped a deleted cloud-init.log with the Wi-Fi PSK in
# plaintext, and any image produced by the same script has the same defect
# until proven otherwise.
#
# Values are never printed. A finding is reported as needle index, length
# and hit count -- enough to act on, never enough to leak.
#
# Usage:
#   ./scan_image_for_secrets.sh --image ~/golden.img.gz --pi magic@10.55.0.1
#   ./scan_image_for_secrets.sh --image ~/golden.img.gz --needles ~/secrets.txt
#
#   --pi HOST        harvest needles from a live box over SSH (Wi-Fi SSID +
#                    PSK, WireGuard key, qBittorrent password)
#   --needles FILE   use a file of literal secrets instead, one per line
#   --skip-integrity skip the gzip CRC check (faster, but a truncated image
#                    can then report a false clean -- not recommended)
#
# Exit: 0 clean, 1 leak found, 2 could not check.
#

set -euo pipefail

IMAGE=""
PI_HOST=""
NEEDLES=""
CHECK_INTEGRITY=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --image)          IMAGE="$2"; shift 2 ;;
        --pi)             PI_HOST="$2"; shift 2 ;;
        --needles)        NEEDLES="$2"; shift 2 ;;
        --skip-integrity) CHECK_INTEGRITY=0; shift ;;
        -h|--help)        sed -n 's/^# \{0,1\}//;1,/^$/p' "$0" | head -30; exit 0 ;;
        *)                echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [[ -t 1 ]]; then
    RED='\033[0;31m' GREEN='\033[0;32m' YELLOW='\033[1;33m' BOLD='\033[1m' DIM='\033[2m' NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' BOLD='' DIM='' NC=''
fi

[[ -z "$IMAGE" ]]  && { echo "--image is required" >&2; exit 2; }
[[ -f "$IMAGE" ]]  || { echo "No such image: $IMAGE" >&2; exit 2; }
if [[ -z "$PI_HOST" && -z "$NEEDLES" ]]; then
    echo "One of --pi or --needles is required (nothing to scan for otherwise)" >&2
    exit 2
fi

TMP_NEEDLES=""
cleanup() { [[ -n "$TMP_NEEDLES" ]] && rm -f "$TMP_NEEDLES"; }
trap cleanup EXIT INT TERM

echo
echo -e "${BOLD}Scanning $(basename "$IMAGE") for credential remnants${NC}"
echo

# ---------------------------------------------------------------------------
# Needles
# ---------------------------------------------------------------------------
if [[ -n "$PI_HOST" ]]; then
    echo -e "  Harvesting secrets from ${BOLD}${PI_HOST}${NC}..."
    TMP_NEEDLES=$(mktemp "${TMPDIR:-/tmp}/mdb-scan-needles.XXXXXX")
    chmod 600 "$TMP_NEEDLES"
    ssh -o ConnectTimeout=10 "$PI_HOST" "
        sudo grep -hoE '^(ssid|psk)=.+' /etc/NetworkManager/system-connections/*.nmconnection 2>/dev/null | sed 's/^[a-z]*=//'
        sudo grep -hoE '^(WIREGUARD_PRIVATE_KEY|MDB_QBIT_PASS|QBIT_PASS)=.+' /opt/magic_dingus_box/services/.env 2>/dev/null | sed 's/^[A-Z_]*=//'
    " 2>/dev/null | sed 's/[[:space:]]*$//' | grep -v '^$' | sort -u > "$TMP_NEEDLES" || true
    NEEDLES="$TMP_NEEDLES"
fi

N_TOTAL=$(grep -c . "$NEEDLES" 2>/dev/null || echo 0)
if [[ "$N_TOTAL" -eq 0 ]]; then
    echo -e "  ${RED}No secrets to scan for.${NC} A vacuous scan is not a pass."
    exit 2
fi
echo -e "  ${GREEN}OK${NC} ${N_TOTAL} secret value(s) loaded (not shown)"

# ---------------------------------------------------------------------------
# Integrity: a truncated stream finds nothing and would look clean
# ---------------------------------------------------------------------------
case "$IMAGE" in
    *.gz) READER=(gzip -dc "$IMAGE"); COMPRESSED=1 ;;
    *)    READER=(cat "$IMAGE");      COMPRESSED=0 ;;
esac

if [[ $COMPRESSED -eq 1 && $CHECK_INTEGRITY -eq 1 ]]; then
    echo -e "  Verifying archive integrity (gzip -t)..."
    if ! gzip -t "$IMAGE" 2>/dev/null; then
        echo -e "  ${RED}Archive is corrupt or truncated.${NC} Cannot scan it meaningfully."
        exit 2
    fi
    echo -e "  ${GREEN}OK${NC} archive intact"
fi

# ---------------------------------------------------------------------------
# Scan
# ---------------------------------------------------------------------------
echo -e "  Scanning (this reads the whole decompressed image)..."
set +e
"${READER[@]}" 2>/dev/null | python3 -c '
import sys

needles = []
with open(sys.argv[1], "rb") as f:
    for line in f:
        v = line.rstrip(b"\n")
        if len(v) >= 12:
            needles.append(v)
        elif v:
            print(f"  SKIPPED a {len(v)}-char secret (too short to distinguish from chance)")

if not needles:
    print("  No needle long enough to scan. Result is NOT a pass.")
    sys.exit(2)

counts = [0] * len(needles)
overlap = max(len(n) for n in needles) - 1
tail = b""
total = 0
CHUNK = 8 << 20


def count_before(buf, needle, limit):
    """Occurrences starting below limit, so tail-resident hits count once."""
    n, start = 0, 0
    while True:
        i = buf.find(needle, start)
        if i == -1 or i >= limit:
            return n
        n += 1
        start = i + 1


src = sys.stdin.buffer
while True:
    chunk = src.read(CHUNK)
    buf = tail + chunk
    last = not chunk
    limit = len(buf) if last else max(0, len(buf) - overlap)
    for i, needle in enumerate(needles):
        counts[i] += count_before(buf, needle, limit)
    if last:
        break
    total += len(chunk)
    tail = buf[-overlap:] if overlap else b""

print(f"  Scanned {total / (1<<30):.1f} GiB against {len(needles)} needle(s).")
hits = [(i, len(needles[i]), c) for i, c in enumerate(counts) if c]
for i, ln, c in hits:
    print(f"  LEAK: needle #{i+1} ({ln} chars) appears {c} time(s)")
sys.exit(1 if hits else 0)
' "$NEEDLES" 2>&1 | sed 's/^/  /'
RC=${PIPESTATUS[1]}
set -e

echo
case "$RC" in
    0)
        echo -e "${GREEN}${BOLD}  CLEAN${NC} — no operator credentials found in this image."
        ;;
    1)
        echo -e "${RED}${BOLD}  LEAK — DO NOT SHIP OR FLASH THIS IMAGE${NC}"
        echo -e "  Every card written from it carries the credential above."
        echo -e "  If cards were already flashed from it, treat those credentials as"
        echo -e "  disclosed: change the Wi-Fi password and re-issue the image."
        ;;
    *)
        echo -e "${YELLOW}${BOLD}  NOT CHECKED${NC} (scanner exited ${RC}) — this is not a pass."
        ;;
esac
echo
exit "$RC"
