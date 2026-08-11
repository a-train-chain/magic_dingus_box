#!/usr/bin/env bash
#
# Magic Dingus Box - bring the SOURCE box up to date before cutting an image
#
# Runs on the OPERATOR'S MAC. Pushes every fix that must be INSIDE the golden
# image to the source Pi, rebuilds the kiosk binary, and verifies each landed.
#
# Why a script and not a checklist: the 2026-08-04 boot-test session found
# five separate golden-image blockers, and every one of them lives in a
# different file. Missing any single file reproduces its bug on every unit
# sold. A checklist is how you miss one at 1am.
#
# Usage:
#   ./sync_source_box.sh --pi magic@magicpi5.local
#   ./sync_source_box.sh --pi magic@10.55.0.1 --no-build   # skip the rebuild
#
set -euo pipefail

PI_HOST=""
DO_BUILD=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pi)       PI_HOST="$2"; shift 2 ;;
        --no-build) DO_BUILD=0; shift ;;
        -h|--help)  sed -n 's/^# \{0,1\}//;1,/^$/p' "$0" | head -20; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done
[[ -z "$PI_HOST" ]] && { echo "--pi is required (never defaulted: the wrong box gets the wrong image)" >&2; exit 2; }

if [[ -t 1 ]]; then
    RED='\033[0;31m' GREEN='\033[0;32m' YELLOW='\033[1;33m' BOLD='\033[1m' NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' BOLD='' NC=''
