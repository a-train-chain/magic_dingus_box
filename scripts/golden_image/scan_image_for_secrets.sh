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
# Values are never printed. A finding is reported by kind, length and hit
# count -- enough to act on, never enough to leak.
#
# Usage:
#   ./scan_image_for_secrets.sh --image ~/golden.img.gz --pi magic@10.55.0.1
#   ./scan_image_for_secrets.sh --image ~/golden.img.gz --needles ~/secrets.txt
#
#   --pi HOST        harvest needles from a live box over SSH (Wi-Fi SSID +
#                    PSK, every services/.env value, the Flask HMAC secret,
#                    SSH host private keys)
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
EXPECT_BYTES=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --image)          IMAGE="$2"; shift 2 ;;
        --pi)             PI_HOST="$2"; shift 2 ;;
        --needles)        NEEDLES="$2"; shift 2 ;;
        --skip-integrity) CHECK_INTEGRITY=0; shift ;;
        --expect-bytes)   EXPECT_BYTES="$2"; CHECK_INTEGRITY=0; shift 2 ;;
        -h|--help)        sed -n 's/^# \{0,1\}//;1,/^$/p' "$0" | head -32; exit 0 ;;
        *)                echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [[ -t 1 ]]; then
    RED='\033[0;31m' GREEN='\033[0;32m' YELLOW='\033[1;33m' BOLD='\033[1m' NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' BOLD='' NC=''
fi

[[ -z "$IMAGE" ]]  && { echo "--image is required" >&2; exit 2; }
[[ -f "$IMAGE" ]]  || { echo "No such image: $IMAGE" >&2; exit 2; }
if [[ -z "$PI_HOST" && -z "$NEEDLES" ]]; then
    echo "One of --pi or --needles is required (nothing to scan for otherwise)" >&2
    exit 2
fi

