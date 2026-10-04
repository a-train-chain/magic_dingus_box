#!/usr/bin/env bash
# OTA rehearsal: a box running an OLD release (default v1.9.14) — with its
# OLD update.sh and OLD web admin — discovers, downloads, installs, verifies
# and rolls back the NEW release (default HEAD, i.e. the v1.10.0 release
# commit), entirely inside native arm64 Debian Trixie containers.
#
# Nothing is published, no tag is created, no Raspberry Pi is touched, and
# GitHub is never contacted: inside the box container github.com,
# api.github.com and the asset CDN hosts resolve to a local fake GitHub
# (box/fake_github.py) behind a container-local CA. The artifacts it serves
# are built by the SAME shell the release workflow runs — the steps are
# lifted out of .github/workflows/release.yml by extract_release_steps.py.
#
#   tests/ota_rehearsal/run.sh                 # everything (artifacts, image, all scenarios)
#   tests/ota_rehearsal/run.sh artifacts       # release artifacts only
#   tests/ota_rehearsal/run.sh image           # box image only
#   tests/ota_rehearsal/run.sh scenario old_cli    # one scenario (see box/scenario.sh)
#
# Scenarios (box/scenario.sh):
#   old_web   OLD web admin drives check + install (exact v1.9.14 HTTP/CLI path),
#             then the NEW web admin's Rollback button (what an owner would click)
#   old_cli   OLD update.sh check/install with full logs, deep assertions, then
#             rollback with the OLD update.sh (the backup's copy)
#   new_path  from v1.10.0, the NEW update.sh: source-only 1.10.1 (build.new +
#             verify + promote), rollback, simulated power cut + `recover`,
#             interrupted-install-then-install
#
# Env:
#   OTA_WORK        work dir (default: $TMPDIR/mdb-ota-rehearsal); logs land in $OTA_WORK/logs
#   OTA_OLD_REF     default v1.9.14         OTA_NEW_REF  default HEAD
#   OTA_BIN_IMAGE   image the release binary is built in (default mdb-pisim:trixie;
#                   arm64v8/debian:trixie reproduces release.yml exactly)
#   OTA_BUILD_CPUS  cpuset for the binary build (default 0-3: nproc=4, like the
#                   GitHub runner, and inside Docker Desktop's 8 GB VM)
#   OTA_REBUILD=1   rebuild artifacts even if present
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
WORK="${OTA_WORK:-${TMPDIR:-/tmp}/mdb-ota-rehearsal}"
WORK="${WORK%/}"
OLD_REF="${OTA_OLD_REF:-v1.9.14}"
NEW_REF="${OTA_NEW_REF:-HEAD}"
PISIM_IMAGE="${PISIM_IMAGE:-mdb-pisim:trixie}"
BIN_IMAGE="${OTA_BIN_IMAGE:-${PISIM_IMAGE}}"
BOX_IMAGE="${OTA_BOX_IMAGE:-mdb-ota-box:trixie}"
BUILD_CPUS="${OTA_BUILD_CPUS:-0-3}"

