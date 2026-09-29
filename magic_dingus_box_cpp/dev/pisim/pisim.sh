#!/usr/bin/env bash
# Pi-userspace dev loop: build + test the REAL kiosk binary in a native
# arm64 Debian Trixie container on the Mac, then (optionally) push the
# binary to a box — no on-Pi compile. See dev/pisim/README.md.
#
#   dev/pisim/pisim.sh image           # (re)build the container image
#   dev/pisim/pisim.sh build           # kiosk binary + all suites, MB on
#   dev/pisim/pisim.sh test            # ctest (9 suites) + bats local tier + web pytest
#   dev/pisim/pisim.sh check           # build + test (the pre-push gate)
#   PI_HOST=magic@magicpi-ab12.local dev/pisim/pisim.sh push
#   dev/pisim/pisim.sh shell           # interactive shell in the container
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
IMAGE="${PISIM_IMAGE:-mdb-pisim:trixie}"
BUILD_DIR="magic_dingus_box_cpp/build-pisim"   # gitignored by build-*/
BINARY="${REPO_ROOT}/${BUILD_DIR}/magic_dingus_box_cpp"
PI_DIR="${PI_DIR:-/opt/magic_dingus_box}"

die() { echo "pisim: $*" >&2; exit 1; }

need_docker() {
    docker info >/dev/null 2>&1 || die "Docker daemon not running (start Docker Desktop / colima / OrbStack)"
}

ensure_image() {
    docker image inspect "${IMAGE}" >/dev/null 2>&1 || cmd_image
}

in_container() {
    # -t only when attached to a terminal so the script also works in CI/pipes.
    local tty=()
    [[ -t 0 && -t 1 ]] && tty=(-t)
    docker run --rm -i ${tty[@]+"${tty[@]}"} --platform linux/arm64 \
        -v "${REPO_ROOT}:/repo" -w /repo "${IMAGE}" bash -c "$1"
}

cmd_image() {
    need_docker
    docker build --platform linux/arm64 -t "${IMAGE}" "${SCRIPT_DIR}"
}