fi

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SSH_OPTS=(-o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
FAILED=0

echo
echo -e "${BOLD}Syncing source box: ${PI_HOST}${NC}"
echo

# Confirm which box we are about to modify. Two Pis were live during the
# session that produced this script and they are trivially confusable.
HOSTNAME_REMOTE=$(ssh "${SSH_OPTS[@]}" "$PI_HOST" hostname 2>/dev/null) || {
    echo -e "${RED}Cannot reach ${PI_HOST}.${NC}" >&2; exit 1; }
echo -e "  target hostname: ${BOLD}${HOSTNAME_REMOTE}${NC}"
echo

# ---------------------------------------------------------------------------
# 1. Files that must be INSIDE the image
# ---------------------------------------------------------------------------
# src → dest, one per line. Everything here was changed on 2026-08-04 and
# every one of them is load-bearing for a unit that has never been booted.
push() {
    local src="$1" dest="$2" mode="${3:-755}" label="$4"
    if [[ ! -f "${REPO}/${src}" ]]; then
        echo -e "  ${RED}MISSING${NC} ${src} — not in the repo"; FAILED=1; return
    fi
    local base; base=$(basename "$dest")
    scp "${SSH_OPTS[@]}" -q "${REPO}/${src}" "${PI_HOST}:/tmp/${base}" 2>/dev/null
    ssh "${SSH_OPTS[@]}" "$PI_HOST" \
        "sudo install -D -m ${mode} -o root -g root /tmp/${base} '${dest}' && rm -f /tmp/${base}" 2>/dev/null
    local l r
    l=$(shasum -a 256 "${REPO}/${src}" | cut -c1-16)
    r=$(ssh "${SSH_OPTS[@]}" "$PI_HOST" "sha256sum '${dest}' | cut -c1-16" 2>/dev/null)
    if [[ "$l" == "$r" ]]; then
        echo -e "  ${GREEN}OK${NC}      ${label}"
    else
        echo -e "  ${RED}MISMATCH${NC} ${label} (local ${l} / pi ${r})"; FAILED=1
    fi
}

echo -e "${BOLD}[1/5] Pushing golden-image scripts${NC}"
push scripts/golden_image/first_boot.sh \
     /opt/magic_dingus_box/scripts/golden_image/first_boot.sh 755 \
     "first_boot.sh          (growpart; expansion no longer aborts boot)"
push scripts/golden_image/prepare_for_cloning.sh \
     /opt/magic_dingus_box/scripts/golden_image/prepare_for_cloning.sh 755 \
     "prepare_for_cloning.sh (reserve+boot zerofill, HUP trap, curation, fstab)"
push scripts/golden_image/restore_after_cloning.sh \
     /opt/magic_dingus_box/scripts/golden_image/restore_after_cloning.sh 755 \
     "restore_after_cloning  (starts containerd+docker before the stack)"

echo
echo -e "${BOLD}[2/5] Pushing service scripts${NC}"
push magic_dingus_box_cpp/scripts/setup_services.sh \
     /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_services.sh 755 \
     "setup_services.sh      (.env backfill, grep guard, Radarr root folder)"
push magic_dingus_box_cpp/scripts/setup_network_hardening.sh \
     /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_network_hardening.sh 755 \
     "setup_network_hardening.sh (IPv6 off + public-DNS-first, any-Wi-Fi posture)"
push magic_dingus_box_cpp/scripts/network_doctor.sh \
     /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/network_doctor.sh 755 \
     "network_doctor.sh      (connectivity ladder + plain-English verdict)"
push magic_dingus_box_cpp/scripts/data/90-magic-ipv6-disable.conf \
     /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/data/90-magic-ipv6-disable.conf 644 \
     "90-magic-ipv6-disable.conf (NM conf.d default — repo copy)"
push magic_dingus_box_cpp/scripts/data/90-magic-network-hardening \
     /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/data/90-magic-network-hardening 755 \
     "90-magic-network-hardening (NM dispatcher — repo copy)"
# Run the installer so the LIVE config (conf.d + dispatcher + repaired
# profiles) matches what the image must carry — pushing repo copies alone
# leaves /etc untouched, which is exactly the present-but-not-correct trap
# the fstab step below exists for.
if ssh "${SSH_OPTS[@]}" "$PI_HOST" \
    "sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_network_hardening.sh" 2>&1 | sed 's/^/          /'; then
    echo -e "  ${GREEN}OK${NC}      network hardening applied live (conf.d + dispatcher + profiles)"
else
    echo -e "  ${RED}FAILED${NC}  network hardening installer did not complete"; FAILED=1
fi
push magic_dingus_box_cpp/scripts/setup_memory_tuning.sh \
     /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_memory_tuning.sh 755 \
     "setup_memory_tuning.sh (kiosk MemoryLow + zram tune + cgroup cmdline)"
# Same present-but-not-correct trap as network hardening above: the repo
# copy alone leaves /etc and cmdline.txt untouched, and the image must
# carry the LIVE posture (the cmdline flags especially — a clone whose
# donor never ran this ships with the memory controller disabled and the
# whole playback protection silently inert; that is the 2026-08-11
# stutter bug shipping again). REBOOT_REQUIRED here means clone AFTER
# the source box's next reboot, never before.
if ssh "${SSH_OPTS[@]}" "$PI_HOST" \
    "sudo /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/setup_memory_tuning.sh" 2>&1 | sed 's/^/          /'; then
    echo -e "  ${GREEN}OK${NC}      memory posture applied live (MemoryLow + zram + cmdline)"
else
    echo -e "  ${RED}FAILED${NC}  memory tuning installer did not complete"; FAILED=1
fi
push magic_dingus_box_cpp/scripts/verify_services.sh \
     /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/verify_services.sh 755 \
     "verify_services.sh     (Radarr root-folder check — 15 checks)"
push magic_dingus_box_cpp/scripts/episode_priority/episode_priority.py \
     /opt/magic_dingus_box/magic_dingus_box_cpp/scripts/episode_priority/episode_priority.py 755 \
     "episode_priority.py    (orders episodes inside a season pack)"

echo
echo -e "${BOLD}[3/5] Pushing the udev mount rule${NC}"
# This one is easy to forget and its absence is silent: without it the fstab
# entry is inert and a customer's movie drive never mounts.
push magic_dingus_box_cpp/udev/99-magic-movies-mount.rules \
     /opt/magic_dingus_box/magic_dingus_box_cpp/udev/99-magic-movies-mount.rules 644 \
     "99-magic-movies-mount.rules (repo copy — shipped in the image)"
push magic_dingus_box_cpp/udev/99-magic-movies-mount.rules \
     /etc/udev/rules.d/99-magic-movies-mount.rules 644 \
     "99-magic-movies-mount.rules (live — mounts the drive on appearance)"
ssh "${SSH_OPTS[@]}" "$PI_HOST" "sudo udevadm control --reload-rules" 2>/dev/null || true

echo
echo -e "${BOLD}[4/5] Normalising /etc/fstab${NC}"
# x-systemd.automount hangs boot for 60s on a unit with NO drive -- which is
# every unit at first power-on -- until the hardware watchdog resets it.
# prepare_for_cloning.sh also normalises this, but fixing the source box
# directly means the very next image is correct even if that path changes.
# The heredoc's exit status is CAPTURED and the outcome content-verified.
# It used to run bare with stderr discarded, which made step 4 the one step
# in this script that could not fail: a sudo error or read-only /etc still
# ended at "Source box ready to clone." The remote side now ends by
# asserting the exact line is present (set -e makes any earlier failure
# propagate), mirroring how every push is hash-verified.
if ssh "${SSH_OPTS[@]}" "$PI_HOST" 'bash -s' <<'REMOTE'
set -euo pipefail
LINE='LABEL=MOVIES /mnt/ssd ext4 noauto,nofail 0 0'
if grep -qxF "$LINE" /etc/fstab; then
    echo "  OK      fstab already correct"
elif grep -q "^[^#]*LABEL=MOVIES" /etc/fstab; then
    sudo cp /etc/fstab "/etc/fstab.bak-$(date +%Y%m%d-%H%M%S)"
    # ^[^#]* guard: only ACTIVE entries are stale entries. A commented-out
    # LABEL=MOVIES line is documentation, not configuration — deleting it
    # too is harmless but surprising, so the sed matches active lines only.
    sudo sed -i '/^[^#]*LABEL=MOVIES/d' /etc/fstab
    echo "$LINE" | sudo tee -a /etc/fstab >/dev/null
    sudo systemctl daemon-reload
    echo "  FIXED   replaced a blocking MOVIES fstab entry"
else
    echo "$LINE" | sudo tee -a /etc/fstab >/dev/null
    sudo systemctl daemon-reload
    echo "  ADDED   MOVIES fstab entry (none was present)"
fi
# Outcome assertion — the reason this step exists at all.
grep -qxF "$LINE" /etc/fstab
grep -i movies /etc/fstab | sed 's/^/          /'
REMOTE
then
    :
else
    echo -e "  ${RED}FAILED${NC}  fstab normalisation did not complete on the box"
    FAILED=1
fi

echo
echo -e "${BOLD}[5/5] Kiosk source + rebuild${NC}"
push magic_dingus_box_cpp/src/retroarch/controller_detector.cpp \
     /opt/magic_dingus_box/magic_dingus_box_cpp/src/retroarch/controller_detector.cpp 644 \
     "controller_detector.cpp (skips the phone remote — REQUIRES REBUILD)"

if [[ $DO_BUILD -eq 1 ]]; then
    echo "  building on the Pi (-j2 per the build contract; several minutes)..."
    if ssh "${SSH_OPTS[@]}" "$PI_HOST" \
        "cd /opt/magic_dingus_box/magic_dingus_box_cpp/build && make -j2" 2>&1 | tail -4 | sed 's/^/    /'; then
        # A zero exit from make is not proof the change is IN the binary --
        # a stale object or a skipped rebuild both exit 0. Assert on content.
        # `|| true` INSIDE the remote command, not `|| echo 0` outside it:
        # grep -c prints "0" AND exits 1 on zero matches, so the outer
        # fallback appended a second line ("0\n0") and the -ge below then
        # blew up in arithmetic context instead of comparing.
        n=$(ssh "${SSH_OPTS[@]}" "$PI_HOST" \
            "strings /opt/magic_dingus_box/magic_dingus_box_cpp/build/magic_dingus_box_cpp 2>/dev/null | grep -c 'is the phone remote' || true" 2>/dev/null)
        n="${n:-0}"
        if [[ "${n:-0}" -ge 2 ]]; then
            echo -e "  ${GREEN}OK${NC}      binary contains the phone-remote skip (both paths)"
        else
            echo -e "  ${RED}FAILED${NC}  binary does NOT contain the skip (found ${n}, want 2)"; FAILED=1
        fi
    else
        echo -e "  ${RED}BUILD FAILED${NC}"; FAILED=1
    fi
else
    echo -e "  ${YELLOW}SKIPPED${NC} rebuild (--no-build) — the image will NOT have the controller fix"
fi

echo
echo "════════════════════════════════════════════════════════════"
if [[ $FAILED -eq 0 ]]; then
    echo -e "  ${GREEN}${BOLD}Source box ready to clone.${NC}"
    echo "  Next: ./clone_live_sd.sh --pi ${PI_HOST} --output <path>.img.gz"
else
    echo -e "  ${RED}${BOLD}NOT READY — fix the items above before cloning.${NC}"
fi
echo "════════════════════════════════════════════════════════════"
echo
exit $FAILED
