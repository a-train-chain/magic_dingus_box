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
    # Each line is SOURCE<TAB>VALUE. The source label is what makes a finding
    # actionable ("services/.env key RADARR_API_KEY appears 3 times") without
    # ever printing the value itself.
    #
    # Non-secret .env keys are skipped deliberately. Harvesting every value
    # regardless of key put TZ in the needle set, which matches /etc/timezone
    # and six Docker config.v2.json files on any correct image -- guaranteed
    # hits that bury a real one.
    #
    # SSH host keys are labelled `expected:` because prepare_for_cloning.sh
    # leaves them in place ON PURPOSE (sshd holds the operator's live session).
    # They are still scanned -- if that design decision changes, the gate
    # notices immediately -- but they are reported separately instead of
    # turning every correct image red.
    ssh -o ConnectTimeout=10 "$PI_HOST" '
        sudo grep -hoE "^ssid=.+" /etc/NetworkManager/system-connections/*.nmconnection 2>/dev/null | sed "s/^ssid=/wifi-ssid\t/"
        sudo grep -hoE "^psk=.+"  /etc/NetworkManager/system-connections/*.nmconnection 2>/dev/null | sed "s/^psk=/wifi-psk\t/"
        sudo grep -hoE "^[A-Za-z_][A-Za-z0-9_]*=.+" /opt/magic_dingus_box/services/.env 2>/dev/null \
          | grep -vE "^(PUID|PGID|TZ|STORAGE_ROOT|VPN_TYPE|VPN_SERVICE_PROVIDER|VPN_COUNTRIES|WIREGUARD_ADDRESSES|WIREGUARD_ENDPOINT_IP|WIREGUARD_ENDPOINT_PORT|WIREGUARD_PUBLIC_KEY|FIREWALL_OUTBOUND_SUBNETS|DOT|COMPOSE_PROJECT_NAME)=" \
          | sed "s/^\([A-Za-z_][A-Za-z0-9_]*\)=/env:\1\t/"
        sudo sed "s/^/flask-secret\t/" /opt/magic_dingus_box/magic_dingus_box_cpp/data/flask_secret.key 2>/dev/null
        sudo grep -h -v -e "-----" /etc/ssh/ssh_host_*_key 2>/dev/null | sed "s/^/expected:ssh-host-key\t/"
    ' 2>/dev/null \
        | sed -e '/\t[[:space:]]*$/d' -e 's/[[:space:]]*$//' -e '/^$/d' \
        | sort -u > "$TMP_NEEDLES" || true
    # Order matters in that sed: the empty-VALUE filter must run BEFORE the
    # trailing-whitespace strip. The old order stripped the tab first, so
    # `label<TAB>` could never match /\t$/ — the line survived as a bare
    # label, which the Python side then treated as an UNLABELLED NEEDLE whose
    # value is the label text itself (and an `expected:` prefix demoted into
    # the value would report as LEAK). Noise, not a false CLEAN, but noise in
    # the one gate whose alarms must stay meaningful.
    NEEDLES="$TMP_NEEDLES"
fi

# A needles file authored on Windows, or pasted out of Notes or a spreadsheet,
# carries CR. Left in place it glues 0x0d to every value, nothing matches, and
# the verdict is a green CLEAN -- a silent false clean, the one direction this
# gate must never fail in.
if [[ -n "$NEEDLES" && -f "$NEEDLES" ]] && LC_ALL=C grep -q $'\r' "$NEEDLES" 2>/dev/null; then
    echo -e "  ${YELLOW}NOTE${NC} needles file contains CR characters — normalising (CRLF would silently match nothing)"
    NORMALISED=$(mktemp "${TMPDIR:-/tmp}/mdb-needles-lf.XXXXXX")
    chmod 600 "$NORMALISED"
    tr -d '\r' < "$NEEDLES" > "$NORMALISED"
    [[ -n "$TMP_NEEDLES" ]] && rm -f "$TMP_NEEDLES"
    TMP_NEEDLES="$NORMALISED"
    NEEDLES="$NORMALISED"
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
# Framings a short secret takes on disk. Split by family so an SSID is looked
# for in SSID syntax and a PSK in password syntax -- the previous list was all
# password/psk framings, so a short SSID was undetectable by either scanner
# even though the v1.9.3 leak included three fragments naming the SSID.
PSK_CONTEXTS = [
    b"psk=", b"password=", b"passwd=", b"pass=",
    b"'password': '", b'"password": "',
    b"'psk': '", b'"psk": "',
    b"password: ", b"psk: ",
]
SSID_CONTEXTS = [
    b"ssid=", b"SSID=", b"'ssid': '", b'"ssid": "', b"ssid: ",
    b"'access-points': {'", b'"access-points": {"',
]
DEFAULT_CONTEXTS = PSK_CONTEXTS + SSID_CONTEXTS


def contexts_for(source):
    if source.endswith("wifi-ssid"):
        return SSID_CONTEXTS
    if source.endswith("wifi-psk"):
        return PSK_CONTEXTS
    return DEFAULT_CONTEXTS


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


# Input is SOURCE<TAB>VALUE. A bare line (no tab) is still accepted so a
# hand-written --needles file keeps working; it is simply labelled "unlabelled".
raw_values = []
with open(sys.argv[1], "rb") as fh:
    for raw in fh:
        line = raw.rstrip(b"\r\n")          # \r matters: see the CRLF note above
        if not line:
            continue
        source, _, value = line.partition(b"\t")
        if not value:
            source, value = b"unlabelled", line
        src = source.decode("utf-8", "replace")
        for form in variants_of(value):
            if (src, form) not in raw_values:
                raw_values.append((src, form))

needles, labels, expected_flags = [], [], []
for src, v in raw_values:
    is_expected = src.startswith("expected:")
    shown = src[len("expected:"):] if is_expected else src
    if len(v) >= MIN_LEN:
        needles.append(v)
        labels.append("%s (%d chars)" % (shown, len(v)))
        expected_flags.append(is_expected)
    else:
        ctxs = contexts_for(src)
        variants = [c + v for c in ctxs if len(c + v) >= MIN_LEN]
        for var in variants:
            ctx = var[:len(var) - len(v)].decode("utf-8", "replace")
            needles.append(var)
            labels.append("%s (%d chars) framed as [%s]" % (shown, len(v), ctx))
            expected_flags.append(is_expected)
        print("  %s: %d chars, too short to scan alone — expanded into %d framed needle(s)"
              % (shown, len(v), len(variants)))

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
# Split real findings from the ones the current design guarantees. SSH host
# keys are in the image ON PURPOSE; reporting them as LEAK made every correct
# image red, and one extra unlabelled line was what a genuine WireGuard-key
# hit would have looked like.
real = [(i, c) for i, c in enumerate(counts) if c and not expected_flags[i]]
expected_hits = [(i, c) for i, c in enumerate(counts) if c and expected_flags[i]]

for i, c in expected_hits:
    print("  expected-present: %s appears %d time(s) "
          "(left in the image by design; first_boot regenerates it per unit)"
          % (labels[i], c))
for i, c in real:
    print("  LEAK: %s appears %d time(s)" % (labels[i], c))
hits = real

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