cmd_build() {
    need_docker; ensure_image
    in_container "
        set -euo pipefail
        cmake -S magic_dingus_box_cpp -B ${BUILD_DIR} -G Ninja \
            -DCMAKE_BUILD_TYPE=Release -DBUILD_KIOSK=ON \
            -DENABLE_MEDIA_BROWSER=ON -DBUILD_TESTS=ON
        # One job per ~1.2 GB of the Docker VM's RAM: main.cpp/renderer.cpp
        # peak past 1 GB each, and a 10-job build on Docker Desktop's
        # default 8 GB VM gets OOM-killed. PISIM_JOBS overrides.
        jobs=${PISIM_JOBS:-\$(( \$(awk '/MemTotal/{print \$2}' /proc/meminfo) / 1200000 ))}
        [ \"\$jobs\" -ge 1 ] || jobs=1
        [ \"\$jobs\" -le \$(nproc) ] || jobs=\$(nproc)
        echo \"pisim: building with -j\$jobs\"
        cmake --build ${BUILD_DIR} -j\$jobs
        b=${BUILD_DIR}/magic_dingus_box_cpp
        # Same release-blocking assertions as release.yml — a binary that
        # silently compiled out a load-bearing capability must not ship.
        # Dump strings to a file first: under pipefail, `strings | grep -q`
        # fails spuriously when grep exits early and strings takes SIGPIPE.
        strings \$b > /tmp/kiosk.strings
        file \$b | grep -q aarch64                 || { echo 'FATAL: not aarch64'; exit 1; }
        grep -q 'READY=1' /tmp/kiosk.strings       || { echo 'FATAL: no sd_notify (libsystemd-dev?)'; exit 1; }
        grep -qi prowlarr /tmp/kiosk.strings       || { echo 'FATAL: Media Browser not compiled in'; exit 1; }
        ! grep -q 'compiled without HAVE_GPIOD' /tmp/kiosk.strings \
                                                    || { echo 'FATAL: GPIO compiled out (libgpiod-dev?)'; exit 1; }
        echo 'pisim: kiosk binary OK (aarch64, sd_notify, Media Browser, GPIO)'
    "
}

cmd_test() {
    need_docker; ensure_image
    [[ -d "${REPO_ROOT}/${BUILD_DIR}" ]] || cmd_build
    in_container "
        set -euo pipefail
        ctest --test-dir ${BUILD_DIR} --output-on-failure
        ./tests/run_local_tests.sh
        python3 -m pytest magic_dingus_box/web/tests/ -q
    "
}

cmd_push() {
    [[ -n "${PI_HOST:-}" ]] || die "set PI_HOST explicitly (e.g. PI_HOST=magic@magicpi-ab12.local) — never defaulted, two boxes are often reachable"
    [[ -x "${BINARY}" ]] || die "no binary at ${BINARY} — run 'pisim.sh build' first"

    echo "Target: ${PI_HOST}"
    ssh "${PI_HOST}" 'echo "  hostname: $(hostname)"; echo "  model:    $(tr -d "\0" </proc/device-tree/model)"'

    local dest="${PI_DIR}/magic_dingus_box_cpp/build/magic_dingus_box_cpp"
    rsync -z --checksum "${BINARY}" "${PI_HOST}:${dest}.new"

    # Verify on the box BEFORE swapping: every shared library resolves
    # against the box's own Trixie libs. A container/box library skew shows
    # up here as "not found" instead of as a dead kiosk.
    # The restart mirrors deploy_cpp.sh's stop → wait → reset-failed →
    # start sequence (plain `systemctl restart` races the old process for
    # DRM master; see the rationale there). The previous binary is kept as
    # .prev and restored automatically if the new one does not come up.
    ssh "${PI_HOST}" DEST="${dest}" bash <<'EOF'
set -euo pipefail
if ldd "${DEST}.new" | grep -q 'not found'; then
    echo "  ✗ binary has unresolved libraries on this box:"; ldd "${DEST}.new" | grep 'not found'
    rm -f "${DEST}.new"; exit 1
fi
chmod +x "${DEST}.new"
svc=magic-dingus-box-cpp.service
restart() {
    sudo systemctl stop "$svc" 2>/dev/null || true
    for i in $(seq 1 30); do systemctl is-active --quiet "$svc" || break; sleep 0.5; done
    sleep 2
    sudo systemctl reset-failed "$svc" 2>/dev/null || true
    sudo systemctl start "$svc" 2>/dev/null || true
    for i in $(seq 1 60); do
        systemctl is-active --quiet "$svc" && return 0
        sleep 0.5
    done
    return 1
}
[ -f "${DEST}" ] && cp -p "${DEST}" "${DEST}.prev"
mv -f "${DEST}.new" "${DEST}"
if restart; then
    echo "  ✓ kiosk active on new binary"
else
    echo "  ✗ kiosk did not come up. Last journal lines:"
    journalctl -u "$svc" --no-pager -n 20 2>&1 | tail -20
    if [ -f "${DEST}.prev" ]; then
        echo "  ↩ restoring previous binary"
        mv -f "${DEST}.prev" "${DEST}"
        restart && echo "  ✓ previous binary restored and active"
    fi
    exit 1
fi
EOF
}

cmd_shell() {
    need_docker; ensure_image
    docker run --rm -it --platform linux/arm64 -v "${REPO_ROOT}:/repo" -w /repo "${IMAGE}" bash
}

case "${1:-}" in
    image) cmd_image ;;
    build) cmd_build ;;
    test)  cmd_test ;;
    check) cmd_build; cmd_test ;;
    push)  cmd_push ;;
    shell) cmd_shell ;;
    *) sed -n '2,11p' "$0"; exit 2 ;;
esac
