#!/usr/bin/env bash
# Off-box reproduction: GstPlayer's stall reload while the media file's
# reads take seconds (an SD card saturated by writes). See README.md.
#
#   dev/sd_stall_repro/run.sh                    # this checkout, both scenarios
#   dev/sd_stall_repro/run.sh --ref origin/main  # GstPlayer from a git ref
#   dev/sd_stall_repro/run.sh -- 8000 0          # one scenario: read ms, open ms
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
REPO_ROOT="$(cd "${CPP_DIR}/.." && pwd)"
IMAGE="${SD_REPRO_IMAGE:-mdb-sdrepro:trixie}"
BASE="${PISIM_IMAGE:-mdb-pisim:trixie}"
REF=""
SCENARIOS=("15000 0" "6000 6000")

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ref) REF="$2"; shift 2 ;;
        --) shift; SCENARIOS=("$*"); break ;;
        *) echo "usage: $0 [--ref <git-ref>] [-- <read_ms> <open_ms>]" >&2; exit 2 ;;
    esac
done

docker info >/dev/null 2>&1 || { echo "sd_stall_repro: Docker is not running" >&2; exit 1; }
docker image inspect "${BASE}" >/dev/null 2>&1 || "${CPP_DIR}/dev/pisim/pisim.sh" image
docker image inspect "${IMAGE}" >/dev/null 2>&1 ||
    docker build --platform linux/arm64 --build-arg BASE="${BASE}" -t "${IMAGE}" "${SCRIPT_DIR}"

# The GstPlayer sources under test: this checkout's, or a git ref's.
SRC="$(mktemp -d)"
trap 'rm -rf "${SRC}"' EXIT
if [[ -n "${REF}" ]]; then
    mkdir -p "${SRC}/video"
    git -C "${REPO_ROOT}" archive "${REF}" magic_dingus_box_cpp/src/video |
        tar -x -C "${SRC}" --strip-components=2
else
    cp -R "${CPP_DIR}/src/video" "${SRC}/video"
fi

docker run --rm -i --platform linux/arm64 \
    -v "${SRC}:/src/video-under-test:ro" -v "${SCRIPT_DIR}:/repro:ro" \
    "${IMAGE}" bash -s -- "${SCENARIOS[@]}" <<'IN_CONTAINER'
set -euo pipefail
mkdir -p /build/src /media
cp -R /src/video-under-test/video /build/src/video
cp -R /repro/stub/utils /build/src/utils
# A 60 s 720p H.264 + AAC MP4, keyframe every 2 s (the transcoder's GOP).
gst-launch-1.0 -q videotestsrc num-buffers=1800 \
    ! video/x-raw,width=1280,height=720,framerate=30/1 \
    ! x264enc key-int-max=60 bitrate=3000 speed-preset=ultrafast ! mp4mux name=m \
    ! filesink location=/media/a.mp4 \
    audiotestsrc num-buffers=2600 ! audioconvert ! avenc_aac ! m.
g++ -std=c++17 -O1 -g -rdynamic -I/build/src -o /build/harness /repro/harness.cpp \
    /build/src/video/gst_player.cpp \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0) -ldl -pthread
for scenario in "$@"; do
    # shellcheck disable=SC2086  # "read_ms open_ms"
    set -- $scenario
    echo "=== media reads ${1} ms, opens ${2} ms"
    /build/harness /media/a.mp4 "$1" "$2" 2>&1 | grep -v '^\[warn\]' || true
done
IN_CONTAINER