say() { printf '\n\033[1;34m[ota-rehearsal]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[ota-rehearsal] FATAL:\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "${WORK}/logs" "${WORK}/release" "${WORK}/steps"

git_rev() { git -C "${REPO}" rev-parse --verify "$1^{commit}"; }

# release.yml's build-source checkout runs with lfs: true (and refuses LFS
# pointers in the tarball), so the archive must carry real LFS content too.
# `git archive` emits the blobs as stored (pointers); materialise them by
# overlaying the working tree's LFS files, which `git lfs pull` populated.
git_archive() {
    local tmp; tmp="$(mktemp -d)"
    git -C "${REPO}" archive --format=tar "$1" | tar -x -C "${tmp}"
    while IFS= read -r f; do
        [ -f "${REPO}/${f}" ] && cp "${REPO}/${f}" "${tmp}/${f}"
    done < <(git -C "${REPO}" lfs ls-files -n "$1" 2>/dev/null || true)
    tar -C "${tmp}" -cf "$2" .
    rm -rf "${tmp}"
}

ensure_pisim() {
    docker image inspect "${PISIM_IMAGE}" >/dev/null 2>&1 \
        || docker build --platform linux/arm64 -t "${PISIM_IMAGE}" "${REPO}/magic_dingus_box_cpp/dev/pisim"
}

in_pisim() {  # in_pisim <script> — runs with ${WORK} at /w
    docker run --rm --platform linux/arm64 -v "${WORK}:/w" "${PISIM_IMAGE}" bash -c "$1"
}

# --- release artifacts ------------------------------------------------------

# extract the release workflow's shell for VERSION into steps/<VERSION>/
extract_steps() {
    local ver="$1"
    mkdir -p "${WORK}/steps/${ver}"
    cp "${REPO}/.github/workflows/release.yml" "${WORK}/steps/release.yml"
    cp "${HERE}/extract_release_steps.py" "${WORK}/steps/"
    in_pisim "python3 /w/steps/extract_release_steps.py /w/steps/release.yml ${ver} /w/steps/${ver}"
}

# build-source job, verbatim, for VERSION from a checkout of NEW_REF.
build_source_tarball() {
    local ver="$1" out="${WORK}/release/v$1" sentinel="${2:-}"
    mkdir -p "${out}"
    [[ -f "${out}/magic-dingus-box-${ver}.tar.gz" && -z "${OTA_REBUILD:-}" ]] && { say "source tarball ${ver}: cached"; return 0; }
    say "build-source (release.yml) for ${ver}"
    in_pisim "
        set -euo pipefail
        rm -rf /tmp/ci && mkdir -p /tmp/ci && tar --no-same-permissions -xf /w/new.tar -C /tmp/ci && cd /tmp/ci  # umask 022, like actions/checkout
        # Derived (fake) releases carry a sentinel so a half-installed or
        # restored tree can be told apart from the real release's tree.
        [ -z '${sentinel}' ] || echo '${ver}' > magic_dingus_box_cpp/REHEARSAL_RELEASE
        for s in /w/steps/${ver}/source_*.sh; do
            echo \"--- \$(basename \$s)\"
            bash --noprofile --norc -eo pipefail \"\$s\" > /tmp/step.log 2>&1 || { cat /tmp/step.log; exit 1; }
            tail -3 /tmp/step.log
        done
        cp magic-dingus-box-${ver}.tar.gz checksum.sha256 /w/release/v${ver}/
    " 2>&1 | tee "${WORK}/logs/build_source_${ver}.log"
}

# build-arm64 job: the extracted inner script, verbatim, then "Package binary".
build_binary_tarball() {
    local ver="$1" out="${WORK}/release/v$1"
    mkdir -p "${out}"
    [[ -f "${out}/magic_dingus_box_cpp-arm64-${ver}.tar.gz" && -z "${OTA_REBUILD:-}" ]] && { say "binary ${ver}: cached"; return 0; }
    say "build-arm64 (release.yml inner script) in ${BIN_IMAGE}, cpuset ${BUILD_CPUS} — several minutes"
    docker run --rm --platform linux/arm64 --cpuset-cpus "${BUILD_CPUS}" \
        -v "${WORK}:/w" "${BIN_IMAGE}" bash -c "
            set -uo pipefail
            mkdir -p /workspace && tar -xf /w/new.tar -C /workspace && cd /workspace
            # CI runs this as \`bash -c '<script>'\`: no -e, the assertions gate it.
            bash --noprofile --norc /w/steps/${ver}/binary_inner.sh || exit \$?
            cd /workspace && bash --noprofile --norc -eo pipefail /w/steps/${ver}/package_binary.sh
            cp /workspace/magic_dingus_box_cpp-arm64-${ver}.tar.gz /w/release/v${ver}/
        " > "${WORK}/logs/build_binary_${ver}.log" 2>&1 \
        || { tail -40 "${WORK}/logs/build_binary_${ver}.log"; die "binary build failed (log: ${WORK}/logs/build_binary_${ver}.log)"; }
    grep -E "Binary assertions passed|aarch64" "${WORK}/logs/build_binary_${ver}.log" | tail -3 >&2
}

# A derived release from the same tree: VERSION bumped by the same
# build-source steps; the binary asset (if wanted) is the NEW_VER binary
# repackaged under the new version's asset name.
derive_release() {
    local ver="$1" with_binary="$2" base="$3"
    extract_steps "${ver}"
    build_source_tarball "${ver}" sentinel
    if [[ "${with_binary}" == "yes" ]]; then
        in_pisim "
            set -euo pipefail
            rm -rf /tmp/b && mkdir /tmp/b && tar -xzf /w/release/v${base}/magic_dingus_box_cpp-arm64-${base}.tar.gz -C /tmp/b
            cd /tmp/b && tar -czf /w/release/v${ver}/magic_dingus_box_cpp-arm64-${ver}.tar.gz magic_dingus_box_cpp
        "
    else
        rm -f "${WORK}/release/v${ver}/magic_dingus_box_cpp-arm64-${ver}.tar.gz"
    fi
}

cmd_artifacts() {
    ensure_pisim
    local new_sha old_sha
    new_sha="$(git_rev "${NEW_REF}")"; old_sha="$(git_rev "${OLD_REF}")"
    NEW_VER="$(git -C "${REPO}" show "${new_sha}:VERSION" | tr -d '[:space:]')"
    OLD_VER="$(git -C "${REPO}" show "${old_sha}:VERSION" | tr -d '[:space:]')"
    say "old ${OLD_REF} = ${old_sha:0:7} (VERSION ${OLD_VER}); new ${NEW_REF} = ${new_sha:0:7} (VERSION ${NEW_VER})"
    printf 'OLD_VER=%s\nNEW_VER=%s\nOLD_SHA=%s\nNEW_SHA=%s\n' "${OLD_VER}" "${NEW_VER}" "${old_sha}" "${new_sha}" > "${WORK}/versions.env"

    # Cached artifacts are only valid for the commit they were built from.
    if [[ "$(cat "${WORK}/release/.built_from" 2>/dev/null)" != "${new_sha}" ]]; then
        rm -rf "${WORK}/release" && mkdir -p "${WORK}/release"
        echo "${new_sha}" > "${WORK}/release/.built_from"
    fi
    git_archive "${new_sha}" "${WORK}/new.tar"
    git_archive "${old_sha}" "${WORK}/old.tar"

    extract_steps "${NEW_VER}"
    build_source_tarball "${NEW_VER}"
    build_binary_tarball "${NEW_VER}"

    # Fake follow-ups for the NEW update.sh's own path (bonus scenario).
    local maj min pat
    IFS=. read -r maj min pat <<<"${NEW_VER}"
    derive_release "${maj}.${min}.$((pat + 1))" no  "${NEW_VER}"   # source-only -> build.new path
    derive_release "${maj}.${min}.$((pat + 2))" yes "${NEW_VER}"   # binary -> install_kiosk_binary path
    derive_release "${maj}.${min}.$((pat + 3))" yes "${NEW_VER}"   # binary, for the crash-rollback case
    cp "${WORK}/steps/${NEW_VER}/release_body.md" "${WORK}/release/body_template.md"
    ls -la "${WORK}"/release/v*/ >&2
}

# --- box image + scenarios --------------------------------------------------

cmd_image() {
    say "building box image ${BOX_IMAGE} (Trixie + the ${OLD_REF} install_deps.sh package set)"
    git -C "${REPO}" show "${OLD_REF}:magic_dingus_box_cpp/scripts/install_deps.sh" > "${HERE}/box/.install_deps.old.sh"
    docker build --platform linux/arm64 -t "${BOX_IMAGE}" "${HERE}/box" 2>&1 | tail -5
    rm -f "${HERE}/box/.install_deps.old.sh"
}

cmd_scenario() {
    local name="$1"
    [[ -f "${WORK}/versions.env" ]] || die "run '$0 artifacts' first"
    docker image inspect "${BOX_IMAGE}" >/dev/null 2>&1 || cmd_image
    say "scenario ${name}"
    local log="${WORK}/logs/scenario_${name}.log"
    rm -rf "${WORK}/logs/${name}" && mkdir -p "${WORK}/logs/${name}"
    # --tmpfs /run: the shim keeps its unit state there, like systemd does.
    if docker run --rm --platform linux/arm64 --hostname magicpi-rehearsal \
        --tmpfs /run:exec,mode=755 \
        -v "${WORK}:/w:ro" -v "${WORK}/logs/${name}:/out" -v "${HERE}/box:/harness:ro" \
        "${BOX_IMAGE}" bash /harness/scenario.sh "${name}" 2>&1 | tee "${log}"; then
        say "scenario ${name}: PASSED (log ${log}, artifacts ${WORK}/logs/${name})"
    else
        say "scenario ${name}: FAILED (log ${log})"; return 1
    fi
}

main() {
    command -v docker >/dev/null && docker info >/dev/null 2>&1 || die "Docker is not running"
    case "${1:-all}" in
        artifacts) cmd_artifacts ;;
        image)     cmd_image ;;
        scenario)  cmd_scenario "${2:?scenario name}" ;;
        all)
            cmd_artifacts; cmd_image
            local rc=0
            for s in old_web old_cli new_path; do cmd_scenario "$s" || rc=1; done
            exit $rc ;;
        *) sed -n '2,40p' "$0"; exit 1 ;;
    esac
}
main "$@"