TMP_NEEDLES=""
PY_SCANNER=""
cleanup() {
    [[ -n "$TMP_NEEDLES" ]] && rm -f "$TMP_NEEDLES"
    [[ -n "$PY_SCANNER"  ]] && rm -f "$PY_SCANNER"
    return 0
}
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
    # Harvest EVERY secret the box holds, not a hand-picked few. The point of
    # this scan is to prove the free-space zeroing reached the blocks freed
    # when prepare_for_cloning.sh stashed ~257 secret files immediately before
    # the dd. Checking only the Wi-Fi PSK proves that for one file and says
    # nothing about the other 256.
    #
    # Every VALUE in services/.env is taken, whatever its key, so a newly
    # added credential is covered without anyone remembering to update this
    # list -- a list going stale is precisely how the scrub failed before.
    ssh -o ConnectTimeout=10 "$PI_HOST" '
        sudo grep -hoE "^(ssid|psk)=.+" /etc/NetworkManager/system-connections/*.nmconnection 2>/dev/null | sed "s/^[a-z]*=//"
        sudo grep -hoE "^[A-Za-z_][A-Za-z0-9_]*=.+" /opt/magic_dingus_box/services/.env 2>/dev/null | sed "s/^[^=]*=//"
        sudo cat /opt/magic_dingus_box/magic_dingus_box_cpp/data/flask_secret.key 2>/dev/null
        sudo grep -h -v -e "-----" /etc/ssh/ssh_host_*_key 2>/dev/null
    ' 2>/dev/null \
        | sed -e 's/[[:space:]]*$//' \
        | grep -v '^$' | sort -u > "$TMP_NEEDLES" || true
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

if [[ "$EXPECT_BYTES" != "0" ]]; then
    echo -e "  ${GREEN}OK${NC} integrity will be checked by asserting the decompressed"
    echo -e "     size is exactly ${EXPECT_BYTES} bytes (stronger than gzip -t, and free)"
fi

if [[ $COMPRESSED -eq 1 && $CHECK_INTEGRITY -eq 1 ]]; then
    echo -e "  Verifying archive integrity (gzip -t)..."
    if ! gzip -t "$IMAGE" 2>/dev/null; then
        echo -e "  ${RED}Archive is corrupt or truncated.${NC} Cannot scan it meaningfully."
        exit 2
    fi
    echo -e "  ${GREEN}OK${NC} archive intact"
fi

# ---------------------------------------------------------------------------
# The scanner
# ---------------------------------------------------------------------------
# Written to a temp file from a QUOTED heredoc rather than passed inline to
# `python3 -c`. An inline single-quoted shell string cannot contain a single
# quote, and the scanner legitimately needs them -- it searches for byte
# patterns like 'password': '. Embedding it inline silently truncated the
# program at the first apostrophe.
PY_SCANNER=$(mktemp "${TMPDIR:-/tmp}/mdb-scan.XXXXXX")
cat > "$PY_SCANNER" <<'PYEOF'
import sys

# A short secret cannot be searched for on its own: this box's Wi-Fi PSK is
# 8 characters, and an 8-character string matched ~17000 times in a 60 GB
# image purely by chance (a descending control sequence hit 516 times).
# Skipping it outright, though, means the gate never looks for the one
# credential that actually shipped.
#
# So expand a short secret into the CONTEXTUAL forms it takes on disk. The
# v1.9.3 leak was a cloud-init log line rendering a Python dict:
#     'password': '<psk>'
# which is unambiguous at 22 characters. These prefixes cover NetworkManager
# keyfiles, cloud-init / JSON / YAML renderings, and shell-style env files.
SHORT_CONTEXTS = [
    b"psk=", b"password=", b"passwd=", b"pass=",
    b"'password': '", b'"password": "',
    b"'psk': '", b'"psk": "',
    b"password: ", b"psk: ",
]
MIN_LEN = 12

def variants_of(v):
    """The forms this value could take on disk.

    .env may quote a value while the image holds it raw, so both are worth
    searching for. Stripping in the shell instead would MANGLE a secret that
    legitimately contains a quote, turning a real hit into a miss -- the one
    direction this gate must never fail in.
    """
    out = [v]
    stripped = v.strip(b"\"'")
    if stripped and stripped != v:
        out.append(stripped)
    return out


raw_values = []
with open(sys.argv[1], "rb") as fh:
    for raw in fh:
        line = raw.rstrip(b"\n")
        if line:
            for form in variants_of(line):
                if form not in raw_values:
                    raw_values.append(form)

needles, labels = [], []
for v in raw_values:
    if len(v) >= MIN_LEN:
        needles.append(v)
        labels.append("a %d-char secret" % len(v))
    else:
        variants = [c + v for c in SHORT_CONTEXTS if len(c + v) >= MIN_LEN]
        for var in variants:
            ctx = var[:len(var) - len(v)].decode("utf-8", "replace")
            needles.append(var)
            labels.append("a %d-char secret preceded by [%s]" % (len(v), ctx))
        print("  %d-char secret expanded into %d contextual needle(s)"
              % (len(v), len(variants)))

if not needles:
    print("  No needle long enough to scan. Result is NOT a pass.")
    sys.exit(2)

counts = [0] * len(needles)
overlap = max(len(n) for n in needles) - 1
tail = b""
total = 0
CHUNK = 8 << 20


def count_before(buf, needle, limit):
    """Occurrences starting below limit, so a tail-resident hit counts once.

    The carried tail makes boundary-straddling hits visible, but a hit lying
    wholly inside that tail would otherwise be seen twice: once here and
    once next round when the tail is prepended.
    """
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

print("  Scanned %.1f GiB against %d needle(s)." % (total / float(1 << 30), len(needles)))

# Report findings BEFORE any verdict -- a hit is definitive evidence and must
# not be swallowed by an incomplete-scan exit.
hits = [(i, c) for i, c in enumerate(counts) if c]
for i, c in hits:
    print("  LEAK: %s appears %d time(s)" % (labels[i], c))

# A decompressor that dies mid-stream hands the scanner a truncated image, in
# which it finds nothing and would report a clean PASS -- a false clean, the
# one direction this gate must never fail in.
expected = int(sys.argv[2]) if len(sys.argv) > 2 else 0
if expected and total != expected:
    print("  SCAN INCOMPLETE: read %d bytes, expected %d." % (total, expected))
    print("  The image is truncated or the decompressor failed. NOT a pass.")
    sys.exit(2)

sys.exit(1 if hits else 0)
PYEOF

python3 -c "compile(open('$PY_SCANNER').read(), 'scanner', 'exec')" 2>/dev/null || {
    echo -e "  ${RED}Internal error: scanner failed to compile.${NC}" >&2
    exit 2
}

# ---------------------------------------------------------------------------
# Scan
# ---------------------------------------------------------------------------
echo -e "  Scanning (this reads the whole decompressed image)..."
set +e
"${READER[@]}" 2>/dev/null | python3 "$PY_SCANNER" "$NEEDLES" "$EXPECT_BYTES" 2>&1 | sed 's/^/  /'
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
        echo -e "  If cards were already flashed from it, treat that credential as"
        echo -e "  disclosed: rotate it and re-issue the image."
        ;;
    *)
        echo -e "${YELLOW}${BOLD}  NOT CHECKED${NC} (scanner exited ${RC}) — this is not a pass."
        ;;
esac
echo
exit "$RC"
